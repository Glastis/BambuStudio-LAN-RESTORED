// MCP tools: slicing, slicing results and exports.

#include "McpToolsCommon.hpp"

#include <boost/filesystem.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GLToolbar.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;

static json slice_result_json(int plate_idx)
{
    PartPlate &plate = plate_at(plate_idx);
    json       out   = {{"plate", plate_idx}, {"sliced", plate.is_slice_result_valid()}};
    if (!plate.is_slice_result_valid())
        return out;

    if (GCodeProcessorResult *res = plate.get_slice_result()) {
        const auto &normal   = res->print_statistics.modes[size_t(PrintEstimatedStatistics::ETimeMode::Normal)];
        out["print_time_s"]  = std::lround(normal.time);
        out["prepare_time_s"] = std::lround(normal.prepare_time);
        // Per filament slot usage, from the extruded volumes (mm^3) like the G-code export does.
        const DynamicPrintConfig full      = preset_bundle().full_config();
        const auto              *diameters = full.option<ConfigOptionFloats>("filament_diameter");
        const auto              *densities = full.option<ConfigOptionFloats>("filament_density");
        json                     per_filament = json::object();
        for (const auto &[extruder, volume] : res->print_statistics.total_volumes_per_extruder) {
            const double d = diameters && extruder < diameters->values.size() ? diameters->values[extruder] : 1.75;
            const double r = densities && extruder < densities->values.size() ? densities->values[extruder] : 0.;
            json         f = {{"length_m", std::round(volume / (M_PI * 0.25 * d * d) / 10) / 100}, {"volume_cm3", std::round(volume / 10) / 100}};
            if (r > 0) f["weight_g"] = std::round(volume * r * 0.001 * 100) / 100;
            per_filament[std::to_string(extruder + 1)] = f;
        }
        out["per_filament_slot"] = per_filament;

        json warnings = json::array();
        for (const auto &w : res->warnings)
            warnings.push_back({{"level", w.level == 2 ? "error" : w.level == 1 ? "warning" : "info"}, {"message", w.msg}, {"code", w.error_code}});
        out["warnings"] = warnings;
    }

    PrintBase *print_base = nullptr;
    plate.get_print(&print_base, nullptr, nullptr);
    if (Print *print = dynamic_cast<Print *>(print_base)) {
        const PrintStatistics &st = print->print_statistics();
        out["print_time"]         = st.estimated_normal_print_time;
        out["filament_weight_g"]  = std::round(st.total_weight * 100) / 100;
        out["filament_length_m"]  = std::round(st.total_used_filament / 10) / 100;
        out["cost"]               = std::round(st.total_cost * 100) / 100;
        out["filament_changes"]   = st.total_toolchanges;
    }
    return out;
}

void register_slice_tools(ToolRegistry &r)
{
    Tool slice{"slice",
               "Slice a plate (default: the current one) or every plate (`all`), wait for the end and return the estimated print "
               "time, filament usage and slicing warnings. Bambu Studio switches to the Preview tab.",
               schema({{"plate", prop_int("Plate index (default: current plate).")},
                       {"all", prop_bool("Slice every plate (default false).")},
                       {"run_post_process_scripts",
                        prop_bool("Execute the post-processing scripts embedded in the project, if any (default false: the "
                                  "G-code is generated without running them). Only enable for trusted projects.")},
                       {"timeout_s", prop_int("Max wait in seconds (default 600).")}})};
    slice.on_gui_thread = false;
    slice.handler       = [](const json &args) {
        const bool all     = arg_bool(args, "all", false);
        const int  timeout = std::clamp(arg_int(args, "timeout_s", 600), 10, 3600);
        // Explicit answer to the "this project contains post-processing scripts" security prompt.
        const bool run_scripts = arg_bool(args, "run_post_process_scripts", false);
        JobDialogScope dialogs; // slicing of the following plates continues in background
        set_dialog_override([run_scripts](wxDialog *dialog) {
            return dynamic_cast<PostProcessScriptDialog *>(dialog) ? (run_scripts ? wxID_YES : wxID_NO) : wxID_NONE;
        });

        json plates = run_on_gui([args, all]() {
            Plater &p = plater();
            if (p.model().objects.empty()) throw ToolError("Nothing to slice: the project has no objects.");
            json targets = json::array();
            if (all) {
                for (int i = 0; i < p.get_partplate_list().get_plate_count(); ++i)
                    if (!p.get_partplate_list().get_plate(i)->empty()) targets.push_back(i);
                // Processed synchronously so that any prompt is answered within this call.
                SimpleEvent evt(EVT_GLTOOLBAR_SLICE_ALL);
                p.ProcessWindowEvent(evt);
            } else {
                const int idx = arg_int(args, "plate", p.get_partplate_list().get_curr_plate_index());
                if (plate_at(idx).empty()) throw ToolError("Plate " + std::to_string(idx) + " is empty.");
                if (idx != p.get_partplate_list().get_curr_plate_index()) p.select_plate(idx);
                targets.push_back(idx);
                p.update(true, true);
                SimpleEvent evt(EVT_GLTOOLBAR_SLICE_PLATE);
                p.ProcessWindowEvent(evt);
            }
            return targets;
        });

        auto all_done = [plates]() {
            if (plater().is_background_process_slicing()) return false;
            for (const json &i : plates)
                if (!plate_at(i.get<int>()).is_slice_result_valid()) return false;
            return true;
        };

        // Slicing starts asynchronously: give it a moment, then wait for the end.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        auto       idle_since   = std::chrono::steady_clock::time_point{};
        bool       done         = false;
        while (std::chrono::steady_clock::now() < deadline) {
            json state = run_on_gui([all_done]() { return json{{"done", all_done()}, {"slicing", plater().is_background_process_slicing()}}; });
            if (state["done"].get<bool>()) { done = true; break; }
            if (!state["slicing"].get<bool>()) {
                if (idle_since == std::chrono::steady_clock::time_point{}) idle_since = std::chrono::steady_clock::now();
                else if (std::chrono::steady_clock::now() - idle_since > std::chrono::seconds(8)) break; // stopped without a valid result
            } else
                idle_since = {};
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        json result = run_on_gui([plates]() {
            json out = json::array();
            for (const json &i : plates) out.push_back(slice_result_json(i.get<int>()));
            return out;
        });
        if (!done) {
            ToolResult res = ToolResult::data(result);
            res.add_text("Slicing did not produce a valid result for every plate (error, objects outside the plate, or timeout). "
                         "Check the warnings above and take a screenshot / look at Bambu Studio notifications.");
            res.is_error = true;
            return res;
        }
        return ToolResult::data(result);
    };
    r.add(slice);

    r.add({"get_slice_result", "Estimated print time, filament usage and warnings of an already sliced plate.",
           schema({{"plate", prop_int("Plate index (default: current plate).")}}), true, true, false, [](const json &args) {
               return ToolResult::data(slice_result_json(arg_int(args, "plate", plater().get_partplate_list().get_curr_plate_index())));
           }});

    r.add({"export_gcode", "Write the G-code of a sliced plate to a file.",
           schema({{"plate", prop_int("Plate index (default: current plate).")}, {"path", prop_str("Absolute destination path (.gcode).")}}, {"path"}),
           true, false, false, [](const json &args) {
               const int  idx   = arg_int(args, "plate", plater().get_partplate_list().get_curr_plate_index());
               PartPlate &plate = plate_at(idx);
               fs::path   dst(arg_str(args, "path"));
               if (!dst.is_absolute()) throw ToolError("`path` must be absolute.");
               if (!plate.is_slice_result_valid() || plate.get_slice_result() == nullptr) throw ToolError("Plate " + std::to_string(idx) + " is not sliced: call slice first.");
               const fs::path src(plate.get_slice_result()->filename);
               if (!fs::exists(src)) throw ToolError("The temporary G-code of this plate is not available any more: slice it again.");
               boost::system::error_code ec;
               fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
               if (ec) throw ToolError("Copy failed: " + ec.message());
               return ToolResult::data({{"written", dst.string()}, {"bytes", fs::file_size(dst)}});
           }});

    r.add({"export_sliced_3mf",
           "Write a printable .gcode.3mf (the file format sent to Bambu printers) for a sliced plate or for every plate.",
           schema({{"path", prop_str("Absolute destination path (.gcode.3mf).")},
                   {"plate", prop_int("Plate index (default: current plate).")},
                   {"all", prop_bool("Export every sliced plate (default false).")}},
                  {"path"}),
           true, false, false, [](const json &args) {
               Plater  &p = plater();
               fs::path dst(arg_str(args, "path"));
               if (!dst.is_absolute()) throw ToolError("`path` must be absolute.");
               int idx = PLATE_ALL_IDX;
               if (!arg_bool(args, "all", false)) {
                   idx = arg_int(args, "plate", p.get_partplate_list().get_curr_plate_index());
                   if (!plate_at(idx).is_slice_result_valid()) throw ToolError("Plate " + std::to_string(idx) + " is not sliced: call slice first.");
               } else if (!p.get_partplate_list().is_all_slice_results_valid())
                   throw ToolError("Not every plate is sliced: call slice with all=true first.");
               if (p.export_3mf(dst, SaveStrategy::Silence | SaveStrategy::SplitModel | SaveStrategy::WithGcode | SaveStrategy::SkipModel, idx) < 0)
                   throw ToolError("Export failed.");
               return ToolResult::data({{"written", dst.string()}});
           }});
}

}}} // namespace Slic3r::GUI::Mcp
