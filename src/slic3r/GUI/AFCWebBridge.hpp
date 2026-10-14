#ifndef slic3r_GUI_AFCWebBridge_hpp_
#define slic3r_GUI_AFCWebBridge_hpp_

#include "SSWCP.hpp"

#include <string>
#include <unordered_set>

namespace Slic3r {
namespace GUI {

// SSWCP commands behind the AFC pages (resources/web/afc): the lane panel on
// the Device tab and the lane-mapping step before a print. Lane edits are
// built from structured fields in C++ (AFCProtocol) and never taken as raw
// G-code from the page.
//
//   sw_AFC_GetState          {refresh?, project?}        -> {connected, afc, project?}
//   sw_AFC_EditLane          {lane, changes{...}}        -> new state
//   sw_AFC_LaneAction        {lane, action}              load / unload / eject, idle printer only
//   sw_AFC_ApplyMapping      {mapping:[{tool, lane}]}    SET_MAP each T(n), -> new state
//   sw_AFC_SyncToSlicer      {}                          opens the filament sync with AFC slots
//   sw_AFC_OpenSpoolman      {spool_id}                  opens the spool in Spoolman
//   sw_AFC_FinishPrintMapping {action}                   "continue" | "cancel" (print step)
//   sw_AFC_SetPanelVisible   {visible}                   shows/hides the Device-tab panel
//   sw_AFC_Dry               {unit, action, temp?, minutes?, rotate?}  start / stop a unit dryer
class SSWCP_AFC_Instance : public SSWCP_Instance
{
public:
    SSWCP_AFC_Instance(std::string cmd, const json &header, const json &data, std::string event_id, wxWebView *webview)
        : SSWCP_Instance(cmd, header, data, event_id, webview)
    {}

    void process() override;

    static const std::unordered_set<std::string> &commands();

private:
    void sw_AFC_GetState();
    void sw_AFC_EditLane();
    void sw_AFC_LaneAction();
    void sw_AFC_ApplyMapping();
    void sw_AFC_SyncToSlicer();
    void sw_AFC_OpenSpoolman();
    void sw_AFC_FinishPrintMapping();
    void sw_AFC_SetPanelVisible();
    void sw_AFC_Dry();

    // Answer on the UI thread with the current page state (after a refresh when asked).
    void reply_state(bool refresh, bool include_project, const std::string &error = "");
    void fail(const std::string &message);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AFCWebBridge_hpp_
