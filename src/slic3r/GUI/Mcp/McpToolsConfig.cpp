// MCP tools: slicer settings (global and per object/part), presets, filaments.

#include "McpToolsCommon.hpp"

#include <algorithm>
#include <unordered_set>

#include <boost/algorithm/string.hpp>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Tab.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

enum class Scope { Process, Filament, Printer, Project, Unknown };

static const char *scope_name(Scope s)
{
    switch (s) {
    case Scope::Process: return "process";
    case Scope::Filament: return "filament";
    case Scope::Printer: return "printer";
    case Scope::Project: return "project";
    default: return "other";
    }
}

static Scope key_scope(const std::string &key)
{
    static const std::unordered_set<std::string> process(Preset::print_options().begin(), Preset::print_options().end());
    static const std::unordered_set<std::string> filament(Preset::filament_options().begin(), Preset::filament_options().end());
    static const std::unordered_set<std::string> printer(Preset::printer_options().begin(), Preset::printer_options().end());
    if (process.count(key)) return Scope::Process;
    if (filament.count(key)) return Scope::Filament;
    if (printer.count(key)) return Scope::Printer;
    if (wxGetApp().preset_bundle && wxGetApp().preset_bundle->project_config.has(key)) return Scope::Project;
    return Scope::Unknown;
}

static const char *type_name(ConfigOptionType t)
{
    switch (t) {
    case coFloat: return "float";
    case coFloats: return "float[] (one per filament/extruder, comma separated)";
    case coInt: return "int";
    case coInts: return "int[] (comma separated)";
    case coString: return "string";
    case coStrings: return "string[] (separated by ;)";
    case coPercent: return "percent (e.g. 15%)";
    case coPercents: return "percent[]";
    case coFloatOrPercent: return "float or percent (e.g. 0.4 or 80%)";
    case coFloatsOrPercents: return "float or percent[]";
    case coPoint: return "point (x,y)";
    case coPoints: return "points (x1xy1,x2xy2,...)";
    case coBool: return "bool";
    case coBools: return "bool[] (comma separated 0/1)";
    case coEnum: return "enum";
    case coEnums: return "enum[]";
    default: return "other";
    }
}

static const ConfigOptionDef &option_def(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr)
        throw ToolError("Unknown setting key '" + key + "'. Use search_settings to find the right key.");
    return *def;
}

static json option_def_json(const ConfigOptionDef &def)
{
    json o = {{"key", def.opt_key},
              {"label", def.full_label.empty() ? def.label : def.full_label},
              {"category", def.category},
              {"type", type_name(def.type)},
              {"scope", scope_name(key_scope(def.opt_key))}};
    if (!def.tooltip.empty()) o["description"] = def.tooltip;
    if (!def.sidetext.empty()) o["unit"] = def.sidetext;
    if (!def.enum_values.empty()) {
        o["allowed_values"] = def.enum_values;
        if (def.enum_labels.size() == def.enum_values.size()) o["value_labels"] = def.enum_labels;
    }
    if (def.min != ConfigOptionDef::min_default) o["min"] = def.min;
    if (def.max != ConfigOptionDef::max_default) o["max"] = def.max;
    return o;
}

// JSON value -> string accepted by ConfigOption::deserialize().
static std::string value_to_string(const json &v)
{
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_number()) return v.dump();
    if (v.is_array()) {
        std::vector<std::string> parts;
        for (const json &e : v) parts.push_back(value_to_string(e));
        return boost::join(parts, ",");
    }
    throw ToolError("Unsupported value " + v.dump());
}

static void set_value(DynamicPrintConfig &config, const std::string &key, const json &value)
{
    option_def(key);
    ConfigSubstitutionContext ctx{ForwardCompatibilitySubstitutionRule::Disable};
    try {
        config.set_deserialize(key, value_to_string(value), ctx);
    } catch (const std::exception &e) {
        throw ToolError("Invalid value " + value.dump() + " for '" + key + "': " + e.what());
    }
}

static Tab *tab(Preset::Type type)
{
    Tab *t = wxGetApp().get_tab(type);
    if (t == nullptr) throw ToolError("Settings tab not available.");
    return t;
}

// Applies new values to the edited preset of a collection through its Tab, so that
// the GUI, the "modified" markers and the background slicing are refreshed.
static void apply_to_preset(Preset::Type type, PresetCollection &collection, const json &values)
{
    DynamicPrintConfig config = collection.get_edited_preset().config;
    const DynamicPrintConfig &saved = collection.get_selected_preset().config;
    for (const auto &[key, value] : values.items()) {
        if (value.is_null()) {
            if (const ConfigOption *opt = saved.option(key)) config.set_key_value(key, opt->clone());
        } else
            set_value(config, key, value);
    }
    tab(type)->load_config(config);
}

static void apply_to_project(const json &values)
{
    PresetBundle      &pb     = preset_bundle();
    DynamicPrintConfig config = pb.project_config;
    for (const auto &[key, value] : values.items())
        if (!value.is_null()) set_value(config, key, value);
    pb.project_config.apply(config);
    plater().update_project_dirty_from_presets();
    plater().on_config_change(config);
}

static json filament_json(int slot)
{
    PresetBundle      &pb   = preset_bundle();
    const std::string &name = pb.filament_presets[slot - 1];
    json               f    = {{"slot", slot}, {"preset", name}};
    if (auto *colours = pb.project_config.option<ConfigOptionStrings>("filament_colour"); colours && size_t(slot - 1) < colours->values.size())
        f["color"] = colours->values[slot - 1];
    if (const Preset *preset = pb.filaments.find_preset(name)) {
        if (auto *type = preset->config.option<ConfigOptionStrings>("filament_type"); type && !type->values.empty()) f["type"] = type->values.front();
        if (auto *vendor = preset->config.option<ConfigOptionStrings>("filament_vendor"); vendor && !vendor->values.empty())
            f["vendor"] = vendor->values.front();
        f["system"] = preset->is_system;
    }
    json used = json::array();
    Model &m  = model();
    for (int i = 0; i < int(m.objects.size()); ++i) {
        const ModelObject *mo = m.objects[i];
        bool uses = mo->config.has("extruder") ? mo->config.opt_int("extruder") == slot : slot == 1;
        for (const ModelVolume *mv : mo->volumes) uses |= mv->config.has("extruder") && mv->config.opt_int("extruder") == slot;
        if (uses) used.push_back(i);
    }
    f["used_by_objects"] = used;
    return f;
}

static void refresh_filament_ui()
{
    PresetBundle &pb = preset_bundle();
    plater().sidebar().update_presets(Preset::TYPE_FILAMENT);
    if (ObjectList *list = wxGetApp().obj_list()) list->update_filament_colors();
    plater().update_project_dirty_from_presets();
    pb.export_selections(*wxGetApp().app_config);
    plater().on_config_change(pb.full_config());
}

static void set_filament_colour(int slot, const std::string &colour)
{
    std::string c = colour;
    if (!c.empty() && c[0] != '#') c = "#" + c;
    if (c.size() != 7 && c.size() != 9) throw ToolError("Colours must be #RRGGBB.");
    c = boost::to_upper_copy(c);

    PresetBundle       &pb  = preset_bundle();
    DynamicPrintConfig &cfg = pb.project_config;
    DynamicPrintConfig  cfg_new = cfg;
    const size_t        idx = size_t(slot - 1);
    for (const char *key : {"filament_colour", "filament_multi_colour", "filament_colour_type"}) {
        ConfigOptionStrings *opt = cfg_new.option<ConfigOptionStrings>(key, true);
        if (opt->values.size() <= idx) opt->values.resize(idx + 1);
        opt->values[idx] = std::string(key) == "filament_colour_type" ? "1" : c;
    }
    cfg.apply(cfg_new);
}

static Preset::Type preset_type(const std::string &name)
{
    if (name == "printer") return Preset::TYPE_PRINTER;
    if (name == "process") return Preset::TYPE_PRINT;
    if (name == "filament") return Preset::TYPE_FILAMENT;
    throw ToolError("`type` must be printer, process or filament.");
}

static PresetCollection &collection(Preset::Type type)
{
    PresetBundle &pb = preset_bundle();
    switch (type) {
    case Preset::TYPE_PRINTER: return pb.printers;
    case Preset::TYPE_PRINT: return pb.prints;
    default: return pb.filaments;
    }
}

// Switches a preset without the "unsaved changes" dialog of Tab::select_preset():
// pending modifications of the presets are explicitly kept (re-applied on top of the
// newly selected presets, like "Transfer") or discarded.
static void switch_preset(Preset::Type type, const std::string &name, bool keep_changes)
{
    PresetBundle &pb = preset_bundle();
    struct Pending
    {
        Preset::Type       type;
        PresetCollection  *collection;
        DynamicPrintConfig values;
    };
    std::vector<Pending> pending;
    const std::pair<Preset::Type, PresetCollection *> collections[] = {
        {Preset::TYPE_PRINTER, &pb.printers}, {Preset::TYPE_PRINT, &pb.prints}, {Preset::TYPE_FILAMENT, &pb.filaments}};
    for (const auto &[t, c] : collections) {
        if (!c->current_is_dirty()) continue;
        Pending p{t, c, {}};
        if (keep_changes) {
            const DynamicPrintConfig &edited = c->get_edited_preset().config;
            for (const std::string &key : c->current_dirty_options())
                if (const ConfigOption *opt = edited.option(key)) p.values.set_key_value(key, opt->clone());
        }
        c->discard_current_changes();
        pending.push_back(std::move(p));
    }
    if (!pending.empty()) wxGetApp().load_current_presets(false, false);

    // Nothing is dirty any more: select_preset() has nothing to ask.
    tab(type)->select_preset(name, false, "", false, true);

    if (keep_changes)
        for (Pending &p : pending) {
            if (p.values.empty()) continue;
            DynamicPrintConfig config = p.collection->get_edited_preset().config;
            config.apply_only(p.values, p.values.keys(), true);
            tab(p.type)->load_config(config);
        }
}

static bool keep_changes_arg(const json &args)
{
    const std::string policy = arg_str(args, "unsaved_changes", "keep");
    if (policy != "keep" && policy != "discard") throw ToolError("`unsaved_changes` must be keep or discard.");
    return policy == "keep";
}

// Same check as Plater::on_filament_change(), reported instead of asked.
static std::string filament_warning(const std::string &preset_name)
{
    const Preset *filament = preset_bundle().filaments.find_preset(preset_name);
    if (filament == nullptr) return {};
    const auto *type = filament->config.option<ConfigOptionStrings>("filament_type");
    if (type == nullptr || type->values.empty() || type->values.front() != "PVA") return {};
    const auto *nozzles = preset_bundle().printers.get_edited_preset().config.option<ConfigOptionFloatsNullable>("nozzle_diameter");
    if (nozzles && std::find(nozzles->values.begin(), nozzles->values.end(), 0.2) != nozzles->values.end())
        return "It is not recommended to use PVA filaments with 0.2mm nozzles.";
    return {};
}

static ToolResult with_warning(json data, const std::string &warning)
{
    if (!warning.empty()) data["warning"] = warning;
    return ToolResult::data(data);
}

// Assigns a filament preset to a slot without the GUI confirmations.
static std::string set_slot_preset(int slot, const std::string &name, bool keep_changes)
{
    PresetBundle &pb = preset_bundle();
    pb.set_filament_preset(size_t(slot - 1), name);
    if (pb.filament_presets.size() == 1 && pb.filaments.get_edited_preset().name != name)
        switch_preset(Preset::TYPE_FILAMENT, name, keep_changes);
    return filament_warning(name);
}

void register_config_tools(ToolRegistry &r)
{
    r.add({"search_settings",
           "Search the slicer settings by key, label, category or description (e.g. 'support', 'wall', 'infill density', 'seam'). "
           "Returns the keys with their type, unit, allowed values, scope (process/filament/printer) and current value.",
           schema({{"query", prop_str("Words to look for (all must match).")}, {"limit", prop_int("Max results (default 25).")}}, {"query"}), true, true,
           false, [](const json &args) {
               std::vector<std::string> words;
               const std::string        q = boost::to_lower_copy(arg_str(args, "query"));
               boost::split(words, q, boost::is_any_of(" _"), boost::token_compress_on);
               const int          limit = std::clamp(arg_int(args, "limit", 25), 1, 200);
               DynamicPrintConfig full  = preset_bundle().full_config();

               std::vector<std::pair<int, const ConfigOptionDef *>> hits;
               for (const auto &[key, def] : print_config_def.options) {
                   if (key_scope(key) == Scope::Unknown) continue;
                   const std::string head = boost::to_lower_copy(key + " " + def.label + " " + def.full_label + " " + def.category);
                   const std::string text = head + " " + boost::to_lower_copy(def.tooltip);
                   int               score = 0;
                   bool              all   = true;
                   for (const std::string &w : words) {
                       if (w.empty()) continue;
                       if (head.find(w) != std::string::npos) score += 3;
                       else if (text.find(w) != std::string::npos) score += 1;
                       else all = false;
                   }
                   if (all && score > 0) hits.emplace_back(score, &def);
               }
               std::stable_sort(hits.begin(), hits.end(), [](auto &a, auto &b) { return a.first > b.first; });
               json out = json::array();
               for (size_t i = 0; i < hits.size() && int(i) < limit; ++i) {
                   json o     = option_def_json(*hits[i].second);
                   o["value"] = config_value_json(full, hits[i].second->opt_key);
                   out.push_back(o);
               }
               return ToolResult::data({{"total_matches", hits.size()}, {"settings", out}});
           }});

    r.add({"get_settings",
           "Read setting values. Without `object` the effective project-wide values are returned; with `object` (and optionally "
           "`volume`) the per-object/part override (or null) and the effective value are returned. Without `keys`, returns the "
           "list of keys that differ from the saved presets (global) or the overrides (object/part).",
           schema({{"keys", prop_array(prop_str("Setting key."), "Keys to read (optional).")},
                   {"object", prop_int("Object index (optional).")},
                   {"volume", prop_int("Part/volume index inside the object (optional).")},
                   {"describe", prop_bool("Also return the definition (type, unit, allowed values) of each key (default false).")}}),
           true, true, false, [](const json &args) {
               PresetBundle      &pb       = preset_bundle();
               DynamicPrintConfig full     = pb.full_config();
               const bool         describe = arg_bool(args, "describe", false);
               json               out      = json::object();

               std::vector<std::string> keys;
               if (args.contains("keys") && args["keys"].is_array())
                   for (const json &k : args["keys"]) keys.push_back(k.get<std::string>());

               if (!has_arg(args, "object")) {
                   if (keys.empty()) {
                       json modified = json::object();
                       for (PresetCollection *c : std::vector<PresetCollection *>{&pb.prints, &pb.printers, &pb.filaments})
                           for (const std::string &k : c->current_dirty_options()) modified[k] = config_value_json(c->get_edited_preset().config, k);
                       return ToolResult::data({{"modified_from_saved_presets", modified}});
                   }
                   for (const std::string &k : keys) {
                       const ConfigOptionDef &def = option_def(k);
                       json v = {{"value", config_value_json(full, k)}, {"scope", scope_name(key_scope(k))}};
                       if (describe) v["definition"] = option_def_json(def);
                       out[k] = v;
                   }
                   return ToolResult::data(out);
               }

               const int    obj = arg_int(args, "object");
               ModelObject &mo  = object_at(obj);
               ModelVolume *mv  = has_arg(args, "volume") ? &volume_at(obj, arg_int(args, "volume")) : nullptr;
               const ModelConfig &cfg = mv ? static_cast<const ModelConfig &>(mv->config) : static_cast<const ModelConfig &>(mo.config);
               if (keys.empty()) keys = cfg.keys();
               for (const std::string &k : keys) {
                   const ConfigOptionDef &def = option_def(k);
                   json v = {{"override", config_value_json(cfg.get(), k)}};
                   if (cfg.has(k)) v["effective"] = v["override"];
                   else if (mv && mo.config.has(k)) v["effective"] = config_value_json(mo.config.get(), k);
                   else v["effective"] = config_value_json(full, k);
                   if (describe) v["definition"] = option_def_json(def);
                   out[k] = v;
               }
               return ToolResult::data(out);
           }});

    r.add({"set_settings",
           "Change settings. `values` maps setting keys to values (numbers, booleans, strings like '15%' or enum names; null resets the "
           "key: to the saved preset value globally, or removes the override for an object/part). Without `object` the change is "
           "global: process keys modify the current process preset, printer keys the printer preset, filament keys the preset of "
           "`filament_slot`. With `object` (and optionally `volume`) the process keys are set as per-object/part overrides, e.g. "
           "{\"enable_support\": true, \"support_type\": \"tree(auto)\"} on one object only.",
           schema({{"values", json{{"type", "object"}, {"description", "Map of setting key -> value."}}},
                   {"object", prop_int("Object index for a per-object override (optional).")},
                   {"volume", prop_int("Part/volume index for a per-part override (optional, requires object).")},
                   {"filament_slot", prop_int("1-based filament slot whose preset receives filament keys (default 1).")}},
                  {"values"}),
           true, false, false, [](const json &args) {
               if (!args.contains("values") || !args["values"].is_object() || args["values"].empty())
                   throw ToolError("`values` must be a non-empty object.");
               const json   &values = args["values"];
               PresetBundle &pb     = preset_bundle();

               if (has_arg(args, "object")) {
                   const int    obj = arg_int(args, "object");
                   ModelObject &mo  = object_at(obj);
                   ModelVolume *mv  = has_arg(args, "volume") ? &volume_at(obj, arg_int(args, "volume")) : nullptr;
                   ModelConfig &cfg = mv ? static_cast<ModelConfig &>(mv->config) : static_cast<ModelConfig &>(mo.config);
                   DynamicPrintConfig changes;
                   std::vector<std::string> erase;
                   for (const auto &[key, value] : values.items()) {
                       if (key_scope(key) != Scope::Process && key != "extruder")
                           throw ToolError("'" + key + "' is a " + scope_name(key_scope(key)) + " setting and cannot be overridden per object.");
                       if (value.is_null()) erase.push_back(key);
                       else set_value(changes, key, value);
                   }
                   Plater::TakeSnapshot snapshot(&plater(), "MCP: change object settings");
                   cfg.apply(changes);
                   for (const std::string &k : erase) cfg.erase(k);
                   after_object_config_change(obj, mv);
                   json result = json::object();
                   for (const std::string &k : cfg.keys()) result[k] = config_value_json(cfg.get(), k);
                   return ToolResult::data({{"object", obj}, {"volume", mv ? json(arg_int(args, "volume")) : json(nullptr)}, {"overrides", result}});
               }

               json process = json::object(), printer = json::object(), filament = json::object(), project = json::object();
               for (const auto &[key, value] : values.items()) {
                   option_def(key);
                   switch (key_scope(key)) {
                   case Scope::Process: process[key] = value; break;
                   case Scope::Printer: printer[key] = value; break;
                   case Scope::Filament: filament[key] = value; break;
                   case Scope::Project: project[key] = value; break;
                   default: throw ToolError("'" + key + "' cannot be set from here.");
                   }
               }
               if (!printer.empty()) apply_to_preset(Preset::TYPE_PRINTER, pb.printers, printer);
               if (!process.empty()) apply_to_preset(Preset::TYPE_PRINT, pb.prints, process);
               if (!filament.empty()) {
                   const int slot = arg_int(args, "filament_slot", 1);
                   check_filament_slot(slot);
                   const std::string &name = pb.filament_presets[slot - 1];
                   if (pb.filaments.get_edited_preset().name != name) switch_preset(Preset::TYPE_FILAMENT, name, true);
                   apply_to_preset(Preset::TYPE_FILAMENT, pb.filaments, filament);
               }
               if (!project.empty()) apply_to_project(project);

               DynamicPrintConfig full   = pb.full_config();
               json               result = json::object();
               for (const auto &[key, value] : values.items()) result[key] = config_value_json(full, key);
               return ToolResult::data({{"values", result}});
           }});

    // --- Presets --------------------------------------------------------------------------------------------------------

    r.add({"list_presets",
           "List the available presets of a type. By default only the visible presets compatible with the current printer are listed.",
           schema({{"type", prop_enum("Preset type.", {"printer", "process", "filament"})},
                   {"filter", prop_str("Case-insensitive substring filter on the name (optional).")},
                   {"include_incompatible", prop_bool("Also list incompatible presets (default false).")}},
                  {"type"}),
           true, true, false, [](const json &args) {
               const Preset::Type type   = preset_type(arg_str(args, "type"));
               PresetCollection  &c      = collection(type);
               const std::string  filter = boost::to_lower_copy(arg_str(args, "filter", ""));
               const bool         all    = arg_bool(args, "include_incompatible", false);
               json               out    = json::array();
               for (const Preset &p : c) {
                   if (!p.is_visible || (!all && !p.is_compatible)) continue;
                   if (!filter.empty() && boost::to_lower_copy(p.name).find(filter) == std::string::npos) continue;
                   out.push_back({{"name", p.name}, {"system", p.is_system}, {"user", p.is_user()}, {"compatible", p.is_compatible}});
               }
               return ToolResult::data({{"selected", c.get_selected_preset().name}, {"presets", out}});
           }});

    r.add({"select_preset",
           "Select the printer or process preset, or the filament preset of a filament slot. Pending (unsaved) modifications "
           "of the presets are kept and re-applied on the new preset by default (`unsaved_changes`='keep'), or dropped "
           "('discard').",
           schema({{"type", prop_enum("Preset type.", {"printer", "process", "filament"})},
                   {"name", prop_str("Exact preset name (see list_presets).")},
                   {"filament_slot", prop_int("1-based filament slot (filament only, default 1).")},
                   {"unsaved_changes", prop_enum("What to do with modified preset values (default keep).", {"keep", "discard"})}},
                  {"type", "name"}),
           true, false, false, [](const json &args) {
               const Preset::Type type = preset_type(arg_str(args, "type"));
               PresetBundle      &pb   = preset_bundle();
               const std::string  name = pb.get_preset_name_by_alias(type, arg_str(args, "name"));
               if (collection(type).find_preset(name) == nullptr) throw ToolError("No " + arg_str(args, "type") + " preset named '" + name + "'.");

               const bool keep = keep_changes_arg(args);
               if (type == Preset::TYPE_FILAMENT) {
                   const int slot = arg_int(args, "filament_slot", 1);
                   check_filament_slot(slot);
                   const std::string warning = set_slot_preset(slot, name, keep);
                   refresh_filament_ui();
                   return with_warning(filament_json(slot), warning);
               }

               if (type == Preset::TYPE_PRINTER) {
                   pb.physical_printers.unselect_printer();
                   plater().update_objects_position_when_select_preset([type, name, keep]() {
                       switch_preset(type, name, keep);
                       plater().on_config_change(preset_bundle().full_config());
                   });
               } else {
                   switch_preset(type, name, keep);
                   plater().on_config_change(pb.full_config());
               }
               return ToolResult::data({{"selected", collection(type).get_edited_preset().name}});
           }});

    // --- Filaments --------------------------------------------------------------------------------------------------------

    r.add({"list_filaments", "List the filament slots of the project (1-based) with their preset, colour, material type and the objects using them.",
           schema(), true, true, false, [](const json &) {
               json out = json::array();
               for (int i = 1; i <= int(preset_bundle().filament_presets.size()); ++i) out.push_back(filament_json(i));
               return ToolResult::data(out);
           }});

    r.add({"set_filament", "Change the preset and/or the colour of a filament slot.",
           schema({{"slot", prop_int("1-based filament slot.")},
                   {"preset", prop_str("Filament preset name (optional, see list_presets type=filament).")},
                   {"color", prop_str("Colour as #RRGGBB (optional).")}},
                  {"slot"}),
           true, false, false, [](const json &args) {
               const int slot = arg_int(args, "slot");
               check_filament_slot(slot);
               PresetBundle &pb = preset_bundle();
               std::string   warning;
               if (has_arg(args, "preset")) {
                   const std::string name = pb.get_preset_name_by_alias(Preset::TYPE_FILAMENT, arg_str(args, "preset"));
                   if (pb.filaments.find_preset(name) == nullptr) throw ToolError("No filament preset named '" + name + "'.");
                   warning = set_slot_preset(slot, name, true);
               }
               if (has_arg(args, "color")) set_filament_colour(slot, arg_str(args, "color"));
               refresh_filament_ui();
               return with_warning(filament_json(slot), warning);
           }});

    r.add({"add_filament", "Add a filament slot at the end of the list.",
           schema({{"preset", prop_str("Filament preset name (optional, defaults to the last one).")}, {"color", prop_str("Colour #RRGGBB (optional).")}}),
           true, false, false, [](const json &args) {
               std::string preset;
               if (has_arg(args, "preset")) {
                   preset = preset_bundle().get_preset_name_by_alias(Preset::TYPE_FILAMENT, arg_str(args, "preset"));
                   if (preset_bundle().filaments.find_preset(preset) == nullptr) throw ToolError("No filament preset named '" + preset + "'.");
               }
               wxColour colour = has_arg(args, "color") ? wxColour(wxString::FromUTF8(arg_str(args, "color"))) : Plater::get_next_color_for_filament();
               if (!colour.IsOk()) throw ToolError("Invalid colour.");
               plater().sidebar().add_custom_filament(colour, preset);
               return ToolResult::data(filament_json(int(preset_bundle().filament_presets.size())));
           }});

    r.add({"remove_filament", "Remove a filament slot. Objects using it are switched to `replace_with` (default: slot 1).",
           schema({{"slot", prop_int("1-based slot to remove.")}, {"replace_with", prop_int("1-based slot that takes over (optional).")}}, {"slot"}),
           true, false, true, [](const json &args) {
               const int slot = arg_int(args, "slot");
               check_filament_slot(slot);
               if (preset_bundle().filament_presets.size() <= 1) throw ToolError("The last filament cannot be removed.");
               int replace = -1;
               if (has_arg(args, "replace_with")) {
                   replace = arg_int(args, "replace_with");
                   check_filament_slot(replace);
                   if (replace == slot) throw ToolError("`replace_with` must differ from `slot`.");
                   replace -= 1;
               }
               plater().sidebar().delete_filament(size_t(slot - 1), replace);
               json out = json::array();
               for (int i = 1; i <= int(preset_bundle().filament_presets.size()); ++i) out.push_back(filament_json(i));
               return ToolResult::data(out);
           }});

    r.add({"assign_filament", "Print an object, or one of its parts, with a given filament slot.",
           schema({{"object", prop_int("Object index.")},
                   {"volume", prop_int("Part/volume index (optional: whole object when omitted).")},
                   {"filament", prop_int("1-based filament slot.")}},
                  {"object", "filament"}),
           true, false, false, [](const json &args) {
               const int obj  = arg_int(args, "object");
               const int slot = arg_int(args, "filament");
               check_filament_slot(slot);
               ModelObject &mo = object_at(obj);
               Plater::TakeSnapshot snapshot(&plater(), "MCP: change filament");
               if (has_arg(args, "volume")) {
                   ModelVolume &mv = volume_at(obj, arg_int(args, "volume"));
                   mv.config.set_key_value("extruder", new ConfigOptionInt(slot));
               } else {
                   mo.config.set_key_value("extruder", new ConfigOptionInt(slot));
                   for (ModelVolume *mv : mo.volumes)
                       if (mv->is_model_part()) mv->config.erase("extruder");
               }
               if (ObjectList *list = wxGetApp().obj_list()) list->update_filament_values_for_items(preset_bundle().filament_presets.size());
               plater().get_partplate_list().reset_thumbnail_assembly_view_data();
               plater().update();
               return ToolResult::data(object_summary_json(obj, true));
           }});
}

}}} // namespace Slic3r::GUI::Mcp
