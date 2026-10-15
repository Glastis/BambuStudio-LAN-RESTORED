// MCP tools: objects listing, selection, transformations, duplication, arrangement.

#include "McpToolsCommon.hpp"

#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Jobs/Job.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

static const json OBJECT_ARG   = prop_int("Object index (see list_objects).");
static const json INSTANCE_ARG = prop_int("Instance index (default 0).");

// Parses [{object, instance?}] or [object, ...] into (object, instance) pairs; instance -1 = whole object.
static std::vector<std::pair<int, int>> parse_targets(const json &args)
{
    if (!args.contains("objects") || !args["objects"].is_array())
        throw ToolError("`objects` must be an array of object indices or {object, instance} items.");
    std::vector<std::pair<int, int>> out;
    for (const json &item : args["objects"]) {
        if (item.is_number())
            out.emplace_back(item.get<int>(), -1);
        else if (item.is_object())
            out.emplace_back(arg_int(item, "object"), arg_int(item, "instance", -1));
        else
            throw ToolError("Invalid item in `objects`: " + item.dump());
    }
    return out;
}

static Vec3d deg2rad(const Vec3d &v) { return v * (M_PI / 180.0); }

// Waits (off the GUI thread) for arrange/orient jobs to finish.
static void wait_for_jobs(int timeout_s)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    if (!wait_on_gui([] { return !plater().is_any_job_running() && plater().get_ui_job_worker().is_idle(); }, std::chrono::seconds(timeout_s)))
        throw ToolError("The operation is still running after " + std::to_string(timeout_s) + " s.");
}

void register_object_tools(ToolRegistry &r)
{
    r.add({"list_objects",
           "List the objects of the project: index, name, filament, per-object setting overrides, painting flags and every instance "
           "(plate, position, rotation, scale, world bounding box).",
           schema({{"plate", prop_int("Only objects having an instance on this plate (optional).")},
                   {"with_parts", prop_bool("Also list the parts/modifiers (volumes) of each object (default false).")}}),
           true, true, false, [](const json &args) {
               Model     &m          = model();
               const int  plate      = arg_int(args, "plate", -1);
               const bool with_parts = arg_bool(args, "with_parts", false);
               if (plate >= 0) plate_at(plate);
               json out = json::array();
               for (int i = 0; i < int(m.objects.size()); ++i) {
                   json o = object_summary_json(i, with_parts);
                   if (plate >= 0) {
                       bool on_plate = false;
                       for (const json &inst : o["instances"]) on_plate |= inst["plate"].get<int>() == plate;
                       if (!on_plate) continue;
                   }
                   out.push_back(std::move(o));
               }
               return ToolResult::data(out);
           }});

    r.add({"get_object", "Full description of one object, including its parts/modifiers (volumes), their filament and setting overrides.",
           schema({{"object", OBJECT_ARG}}, {"object"}), true, true, false,
           [](const json &args) { return ToolResult::data(object_summary_json(arg_int(args, "object"), true)); }});

    r.add({"select_objects", "Select objects (or specific instances) in the 3D view, replacing the current selection. An empty list clears it.",
           schema({{"objects", prop_array(json{{"type", json::array({"integer", "object"})}}, "Object indices, or {object, instance} items.")}}, {"objects"}),
           true, false, false, [](const json &args) {
               select_instances(parse_targets(args));
               return ToolResult::data({{"selected", args["objects"]}});
           }});

    r.add({"transform_object",
           "Move, rotate and/or scale an object instance. The object is always dropped back onto the bed afterwards. "
           "`center` places the centre of the bounding box at the given XY (simplest way to position an object on a plate), "
           "`position` sets the instance origin, `translate` moves relatively. `rotation` sets absolute Euler angles (degrees), "
           "`rotate` rotates relatively around the world axes (degrees). `scale` is a uniform factor or {x,y,z} factors; "
           "`size` scales to an absolute bounding-box size in mm.",
           schema({{"object", OBJECT_ARG},
                   {"instance", INSTANCE_ARG},
                   {"center", prop_vec3("Target XY of the bounding-box centre (z ignored).")},
                   {"position", prop_vec3("Absolute instance origin, mm.")},
                   {"translate", prop_vec3("Relative move, mm.")},
                   {"rotation", prop_vec3("Absolute rotation, degrees.")},
                   {"rotate", prop_vec3("Relative rotation around world axes, degrees.")},
                   {"scale", json{{"description", "Uniform factor (number, 1 = 100%) or {x,y,z} factors."}}},
                   {"size", prop_vec3("Absolute bounding-box size in mm (missing axes keep the aspect ratio when only one is given).")},
                   {"mirror", prop_enum("Mirror along this axis.", {"x", "y", "z"})}},
                  {"object"}),
           true, false, false, [](const json &args) {
               const int      obj  = arg_int(args, "object");
               const int      inst = arg_int(args, "instance", 0);
               ModelObject   &mo   = object_at(obj);
               ModelInstance &mi   = instance_at(obj, inst);

               Plater::TakeSnapshot snapshot(&plater(), "MCP: transform object");
               Geometry::Transformation t = mi.get_transformation();

               if (has_arg(args, "rotation")) t.set_rotation(deg2rad(arg_vec3(args, "rotation", t.get_rotation() * (180.0 / M_PI))));
               if (has_arg(args, "rotate")) {
                   const Vec3d       d     = deg2rad(arg_vec3(args, "rotate", Vec3d::Zero()));
                   const Transform3d delta = Geometry::rotation_transform(d);
                   const Transform3d cur   = Geometry::rotation_transform(t.get_rotation());
                   t.set_rotation(Geometry::extract_euler_angles(Transform3d(delta * cur)));
               }
               if (has_arg(args, "scale")) {
                   const json &s = args["scale"];
                   if (s.is_number()) t.set_scaling_factor(Vec3d::Constant(s.get<double>()));
                   else t.set_scaling_factor(arg_vec3(args, "scale", t.get_scaling_factor()));
               }
               if (has_arg(args, "mirror")) {
                   const std::string axis = arg_str(args, "mirror");
                   Vec3d             m    = t.get_mirror();
                   const int         a    = axis == "x" ? 0 : axis == "y" ? 1 : axis == "z" ? 2 : -1;
                   if (a < 0) throw ToolError("`mirror` must be x, y or z.");
                   m[a] = -m[a];
                   t.set_mirror(m);
               }
               mi.set_transformation(t);
               mo.invalidate_bounding_box();

               if (has_arg(args, "size")) {
                   const BoundingBoxf3 bb   = mo.instance_bounding_box(size_t(inst));
                   const Vec3d         cur  = bb.size();
                   const json         &sz   = args["size"];
                   Vec3d               f    = Vec3d::Constant(std::nan(""));
                   const char         *n[3] = {"x", "y", "z"};
                   for (int i = 0; i < 3; ++i)
                       if (sz.is_object() && sz.contains(n[i]) && sz[n[i]].is_number() && cur[i] > EPSILON) f[i] = sz[n[i]].get<double>() / cur[i];
                   int given = 0;
                   double any = 1.0;
                   for (int i = 0; i < 3; ++i)
                       if (!std::isnan(f[i])) { ++given; any = f[i]; }
                   if (given == 0) throw ToolError("`size` needs at least one of x, y, z.");
                   for (int i = 0; i < 3; ++i)
                       if (std::isnan(f[i])) f[i] = given == 1 ? any : 1.0;
                   Geometry::Transformation t2 = mi.get_transformation();
                   t2.set_scaling_factor(t2.get_scaling_factor().cwiseProduct(f));
                   mi.set_transformation(t2);
                   mo.invalidate_bounding_box();
               }

               Vec3d offset = mi.get_offset();
               if (has_arg(args, "position")) offset = arg_vec3(args, "position", offset);
               if (has_arg(args, "translate")) offset += arg_vec3(args, "translate", Vec3d::Zero());
               if (has_arg(args, "center")) {
                   const Vec3d c      = mo.instance_bounding_box(size_t(inst)).center();
                   const Vec3d target = arg_vec3(args, "center", c);
                   offset += Vec3d(target.x() - c.x(), target.y() - c.y(), 0.0);
               }
               mi.set_offset(offset);

               after_instance_transform(obj, inst, true);
               return ToolResult::data(instance_json(obj, inst));
           }});

    r.add({"move_to_plate",
           "Move an object instance to the centre of another plate. Use `arrange` afterwards if several objects overlap.",
           schema({{"object", OBJECT_ARG}, {"instance", INSTANCE_ARG}, {"plate", prop_int("Destination plate index.")}}, {"object", "plate"}), true,
           false, false, [](const json &args) {
               const int      obj   = arg_int(args, "object");
               const int      inst  = arg_int(args, "instance", 0);
               ModelObject   &mo    = object_at(obj);
               ModelInstance &mi    = instance_at(obj, inst);
               PartPlate     &plate = plate_at(arg_int(args, "plate"));

               Plater::TakeSnapshot snapshot(&plater(), "MCP: move to plate");
               const Vec3d target = plate.get_build_volume().center();
               const Vec3d c      = mo.instance_bounding_box(size_t(inst)).center();
               mi.set_offset(mi.get_offset() + Vec3d(target.x() - c.x(), target.y() - c.y(), 0.0));
               after_instance_transform(obj, inst, true);
               return ToolResult::data(instance_json(obj, inst));
           }});

    r.add({"rename_object", "Rename an object.", schema({{"object", OBJECT_ARG}, {"name", prop_str("New name.")}}, {"object", "name"}), true, false,
           false, [](const json &args) {
               const int    obj = arg_int(args, "object");
               ModelObject &mo  = object_at(obj);
               Plater::TakeSnapshot snapshot(&plater(), "MCP: rename object");
               mo.name = arg_str(args, "name");
               if (ObjectList *list = wxGetApp().obj_list()) list->update_name_in_list(obj, -1);
               return ToolResult::data({{"object", obj}, {"name", mo.name}});
           }});

    r.add({"delete_objects", "Delete objects (or single instances). Indices of the following objects shift down afterwards.",
           schema({{"objects", prop_array(json{{"type", json::array({"integer", "object"})}}, "Object indices, or {object, instance} items.")}}, {"objects"}),
           true, false, true, [](const json &args) {
               auto targets = parse_targets(args);
               if (targets.empty()) throw ToolError("`objects` is empty.");
               select_instances(targets);
               if (!plater().can_delete()) throw ToolError("These objects cannot be deleted right now.");
               plater().remove_selected();
               return ToolResult::data({{"object_count", model().objects.size()}});
           }});

    r.add({"duplicate_object", "Create copies of an object (as new objects, like the Clone command). Returns the new object indices.",
           schema({{"object", OBJECT_ARG}, {"count", prop_int("Number of copies (1..100, default 1).")}}, {"object"}), true, false, false,
           [](const json &args) {
               const int obj   = arg_int(args, "object");
               const int count = std::clamp(arg_int(args, "count", 1), 1, 100);
               object_at(obj);
               const size_t before = model().objects.size();
               select_instances({{obj, -1}});
               plater().canvas3D()->get_selection().clone(count);
               json created = json::array();
               for (size_t i = before; i < model().objects.size(); ++i) created.push_back(int(i));
               return ToolResult::data({{"created_objects", created}});
           }});

    // Arrange / orient run as background jobs: not executed on the GUI thread.
    Tool arrange{"arrange",
                 "Automatically arrange objects on the plates (like the Arrange button). With `plate` only that plate is arranged, "
                 "otherwise all objects of the project are arranged across plates.",
                 schema({{"plate", prop_int("Arrange only this plate (optional).")}})};
    arrange.on_gui_thread = false;
    arrange.handler       = [](const json &args) {
        JobDialogScope dialogs; // also covers the end of the background job
        run_on_gui([args]() {
            Plater &p = plater();
            if (p.is_any_job_running()) throw ToolError("Another arrange/orient job is running, retry later.");
            p.canvas3D()->get_selection().clear();
            if (has_arg(args, "plate")) {
                plate_at(arg_int(args, "plate"));
                p.select_plate(arg_int(args, "plate"));
                p.set_prepare_state(Job::PREPARE_STATE_MENU);
            } else
                p.set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            p.arrange();
            return json();
        });
        wait_for_jobs(180);
        return ToolResult::data(run_on_gui([]() {
            json out = json::array();
            for (int i = 0; i < int(model().objects.size()); ++i) out.push_back(object_summary_json(i, false));
            return out;
        }));
    };
    r.add(arrange);

    Tool orient{"auto_orient",
                "Automatically rotate objects to the orientation that is best for printing (least supports/overhangs). "
                "Targets the given objects, or the given plate, or everything.",
                schema({{"objects", prop_array(json{{"type", "integer"}}, "Object indices (optional).")},
                        {"plate", prop_int("Orient only the objects of this plate (optional).")}})};
    orient.on_gui_thread = false;
    orient.handler       = [](const json &args) {
        JobDialogScope dialogs; // also covers the end of the background job
        run_on_gui([args]() {
            Plater &p = plater();
            if (p.is_any_job_running()) throw ToolError("Another arrange/orient job is running, retry later.");
            if (args.contains("objects") && args["objects"].is_array() && !args["objects"].empty()) {
                select_instances(parse_targets(args));
                p.set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            } else if (has_arg(args, "plate")) {
                p.canvas3D()->get_selection().clear();
                plate_at(arg_int(args, "plate"));
                p.select_plate(arg_int(args, "plate"));
                p.set_prepare_state(Job::PREPARE_STATE_MENU);
            } else {
                p.canvas3D()->get_selection().clear();
                p.set_prepare_state(Job::PREPARE_STATE_DEFAULT);
            }
            p.orient();
            return json();
        });
        wait_for_jobs(180);
        return ToolResult::data(run_on_gui([]() {
            json out = json::array();
            for (int i = 0; i < int(model().objects.size()); ++i) out.push_back(object_summary_json(i, false));
            return out;
        }));
    };
    r.add(orient);
}

}}} // namespace Slic3r::GUI::Mcp
