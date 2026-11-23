// Prusaslicer - Web Boss 2.9.7: the engine.
//
// Holds one project (models + settings + sliced state) in memory and slices it with
// BOSS's own libslic3r, the way BOSS's GUI does: every change goes through Print::apply(),
// which invalidates only the steps that change affects, and Print::process() redoes just
// those. Talks JSON lines: one command per line on stdin, one event per line on stdout.
// Everything else (libslic3r's own logging) goes to stderr.
//
// Commands (an optional "id" is echoed back on the reply):
//   {"cmd":"hello"}
//   {"cmd":"catalog"}                                     every FFF printer of every bundled vendor
//   {"cmd":"schema"}                                      layout and option definitions of the settings tabs
//   {"cmd":"select_presets","vendor":"PrusaResearch","printer":"...","print":"...","filament":"...",
//    "transfer":false}                                    print and filament are optional; a preset that
//                                                         changes drops its edits, or with "transfer"
//                                                         carries them over to the new one
//   {"cmd":"set","preset":"print","config":{"fill_density":"20%","temperature":{"index":0,"value":"215"},...}}
//                                                         edit the selected presets (values as in a .ini; a
//                                                         list for a whole vector, or one element of it);
//                                                         without "preset", whichever preset has the key
//   {"cmd":"reset","preset":"print","keys":["fill_density","retract_length#1"]}
//                                                         back to the preset's values (all, without keys)
//   {"cmd":"settings"}                                    values, edits and disabled options of all tabs
//   {"cmd":"save_preset","preset":"print","name":"My PLA"} save as a user preset (in $USER_PRESETS_DIR)
//   {"cmd":"delete_preset","preset":"print"}              delete the selected user preset
//   {"cmd":"load","path":"/data/model.stl"}               add the objects in a model file
//   {"cmd":"add_shape","shape":"cube"|"sphere","size":20} add a test shape (mm)
//   {"cmd":"transform","object":3,"instance":0,"matrix":[16 numbers, column-major]}
//   {"cmd":"object_config","object":3,"set":{"fill_density":"40%"},"remove":["perimeters"]}
//                                                         one object's own settings
//   {"cmd":"add_volume","object":3,"type":"modifier","shape":"box"}
//                                                         type: part, negative, modifier, support_enforcer,
//                                                         support_blocker; shape: box, cylinder, sphere, slab
//   {"cmd":"volume_transform","object":3,"volume":5,"matrix":[16 numbers, in the object's coordinates]}
//   {"cmd":"volume_config","object":3,"volume":5,"set":{...},"remove":[...]}
//   {"cmd":"delete_volume","object":3,"volume":5}
//   {"cmd":"delete","object":3}  {"cmd":"clear"}  {"cmd":"arrange"}  {"cmd":"state"}
//   {"cmd":"slice"}                                       slice what changed, write G-code + preview
//   {"cmd":"quit"}
//
// Files go to the current directory: mesh-<volume>.bin (geometry for the browser),
// preview.bin (toolpaths for the browser) and output.gcode or output.bgcode. G-code thumbnails
// are read from thumb-<w>x<h>.rgba, which the browser draws (see browser_thumbnails).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <unistd.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <arrange-wrapper/ModelArrange.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r_version.h" // generated into libslic3r's build folder
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/FileReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MultipleBeds.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "settings.hpp"

using json = nlohmann::json;
using namespace Slic3r;

namespace {

std::mutex out_mutex;
// The real stdout, for events only; see main()
FILE* events_out = stdout;

void emit(const json& event)
{
    std::lock_guard<std::mutex> lock(out_mutex);
    const std::string line = event.dump() + "\n";
    std::fwrite(line.data(), 1, line.size(), events_out);
    std::fflush(events_out);
}

const char* const OBJECT_STEPS[] = {
    "slice", "perimeters", "prepare_infill", "infill", "ironing",
    "support_spots_search", "support_material", "estimate_curled_extrusions",
    "calculate_overhanging_perimeters",
};
static_assert(std::size(OBJECT_STEPS) == posCount, "object step names out of date");

const char* const PRINT_STEPS[] = { "wipe_tower", "alert_when_supports_needed", "skirt_brim", "gcode_export" };
static_assert(std::size(PRINT_STEPS) == psCount, "print step names out of date");

// Settings the plater sidebar shows next to the presets, sent with every state
const char* const SIDEBAR_SETTINGS[] = {
    "support_material", "support_material_auto", "support_material_buildplate_only",
    "fill_density", "brim_width",
};

// Which steps are finished right now, as "object 0: infill" / "print: skirt_brim" labels
std::vector<std::string> finished_steps(const Print& print)
{
    std::vector<std::string> done;
    for (size_t i = 0; i < print.objects().size(); ++i)
        for (int s = 0; s < posCount; ++s)
            if (print.objects()[i]->is_step_done(PrintObjectStep(s)))
                done.push_back("object " + std::to_string(i) + ": " + OBJECT_STEPS[s]);
    for (int s = 0; s < psCount; ++s)
        if (print.is_step_done(PrintStep(s)))
            done.push_back(std::string("print: ") + PRINT_STEPS[s]);
    return done;
}

long long ms_since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

std::string profiles_dir() { return resources_dir() + "/profiles"; }

// Where users' own presets are kept, one folder per vendor with print/, filament/ and
// printer/ inside, as in BOSS's data folder. Unset: no user presets.
std::string user_presets_dir()
{
    const char* dir = std::getenv("USER_PRESETS_DIR");
    return dir ? dir : "";
}

const char* const PRESET_SUBDIRS[] = { "print", "filament", "printer" };

// One vendor's config bundle (resources/profiles/<vendor>.ini) as system presets, and
// optionally the user presets saved for that vendor
std::unique_ptr<PresetBundle> load_vendor_bundle(const std::string& vendor, bool with_user_presets = false)
{
    if (vendor.empty() || vendor.find_first_of("/\\.") != std::string::npos)
        throw std::runtime_error("bad vendor id: " + vendor);
    const std::string path = profiles_dir() + "/" + vendor + ".ini";
    if (!boost::filesystem::exists(path))
        throw std::runtime_error("unknown vendor: " + vendor);
    auto bundle = std::make_unique<PresetBundle>();
    bundle->load_configbundle(path, PresetBundle::LoadSystem, ForwardCompatibilitySubstitutionRule::EnableSilent);

    if (with_user_presets && !user_presets_dir().empty()) {
        const boost::filesystem::path dir = boost::filesystem::path(user_presets_dir()) / vendor;
        PresetCollection* collections[] = { &bundle->prints, &bundle->filaments, &bundle->printers };
        for (int i = 0; i < 3; ++i) {
            boost::filesystem::create_directories(dir / PRESET_SUBDIRS[i]);
            PresetsConfigSubstitutions substitutions;
            try {
                // Also points the collection at this folder, for saving
                collections[i]->load_presets(dir.string(), PRESET_SUBDIRS[i], substitutions,
                                             ForwardCompatibilitySubstitutionRule::EnableSilent);
            } catch (const std::exception& e) {
                emit({ { "event", "warning" }, { "message", std::string("some saved presets could not be loaded: ") + e.what() } });
            }
        }
    }
    return bundle;
}

// A name users can give a preset: it becomes a file name, as in BOSS
void check_preset_name(const std::string& name)
{
    if (name.empty() || name.size() > 120)
        throw std::runtime_error("a preset name needs 1 to 120 characters");
    if (name.find_first_of("<>:\"/\\|?*") != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; }))
        throw std::runtime_error("a preset name can't contain any of < > : \" / \\ | ? *");
    if (name.front() == ' ' || name.back() == ' ' || name.front() == '.' || boost::algorithm::iends_with(name, ".ini"))
        throw std::runtime_error("a preset name can't start or end with a space, start with a dot, or end with .ini");
}

// Every FFF printer preset of every bundled vendor, grouped vendor > model
json catalog()
{
    std::vector<boost::filesystem::path> files;
    for (const auto& entry : boost::filesystem::directory_iterator(profiles_dir()))
        if (entry.path().extension() == ".ini")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    json vendors = json::array();
    for (const auto& file : files) {
        std::unique_ptr<PresetBundle> bundle;
        try {
            bundle = load_vendor_bundle(file.stem().string());
        } catch (const std::exception& e) {
            emit({ { "event", "warning" }, { "message", file.filename().string() + ": " + e.what() } });
            continue;
        }
        for (const auto& [vendor_id, vendor] : bundle->vendors) {
            json models = json::array();
            for (const VendorProfile::PrinterModel& model : vendor.models) {
                if (model.technology != ptFFF)
                    continue;
                json printers = json::array();
                for (const Preset& p : bundle->printers)
                    if (!p.is_default && p.vendor && p.printer_technology() == ptFFF &&
                        p.config.opt_string("printer_model") == model.id)
                        printers.push_back({ { "name", p.name }, { "variant", p.config.opt_string("printer_variant") } });
                if (!printers.empty())
                    models.push_back({ { "id", model.id }, { "name", model.name }, { "family", model.family },
                                       { "printers", printers } });
            }
            if (!models.empty())
                vendors.push_back({ { "id", vendor_id }, { "name", vendor.name }, { "models", models } });
        }
    }
    return vendors;
}

class Project {
public:
    Project() : m_config(DynamicPrintConfig::full_print_config())
    {
        m_print.set_status_callback([](const PrintBase::SlicingStatus& s) {
            if (s.percent >= 0)
                emit({ { "event", "progress" }, { "percent", s.percent }, { "text", s.text } });
        });
    }

    // Picks the printer, then a print and a filament compatible with it (the requested ones
    // when they fit, else the current ones, else the printer's defaults, else the first).
    // A preset that stays selected keeps its edits. One that changes drops them, or, with
    // transfer, they move to the new preset where it has those settings, like BOSS's
    // "Transfer" / "Discard" when switching presets with unsaved changes.
    json select_presets(const std::string& vendor, const std::string& printer_name,
                        const std::string& print_name, const std::string& filament_name, bool transfer)
    {
        // Edits of the presets selected now, in case they're transferred
        std::map<Preset::Type, std::vector<std::pair<std::string, std::unique_ptr<ConfigOption>>>> edits;
        if (m_bundle)
            for (PresetCollection* c : collections()) {
                const DynamicPrintConfig& cfg = c->get_edited_preset().config;
                for (const std::string& key : c->current_dirty_options())
                    if (const ConfigOption* opt = cfg.option(key))
                        edits[c->type()].emplace_back(key, std::unique_ptr<ConfigOption>(opt->clone()));
            }
        const std::string old_selected[3] = { m_print_name, m_filament_name, m_printer_name };

        // A printer saved by another project since this one loaded the vendor's presets: load
        // them again (this project's unsaved edits are lost, as on a vendor change)
        const bool unknown_printer = m_bundle && vendor == m_vendor && !m_bundle->printers.find_preset(printer_name, false, false);
        const bool new_bundle = vendor != m_vendor || !m_bundle || unknown_printer;
        if (new_bundle) {
            m_bundle = load_vendor_bundle(vendor, true);
            m_vendor = vendor;
        }
        const std::string old_bed = m_config.opt_serialize("bed_shape");
        PresetBundle& b = *m_bundle;

        const Preset* printer = b.printers.find_preset(printer_name, false, false);
        if (!printer || printer->is_default || printer->printer_technology() != ptFFF)
            throw std::runtime_error("unknown printer: " + printer_name);
        const PresetWithVendorProfile printer_v = b.printers.get_preset_with_vendor_profile(*printer);

        m_print_choices.clear();
        for (const Preset& p : b.prints)
            if (!p.is_default && is_compatible_with_printer(b.prints.get_preset_with_vendor_profile(p), printer_v))
                m_print_choices.push_back(p.name);
        if (m_print_choices.empty())
            throw std::runtime_error("no print profile fits " + printer_name);
        const std::string print = pick(m_print_choices,
            { print_name, m_print_name, printer->config.opt_string("default_print_profile") });
        const PresetWithVendorProfile print_v = b.prints.get_preset_with_vendor_profile(*b.prints.find_preset(print, false, false));

        m_filament_choices.clear();
        for (const Preset& f : b.filaments) {
            if (f.is_default || !f.is_visible || (f.vendor && f.vendor->templates_profile))
                continue;
            const PresetWithVendorProfile f_v = b.filaments.get_preset_with_vendor_profile(f);
            if (is_compatible_with_printer(f_v, printer_v) && is_compatible_with_print(f_v, print_v, printer_v))
                m_filament_choices.push_back(f.name);
        }
        if (m_filament_choices.empty())
            throw std::runtime_error("no filament profile fits " + printer_name);
        const auto* default_filaments = printer->config.option<ConfigOptionStrings>("default_filament_profile");
        const std::string filament = pick(m_filament_choices,
            { filament_name, m_filament_name,
              default_filaments && !default_filaments->values.empty() ? default_filaments->values.front() : "" });

        // Same steps as BOSS's CLI (load_full_print_config): every extruder gets the filament.
        // Selecting a preset again would reset its edits, so unchanged ones are left alone.
        const std::string wanted[3] = { print, filament, printer->name };
        PresetCollection* const order[3] = { &b.prints, &b.filaments, &b.printers };
        for (int i : { 2, 0, 1 }) {
            if (!new_bundle && wanted[i] == old_selected[i])
                continue;
            order[i]->select_preset_by_name(wanted[i], true);
            if (!transfer)
                continue;
            DynamicPrintConfig& cfg = order[i]->get_edited_preset().config;
            for (const auto& [key, opt] : edits[order[i]->type()]) {
                const ConfigOption* now = cfg.option(key);
                // Per-extruder values only carry over between printers with as many extruders
                if (now && (now->is_scalar() || static_cast<const ConfigOptionVectorBase*>(now)->size() ==
                                                static_cast<const ConfigOptionVectorBase*>(opt.get())->size()))
                    cfg.set_key_value(key, opt->clone());
            }
        }
        m_printer_name  = printer->name;
        m_print_name    = print;
        m_filament_name = filament;
        update_config();
        // A different bed moves the plate's center
        if (m_config.opt_serialize("bed_shape") != old_bed)
            place_on_bed();
        json out = state(true);
        out["settings"] = settings();
        return out;
    }

    json set(const json& values, const std::string& preset)
    {
        if (!m_bundle)
            throw std::runtime_error("choose a printer first");
        json changed = json::array();
        for (auto& [key, value] : values.items()) {
            if (key == "extruders_count") {
                set_extruders_count(value.is_string() ? std::stoi(value.get<std::string>()) : value.get<int>());
                changed.push_back("nozzle_diameter");
                continue;
            }
            const ConfigOptionDef* def = print_config_def.get(key);
            PresetCollection* c = owner(key, preset);
            if (!def || !c)
                throw std::runtime_error("unknown setting: " + key);
            DynamicPrintConfig& cfg = c->get_edited_preset().config;
            std::string text;
            if (value.is_object()) {
                // One element of a vector, e.g. one extruder's retraction length
                const auto* vec = dynamic_cast<const ConfigOptionVectorBase*>(cfg.option(key));
                const size_t index = value.at("index").get<size_t>();
                if (!vec || index >= vec->size())
                    throw std::runtime_error(key + " has no value #" + std::to_string(index));
                std::vector<std::string> elements = web::vector_elements(*vec);
                elements[index] = text_of(value.at("value"));
                text = web::serialize_vector(*def, elements);
            } else if (value.is_array()) {
                std::vector<std::string> elements;
                for (const json& v : value)
                    elements.push_back(text_of(v));
                text = web::serialize_vector(*def, elements);
            } else
                text = text_of(value);
            if (cfg.opt_serialize(key) == text)
                continue;
            // Parsed into a copy first, so a bad value leaves the setting as it was
            std::unique_ptr<ConfigOption> parsed(cfg.option(key)->clone());
            if (!parsed->deserialize(text, false))
                throw std::runtime_error("invalid value for " + (def->label.empty() ? key : def->label) + ": " + text);
            cfg.set_key_value(key, parsed.release());
            changed.push_back(key);
        }
        update_config();
        json out = state(false);
        out["changed"] = changed;
        out["settings"] = settings();
        return out;
    }

    // Saves a preset's current values as a user preset (BOSS's "Save preset"): a new one
    // inherits from the system preset it started from, an existing user preset is
    // overwritten. The saved preset becomes the selected one.
    json save_preset(const std::string& type, const std::string& name)
    {
        if (user_presets_dir().empty())
            throw std::runtime_error("saving presets is turned off on this server");
        check_preset_name(name);
        PresetCollection* c = collection(type);
        if (const Preset* existing = c->find_preset(name, false, false); existing && !existing->is_user())
            throw std::runtime_error("\"" + name + "\" is a system preset; choose another name");
        c->save_current_preset(name);
        selected_name(type) = name;
        // Re-selecting with the same names keeps every edit; it refreshes the lists of
        // compatible presets, which now include the new one
        return select_presets(m_vendor, m_printer_name, m_print_name, m_filament_name, false);
    }

    // Deletes the selected user preset, file and all, and selects the preset it was based on
    json delete_preset(const std::string& type)
    {
        PresetCollection* c = collection(type);
        const Preset& selected = c->get_selected_preset();
        if (!selected.is_user())
            throw std::runtime_error("only presets you saved can be deleted");
        const Preset* parent = c->get_selected_preset_parent();
        std::string next = parent && parent != &selected && !parent->is_default ? parent->name : "";
        c->delete_current_preset();
        if (type == "printer" && next.empty())
            for (const Preset& p : m_bundle->printers)
                if (p.is_system && p.printer_technology() == ptFFF) {
                    next = p.name;
                    break;
                }
        const std::string printer = type == "printer" ? next : m_printer_name;
        return select_presets(m_vendor, printer, type == "print" ? next : m_print_name,
                              type == "filament" ? next : m_filament_name, false);
    }

    // Puts settings back to the selected preset's values: some ("key", or "key#i" for one
    // extruder's value), or all of them
    json reset(const std::string& preset, const std::vector<std::string>& keys)
    {
        PresetCollection* c = collection(preset);
        const DynamicPrintConfig& saved = c->get_selected_preset().config;
        DynamicPrintConfig& cfg = c->get_edited_preset().config;
        if (keys.empty())
            c->discard_current_changes();
        for (const std::string& k : keys) {
            const size_t hash = k.find('#');
            const std::string key = k.substr(0, hash);
            const ConfigOption* from = saved.option(key);
            ConfigOption* to = cfg.option(key);
            if (!from || !to)
                throw std::runtime_error("unknown setting: " + key);
            if (hash == std::string::npos)
                to->set(from);
            else {
                const size_t i = std::stoul(k.substr(hash + 1));
                auto* vec = static_cast<ConfigOptionVectorBase*>(to);
                const size_t saved_count = static_cast<const ConfigOptionVectorBase*>(from)->size();
                // An extruder the preset doesn't have takes the preset's first extruder's value,
                // as when extruders are added (set_num_extruders)
                if (i < vec->size() && saved_count > 0)
                    vec->set_at(from, i, i < saved_count ? i : 0);
            }
        }
        update_config();
        json out = state(false);
        out["settings"] = settings();
        return out;
    }

    // Everything the settings tabs show: each preset's values, which of them differ from the
    // selected preset ("key" or "key#extruder"), and which BOSS would grey out
    json settings() const
    {
        if (!m_bundle)
            return nullptr;
        const PresetBundle& b = *m_bundle;
        json values = json::object(), modified = json::object();
        for (const PresetCollection* c : collections()) {
            const std::string name = type_name(c->type());
            values[name]   = web::preset_values(c->get_edited_preset().config);
            modified[name] = c->current_dirty_options(true);
        }
        return { { "values", values }, { "modified", modified }, { "disabled", web::disabled_options(b) } };
    }

    json load(const std::string& path)
    {
        Model loaded = FileReader::load_model(path);
        if (loaded.objects.empty())
            throw std::runtime_error("no objects in " + boost::filesystem::path(path).filename().string());
        // Project files (3MF) keep their own placement, like BOSS's CLI does
        const bool keep_placement = FileReader::is_project_file(path);
        const bool plate_was_empty = m_model.objects.empty();
        for (ModelObject* o : loaded.objects)
            add_object(*o, !keep_placement || o->instances.empty());
        if (!keep_placement && !(plate_was_empty && loaded.objects.size() == 1))
            arrange_plate();
        return state(false);
    }

    json add_shape(const std::string& shape, double size)
    {
        if (!(size > 0 && size < 10000))
            throw std::runtime_error("size must be between 0 and 10000 mm");
        Model shapes;
        ModelObject* o = shapes.add_object();
        o->name = shape;
        if (shape == "cube")
            o->add_volume(make_cube(size, size, size));
        else if (shape == "sphere")
            o->add_volume(make_sphere(size / 2.));
        else
            throw std::runtime_error("unknown shape: " + shape);
        const bool plate_was_empty = m_model.objects.empty();
        add_object(*o, true);
        if (!plate_was_empty)
            arrange_plate();
        return state(false);
    }

    json transform(size_t object_id, size_t instance_idx, const json& matrix)
    {
        ModelObject* o = find_object(object_id);
        if (instance_idx >= o->instances.size())
            throw std::runtime_error("no such instance");
        o->instances[instance_idx]->set_transformation(Geometry::Transformation(matrix_of(matrix)));
        o->invalidate_bounding_box();
        o->ensure_on_bed();
        return state(false);
    }

    // A transformation from the browser: 16 numbers, column-major (as Eigen and three.js)
    static Transform3d matrix_of(const json& matrix)
    {
        if (!matrix.is_array() || matrix.size() != 16)
            throw std::runtime_error("matrix must be 16 numbers");
        Transform3d t = Transform3d::Identity();
        for (int i = 0; i < 16; ++i)
            t.matrix().data()[i] = matrix[i].get<double>();
        if (std::abs(t.linear().determinant()) < 1e-9)
            throw std::runtime_error("matrix is degenerate");
        return t;
    }

    // An object's own settings, overriding the print preset for that object only (BOSS's
    // "Add settings" in the object list). Values as in a .ini; "remove" drops overrides.
    json object_config(size_t object_id, const json& values, const std::vector<std::string>& remove)
    {
        static const std::vector<std::string> allowed = web::object_options(false);
        apply_overrides(find_object(object_id)->config, values, remove, allowed, "one object");
        return state(false);
    }

    // Adds a part, modifier, negative volume, support enforcer or blocker to an object, as
    // BOSS's object list does (ObjectList::load_generic_subobject): a box, cylinder or sphere a
    // tenth of the bed's size at the object's front right corner, or a slab across its base,
    // lined up with the bed.
    json add_volume(size_t object_id, const std::string& type_name, const std::string& shape)
    {
        ModelObject* o = find_object(object_id);
        const ModelVolumeType type = volume_type(type_name);
        if (o->instances.empty())
            throw std::runtime_error("the object has no instance");
        const BoundingBoxf3 instance_bb = o->instance_bounding_box(0);
        const BoundingBoxf bed = bed_box();
        const double side = 0.1 * std::max(bed.size().x(), bed.size().y());

        indexed_triangle_set its;
        std::string label;
        if (shape == "box")           { its = its_make_cube(side, side, side); label = "Box"; }
        else if (shape == "cylinder") { its = its_make_cylinder(0.564 * side, side); label = "Cylinder"; }
        else if (shape == "sphere")   { its = its_make_sphere(0.62 * side, PI / 18); label = "Sphere"; }
        else if (shape == "slab") {
            its = its_make_cube(instance_bb.size().x() * 1.5, instance_bb.size().y() * 1.5, instance_bb.size().z() * 0.5);
            label = "Slab";
        } else
            throw std::runtime_error("unknown shape: " + shape);

        // The mesh is centered when added
        ModelVolume* v = o->add_volume(TriangleMesh(std::move(its)), type);
        const Geometry::Transformation& inst = o->instances[0]->get_transformation();
        const Transform3d to_object = inst.get_matrix_no_offset().inverse();
        v->set_transformation(Geometry::Transformation(to_object));
        const BoundingBoxf3 mesh_bb = v->mesh().bounding_box();
        Vec3d offset;
        if (shape == "slab") {
            const Vec3d c = instance_bb.center() - inst.get_offset();
            offset = Vec3d(c.x(), c.y(), 0.5 * mesh_bb.size().z() + instance_bb.min.z() - inst.get_offset().z());
        } else
            offset = Vec3d(instance_bb.max.x(), instance_bb.min.y(), instance_bb.min.z()) + 0.5 * mesh_bb.size() - inst.get_offset();
        v->set_offset(to_object * offset);
        v->name = "Generic-" + label;
        v->config.set_key_value("extruder", new ConfigOptionInt(0));
        v->source.is_from_builtin_objects = true;
        o->invalidate_bounding_box();
        if (type == ModelVolumeType::MODEL_PART)
            o->ensure_on_bed();
        write_mesh(*v);
        json out = state(false);
        out["added"] = v->id().id;
        return out;
    }

    // Moves / rotates / scales a volume within its object (matrix in the object's coordinates)
    json volume_transform(size_t object_id, size_t volume_id, const json& matrix)
    {
        auto [o, v] = find_volume(object_id, volume_id);
        v->set_transformation(Geometry::Transformation(matrix_of(matrix)));
        o->invalidate_bounding_box();
        if (v->is_model_part())
            o->ensure_on_bed();
        return state(false);
    }

    json delete_volume(size_t object_id, size_t volume_id)
    {
        auto [o, v] = find_volume(object_id, volume_id);
        if (v->is_model_part() && std::count_if(o->volumes.begin(), o->volumes.end(),
                                                [](const ModelVolume* x) { return x->is_model_part(); }) == 1)
            throw std::runtime_error("an object needs at least one part; delete the object instead");
        o->delete_volume(std::find(o->volumes.begin(), o->volumes.end(), v) - o->volumes.begin());
        o->invalidate_bounding_box();
        o->ensure_on_bed();
        return state(false);
    }

    // A part's or modifier's own settings (BOSS offers the region settings for these)
    json volume_config(size_t object_id, size_t volume_id, const json& values, const std::vector<std::string>& remove)
    {
        static const std::vector<std::string> allowed = web::object_options(true);
        auto [o, v] = find_volume(object_id, volume_id);
        if (!v->is_model_part() && !v->is_modifier())
            throw std::runtime_error("only parts and modifiers have settings");
        apply_overrides(v->config, values, remove, allowed, "a part or modifier");
        return state(false);
    }

    json remove(size_t object_id)
    {
        ModelObject* o = find_object(object_id);
        m_model.delete_object(o);
        return state(false);
    }

    json clear()
    {
        m_model.clear_objects();
        return state(false);
    }

    json arrange()
    {
        arrange_plate();
        return state(false);
    }

    json slice()
    {
        if (m_model.objects.empty())
            throw std::runtime_error("nothing to slice: add a model first");

        const auto t_apply = std::chrono::steady_clock::now();
        for (ModelObject* o : m_model.objects)
            m_print.auto_assign_extruders(o);
        update_print_volume();
        PrintBase::ApplyStatus status = PrintBase::APPLY_STATUS_UNCHANGED;
        MultipleBedsUtils::with_single_bed_model_fff(m_model, 0, [&]() { status = m_print.apply(m_model, m_config); });
        const long long apply_ms = ms_since(t_apply);

        if (std::string err = m_print.validate(); !err.empty())
            throw std::runtime_error(err);
        if (m_print.empty())
            throw std::runtime_error("nothing to print: no object is fully inside the print volume");

        const std::vector<std::string> before = finished_steps(m_print);

        const auto t_process = std::chrono::steady_clock::now();
        m_print.process();
        const long long process_ms = ms_since(t_process);

        // G-code is written again only when the print changed (or nothing was written yet)
        const auto t_export = std::chrono::steady_clock::now();
        const std::string gcode_file = m_config.opt_bool("binary_gcode") ? "output.bgcode" : "output.gcode";
        if (status != PrintBase::APPLY_STATUS_UNCHANGED || gcode_file != m_gcode_file) {
            GCodeProcessorResult result;
            m_print.export_gcode(gcode_file, &result, browser_thumbnails);
            m_gcode_file = gcode_file;
            m_preview = write_preview(result, "preview.bin");
            m_output_name = m_print.output_filename();
        }
        const long long export_ms = ms_since(t_export);

        json recomputed = json::array();
        for (const std::string& step : finished_steps(m_print))
            if (std::find(before.begin(), before.end(), step) == before.end())
                recomputed.push_back(step);

        const PrintStatistics& stats = m_print.print_statistics();
        return {
            { "apply_status", status == PrintBase::APPLY_STATUS_UNCHANGED ? "unchanged"
                            : status == PrintBase::APPLY_STATUS_CHANGED   ? "changed" : "invalidated" },
            { "reused", before },
            { "recomputed", recomputed },
            { "ms", { { "apply", apply_ms }, { "process", process_ms }, { "export", export_ms } } },
            { "gcode", m_gcode_file },
            { "output_name", m_output_name },
            { "preview", m_preview },
            { "stats", { { "print_time", stats.estimated_normal_print_time },
                         { "filament_mm", stats.total_used_filament },
                         { "filament_g", stats.total_weight },
                         { "cost", stats.total_cost },
                         { "layers", m_print.objects().empty() ? 0 : m_print.objects().front()->layer_count() } } },
        };
    }

    // The plate as the browser draws it. Lists of compatible presets only when asked
    // (they're long, and only change with the printer).
    json state(bool with_choices) const
    {
        json objects = json::array();
        for (const ModelObject* o : m_model.objects) {
            json instances = json::array();
            for (const ModelInstance* i : o->instances) {
                const Transform3d& t = i->get_matrix();
                instances.push_back({ { "matrix", std::vector<double>(t.matrix().data(), t.matrix().data() + 16) } });
            }
            const Vec3d size = o->instances.empty() ? Vec3d::Zero() : o->instance_bounding_box(0).size();
            // Parts, modifiers, negative volumes, support enforcers and blockers, each placed
            // in the object's coordinates by its own matrix
            json volumes = json::array();
            for (const ModelVolume* v : o->volumes) {
                const Transform3d& t = v->get_matrix();
                volumes.push_back({ { "id", v->id().id },
                                    { "name", v->name },
                                    { "type", volume_type_name(v->type()) },
                                    { "mesh", mesh_file(v) },
                                    { "matrix", std::vector<double>(t.matrix().data(), t.matrix().data() + 16) },
                                    { "settings", overrides_json(v->config) } });
            }
            objects.push_back({ { "id", o->id().id },
                                { "name", o->name },
                                { "volumes", volumes },
                                { "instances", instances },
                                { "size", { size.x(), size.y(), size.z() } },
                                { "settings", overrides_json(o->config) } });
        }

        json settings = json::object();
        for (const char* key : SIDEBAR_SETTINGS)
            if (m_config.has(key))
                settings[key] = m_config.opt_serialize(key);

        json bed = json::array();
        for (const Vec2d& p : m_config.option<ConfigOptionPoints>("bed_shape")->values)
            bed.push_back({ p.x(), p.y() });

        json presets = nullptr;
        if (m_bundle) {
            // Which of the selected presets are the user's own (they can be deleted)
            json user = json::object();
            for (const PresetCollection* c : collections())
                user[type_name(c->type())] = c->get_selected_preset().is_user();
            presets = { { "vendor", m_vendor }, { "printer", m_printer_name }, { "print", m_print_name },
                        { "filament", m_filament_name }, { "user", user } };
        }
        json out = {
            { "presets", presets },
            { "bed", { { "shape", bed }, { "max_height", m_config.opt_float("max_print_height") } } },
            { "settings", settings },
            { "objects", objects },
        };
        if (with_choices && m_bundle) {
            // User presets are listed apart, as in BOSS's combo boxes
            json user = json::array();
            for (const auto& [c, names] : { std::pair{ &m_bundle->prints, &m_print_choices },
                                            std::pair{ &m_bundle->filaments, &m_filament_choices } })
                for (const std::string& name : *names)
                    if (const Preset* p = c->find_preset(name, false, false); p && p->is_user())
                        user.push_back(name);
            out["choices"] = { { "prints", m_print_choices }, { "filaments", m_filament_choices }, { "user", user } };
        }
        return { { "state", out } };
    }

private:
    std::vector<PresetCollection*> collections() const
    {
        return { &m_bundle->prints, &m_bundle->filaments, &m_bundle->printers };
    }

    static const char* type_name(Preset::Type type)
    {
        return type == Preset::TYPE_PRINT ? "print" : type == Preset::TYPE_FILAMENT ? "filament" : "printer";
    }

    PresetCollection* collection(const std::string& name) const
    {
        if (!m_bundle)
            throw std::runtime_error("choose a printer first");
        for (PresetCollection* c : collections())
            if (name == type_name(c->type()))
                return c;
        throw std::runtime_error("unknown preset type: " + name);
    }

    // The preset a setting belongs to: the one named, or the first that has it
    PresetCollection* owner(const std::string& key, const std::string& preset) const
    {
        if (!preset.empty()) {
            PresetCollection* c = collection(preset);
            return c->get_edited_preset().config.has(key) ? c : nullptr;
        }
        for (PresetCollection* c : collections())
            if (c->get_edited_preset().config.has(key))
                return c;
        return nullptr;
    }

    static std::string text_of(const json& v) { return v.is_string() ? v.get<std::string>() : v.dump(); }

    std::string& selected_name(const std::string& type)
    {
        collection(type);
        return type == "print" ? m_print_name : type == "filament" ? m_filament_name : m_printer_name;
    }

    // BOSS's printer tab changes the number of extruders by resizing every per-extruder
    // option (Preset::set_num_extruders)
    void set_extruders_count(int n)
    {
        if (n < 1 || n > 256)
            throw std::runtime_error("the number of extruders must be between 1 and 256");
        m_bundle->printers.get_edited_preset().set_num_extruders(unsigned(n));
    }

    // The config slicing uses, from the edited presets. Every extruder gets the filament.
    void update_config()
    {
        PresetBundle& b = *m_bundle;
        const size_t extruders = b.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter")->values.size();
        b.extruders_filaments.clear();
        for (size_t i = 0; i < extruders; ++i)
            b.extruders_filaments.emplace_back(ExtruderFilaments(&b.filaments, i, m_filament_name));
        m_config = b.full_config();
        m_config.normalize_fdm();
    }

    static std::string pick(const std::vector<std::string>& choices, std::initializer_list<std::string> wanted)
    {
        for (std::string w : wanted) {
            // Some bundles quote the default profile names (default_print_profile = "...")
            if (w.size() >= 2 && w.front() == '"' && w.back() == '"')
                w = w.substr(1, w.size() - 2);
            if (!w.empty() && std::find(choices.begin(), choices.end(), w) != choices.end())
                return w;
        }
        return choices.front();
    }

    static std::string mesh_file(const ModelVolume* v) { return "mesh-" + std::to_string(v->id().id) + ".bin"; }

    ModelObject* find_object(size_t id)
    {
        for (ModelObject* o : m_model.objects)
            if (o->id().id == id)
                return o;
        throw std::runtime_error("no such object: " + std::to_string(id));
    }

    // Copies an object onto the plate and writes its mesh for the browser. When placing,
    // its single instance goes to the bed's center.
    void add_object(const ModelObject& source, bool place)
    {
        ModelObject* o = m_model.add_object(source);
        if (place) {
            o->clear_instances();
            o->add_instance();
            // Like BOSS's plater: the object's origin is its middle, so it rotates and
            // scales around that
            o->center_around_origin();
        }
        o->ensure_on_bed();
        if (place)
            center_on_bed(*o);
        for (const ModelVolume* v : o->volumes)
            write_mesh(*v);
    }

    // Moves an object's first instance so it sits in the middle of the bed
    void center_on_bed(ModelObject& o) const
    {
        const Vec3d c = o.instance_bounding_box(0).center();
        const Vec2d bed = bed_box().center();
        o.instances[0]->set_offset(o.instances[0]->get_offset() + Vec3d(bed.x() - c.x(), bed.y() - c.y(), 0.));
        o.invalidate_bounding_box();
    }

    // After a bed change: a lone object goes to the new center, several get arranged
    void place_on_bed()
    {
        if (m_model.objects.size() == 1 && m_model.objects.front()->instances.size() == 1)
            center_on_bed(*m_model.objects.front());
        else if (!m_model.objects.empty())
            arrange_plate();
    }

    void arrange_plate()
    {
        if (m_model.objects.empty())
            return;
        const arr2::ArrangeBed bed = arr2::to_arrange_bed(get_bed_shape(m_config), s_multiple_beds.get_bed_gap());
        arr2::ArrangeSettings settings;
        // BOSS's plater default spacing, or more if the print needs it (skirt, brim)
        settings.set_distance_from_objects(float(std::max(6., min_object_distance(m_config))));
        arrange_objects(m_model, bed, settings);
        for (ModelObject* o : m_model.objects) {
            o->invalidate_bounding_box();
            o->ensure_on_bed();
        }
    }

    // Binary mesh for the browser, one per volume (part, modifier, ...), in the volume's own
    // coordinates (volume and instance matrices place it): "PSWM", uint32 vertex count,
    // uint32 triangle count, float32 xyz per vertex, uint32 indices per triangle.
    // Little-endian. A volume's mesh never changes, only its matrix.
    static void write_mesh(const ModelVolume& v)
    {
        const auto& its = v.mesh().its;
        std::ofstream f(mesh_file(&v), std::ios::binary);
        const uint32_t counts[2] = { uint32_t(its.vertices.size()), uint32_t(its.indices.size()) };
        f.write("PSWM", 4);
        f.write(reinterpret_cast<const char*>(counts), sizeof(counts));
        f.write(reinterpret_cast<const char*>(its.vertices.data()), its.vertices.size() * sizeof(stl_vertex));
        f.write(reinterpret_cast<const char*>(its.indices.data()), its.indices.size() * sizeof(stl_triangle_vertex_indices));
        if (!f)
            throw std::runtime_error("could not write " + mesh_file(&v));
    }

    static const char* volume_type_name(ModelVolumeType type)
    {
        switch (type) {
        case ModelVolumeType::NEGATIVE_VOLUME:    return "negative";
        case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
        case ModelVolumeType::SUPPORT_BLOCKER:    return "support_blocker";
        case ModelVolumeType::SUPPORT_ENFORCER:   return "support_enforcer";
        default:                                  return "part";
        }
    }

    static ModelVolumeType volume_type(const std::string& name)
    {
        for (ModelVolumeType t : { ModelVolumeType::MODEL_PART, ModelVolumeType::NEGATIVE_VOLUME, ModelVolumeType::PARAMETER_MODIFIER,
                                   ModelVolumeType::SUPPORT_BLOCKER, ModelVolumeType::SUPPORT_ENFORCER })
            if (name == volume_type_name(t))
                return t;
        throw std::runtime_error("unknown volume type: " + name);
    }

    std::pair<ModelObject*, ModelVolume*> find_volume(size_t object_id, size_t volume_id)
    {
        ModelObject* o = find_object(object_id);
        for (ModelVolume* v : o->volumes)
            if (v->id().id == volume_id)
                return { o, v };
        throw std::runtime_error("no such part: " + std::to_string(volume_id));
    }

    // Settings overrides on an object or a volume; values as in a .ini
    void apply_overrides(ModelConfigObject& config, const json& values, const std::vector<std::string>& remove,
                         const std::vector<std::string>& allowed, const char* what)
    {
        for (auto& [key, value] : values.items()) {
            const ConfigOptionDef* def = print_config_def.get(key);
            if (!def || std::find(allowed.begin(), allowed.end(), key) == allowed.end())
                throw std::runtime_error(key + " can't be set for " + what);
            std::unique_ptr<ConfigOption> opt(def->create_default_option());
            const std::string text = text_of(value);
            if (!opt->deserialize(text, false))
                throw std::runtime_error("invalid value for " + (def->label.empty() ? key : def->label) + ": " + text);
            config.set_key_value(key, opt.release());
        }
        for (const std::string& key : remove)
            config.erase(key);
    }

    // Overrides as the browser shows them. "extruder" is BOSS's object list column, not a
    // setting there.
    static json overrides_json(const ModelConfigObject& config)
    {
        json out = json::object();
        for (const std::string& key : config.keys())
            if (key != "extruder")
                out[key] = config.opt_serialize(key);
        return out;
    }

    // Toolpaths for the browser, one segment per extrusion/travel/wipe move: "PSWP",
    // uint32 version, uint32 segment count n, then arrays of n: float32 xyz start, float32
    // xyz end, float32 width, float32 height, float32 feedrate (mm/s), uint32 layer, and
    // uint8 move type, uint8 feature (GCodeExtrusionRole), uint8 extruder.
    static json write_preview(const GCodeProcessorResult& result, const std::string& path)
    {
        using MoveType = EMoveType;
        std::vector<float> start, end, width, height, feedrate;
        std::vector<uint32_t> layer;
        std::vector<uint8_t> type, role, extruder;
        uint32_t layers = 0;
        for (size_t i = 1; i < result.moves.size(); ++i) {
            const auto& m = result.moves[i];
            if (m.type != MoveType::Extrude && m.type != MoveType::Travel && m.type != MoveType::Wipe)
                continue;
            const Vec3f& a = result.moves[i - 1].position;
            if (a == m.position)
                continue;
            start.insert(start.end(), { a.x(), a.y(), a.z() });
            end.insert(end.end(), { m.position.x(), m.position.y(), m.position.z() });
            width.push_back(m.width);
            height.push_back(m.height);
            feedrate.push_back(m.feedrate);
            layer.push_back(m.layer_id);
            type.push_back(uint8_t(m.type));
            role.push_back(uint8_t(m.extrusion_role));
            extruder.push_back(m.extruder_id);
            layers = std::max(layers, m.layer_id + 1);
        }

        std::ofstream f(path, std::ios::binary);
        const uint32_t header[2] = { 1, uint32_t(layer.size()) };
        f.write("PSWP", 4);
        f.write(reinterpret_cast<const char*>(header), sizeof(header));
        auto put = [&f](const auto& v) { f.write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(v[0])); };
        put(start); put(end); put(width); put(height); put(feedrate); put(layer);
        put(type); put(role); put(extruder);
        if (!f)
            throw std::runtime_error("could not write " + path);
        return { { "file", path }, { "segments", layer.size() }, { "layers", layers } };
    }

    // G-code thumbnails. BOSS draws them with OpenGL in its window; the engine has none, so
    // the browser draws the plate at each size the printer wants (its "thumbnails" setting)
    // and uploads the pixels as thumb-<w>x<h>.rgba before slicing: RGBA, bottom row first,
    // as glReadPixels gives them to BOSS. libslic3r encodes them (PNG, JPG, QOI) itself.
    // A size the browser didn't draw is left out.
    static ThumbnailsList browser_thumbnails(const ThumbnailsParams& params)
    {
        ThumbnailsList list;
        for (const Vec2d& size : params.sizes) {
            const unsigned w = unsigned(size.x()), h = unsigned(size.y());
            const std::string file = "thumb-" + std::to_string(w) + "x" + std::to_string(h) + ".rgba";
            std::ifstream f(file, std::ios::binary);
            if (!f || w == 0 || h == 0)
                continue;
            ThumbnailData t;
            t.set(w, h);
            f.read(reinterpret_cast<char*>(t.pixels.data()), std::streamsize(t.pixels.size()));
            if (f.gcount() == std::streamsize(t.pixels.size()))
                list.push_back(std::move(t));
        }
        return list;
    }

    BoundingBoxf bed_box() const
    {
        return BoundingBoxf(m_config.option<ConfigOptionPoints>("bed_shape")->values);
    }

    void update_print_volume()
    {
        const Pointfs& bed_shape = m_config.option<ConfigOptionPoints>("bed_shape")->values;
        BuildVolume build_volume(bed_shape, m_config.opt_float("max_print_height"));
        s_multiple_beds.update_build_volume(BoundingBoxf(bed_shape));
        m_model.update_print_volume_state(build_volume);
    }

    Model              m_model;
    DynamicPrintConfig m_config;
    Print              m_print;

    std::string                   m_vendor;
    std::unique_ptr<PresetBundle> m_bundle;
    std::string                   m_printer_name, m_print_name, m_filament_name;
    std::vector<std::string>      m_print_choices, m_filament_choices;

    std::string m_gcode_file, m_output_name;
    json        m_preview;
};

json handle(Project& project, const json& msg)
{
    const std::string cmd = msg.value("cmd", "");
    json reply;
    if (cmd == "hello")
        reply = { { "engine", "prusaslicer-web-boss-297" }, { "libslic3r", SLIC3R_VERSION } };
    else if (cmd == "catalog")
        reply = { { "vendors", catalog() } };
    else if (cmd == "schema")
        reply = web::settings_schema();
    else if (cmd == "select_presets")
        reply = project.select_presets(msg.at("vendor").get<std::string>(), msg.at("printer").get<std::string>(),
                                       msg.value("print", ""), msg.value("filament", ""), msg.value("transfer", false));
    else if (cmd == "set")
        reply = project.set(msg.at("config"), msg.value("preset", ""));
    else if (cmd == "reset")
        reply = project.reset(msg.at("preset").get<std::string>(), msg.value("keys", std::vector<std::string>{}));
    else if (cmd == "settings")
        reply = { { "settings", project.settings() } };
    else if (cmd == "save_preset")
        reply = project.save_preset(msg.at("preset").get<std::string>(), msg.at("name").get<std::string>());
    else if (cmd == "delete_preset")
        reply = project.delete_preset(msg.at("preset").get<std::string>());
    else if (cmd == "load")
        reply = project.load(msg.at("path").get<std::string>());
    else if (cmd == "add_shape")
        reply = project.add_shape(msg.at("shape").get<std::string>(), msg.value("size", 20.0));
    else if (cmd == "transform")
        reply = project.transform(msg.at("object").get<size_t>(), msg.value("instance", size_t(0)), msg.at("matrix"));
    else if (cmd == "object_config")
        reply = project.object_config(msg.at("object").get<size_t>(), msg.value("set", json::object()),
                                      msg.value("remove", std::vector<std::string>{}));
    else if (cmd == "add_volume")
        reply = project.add_volume(msg.at("object").get<size_t>(), msg.at("type").get<std::string>(),
                                   msg.at("shape").get<std::string>());
    else if (cmd == "volume_transform")
        reply = project.volume_transform(msg.at("object").get<size_t>(), msg.at("volume").get<size_t>(), msg.at("matrix"));
    else if (cmd == "delete_volume")
        reply = project.delete_volume(msg.at("object").get<size_t>(), msg.at("volume").get<size_t>());
    else if (cmd == "volume_config")
        reply = project.volume_config(msg.at("object").get<size_t>(), msg.at("volume").get<size_t>(),
                                      msg.value("set", json::object()), msg.value("remove", std::vector<std::string>{}));
    else if (cmd == "delete")
        reply = project.remove(msg.at("object").get<size_t>());
    else if (cmd == "clear")
        reply = project.clear();
    else if (cmd == "arrange")
        reply = project.arrange();
    else if (cmd == "state")
        reply = project.state(true);
    else if (cmd == "slice")
        reply = project.slice();
    else
        throw std::runtime_error("unknown command: " + cmd);
    reply["event"] = cmd;
    return reply;
}

} // namespace

int main()
{
    // Keep stdout for events: anything else that writes there (libslic3r's logging can)
    // goes to stderr instead
    fflush(stdout);
    if (const int fd = dup(STDOUT_FILENO); fd >= 0 && (events_out = fdopen(fd, "w")) != nullptr)
        dup2(STDERR_FILENO, STDOUT_FILENO);
    else
        events_out = stdout;

    const char* resources = std::getenv("ENGINE_RESOURCES");
    set_resources_dir(resources ? resources : "/opt/engine/resources");
    set_data_dir("/tmp/engine-data");
    set_logging_level(1);

    Project project;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        json msg;
        try {
            msg = json::parse(line);
            if (msg.value("cmd", "") == "quit")
                break;
            json reply = handle(project, msg);
            if (msg.contains("id"))
                reply["id"] = msg["id"];
            emit(reply);
        } catch (const std::exception& e) {
            json err = { { "event", "error" }, { "message", e.what() } };
            if (msg.is_object()) {
                if (msg.contains("id"))
                    err["id"] = msg["id"];
                err["cmd"] = msg.value("cmd", "");
            }
            emit(err);
        }
    }
    return 0;
}
