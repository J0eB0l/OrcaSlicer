#ifndef slic3r_PrinterWebView_hpp_
#define slic3r_PrinterWebView_hpp_


#include "wx/artprov.h"
#include "wx/cmdline.h"
#include "wx/notifmsg.h"
#include "wx/settings.h"
#include <wx/webview.h>
#include <wx/string.h>

#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"
#include "wx/numdlg.h"
#include "wx/infobar.h"
#include "wx/filesys.h"
#include "wx/fs_arc.h"
#include "wx/fs_mem.h"
#include "wx/stdpaths.h"
#include <wx/panel.h>
#include <wx/tbarbase.h>
#include "wx/textctrl.h"
#include <wx/timer.h>
#include <wx/button.h>
#include <wx/sizer.h>


namespace Slic3r {
namespace GUI {


class PrinterWebView : public wxPanel{
public:
    PrinterWebView(wxWindow *parent);
    virtual ~PrinterWebView();

    void load_url(wxString& url, wxString apikey = "");
    void UpdateState();
    void OnClose(wxCloseEvent& evt);
    void OnError(wxWebViewEvent& evt);
    void OnLoaded(wxWebViewEvent& evt);
    void OnScriptMessage(wxWebViewEvent& evt);
    void reload();
    void update_mode();
    bool isSnapmakerPage();
    bool is_u1_device_page();
    void sendMessage(const std::string& msg);
    wxWebView* get_browser() const { return m_browser; }

    // AFC lane panel beside the device page, for printers running AFC.
    void show_afc_panel(bool show);

private:
    void SendAPIKey();
    void OnAfcTimer(wxTimerEvent& evt);
    void OnAfcScriptMessage(wxWebViewEvent& evt);
    void ensure_afc_browser();
    void update_afc_layout();

    wxWebView* m_browser;
    long m_zoomFactor;
    wxString m_apikey;

    wxBoxSizer* m_main_sizer{nullptr};
    wxWebView*  m_afc_browser{nullptr};
    wxButton*   m_afc_toggle{nullptr};
    wxTimer     m_afc_timer;
    bool        m_afc_detected{false};

    // DECLARE_EVENT_TABLE()
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
