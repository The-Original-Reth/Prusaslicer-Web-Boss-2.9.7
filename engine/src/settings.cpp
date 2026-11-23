#include "settings.hpp"

#include <algorithm>
#include <cfloat>
#include <fstream>
#include <map>
#include <set>

#include <boost/algorithm/string.hpp>

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"

using json = nlohmann::json;
using namespace Slic3r;

namespace web {

namespace {

const char* type_name(ConfigOptionType type)
{
    switch (type) {
    case coFloat: case coFloats:                     return "float";
    case coInt: case coInts:                         return "int";
    case coString: case coStrings:                   return "string";
    case coPercent: case coPercents:                 return "percent";
    case coFloatOrPercent: case coFloatsOrPercents:  return "float_or_percent";
    case coPoint: case coPoints:                     return "point";
    case coPoint3:                                   return "point3";
    case coBool: case coBools:                       return "bool";
    case coEnum: case coEnums:                       return "enum";
    default:                                         return "none";
    }
}

const char* gui_type_name(ConfigOptionDef::GUIType t)
{
    using G = ConfigOptionDef::GUIType;
    switch (t) {
    case G::i_enum_open:  return "i_enum_open";
    case G::f_enum_open:  return "f_enum_open";
    case G::select_open:  return "select_open";
    case G::color:        return "color";
    case G::slider:       return "slider";
    case G::legend:       return "legend";
    case G::one_string:   return "one_string";
    case G::select_close: return "select_close";
    case G::password:     return "password";
    default:              return "";
    }
}

json option_json(const ConfigOptionDef& d)
{
    static const char* const modes[] = { "simple", "advanced", "expert" };
    json j = {
        { "type", type_name(d.type) },
        { "label", d.label },
        { "full_label", d.full_label.empty() ? d.label : d.full_label },
        { "tooltip", d.tooltip },
        { "sidetext", d.sidetext },
        { "mode", d.mode <= comExpert ? modes[d.mode] : "expert" },
        { "category", d.category },
    };
    if (!d.is_scalar()) {
        j["vector"] = true;
        // Vectors shown as one input (bed shape, post-processing scripts), not one per extruder
        if (PresetCollection::is_independent_from_extruder_number_option(d.opt_key))
            j["whole"] = true;
    }
    if (d.nullable)           j["nullable"] = true;
    if (d.min > -FLT_MAX)     j["min"] = d.min;
    if (d.max < FLT_MAX)      j["max"] = d.max;
    if (d.multiline)          j["multiline"] = true;
    if (d.full_width)         j["full_width"] = true;
    if (d.is_code)            j["is_code"] = true;
    if (d.readonly)           j["readonly"] = true;
    if (d.height > 0)         j["height"] = d.height;
    if (!d.ratio_over.empty()) j["ratio_over"] = d.ratio_over;
    if (!d.gui_flags.empty()) j["gui_flags"] = d.gui_flags;
    if (const char* g = gui_type_name(d.gui_type); *g)
        j["gui_type"] = g;
    // Turning on a filament override starts from this when the printer has no such value
    if (const ConfigOption* def = d.default_value.get()) {
        if (def->is_scalar())
            j["default"] = def->serialize();
        else if (const auto v = static_cast<const ConfigOptionVectorBase*>(def)->vserialize(); !v.empty())
            j["default"] = v.front();
    }
    if (d.enum_def && (d.enum_def->has_values() || d.enum_def->has_labels())) {
        j["enum_values"] = d.enum_def->has_values() ? d.enum_def->values() : d.enum_def->labels();
        j["enum_labels"] = d.enum_def->labels();
    }
    return j;
}

// Not a stored setting: BOSS's printer tab edits the number of extruders through the length
// of nozzle_diameter (TabPrinter::build_fff)
json extruders_count_json()
{
    return { { "type", "int" }, { "label", "Extruders" }, { "full_label", "Extruders" },
             { "tooltip", "Number of extruders of the printer." }, { "sidetext", "" },
             { "mode", "expert" }, { "min", 1 }, { "max", 256 } };
}

void collect_keys(const json& node, std::set<std::string>& keys)
{
    if (node.is_object()) {
        if (node.contains("key") && node["key"].is_string())
            keys.insert(node["key"].get<std::string>());
        for (const auto& [_, v] : node.items())
            collect_keys(v, keys);
    } else if (node.is_array()) {
        for (const auto& v : node)
            collect_keys(v, keys);
    }
}

} // namespace

std::vector<std::string> object_options(bool part)
{
    std::vector<std::string> keys = PrintRegionConfig().keys();
    if (!part) {
        const std::vector<std::string> object_keys = PrintObjectConfig().keys();
        keys.insert(keys.end(), object_keys.begin(), object_keys.end());
    }
    // Options without a category aren't offered (is_improper_category)
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const std::string& k) {
                   const ConfigOptionDef* d = print_config_def.get(k);
                   return !d || d->category.empty();
               }), keys.end());
    return keys;
}

json settings_schema()
{
    const std::string path = resources_dir() + "/web/tab_layout.json";
    std::ifstream f(path);
    if (!f)
        throw std::runtime_error("missing " + path);
    const json layout = json::parse(f);

    std::set<std::string> keys;
    collect_keys(layout, keys);
    const std::vector<std::string> per_object = object_options(false), per_part = object_options(true);
    keys.insert(per_object.begin(), per_object.end());
    json options = json::object();
    json missing = json::array();
    for (const std::string& key : keys) {
        if (key == "extruders_count")
            options[key] = extruders_count_json();
        else if (const ConfigOptionDef* d = print_config_def.get(key))
            options[key] = option_json(*d);
        else
            missing.push_back(key);
    }
    return { { "layout", layout }, { "options", options }, { "missing", missing },
             { "object_options", per_object }, { "part_options", per_part } };
}

json preset_values(const DynamicPrintConfig& config)
{
    json values = json::object();
    for (const std::string& key : config.keys()) {
        const ConfigOption* opt = config.option(key);
        if (opt->is_scalar())
            values[key] = opt->serialize();
        else
            values[key] = vector_elements(*opt);
    }
    return values;
}

std::vector<std::string> vector_elements(const ConfigOption& opt)
{
    if (opt.type() == coPoints) {
        // Points as in a .ini ("220x0"); vserialize() would give "220,0"
        std::vector<std::string> points;
        const std::string text = opt.serialize();
        if (!text.empty())
            boost::split(points, text, boost::is_any_of(","));
        return points;
    }
    return static_cast<const ConfigOptionVectorBase&>(opt).vserialize();
}

std::string serialize_vector(const ConfigOptionDef& def, const std::vector<std::string>& values)
{
    if (def.type == coStrings)
        return escape_strings_cstyle(values);
    std::string out;
    for (size_t i = 0; i < values.size(); ++i)
        out += (i ? "," : "") + values[i];
    return out;
}

// The rules below are BOSS's, from src/slic3r/GUI/ConfigManipulation.cpp and Tab.cpp, with
// toggle_field(key, on) collecting the keys that are off. The GUI reads these configs through
// non-const accessors; they don't change anything unless asked to create an option.
json disabled_options(const PresetBundle& bundle)
{
    // Several rules can toggle the same field; as in the GUI, the last one wins
    std::map<std::string, bool> enabled_state;
    auto toggle = [&enabled_state](const std::string& key, bool on, int index = -1) {
        enabled_state[index < 0 ? key : key + "#" + std::to_string(index)] = on;
    };

    DynamicPrintConfig* config  = const_cast<DynamicPrintConfig*>(&bundle.prints.get_edited_preset().config);
    DynamicPrintConfig* fconfig = const_cast<DynamicPrintConfig*>(&bundle.filaments.get_edited_preset().config);
    DynamicPrintConfig* pconfig = const_cast<DynamicPrintConfig*>(&bundle.printers.get_edited_preset().config);
    const auto gcflavor = pconfig->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;

    // ---- Print Settings: ConfigManipulation::toggle_print_fff_options ----
    {
        auto toggle_field = [&](const std::string& key, bool on) { toggle(key, on); };

        bool have_perimeters = config->opt_int("perimeters") > 0;
        for (auto el : { "extra_perimeters","extra_perimeters_on_overhangs", "thin_walls", "overhangs",
                        "seam_position","staggered_inner_seams", "external_perimeters_first", "external_perimeter_extrusion_width",
                        "perimeter_speed", "small_perimeter_speed", "external_perimeter_speed", "enable_dynamic_overhang_speeds",
                        "small_perimeter_min_length", "small_perimeter_max_length", "external_perimeters_first_holes",
                        "external_perimeters_first_holes_min_size", "external_perimeters_first_disabled_first_layers",
                        "seam_type", "seam_notch_width", "seam_notch_angle" })
            toggle_field(el, have_perimeters);

        bool has_seam_notch = have_perimeters && config->opt_enum<SeamNotchType>("seam_type") != sntRegular;
        toggle_field("seam_notch_width", has_seam_notch);
        toggle_field("seam_notch_angle", has_seam_notch);

        for (size_t i = 0; i < 4; i++)
            toggle_field("overhang_speed_" + std::to_string(i), config->opt_bool("enable_dynamic_overhang_speeds"));

        const bool have_infill = config->option<ConfigOptionPercent>("fill_density")->value > 0;
        const bool has_automatic_infill_combination = config->option<ConfigOptionBool>("automatic_infill_combination")->value;
        for (auto el : { "fill_pattern","solid_infill_every_layers", "solid_infill_below_area", "infill_extruder",
                        "infill_anchor_max", "automatic_infill_combination" })
            toggle_field(el, have_infill);
        toggle_field("infill_every_layers", have_infill && !has_automatic_infill_combination);
        toggle_field("automatic_infill_combination_max_layer_height", have_infill && has_automatic_infill_combination);

        bool has_infill_anchors = have_infill && config->option<ConfigOptionFloatOrPercent>("infill_anchor_max")->value > 0;
        toggle_field("infill_anchor", has_infill_anchors);

        bool has_spiral_vase         = config->opt_bool("spiral_vase");
        bool has_top_solid_infill    = config->opt_int("top_solid_layers") > 0;
        bool has_bottom_solid_infill = config->opt_int("bottom_solid_layers") > 0;
        bool has_solid_infill        = has_top_solid_infill || has_bottom_solid_infill;
        for (auto el : { "top_fill_pattern", "bottom_fill_pattern", "solid_fill_pattern", "infill_first", "solid_infill_extruder",
                        "solid_infill_extrusion_width", "solid_infill_speed" })
            toggle_field(el, has_solid_infill);

        bool has_narrow_detection = has_solid_infill && config->opt_bool("detect_narrow_solid_infill");
        toggle_field("detect_narrow_solid_infill", has_solid_infill);
        toggle_field("detect_narrow_solid_infill_threshold", has_narrow_detection);

        for (auto el : { "fill_angle", "bridge_angle", "infill_extrusion_width",
                        "infill_speed", "bridge_speed", "over_bridge_speed" })
            toggle_field(el, have_infill || has_solid_infill);

        for (auto el : { "small_perimeter_min_length", "small_perimeter_max_length" })
            toggle_field(el, config->option<ConfigOptionFloatOrPercent>("small_perimeter_speed")->value > 0);

        const bool has_ensure_vertical_shell_thickness = config->opt_enum<EnsureVerticalShellThickness>("ensure_vertical_shell_thickness") != EnsureVerticalShellThickness::Disabled;
        toggle_field("top_solid_min_thickness", !has_spiral_vase && has_top_solid_infill && has_ensure_vertical_shell_thickness);
        toggle_field("bottom_solid_min_thickness", !has_spiral_vase && has_bottom_solid_infill && has_ensure_vertical_shell_thickness);

        toggle_field("gap_fill_speed", have_perimeters);

        for (auto el : { "top_infill_extrusion_width", "top_solid_infill_speed" })
            toggle_field(el, has_top_solid_infill || (has_spiral_vase && has_bottom_solid_infill));

        bool have_default_acceleration = config->opt_float("default_acceleration") > 0;
        for (auto el : { "perimeter_acceleration", "infill_acceleration", "top_solid_infill_acceleration",
                        "solid_infill_acceleration", "external_perimeter_acceleration",
                        "bridge_acceleration", "first_layer_acceleration", "wipe_tower_acceleration"})
            toggle_field(el, have_default_acceleration);

        bool have_default_jerk = config->opt_int("default_jerk") > 0;
        for (auto el : { "perimeter_jerk", "infill_jerk", "top_solid_infill_jerk",
                        "solid_infill_jerk", "external_perimeter_jerk", "bridge_jerk",
                        "first_layer_jerk", "first_layer_jerk_over_raft", "wipe_tower_jerk", "travel_jerk" })
            toggle_field(el, have_default_jerk);

        bool have_minimum_cruise_ratio = config->opt_float("default_minimum_cruise_ratio") > 0 && gcflavor == gcfKlipper;
        for (auto el : { "perimeter_minimum_cruise_ratio", "infill_minimum_cruise_ratio", "top_solid_infill_minimum_cruise_ratio",
                        "solid_infill_minimum_cruise_ratio", "external_perimeter_minimum_cruise_ratio", "bridge_minimum_cruise_ratio",
                        "first_layer_minimum_cruise_ratio", "first_layer_minimum_cruise_ratio_over_raft", "travel_minimum_cruise_ratio",
                        "wipe_tower_minimum_cruise_ratio" })
            toggle_field(el, have_minimum_cruise_ratio);

        bool have_skirt = config->opt_int("skirts") > 0;
        toggle_field("skirt_height", have_skirt && config->opt_enum<DraftShield>("draft_shield") != dsEnabled);
        for (auto el : { "skirt_distance", "draft_shield", "min_skirt_length" })
            toggle_field(el, have_skirt);

        bool have_brim = config->opt_enum<BrimType>("brim_type") != btNoBrim;
        for (auto el : { "brim_width", "brim_separation" })
            toggle_field(el, have_brim);
        toggle_field("perimeter_extruder", have_perimeters || have_brim);

        bool have_raft = config->opt_int("raft_layers") > 0;
        bool have_support_material = config->opt_bool("support_material") || have_raft;
        bool have_support_material_auto = have_support_material && config->opt_bool("support_material_auto");
        bool have_support_interface = config->opt_int("support_material_interface_layers") > 0;
        bool have_support_soluble = have_support_material && config->opt_float("support_material_contact_distance") == 0;
        auto support_material_style = config->opt_enum<SupportMaterialStyle>("support_material_style");
        for (auto el : { "support_material_style", "support_material_pattern", "support_material_with_sheath",
                        "support_material_spacing", "support_material_angle",
                        "support_material_interface_pattern", "support_material_interface_layers",
                        "dont_support_bridges", "support_material_extrusion_width", "support_material_contact_distance",
                        "support_material_xy_spacing" })
            toggle_field(el, have_support_material);
        toggle_field("support_material_threshold", have_support_material_auto);
        toggle_field("support_material_bottom_contact_distance", have_support_material && ! have_support_soluble);
        toggle_field("support_material_closing_radius", have_support_material && support_material_style == smsSnug);

        const bool has_organic_supports = support_material_style == smsOrganic &&
                                         (config->opt_bool("support_material") ||
                                          config->opt_int("support_material_enforce_layers") > 0);
        for (const std::string& key : { "support_tree_angle", "support_tree_angle_slow", "support_tree_branch_diameter",
                                        "support_tree_branch_diameter_angle", "support_tree_branch_diameter_double_wall",
                                        "support_tree_tip_diameter", "support_tree_branch_distance", "support_tree_top_rate" })
            toggle_field(key, has_organic_supports);

        for (auto el : { "support_material_bottom_interface_layers", "support_material_interface_spacing", "support_material_interface_extruder",
                        "support_material_interface_speed", "support_material_interface_contact_loops" })
            toggle_field(el, have_support_material && have_support_interface);
        toggle_field("support_material_synchronize_layers", have_support_soluble);

        toggle_field("perimeter_extrusion_width", have_perimeters || have_skirt || have_brim);
        toggle_field("support_material_extruder", have_support_material || have_skirt);
        toggle_field("support_material_speed", have_support_material || have_brim || have_skirt);

        toggle_field("raft_contact_distance", have_raft && !have_support_soluble);
        for (auto el : { "raft_expansion", "first_layer_speed_over_raft" })
            toggle_field(el, have_raft);

        bool have_default_width = config->option("first_layer_extrusion_width")->getFloat() == 0 ||
            (config->option("perimeter_extrusion_width")->getFloat() == 0 && (have_perimeters || have_brim)) ||
            (config->option("external_perimeter_extrusion_width")->getFloat() == 0 && have_perimeters) ||
            (config->option("infill_extrusion_width")->getFloat() == 0 && (have_infill || has_solid_infill)) ||
            (config->option("solid_infill_extrusion_width")->getFloat() == 0 && has_solid_infill) ||
            (config->option("top_infill_extrusion_width")->getFloat() == 0 && has_top_solid_infill) ||
            (config->option("support_material_extrusion_width")->getFloat() == 0 && have_support_material);
        toggle_field("extrusion_width", have_default_width);

        bool has_ironing = config->opt_bool("ironing");
        for (auto el : { "ironing_type", "ironing_flowrate", "ironing_spacing", "ironing_speed" })
            toggle_field(el, has_ironing);

        bool has_fuzzy_skin = config->opt_enum<FuzzySkinType>("fuzzy_skin") != FuzzySkinType::None;
        for (auto el : { "fuzzy_skin_thickness", "fuzzy_skin_point_dist", "fuzzy_skin_noise_type" })
            toggle_field(el, has_fuzzy_skin);
        auto fuzzy_noise_type = config->opt_enum<FuzzySkinNoiseType>("fuzzy_skin_noise_type");
        bool has_structured_noise = has_fuzzy_skin && fuzzy_noise_type != FuzzySkinNoiseType::Classic;
        toggle_field("fuzzy_skin_feature_size", has_structured_noise);
        toggle_field("fuzzy_skin_octaves", has_structured_noise && fuzzy_noise_type != FuzzySkinNoiseType::Voronoi);
        toggle_field("fuzzy_skin_persistence", has_structured_noise &&
            (fuzzy_noise_type == FuzzySkinNoiseType::Perlin || fuzzy_noise_type == FuzzySkinNoiseType::Billow));

        bool have_ooze_prevention = config->opt_bool("ooze_prevention");
        toggle_field("standby_temperature_delta", have_ooze_prevention);
        toggle_field("preheat_time", have_ooze_prevention);
        toggle_field("preheat_steps", have_ooze_prevention);

        bool have_wipe_tower = config->opt_bool("wipe_tower");
        for (auto el : { "wipe_tower_width",  "wipe_tower_brim_width", "wipe_tower_cone_angle",
                         "wipe_tower_extra_spacing", "wipe_tower_extra_flow", "wipe_tower_bridging", "wipe_tower_no_sparse_layers", "single_extruder_multi_material_priming",
                         "wipe_tower_disable_filament_ramming", "wipe_tower_disable_cooling_moves", "wipe_tower_max_purge_speed", "wipe_tower_disable_linear_advance" })
            toggle_field(el, have_wipe_tower);

        toggle_field("avoid_crossing_curled_overhangs", !config->opt_bool("avoid_crossing_perimeters"));
        toggle_field("avoid_crossing_perimeters", !config->opt_bool("avoid_crossing_curled_overhangs"));

        bool have_avoid_crossing_perimeters = config->opt_bool("avoid_crossing_perimeters");
        toggle_field("avoid_crossing_perimeters_max_detour", have_avoid_crossing_perimeters);

        bool have_arachne = config->opt_enum<PerimeterGeneratorType>("perimeter_generator") == PerimeterGeneratorType::Arachne;
        for (auto el : { "wall_transition_length", "wall_transition_filter_deviation", "wall_transition_angle",
                         "wall_distribution_count", "min_feature_size", "min_bead_width" })
            toggle_field(el, have_arachne);
        toggle_field("thin_walls", !have_arachne);

        toggle_field("scarf_seam_placement", !has_spiral_vase);
        const auto scarf_seam_placement{config->opt_enum<ScarfSeamPlacement>("scarf_seam_placement")};
        const bool uses_scarf_seam{!has_spiral_vase && scarf_seam_placement != ScarfSeamPlacement::nowhere};
        for (auto el : { "scarf_seam_only_on_smooth", "scarf_seam_start_height", "scarf_seam_entire_loop",
                         "scarf_seam_length", "scarf_seam_max_segment_length", "scarf_seam_on_inner_perimeters" })
            toggle_field(el, uses_scarf_seam);

        bool use_beam_interlocking = config->opt_bool("interlocking_beam");
        for (auto el : { "interlocking_beam_width", "interlocking_orientation", "interlocking_beam_layer_count",
                         "interlocking_depth", "interlocking_boundary_avoidance" })
            toggle_field(el, use_beam_interlocking);
        toggle_field("mmu_segmented_region_max_width", !use_beam_interlocking);

        bool have_non_zero_mmu_segmented_region_max_width = !use_beam_interlocking && config->opt_float("mmu_segmented_region_max_width") > 0.;
        toggle_field("mmu_segmented_region_interlocking_depth", have_non_zero_mmu_segmented_region_max_width);

        bool have_small_area_infill_flow_compensation = config->opt_bool("small_area_infill_flow_compensation");
        for (int i = 0; i < 10; ++i) {
            toggle_field("small_area_infill_flow_compensation_extrusion_length_" + std::to_string(i), have_small_area_infill_flow_compensation);
            toggle_field("small_area_infill_flow_compensation_compensation_factor_" + std::to_string(i), have_small_area_infill_flow_compensation);
        }

        toggle_field("external_perimeters_first_holes", config->opt_bool("external_perimeters_first"));
        toggle_field("external_perimeters_first_disabled_first_layers", config->opt_bool("external_perimeters_first"));
        toggle_field("external_perimeters_first_holes_min_size", config->opt_bool("external_perimeters_first") && config->opt_bool("external_perimeters_first_holes"));
    }

    // ---- Filament Settings: TabFilament::toggle_options and update_filament_overrides_page ----
    {
        DynamicPrintConfig* m_config = fconfig;
        auto toggle_option = [&](const std::string& key, bool on) { toggle(key, on); };

        bool cooling = m_config->opt_bool("cooling", 0);
        bool fan_always_on = cooling || m_config->opt_bool("fan_always_on", 0);
        for (auto el : { "max_fan_speed", "fan_below_layer_time", "slowdown_below_layer_time", "min_print_speed", "cooling_slowdown_logic" })
            toggle_option(el, cooling);
        for (auto el : { "min_fan_speed", "disable_fan_first_layers", "full_fan_speed_layer" })
            toggle_option(el, fan_always_on);

        bool dynamic_fan_speeds = m_config->opt_bool("enable_dynamic_fan_speeds", 0);
        for (int i = 0; i < 4; i++)
            toggle_option("overhang_fan_speed_" + std::to_string(i), dynamic_fan_speeds);

        bool cooling_preserve_perimeters = cooling && static_cast<CoolingSlowdownLogicType>(m_config->option("cooling_slowdown_logic")->getInts().at(0)) == CoolingSlowdownLogicType::ConsistentSurface;
        toggle_option("cooling_perimeter_transition_distance", cooling_preserve_perimeters);

        bool multitool_ramming = m_config->opt_bool("filament_multitool_ramming", 0);
        toggle_option("filament_multitool_ramming_volume", multitool_ramming);
        toggle_option("filament_multitool_ramming_flow", multitool_ramming);

        // Overrides: an unchecked override is nil; these rules also lock the checkbox
        const int extruder_idx = 0;
        const bool have_retract_length = m_config->option("filament_retract_length")->is_nil()
            || m_config->opt_float("filament_retract_length", extruder_idx) > 0;
        const bool uses_ramping_lift = m_config->option("filament_travel_ramping_lift")->is_nil()
            || m_config->opt_bool("filament_travel_ramping_lift", extruder_idx);
        const bool is_lifting = m_config->option("filament_travel_max_lift")->is_nil()
            || m_config->opt_float("filament_travel_max_lift", extruder_idx) > 0
            || m_config->option("filament_retract_lift")->is_nil()
            || m_config->opt_float("filament_retract_lift", extruder_idx) > 0;
        for (auto el : { "filament_retract_speed", "filament_deretract_speed", "filament_retract_restart_extra",
                         "filament_retract_before_travel", "filament_retract_layer_change", "filament_wipe",
                         "filament_retract_before_wipe", "filament_prime_length_at_start" })
            toggle_option(el, have_retract_length);
        if (uses_ramping_lift && !m_config->option("filament_travel_ramping_lift")->is_nil()
            && m_config->opt_bool("filament_travel_ramping_lift", extruder_idx))
            toggle_option("filament_retract_lift", false);
        if (!is_lifting) {
            toggle_option("filament_retract_lift_above", false);
            toggle_option("filament_retract_lift_below", false);
        }
        if (!uses_ramping_lift)
            for (auto el : { "filament_travel_max_lift", "filament_travel_slope", "filament_travel_lift_before_obstacle" })
                toggle_option(el, false);
    }

    // ---- Printer Settings: TabPrinter::toggle_options ----
    {
        DynamicPrintConfig* m_config = pconfig;
        auto toggle_option = [&](const std::string& key, bool on, int i = -1) { toggle(key, on, i); };
        const size_t m_extruders_count = m_config->option<ConfigOptionFloats>("nozzle_diameter")->values.size();
        const GCodeFlavor flavor = gcflavor;
        bool have_multiple_extruders = m_extruders_count > 1;
        toggle_option("toolchange_gcode", have_multiple_extruders);
        toggle_option("single_extruder_multi_material", have_multiple_extruders);

        bool is_marlin_flavor = flavor == gcfMarlinLegacy || flavor == gcfMarlinFirmware;
        toggle_option("silent_mode", is_marlin_flavor);

        for (size_t i = 0; i < m_extruders_count; ++i) {
            bool have_retract_length = m_config->opt_float("retract_length", i) > 0;
            const bool ramping_lift = m_config->opt_bool("travel_ramping_lift", i);
            const bool lifts_z = (ramping_lift && m_config->opt_float("travel_max_lift", i) > 0)
                              || (! ramping_lift && m_config->opt_float("retract_lift", i) > 0);

            bool use_firmware_retraction = m_config->opt_bool("use_firmware_retraction");
            toggle_option("retract_length", !use_firmware_retraction, i);

            toggle_option("retract_lift", ! ramping_lift, i);
            toggle_option("travel_max_lift", ramping_lift, i);
            toggle_option("travel_slope", ramping_lift, i);

            toggle_option("retract_before_travel", have_retract_length || use_firmware_retraction, i);

            bool retraction = (have_retract_length || use_firmware_retraction);
            for (auto el : { "retract_lift_above", "retract_lift_below", "retract_lift_enforce" })
                toggle_option(el, lifts_z, i);

            for (auto el : { "retract_speed", "deretract_speed", "retract_before_wipe", "retract_restart_extra", "wipe" })
                toggle_option(el, retraction && !use_firmware_retraction, i);

            bool wipe = m_config->opt_bool("wipe", i);
            toggle_option("retract_before_wipe", wipe, i);

            toggle_option("travel_lift_before_obstacle", ramping_lift, i);

            toggle_option("retract_length_toolchange", have_multiple_extruders, i);

            bool toolchange_retraction = m_config->opt_float("retract_length_toolchange", i) > 0;
            toggle_option("retract_restart_extra_toolchange", have_multiple_extruders && toolchange_retraction, i);
        }

        const auto* machine_limits_usage = m_config->option<ConfigOptionEnum<MachineLimitsUsage>>("machine_limits_usage");
        bool enabled = machine_limits_usage->value != MachineLimitsUsage::Ignore;
        bool silent_mode = m_config->opt_bool("silent_mode");
        int  max_field = silent_mode ? 2 : 1;
        for (const std::string& opt : Preset::machine_limits_options()) {
            const bool option_enabled = (opt == "machine_max_junction_deviation") ? flavor == gcfMarlinFirmware && enabled : enabled;
            for (int i = 0; i < max_field; ++i)
                toggle_option(opt, option_enabled, i);
        }
    }

    json off = json::array();
    for (const auto& [key, on] : enabled_state)
        if (!on)
            off.push_back(key);
    return off;
}

} // namespace web
