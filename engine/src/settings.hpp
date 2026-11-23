// The Print / Filament / Printer Settings tabs: what the web page needs to draw them
// (layout and option definitions) and which options BOSS greys out for the current values.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
class PresetBundle;
}

namespace web {

// The tabs' layout, read from BOSS's own GUI source at build time (engine/tools/tab_layout.py),
// with the definition (label, tooltip, type, mode, limits, choices) of every option on it
nlohmann::json settings_schema();

// Settings an object (or, with part, one of its parts or modifiers) can override, as BOSS's
// object list offers them (SettingsFactory::get_options)
std::vector<std::string> object_options(bool part);

// One preset's values: scalars as their .ini text, vectors as a list of their elements' texts
nlohmann::json preset_values(const Slic3r::DynamicPrintConfig& config);

// Options BOSS disables in its tabs for these values ("key", or "key#index" for one
// extruder's value), following ConfigManipulation::toggle_print_fff_options and
// TabFilament/TabPrinter::toggle_options
nlohmann::json disabled_options(const Slic3r::PresetBundle& bundle);

// A vector option's elements as texts, in the same notation as a .ini
std::vector<std::string> vector_elements(const Slic3r::ConfigOption& opt);

// Turns a list of element texts into the .ini text of a vector option
std::string serialize_vector(const Slic3r::ConfigOptionDef& def, const std::vector<std::string>& values);

} // namespace web
