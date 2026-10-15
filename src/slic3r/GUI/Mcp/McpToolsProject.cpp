// MCP tools: application state, project lifecycle, undo/redo, plates, screenshots.

#include "McpToolsCommon.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/miniz_extension.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Selection.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;

static fs::path absolute_existing_path(const std::string &path)
{
    fs::path p(path);
    if (!p.is_absolute())
        throw ToolError("Path '" + path + "' must be absolute.");
    if (!fs::exists(p))
        throw ToolError("File '" + path + "' does not exist.");
    return p;
}

static void require_clean_or_discard(const json &args)
{
    if (plater().is_project_dirty() && !arg_bool(args, "discard_changes", false))
        throw ToolError("The current project has unsaved changes. Save it with save_project first, or pass discard_changes=true to drop them.");
}

// Opening a project replaces the presets: modified preset values must be dropped
// explicitly instead of letting Bambu Studio ask what to do with them.
static void drop_preset_changes_or_fail(const json &args)
{
    if (!wxGetApp().has_current_preset_changes()) return;
    if (!arg_bool(args, "discard_changes", false))
        throw ToolError("Some printer/process/filament presets have unsaved modifications (see get_settings). Pass discard_changes=true "
                        "to drop them, or save the project first.");
    PresetBundle &pb = preset_bundle();
    for (PresetCollection *c : std::vector<PresetCollection *>{&pb.printers, &pb.prints, &pb.filaments})
        if (c->current_is_dirty()) c->discard_current_changes();
    wxGetApp().load_current_presets(false, false);
}

json app_state_json()
{
    Plater       &p     = plater();
    PresetBundle &pb    = preset_bundle();
    Model        &m     = p.model();
    auto         &plates = p.get_partplate_list();

    json selection = json::array();
    for (const auto &[obj, insts] : p.get_selection().get_content())
        for (int inst : insts) selection.push_back({{"object", obj}, {"instance", inst}});

    json filaments = json::array();
    for (const std::string &name : pb.filament_presets) filaments.push_back(name);

    return json{
        {"project_name", into_u8(p.get_project_name())},
        {"project_file", into_u8(p.get_project_filename(".3mf"))},
        {"unsaved_changes", p.is_project_dirty()},
        {"object_count", m.objects.size()},
        {"plate_count", plates.get_plate_count()},
        {"current_plate", plates.get_curr_plate_index()},
        {"view", p.is_preview_shown() ? "preview" : "3d"},
        {"busy", p.is_any_job_running() || !p.get_ui_job_worker().is_idle()},
        {"slicing", p.is_background_process_slicing()},
        {"selection", selection},
        {"printer_preset", {{"name", pb.printers.get_edited_preset().name}, {"modified", pb.printers.get_edited_preset().is_dirty}}},
        {"process_preset", {{"name", pb.prints.get_edited_preset().name}, {"modified", pb.prints.get_edited_preset().is_dirty}}},
        {"filament_presets", filaments},
    };
}

static json plate_json(int idx)
{
    PartPlate &plate = plate_at(idx);
    json       objs  = json::array();
    for (const auto &[obj, inst] : plate.get_obj_and_inst_set()) objs.push_back({{"object", obj}, {"instance", inst}});
    const BoundingBoxf3 bed = plate.get_build_volume();
    return json{{"index", idx},
                {"name", plate.get_plate_name()},
                {"origin", vec3_json(plate.get_origin())},
                {"printable_area", bbox_json(bed)},
                {"current", plater().get_partplate_list().get_curr_plate_index() == idx},
                {"locked", plate.is_locked()},
                {"sliced", plate.is_slice_result_valid()},
                {"instances", objs}};
}

static std::string encode_png_base64(const ThumbnailData &data)
{
    size_t png_size = 0;
    void  *png      = tdefl_write_image_to_png_file_in_memory_ex((const void *) data.pixels.data(), data.width, data.height, 4, &png_size,
                                                                 MZ_DEFAULT_COMPRESSION, 1);
    if (png == nullptr)
        throw ToolError("PNG encoding failed.");
    std::string out(boost::beast::detail::base64::encoded_size(png_size), '\0');
    out.resize(boost::beast::detail::base64::encode(out.data(), png, png_size));
    mz_free(png);
    return out;
}

static Camera::ViewAngleType view_angle(const std::string &name)
{
    if (name == "iso") return Camera::ViewAngleType::Iso;
    if (name == "top") return Camera::ViewAngleType::Top;
    if (name == "bottom") return Camera::ViewAngleType::Bottom;
    if (name == "front") return Camera::ViewAngleType::Front;
    if (name == "rear") return Camera::ViewAngleType::Rear;
    if (name == "left") return Camera::ViewAngleType::Left;
    if (name == "right") return Camera::ViewAngleType::Right;
    if (name == "top_front") return Camera::ViewAngleType::Top_Front;
    throw ToolError("Unknown view '" + name + "'.");
}

void register_project_tools(ToolRegistry &r)
{
    r.add({"get_app_state",
           "Overview of the running Bambu Studio: project name/file, unsaved changes, number of objects and plates, current plate, "
           "active view, selection, and the selected printer/process/filament presets. Call this first.",
           schema(), true, true, false, [](const json &) { return ToolResult::data(app_state_json()); }});

    r.add({"new_project", "Close the current project and start an empty one (the printer/process/filament presets are kept).",
           schema({{"discard_changes", prop_bool("Required to be true when the current project has unsaved changes.")}}), true, false, true,
           [](const json &args) {
               require_clean_or_discard(args);
               plater().new_project(true, true);
               return ToolResult::data(app_state_json());
           }});

    r.add({"open_project",
           "Open a Bambu Studio project (.3mf), replacing the current one. Model files (STL, STEP, OBJ...) must be added with "
           "import_models instead.",
           schema({{"path", prop_str("Absolute path of the .3mf project.")},
                   {"discard_changes", prop_bool("Required to be true when the current project or the presets have unsaved changes.")}},
                  {"path"}),
           true, false, true, [](const json &args) {
               const fs::path path = absolute_existing_path(arg_str(args, "path"));
               require_clean_or_discard(args);
               drop_preset_changes_or_fail(args);
               Plater &p = plater();
               // Start from a clean state so that load_project() does not ask to save the current project.
               p.new_project(true, true);
               p.load_project(from_path(path));
               return ToolResult::data(app_state_json());
           }});

    r.add({"save_project",
           "Save the project as a .3mf. Without `path` the project is saved to its current file (fails for a never-saved project). "
           "With `path` it is saved there and that file becomes the project file (save as).",
           schema({{"path", prop_str("Absolute destination path ending with .3mf (optional).")}}), true, false, false, [](const json &args) {
               Plater  &p = plater();
               fs::path path;
               if (has_arg(args, "path")) {
                   path = fs::path(arg_str(args, "path"));
                   if (!path.is_absolute()) throw ToolError("`path` must be absolute.");
                   if (!boost::iequals(path.extension().string(), ".3mf")) path += ".3mf";
                   if (!fs::exists(path.parent_path())) throw ToolError("Directory '" + path.parent_path().string() + "' does not exist.");
               } else {
                   wxString current = p.get_project_filename(".3mf");
                   if (current.empty()) throw ToolError("The project has never been saved: pass an absolute `path`.");
                   path = into_path(current);
               }

               auto strategy = SaveStrategy::SplitModel | SaveStrategy::ShareMesh;
               if (wxGetApp().app_config->get_bool("export_sources_full_pathnames")) strategy = strategy | SaveStrategy::FullPathSources;
               if (p.export_3mf(path, strategy) < 0)
                   throw ToolError("Failed to save the project to '" + path.string() + "'.");

               Slic3r::remove_backup(p.model(), false);
               p.set_project_filename(from_path(path));
               p.up_to_date(true, false);
               p.up_to_date(true, true);
               wxGetApp().update_saved_preset_from_current_preset();
               p.reset_project_dirty_after_save();
               return ToolResult::data({{"saved", path.string()}});
           }});

    r.add({"import_models",
           "Add model files (STL, 3MF, STEP, OBJ, AMF, SVG...) to the current project. New objects are placed on the target plate. "
           "Returns the indices of the created objects.",
           schema({{"paths", prop_array(prop_str("Absolute file path."), "Files to import.")},
                   {"plate", prop_int("Target plate index (default: current plate).")}},
                  {"paths"}),
           true, false, false, [](const json &args) {
               if (!args.contains("paths") || !args["paths"].is_array() || args["paths"].empty())
                   throw ToolError("`paths` must be a non-empty array of absolute paths.");
               std::vector<fs::path> paths;
               for (const json &p : args["paths"]) paths.push_back(absolute_existing_path(p.get<std::string>()));

               Plater &p = plater();
               if (has_arg(args, "plate")) {
                   plate_at(arg_int(args, "plate"));
                   p.select_plate(arg_int(args, "plate"));
               }
               Plater::TakeSnapshot snapshot(&p, "MCP: import models");
               const size_t         before = p.model().objects.size();
               std::vector<size_t>  loaded = p.load_files(paths, LoadStrategy::LoadModel);
               json                created = json::array();
               for (size_t i = before; i < p.model().objects.size(); ++i) created.push_back(object_summary_json(int(i), false));
               if (created.empty() && loaded.empty())
                   throw ToolError("Nothing was imported (unsupported or invalid file?).");
               return ToolResult::data({{"created_objects", created}});
           }});

    r.add({"undo", "Undo the last action(s), exactly like Ctrl+Z.", schema({{"steps", prop_int("Number of steps (default 1).")}}), true, false, false,
           [](const json &args) {
               const int steps = std::max(1, arg_int(args, "steps", 1));
               for (int i = 0; i < steps && plater().can_undo(); ++i) plater().undo();
               return ToolResult::data(app_state_json());
           }});

    r.add({"redo", "Redo the last undone action(s), exactly like Ctrl+Y.", schema({{"steps", prop_int("Number of steps (default 1).")}}), true, false,
           false, [](const json &args) {
               const int steps = std::max(1, arg_int(args, "steps", 1));
               for (int i = 0; i < steps && plater().can_redo(); ++i) plater().redo();
               return ToolResult::data(app_state_json());
           }});

    // --- Plates ---------------------------------------------------------------------------------------------------------

    r.add({"list_plates", "List the build plates with their origin, printable area (world coordinates, mm) and the object instances on each.",
           schema(), true, true, false, [](const json &) {
               json plates = json::array();
               for (int i = 0; i < plater().get_partplate_list().get_plate_count(); ++i) plates.push_back(plate_json(i));
               return ToolResult::data(plates);
           }});

    r.add({"add_plate", "Add a new empty plate and make it current.", schema(), true, false, false, [](const json &) {
               if (!plater().can_add_plate()) throw ToolError("Cannot add another plate.");
               plater().get_partplate_list().add_plate();
               plater().update();
               return ToolResult::data(plate_json(plater().get_partplate_list().get_plate_count() - 1));
           }});

    r.add({"delete_plate", "Delete a plate. Objects on it are deleted too. The last remaining plate cannot be deleted.",
           schema({{"plate", prop_int("Plate index.")}}, {"plate"}), true, false, true, [](const json &args) {
               const int idx = arg_int(args, "plate");
               plate_at(idx);
               if (plater().get_partplate_list().get_plate_count() <= 1) throw ToolError("Cannot delete the last plate.");
               plater().delete_plate(idx);
               return ToolResult::data({{"plate_count", plater().get_partplate_list().get_plate_count()}});
           }});

    r.add({"select_plate", "Make a plate the current one (the one sliced/previewed by default).",
           schema({{"plate", prop_int("Plate index.")}}, {"plate"}), true, false, false, [](const json &args) {
               const int idx = arg_int(args, "plate");
               plate_at(idx);
               plater().select_plate(idx);
               return ToolResult::data(plate_json(idx));
           }});

    r.add({"rename_plate", "Rename a plate.", schema({{"plate", prop_int("Plate index.")}, {"name", prop_str("New name.")}}, {"plate", "name"}), true,
           false, false, [](const json &args) {
               PartPlate        &plate = plate_at(arg_int(args, "plate"));
               const std::string name  = arg_str(args, "name");
               if (Plater::has_illegal_filename_characters(name))
                   throw ToolError("The name contains a character that is not allowed: < > : / \\ | ? * \" or an HTML entity.");
               plate.set_plate_name(name);
               if (ObjectList *list = wxGetApp().obj_list()) list->update_plate_values_for_items();
               return ToolResult::data(plate_json(arg_int(args, "plate")));
           }});

    // --- Screenshot -----------------------------------------------------------------------------------------------------

    r.add({"screenshot",
           "Render a PNG image of a plate (models with their filament colours) from a given viewpoint, to visually check the layout, "
           "orientation or painting.",
           schema({{"plate", prop_int("Plate index (default: current plate).")},
                   {"view", prop_enum("Camera direction (default iso).", {"iso", "top", "bottom", "front", "rear", "left", "right", "top_front"})},
                   {"size", prop_int("Image width and height in pixels, 128..1024 (default 512).")}}),
           true, true, false, [](const json &args) {
               Plater   &p     = plater();
               const int plate = arg_int(args, "plate", p.get_partplate_list().get_curr_plate_index());
               plate_at(plate);
               const int size = std::clamp(arg_int(args, "size", 512), 128, 1024);

               ThumbnailData    data;
               ThumbnailsParams params{{}, false, true, true, false, plate};
               params.background_color = Vec4f(0.93f, 0.93f, 0.93f, 1.0f);
               p.get_view3D_canvas3D()->render_thumbnail(data, unsigned(size), unsigned(size), params, Camera::EType::Perspective,
                                                         view_angle(arg_str(args, "view", "iso")));
               if (!data.is_valid())
                   throw ToolError("Rendering failed (is the 3D view initialised?).");
               ToolResult res;
               res.add_image_png(encode_png_base64(data));
               res.add_text("Plate " + std::to_string(plate) + ", view " + arg_str(args, "view", "iso"));
               return res;
           }});
}

}}} // namespace Slic3r::GUI::Mcp
