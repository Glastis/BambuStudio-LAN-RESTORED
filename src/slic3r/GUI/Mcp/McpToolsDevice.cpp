// MCP tools: printers (machines) known to Bambu Studio, their live status and basic controls.

#include "McpToolsCommon.hpp"

#include <set>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevChamber.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevHMS.h"
#include "slic3r/GUI/DeviceCore/DevLamp.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r { namespace GUI { namespace Mcp {

static DeviceManager &device_manager()
{
    DeviceManager *dm = wxGetApp().getDeviceManager();
    if (dm == nullptr) throw ToolError("The device manager is not available.");
    return *dm;
}

static MachineObject &machine(const json &args)
{
    DeviceManager &dm = device_manager();
    if (has_arg(args, "dev_id")) {
        const std::string id = arg_str(args, "dev_id");
        if (MachineObject *m = dm.get_my_machine(id)) return *m;
        if (MachineObject *m = dm.get_local_machine(id)) return *m;
        throw ToolError("Unknown printer '" + id + "' (see list_printers).");
    }
    if (MachineObject *m = dm.get_selected_machine()) return *m;
    throw ToolError("No printer is selected in Bambu Studio: pass `dev_id` (see list_printers).");
}

static json tray_json(DevAmsTray &tray)
{
    json t = {{"id", tray.id}, {"loaded", tray.is_exists}};
    if (!tray.is_exists) return t;
    t["type"]      = tray.get_display_filament_type();
    t["color"]     = tray.color.empty() ? std::string() : "#" + tray.color.substr(0, 6);
    t["remain_pct"] = tray.remain;
    t["filament_id"] = tray.setting_id;
    if (!tray.sub_brands.empty()) t["brand"] = tray.sub_brands;
    return t;
}

static json machine_summary(MachineObject &m, const std::string &selected_id)
{
    return json{{"dev_id", m.get_dev_id()},
                {"name", m.get_dev_name()},
                {"model", m.get_printer_type_display_str().ToUTF8().data()},
                {"ip", m.get_dev_ip()},
                {"connection", m.connection_type()},
                {"online", m.is_online()},
                {"selected", m.get_dev_id() == selected_id}};
}

static json machine_status(MachineObject &m)
{
    json s = machine_summary(m, m.get_dev_id());
    s.erase("selected");
    s["connected"] = m.is_connected();

    json job = {{"status", m.print_status},
                {"stage", into_u8(m.get_curr_stage())},
                {"progress_pct", m.mc_print_percent},
                {"remaining_s", std::max(0, m.mc_left_time)},
                {"layer", m.curr_layer},
                {"total_layers", m.total_layers},
                {"file", m.m_gcode_file},
                {"task", m.subtask_name},
                {"can_pause", m.can_pause()},
                {"can_resume", m.can_resume()},
                {"can_stop", m.can_abort()}};
    if (m.print_error != 0) job["error_code"] = MachineObject::get_error_code_str(m.print_error);
    s["job"] = job;

    json temps = json::object();
    if (DevExtderSystem *ext = m.GetExtderSystem()) {
        json nozzles = json::array();
        for (int i = 0; i < ext->GetTotalExtderCount(); ++i)
            nozzles.push_back({{"extruder", i},
                               {"temp", ext->GetNozzleTempCurrent(i)},
                               {"target", ext->GetNozzleTempTarget(i)},
                               {"diameter_mm", ext->GetNozzleDiameter(i)},
                               {"type", DevNozzle::GetNozzleTypeString(ext->GetNozzleType(i))}});
        temps["nozzles"] = nozzles;
    }
    if (DevBed *bed = m.GetBed()) temps["bed"] = {{"temp", bed->GetBedTemp()}, {"target", bed->GetBedTempTarget()}};
    if (auto chamber = m.GetChamber()) temps["chamber"] = {{"temp", chamber->GetChamberTemp()}, {"target", chamber->GetChamberTempTarget()}};
    s["temperatures"] = temps;

    if (DevFan *fan = m.GetFan())
        s["fans_pct"] = {{"part_cooling", fan->GetCoolingFanSpeed()},
                         {"aux", fan->GetBigFan1Speed()},
                         {"chamber", fan->GetBigFan2Speed()},
                         {"heatbreak", fan->GetHeatBreakFanSpeed()}};
    if (DevLamp *lamp = m.GetLamp()) s["chamber_light_on"] = lamp->IsChamberLightOn();

    json ams = json::array();
    if (auto fila = m.GetFilaSystem()) {
        for (auto &[id, unit] : fila->GetAmsList()) {
            if (unit == nullptr) continue;
            json trays = json::array();
            for (const auto &[tid, tray] : unit->GetTrays())
                if (tray) trays.push_back(tray_json(*tray));
            ams.push_back({{"ams_id", unit->GetAmsId()}, {"name", into_u8(unit->GetDisplayName())}, {"humidity_pct", unit->GetHumidityPercent()}, {"trays", trays}});
        }
    }
    s["ams"] = ams;
    json external = json::array();
    for (DevAmsTray &tray : m.vt_slot) external.push_back(tray_json(tray));
    s["external_spools"] = external;

    json hms = json::array();
    if (DevHMS *h = m.GetHMS())
        for (const auto &item : h->GetHMSItems()) hms.push_back({{"code", item.get_long_error_code()}, {"level", int(item.get_level())}});
    s["hms"] = hms;
    return s;
}

void register_device_tools(ToolRegistry &r)
{
    r.add({"list_printers", "List the printers known to Bambu Studio (LAN and cloud) and which one is selected.", schema(), true, true, false,
           [](const json &) {
               DeviceManager    &dm       = device_manager();
               MachineObject    *selected = dm.get_selected_machine();
               const std::string sel_id   = selected ? selected->get_dev_id() : std::string();
               json              out      = json::array();
               std::set<std::string> seen;
               for (auto &[id, m] : dm.get_my_machine_list())
                   if (m && seen.insert(id).second) out.push_back(machine_summary(*m, sel_id));
               for (auto &[id, m] : dm.get_local_machinelist())
                   if (m && seen.insert(id).second) {
                       json j          = machine_summary(*m, sel_id);
                       j["discovered_only"] = true; // seen on the LAN, not bound / no access code
                       out.push_back(j);
                   }
               return ToolResult::data(out);
           }});

    r.add({"get_printer_status",
           "Live status of a printer (default: the selected one): job state and progress, temperatures, fans, AMS trays and "
           "external spool, HMS error codes.",
           schema({{"dev_id", prop_str("Printer id (optional, see list_printers).")}}), true, true, false,
           [](const json &args) { return ToolResult::data(machine_status(machine(args))); }});

    r.add({"select_printer", "Select (connect to) a printer in the Device tab.", schema({{"dev_id", prop_str("Printer id (see list_printers).")}}, {"dev_id"}),
           true, false, false, [](const json &args) {
               const std::string id = arg_str(args, "dev_id");
               if (!device_manager().set_selected_machine(id))
                   throw ToolError("Cannot select '" + id + "': it must be bound or have a LAN access code (see list_printers).");
               return ToolResult::data({{"selected", id}});
           }});

    r.add({"printer_control",
           "Control the current print job or the chamber light of a printer. pause/resume/stop act on the real machine: confirm "
           "with the user before stopping a print.",
           schema({{"dev_id", prop_str("Printer id (optional, default: selected printer).")},
                   {"action", prop_enum("Action to perform.", {"pause", "resume", "stop", "light_on", "light_off", "refresh"})}},
                  {"action"}),
           true, false, true, [](const json &args) {
               MachineObject    &m      = machine(args);
               const std::string action = arg_str(args, "action");
               int               ret    = 0;
               if (action == "pause") {
                   if (!m.can_pause()) throw ToolError("The printer cannot be paused now (status " + m.print_status + ").");
                   ret = m.command_task_pause();
               } else if (action == "resume") {
                   if (!m.can_resume()) throw ToolError("The printer cannot resume now (status " + m.print_status + ").");
                   ret = m.command_task_resume();
               } else if (action == "stop") {
                   if (!m.can_abort()) throw ToolError("There is no print to stop (status " + m.print_status + ").");
                   ret = m.command_task_abort();
               } else if (action == "light_on" || action == "light_off") {
                   DevLamp *lamp = m.GetLamp();
                   if (lamp == nullptr) throw ToolError("This printer has no controllable light.");
                   lamp->CtrlSetChamberLight(action == "light_on" ? DevLamp::LIGHT_EFFECT_ON : DevLamp::LIGHT_EFFECT_OFF);
               } else if (action == "refresh") {
                   ret = m.command_request_push_all(true);
               } else
                   throw ToolError("Unknown action '" + action + "'.");
               if (ret != 0) throw ToolError("The printer rejected the command (code " + std::to_string(ret) + ").");
               return ToolResult::data({{"sent", action}, {"dev_id", m.get_dev_id()}});
           }});
}

}}} // namespace Slic3r::GUI::Mcp
