#ifndef slic3r_GUI_McpToolsCommon_hpp_
#define slic3r_GUI_McpToolsCommon_hpp_

// Helpers shared by the MCP tool implementations (McpTools*.cpp).
// Everything here must be used on the wx main thread unless stated otherwise.

#include "McpServer.hpp"

#include <initializer_list>
#include <string>
#include <utility>

#include "libslic3r/Point.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r {
class PresetBundle;
namespace GUI {
class Plater;
class PartPlate;
namespace Mcp {

// --- JSON schema builders -------------------------------------------------------------------------------------------

inline json prop(const char *type, const std::string &description) { return json{{"type", type}, {"description", description}}; }
inline json prop_int(const std::string &d) { return prop("integer", d); }
inline json prop_num(const std::string &d) { return prop("number", d); }
inline json prop_str(const std::string &d) { return prop("string", d); }
inline json prop_bool(const std::string &d) { return prop("boolean", d); }
inline json prop_enum(const std::string &d, std::initializer_list<const char *> values)
{
    json p = prop_str(d);
    p["enum"] = json::array();
    for (const char *v : values) p["enum"].push_back(v);
    return p;
}
inline json prop_array(const json &items, const std::string &d) { return json{{"type", "array"}, {"items", items}, {"description", d}}; }
inline json prop_vec3(const std::string &d)
{
    return json{{"type", "object"},
                {"description", d + " Any of x, y, z may be omitted to keep the current value."},
                {"properties", {{"x", {{"type", "number"}}}, {"y", {{"type", "number"}}}, {"z", {{"type", "number"}}}}}};
}

inline json schema(json properties = json::object(), std::initializer_list<const char *> required = {})
{
    json s = {{"type", "object"}, {"properties", std::move(properties)}};
    if (required.size() > 0) {
        s["required"] = json::array();
        for (const char *r : required) s["required"].push_back(r);
    }
    return s;
}

// --- Argument accessors (throw ToolError with a readable message) ---------------------------------------------------

int         arg_int(const json &args, const char *key);
int         arg_int(const json &args, const char *key, int def);
double      arg_num(const json &args, const char *key, double def);
bool        arg_bool(const json &args, const char *key, bool def);
std::string arg_str(const json &args, const char *key);
std::string arg_str(const json &args, const char *key, const std::string &def);
// Partial vector {x?, y?, z?}: missing components are taken from `current`.
Vec3d       arg_vec3(const json &args, const char *key, const Vec3d &current);
bool        has_arg(const json &args, const char *key);

// --- Application accessors (throw ToolError when not available) -----------------------------------------------------

Plater       &plater();
Model        &model();
PresetBundle &preset_bundle();

ModelObject   &object_at(int obj_idx);
ModelInstance &instance_at(int obj_idx, int inst_idx);
ModelVolume   &volume_at(int obj_idx, int vol_idx);
PartPlate     &plate_at(int plate_idx);
// Validates a 1-based filament slot index.
void           check_filament_slot(int slot);

// --- Conversions -----------------------------------------------------------------------------------------------------

json vec3_json(const Vec3d &v, int decimals = 3);
json bbox_json(const BoundingBoxf3 &bb);
json instance_json(int obj_idx, int inst_idx);
json object_summary_json(int obj_idx, bool with_volumes);
// Serialized value of a config option, or null if the key is absent.
json config_value_json(const ConfigBase &config, const std::string &key);

// --- GUI refresh after a direct model edit --------------------------------------------------------------------------

// Re-sync canvas, object list, plates and background processing after the
// transformation of `obj_idx` changed (all instances when inst_idx < 0).
void after_instance_transform(int obj_idx, int inst_idx = -1, bool drop_to_bed = true);
// After a change of per-object / per-volume configuration.
void after_object_config_change(int obj_idx, ModelVolume *volume = nullptr);

// Selects exactly the given (object, instance) pairs; instance -1 = whole object.
void select_instances(const std::vector<std::pair<int, int>> &items);

}}} // namespace Slic3r::GUI::Mcp

#endif
