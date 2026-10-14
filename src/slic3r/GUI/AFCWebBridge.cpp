#include "AFCWebBridge.hpp"

#include "AFCManager.hpp"
#include "AFCPrintDialog.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PrinterWebView.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include "libslic3r/AFCProtocol.hpp"

#include <boost/log/trivial.hpp>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

namespace {

std::shared_ptr<PrintHost> connected_host() { return AFCManager::afc_host(); }

} // namespace

const std::unordered_set<std::string> &SSWCP_AFC_Instance::commands()
{
    static const std::unordered_set<std::string> list = {
        "sw_AFC_GetState",     "sw_AFC_EditLane",           "sw_AFC_LaneAction",     "sw_AFC_ApplyMapping",
        "sw_AFC_SyncToSlicer", "sw_AFC_OpenSpoolman",       "sw_AFC_FinishPrintMapping", "sw_AFC_SetPanelVisible",
        "sw_AFC_Dry",
    };
    return list;
}

void SSWCP_AFC_Instance::process()
{
    try {
        if (m_cmd == "sw_AFC_GetState")
            sw_AFC_GetState();
        else if (m_cmd == "sw_AFC_EditLane")
            sw_AFC_EditLane();
        else if (m_cmd == "sw_AFC_LaneAction")
            sw_AFC_LaneAction();
        else if (m_cmd == "sw_AFC_ApplyMapping")
            sw_AFC_ApplyMapping();
        else if (m_cmd == "sw_AFC_SyncToSlicer")
            sw_AFC_SyncToSlicer();
        else if (m_cmd == "sw_AFC_OpenSpoolman")
            sw_AFC_OpenSpoolman();
        else if (m_cmd == "sw_AFC_FinishPrintMapping")
            sw_AFC_FinishPrintMapping();
        else if (m_cmd == "sw_AFC_SetPanelVisible")
            sw_AFC_SetPanelVisible();
        else if (m_cmd == "sw_AFC_Dry")
            sw_AFC_Dry();
        else
            fail("unknown command");
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "AFC bridge: " << m_cmd << " failed: " << e.what();
        fail(e.what());
    }
}

void SSWCP_AFC_Instance::fail(const std::string &message)
{
    handle_general_fail(-1, wxString::FromUTF8(message));
    finish_job();
}

void SSWCP_AFC_Instance::reply_state(bool refresh, bool include_project, const std::string &error)
{
    auto self = std::static_pointer_cast<SSWCP_AFC_Instance>(shared_from_this());
    auto answer = [self, include_project, error]() {
        wxGetApp().CallAfter([self, include_project, error]() {
            self->m_res_data = AFCManager::instance().page_state(include_project);
            if (!error.empty())
                self->m_res_data["error"] = error;
            self->send_to_js();
            self->finish_job();
        });
    };
    auto host = connected_host();
    if (refresh && host)
        AFCManager::instance().refresh_async(host, [answer](bool) { answer(); });
    else
        answer();
}

void SSWCP_AFC_Instance::sw_AFC_GetState()
{
    const bool refresh = m_param_data.value("refresh", true);
    const bool project = m_param_data.value("project", false);
    reply_state(refresh, project);
}

void SSWCP_AFC_Instance::sw_AFC_EditLane()
{
    auto host = connected_host();
    if (!host)
        return fail("The printer is not connected");
    const std::string lane    = m_param_data.value("lane", "");
    const json        changes = m_param_data.value("changes", json::object());

    std::vector<std::string> gcode;
    std::string              error;
    if (!AFCProtocol::build_lane_edit_gcode(AFCManager::instance().snapshot(), lane, changes, gcode, error))
        return fail(error);

    auto self = std::static_pointer_cast<SSWCP_AFC_Instance>(shared_from_this());
    AFCManager::send_gcode(host, gcode, [self](const std::string &err) { self->reply_state(true, false, err); });
}

void SSWCP_AFC_Instance::sw_AFC_LaneAction()
{
    auto host = connected_host();
    if (!host)
        return fail("The printer is not connected");
    const std::string lane   = m_param_data.value("lane", "");
    const std::string action = m_param_data.value("action", "");

    std::string gcode, error;
    if (!AFCProtocol::build_lane_action_gcode(AFCManager::instance().snapshot(), lane, action, gcode, error))
        return fail(error);

    // Filament moves only on an idle printer: never in the middle of a print.
    auto self = std::static_pointer_cast<SSWCP_AFC_Instance>(shared_from_this());
    host->async_jsonrpc_call("printer.objects.query", json{{"objects", {{"print_stats", {"state"}}}}},
                             [self, host, gcode](const json &res) {
                                 std::string state;
                                 try {
                                     state = res.at("data").at("status").at("print_stats").at("state").get<std::string>();
                                 } catch (...) {}
                                 if (state == "printing" || state == "paused") {
                                     self->reply_state(false, false, "The printer is " + state + "; lane moves wait until it is idle");
                                     return;
                                 }
                                 if (state.empty()) {
                                     self->reply_state(false, false, "Could not read the printer state, so nothing was moved");
                                     return;
                                 }
                                 AFCManager::send_gcode(host, {gcode},
                                                        [self](const std::string &err) { self->reply_state(true, false, err); });
                             });
}

void SSWCP_AFC_Instance::sw_AFC_Dry()
{
    auto host = connected_host();
    if (!host)
        return fail("The printer is not connected");
    const std::string unit = m_param_data.value("unit", "");
    std::string       gcode, error;
    if (!AFCProtocol::build_dry_gcode(AFCManager::instance().snapshot(), unit, m_param_data, gcode, error))
        return fail(error);
    auto self = std::static_pointer_cast<SSWCP_AFC_Instance>(shared_from_this());
    AFCManager::send_gcode(host, {gcode}, [self](const std::string &err) { self->reply_state(true, false, err); });
}

void SSWCP_AFC_Instance::sw_AFC_ApplyMapping()
{
    auto host = connected_host();
    if (!host)
        return fail("The printer is not connected");
    if (!m_param_data.count("mapping") || !m_param_data["mapping"].is_array())
        return fail("param [mapping] required");

    std::vector<std::pair<int, std::string>> pairs;
    for (const json &m : m_param_data["mapping"]) {
        if (!m.is_object() || !m.count("tool") || !m["tool"].is_number_integer() || !m.count("lane") || !m["lane"].is_string())
            return fail("mapping entries need an integer tool and a lane name");
        pairs.emplace_back(m["tool"].get<int>(), m["lane"].get<std::string>());
    }

    std::vector<std::string> gcode;
    std::string              error;
    if (!AFCProtocol::build_tool_mapping_gcode(AFCManager::instance().snapshot(), pairs, gcode, error))
        return fail(error);

    auto self = std::static_pointer_cast<SSWCP_AFC_Instance>(shared_from_this());
    AFCManager::send_gcode(host, gcode, [self](const std::string &err) { self->reply_state(true, true, err); });
}

void SSWCP_AFC_Instance::sw_AFC_SyncToSlicer()
{
    send_to_js();
    finish_job();
    wxGetApp().CallAfter([]() {
        if (wxGetApp().plater())
            wxGetApp().plater()->sidebar().show_sync_filament_dialog();
    });
}

void SSWCP_AFC_Instance::sw_AFC_OpenSpoolman()
{
    const AFCProtocol::Snapshot snap = AFCManager::instance().snapshot();
    if (snap.spoolman_url.empty())
        return fail("AFC did not report a Spoolman address");
    std::string url = snap.spoolman_url;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    const int spool = m_param_data.value("spool_id", 0);
    url += spool > 0 ? "/spool/show/" + std::to_string(spool) : "/spool";
    send_to_js();
    finish_job();
    wxGetApp().CallAfter([url]() { wxLaunchDefaultBrowser(wxString::FromUTF8(url)); });
}

void SSWCP_AFC_Instance::sw_AFC_FinishPrintMapping()
{
    const std::string action = m_param_data.value("action", "cancel");
    send_to_js();
    finish_job();
    wxGetApp().CallAfter([action]() { AFCPrintDialog::finish_active(action == "continue"); });
}

void SSWCP_AFC_Instance::sw_AFC_SetPanelVisible()
{
    const bool visible = m_param_data.value("visible", true);
    send_to_js();
    finish_job();
    wxGetApp().CallAfter([visible]() {
        if (wxGetApp().mainframe && wxGetApp().mainframe->m_printer_view)
            wxGetApp().mainframe->m_printer_view->show_afc_panel(visible);
    });
}

} // namespace GUI
} // namespace Slic3r
