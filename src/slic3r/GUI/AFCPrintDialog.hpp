#ifndef slic3r_GUI_AFCPrintDialog_hpp_
#define slic3r_GUI_AFCPrintDialog_hpp_

#include "libslic3r/AFCProtocol.hpp"

#include <string>

#include <wx/dialog.h>
#include <wx/webview.h>

namespace Slic3r {
namespace GUI {

// Lane-mapping step shown before a print starts when the printer runs AFC
// (the U1 or any other Klipper printer): every project filament gets an AFC
// lane (any unit, any toolhead), applied with SET_MAP before the print starts.
// On a U1 it also holds the U1's own print switches (flow calibration,
// time-lapse, auto leveling).
class AFCPrintDialog : public wxDialog
{
public:
    AFCPrintDialog(wxWindow *parent);
    ~AFCPrintDialog() override;

    // Shows the page modally. True when the user applied a mapping and
    // wants to go on to the print dialog. `snapmaker_sends`: Snapmaker's own
    // print popup opens next and sends the U1 switches itself.
    bool run(bool snapmaker_sends);

    // Called by the page bridge (sw_AFC_FinishPrintMapping).
    static void finish_active(bool proceed, const AFCProtocol::U1PrintOptions &u1_print = {});

    // Reads AFC from the printer and, when it has AFC, runs the mapping step.
    // False only when the user canceled; a printer without AFC passes straight through.
    // `u1_print` gets the U1 switches picked on the page (present only on a U1).
    static bool map_lanes_before_print(wxWindow *parent, bool snapmaker_sends = false,
                                       AFCProtocol::U1PrintOptions *u1_print = nullptr);

    // Query string that opens Snapmaker's print popup with these switches on
    // (read by the flutter_web patch), empty when there are none.
    static std::string preprint_query(const AFCProtocol::U1PrintOptions &u1_print);

private:
    void OnScriptMessage(wxWebViewEvent &evt);
    void OnClose(wxCloseEvent &evt);
    void end(bool proceed);

    wxWebView                  *m_browser{nullptr};
    bool                        m_proceed{false};
    bool                        m_ended{false};
    AFCProtocol::U1PrintOptions m_u1_print;

    static AFCPrintDialog *s_active;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AFCPrintDialog_hpp_
