#ifndef slic3r_GUI_AFCPrintDialog_hpp_
#define slic3r_GUI_AFCPrintDialog_hpp_

#include <wx/dialog.h>
#include <wx/webview.h>

namespace Slic3r {
namespace GUI {

// Lane-mapping step shown before the Snapmaker print dialog when the
// connected printer runs AFC: every project filament gets an AFC lane
// (any unit, any toolhead), applied with SET_MAP before the print starts.
class AFCPrintDialog : public wxDialog
{
public:
    AFCPrintDialog(wxWindow *parent);
    ~AFCPrintDialog() override;

    // Shows the page modally. True when the user applied a mapping and
    // wants to go on to the print dialog.
    bool run();

    // Called by the page bridge (sw_AFC_FinishPrintMapping).
    static void finish_active(bool proceed);

private:
    void OnScriptMessage(wxWebViewEvent &evt);
    void OnClose(wxCloseEvent &evt);
    void end(bool proceed);

    wxWebView *m_browser{nullptr};
    bool       m_proceed{false};
    bool       m_ended{false};

    static AFCPrintDialog *s_active;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AFCPrintDialog_hpp_
