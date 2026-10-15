#include "McpToolsCommon.hpp"

#include <cmath>

#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/ParamsPanel.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

// --- Arguments --------------------------------------------------------------------------------------------------------

bool has_arg(const json &args, const char *key) { return args.is_object() && args.contains(key) && !args[key].is_null(); }

int arg_int(const json &args, const char *key)
{
    if (!has_arg(args, key))
        throw ToolError(std::string("Missing required argument '") + key + "'.");
    const json &v = args[key];
    if (v.is_number()) return int(std::lround(v.get<double>()));
    if (v.is_string()) {
        try { return std::stoi(v.get<std::string>()); } catch (...) {}
    }
    throw ToolError(std::string("Argument '") + key + "' must be an integer.");
}

int arg_int(const json &args, const char *key, int def) { return has_arg(args, key) ? arg_int(args, key) : def; }

double arg_num(const json &args, const char *key, double def)
{
    if (!has_arg(args, key)) return def;
    const json &v = args[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try { return std::stod(v.get<std::string>()); } catch (...) {}
    }
    throw ToolError(std::string("Argument '") + key + "' must be a number.");
}

bool arg_bool(const json &args, const char *key, bool def)
{
    if (!has_arg(args, key)) return def;
    const json &v = args[key];
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) return v == "true" || v == "1";
    throw ToolError(std::string("Argument '") + key + "' must be a boolean.");
}

std::string arg_str(const json &args, const char *key)
{
    if (!has_arg(args, key))
        throw ToolError(std::string("Missing required argument '") + key + "'.");
    const json &v = args[key];
    return v.is_string() ? v.get<std::string>() : v.dump();
}

std::string arg_str(const json &args, const char *key, const std::string &def) { return has_arg(args, key) ? arg_str(args, key) : def; }

Vec3d arg_vec3(const json &args, const char *key, const Vec3d &current)
{
    Vec3d out = current;
    if (!has_arg(args, key)) return out;
    const json &v = args[key];
    if (v.is_array()) {
        for (size_t i = 0; i < 3 && i < v.size(); ++i)
            if (v[i].is_number()) out[i] = v[i].get<double>();
        return out;
    }
    if (!v.is_object())
        throw ToolError(std::string("Argument '") + key + "' must be an object {x, y, z}.");
    const char *names[] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i)
        out[i] = arg_num(v, names[i], current[i]);
    return out;
}

// --- Application accessors --------------------------------------------------------------------------------------------

Plater &plater()
{
    Plater *p = wxGetApp().plater();
    if (p == nullptr)
        throw ToolError("Bambu Studio is not ready yet (no plater).");
    return *p;
}

Model &model() { return plater().model(); }

PresetBundle &preset_bundle()
{
    if (wxGetApp().preset_bundle == nullptr)
        throw ToolError("Bambu Studio is not ready yet (no preset bundle).");
    return *wxGetApp().preset_bundle;
}

ModelObject &object_at(int obj_idx)
{
    Model &m = model();
    if (obj_idx < 0 || obj_idx >= int(m.objects.size()))
        throw ToolError("Invalid object index " + std::to_string(obj_idx) + " (the project has " + std::to_string(m.objects.size()) +
                        " objects, see list_objects).");
    return *m.objects[obj_idx];
}

ModelInstance &instance_at(int obj_idx, int inst_idx)
{
    ModelObject &mo = object_at(obj_idx);
    if (inst_idx < 0 || inst_idx >= int(mo.instances.size()))
        throw ToolError("Invalid instance index " + std::to_string(inst_idx) + " for object " + std::to_string(obj_idx) + " (" +
                        std::to_string(mo.instances.size()) + " instances).");
    return *mo.instances[inst_idx];
}

ModelVolume &volume_at(int obj_idx, int vol_idx)
{
    ModelObject &mo = object_at(obj_idx);
    if (vol_idx < 0 || vol_idx >= int(mo.volumes.size()))
        throw ToolError("Invalid volume (part) index " + std::to_string(vol_idx) + " for object " + std::to_string(obj_idx) + " (" +
                        std::to_string(mo.volumes.size()) + " volumes).");
    return *mo.volumes[vol_idx];
}

PartPlate &plate_at(int plate_idx)
{
    PartPlateList &plates = plater().get_partplate_list();
    if (plate_idx < 0 || plate_idx >= plates.get_plate_count())
        throw ToolError("Invalid plate index " + std::to_string(plate_idx) + " (" + std::to_string(plates.get_plate_count()) + " plates).");
    return *plates.get_plate(plate_idx);
}

void check_filament_slot(int slot)
{
    const int count = int(preset_bundle().filament_presets.size());
    if (slot < 1 || slot > count)
        throw ToolError("Invalid filament slot " + std::to_string(slot) + ": slots are 1-based and the project has " + std::to_string(count) +
                        " filaments (see list_filaments).");
}

// --- Conversions --------------------------------------------------------------------------------------------------------

static double round_to(double v, int decimals)
{
    const double f = std::pow(10.0, decimals);
    double       r = std::round(v * f) / f;
    return r == 0.0 ? 0.0 : r; // no "-0"
}

json vec3_json(const Vec3d &v, int decimals) { return json{{"x", round_to(v.x(), decimals)}, {"y", round_to(v.y(), decimals)}, {"z", round_to(v.z(), decimals)}}; }

json bbox_json(const BoundingBoxf3 &bb)
{
    if (!bb.defined) return nullptr;
    return json{{"min", vec3_json(bb.min)}, {"max", vec3_json(bb.max)}, {"size", vec3_json(bb.size())}};
}

static const char *volume_type_name(ModelVolumeType t)
{
    switch (t) {
    case ModelVolumeType::MODEL_PART: return "part";
    case ModelVolumeType::NEGATIVE_VOLUME: return "negative_volume";
    case ModelVolumeType::PARAMETER_MODIFIER: return "modifier";
    case ModelVolumeType::SUPPORT_BLOCKER: return "support_blocker";
    case ModelVolumeType::SUPPORT_ENFORCER: return "support_enforcer";
    default: return "invalid";
    }
}

json instance_json(int obj_idx, int inst_idx)
{
    ModelObject         &mo    = object_at(obj_idx);
    const ModelInstance &mi    = instance_at(obj_idx, inst_idx);
    const Vec3d          rot   = mi.get_rotation() * (180.0 / M_PI);
    const int            plate = plater().get_partplate_list().find_instance(obj_idx, inst_idx);
    return json{{"index", inst_idx},
                {"plate", plate},
                {"position", vec3_json(mi.get_offset())},
                {"rotation_deg", vec3_json(rot, 2)},
                {"scale", vec3_json(mi.get_scaling_factor(), 4)},
                {"mirrored", mi.is_left_handed()},
                {"printable", mi.printable},
                {"bounding_box", bbox_json(mo.instance_bounding_box(size_t(inst_idx)))}};
}

json config_value_json(const ConfigBase &config, const std::string &key)
{
    const ConfigOption *opt = config.option(key);
    if (opt == nullptr) return nullptr;
    return opt->serialize();
}

json object_summary_json(int obj_idx, bool with_volumes)
{
    ModelObject &mo = object_at(obj_idx);
    json         o  = {{"index", obj_idx}, {"name", mo.name}, {"instances", json::array()}};

    const ConfigOption *ext = mo.config.option("extruder");
    o["filament"]           = ext ? ext->getInt() : 1;
    for (int i = 0; i < int(mo.instances.size()); ++i) o["instances"].push_back(instance_json(obj_idx, i));

    json overrides = json::object();
    for (const std::string &k : mo.config.keys())
        if (k != "extruder") overrides[k] = config_value_json(mo.config.get(), k);
    o["setting_overrides"] = overrides;

    bool seam = false, support = false, color = false;
    json volumes = json::array();
    for (int v = 0; v < int(mo.volumes.size()); ++v) {
        const ModelVolume *mv = mo.volumes[v];
        seam |= mv->is_seam_painted();
        support |= mv->is_fdm_support_painted();
        color |= mv->is_mm_painted();
        if (!with_volumes) continue;
        json vo = {{"index", v},
                   {"name", mv->name},
                   {"type", volume_type_name(mv->type())},
                   {"filament", mv->extruder_id()},
                   {"triangles", mv->mesh().facets_count()},
                   {"seam_painted", mv->is_seam_painted()},
                   {"support_painted", mv->is_fdm_support_painted()},
                   {"color_painted", mv->is_mm_painted()}};
        json vo_overrides = json::object();
        for (const std::string &k : mv->config.keys())
            if (k != "extruder") vo_overrides[k] = config_value_json(mv->config.get(), k);
        if (!vo_overrides.empty()) vo["setting_overrides"] = vo_overrides;
        volumes.push_back(vo);
    }
    if (with_volumes) o["volumes"] = volumes;
    else o["volume_count"] = mo.volumes.size();
    o["painted"] = {{"seam", seam}, {"support", support}, {"color", color}};
    return o;
}

// --- GUI refresh --------------------------------------------------------------------------------------------------------

void after_instance_transform(int obj_idx, int inst_idx, bool drop_to_bed)
{
    Plater      &p  = plater();
    ModelObject &mo = object_at(obj_idx);
    mo.invalidate_bounding_box();
    if (drop_to_bed) {
        // ensure_on_bed() tolerates sunk objects (negative z), so put the lowest point
        // back on the bed explicitly, like the move/rotate/scale gizmos do.
        for (int i = 0; i < int(mo.instances.size()); ++i) {
            if (inst_idx >= 0 && i != inst_idx) continue;
            const double min_z = mo.get_instance_min_z(size_t(i));
            if (std::abs(min_z) > EPSILON) mo.translate_instance(size_t(i), Vec3d(0., 0., -min_z));
        }
        mo.invalidate_bounding_box();
    }
    if (drop_to_bed)
        p.changed_object(obj_idx); // ensure_on_bed + reload_scene + schedule background processing
    else {
        p.canvas3D()->reload_scene(true);
        p.schedule_background_process();
    }

    PartPlateList &plates = p.get_partplate_list();
    if (inst_idx >= 0)
        plates.notify_instance_update(obj_idx, inst_idx);
    else
        for (int i = 0; i < int(mo.instances.size()); ++i) plates.notify_instance_update(obj_idx, i);

    if (ObjectList *list = wxGetApp().obj_list()) {
        list->update_info_items(size_t(obj_idx));
        list->update_plate_values_for_items();
    }
    p.update();
}

void after_object_config_change(int obj_idx, ModelVolume *volume)
{
    ModelObject &mo = object_at(obj_idx);
    if (ObjectList *list = wxGetApp().obj_list()) {
        list->object_config_options_changed(ObjectVolumeID{&mo, volume});
        list->changed_object(obj_idx);
    }
    if (ParamsPanel *panel = wxGetApp().params_panel())
        panel->notify_object_config_changed();
}

void select_instances(const std::vector<std::pair<int, int>> &items)
{
    Plater &p = plater();
    // Selection-based commands (delete, clone...) only act on the 3D editor view.
    if (!p.is_view3D_shown()) p.select_view_3D("3D");
    Selection &sel = p.canvas3D()->get_selection();
    sel.clear();
    for (const auto &[obj, inst] : items) {
        object_at(obj);
        if (inst < 0)
            sel.add_object(unsigned(obj), false);
        else {
            instance_at(obj, inst);
            sel.add_instance(unsigned(obj), unsigned(inst), false);
        }
    }
    if (ObjectList *list = wxGetApp().obj_list())
        list->update_selections();
    p.canvas3D()->set_as_dirty();
}

}}} // namespace Slic3r::GUI::Mcp
