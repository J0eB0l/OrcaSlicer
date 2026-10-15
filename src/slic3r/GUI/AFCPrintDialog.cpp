#include "AFCPrintDialog.hpp"

#include "AFCManager.hpp"
#include "GUI_App.hpp"
#include "HttpServer.hpp"
#include "I18N.hpp"
#include "SSWCP.hpp"
#include "Widgets/WebView.hpp"

#include <wx/sizer.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

AFCPrintDialog *AFCPrintDialog::s_active = nullptr;

AFCPrintDialog::AFCPrintDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("AFC lanes for this print"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    SetBackgroundColour(*wxWHITE);
    m_browser = WebView::CreateWebView(this, "about:blank");
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }
    Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &AFCPrintDialog::OnScriptMessage, this, m_browser->GetId());
    Bind(wxEVT_CLOSE_WINDOW, &AFCPrintDialog::OnClose, this);

    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);
    SetMinSize(FromDIP(wxSize(760, 640)));
    SetSize(FromDIP(wxSize(820, 720)));
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

AFCPrintDialog::~AFCPrintDialog()
{
    if (s_active == this)
        s_active = nullptr;
    if (m_browser)
        SSWCP::on_webview_delete(m_browser);
}

bool AFCPrintDialog::run(bool snapmaker_sends)
{
    if (m_browser == nullptr)
        return true; // no webview: do not block printing
    const wxString url = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) +
                                            "/web/afc/index.html?view=print" + (snapmaker_sends ? "&sender=snapmaker" : ""));
    m_browser->LoadURL(wxGetApp().get_international_url(url));
    m_proceed  = false;
    m_ended    = false;
    m_u1_print = {};
    s_active  = this;
    ShowModal();
    s_active = nullptr;
    return m_proceed;
}

bool AFCPrintDialog::map_lanes_before_print(wxWindow *parent, bool snapmaker_sends, AFCProtocol::U1PrintOptions *u1_print)
{
    if (u1_print)
        *u1_print = {};
    const std::shared_ptr<PrintHost> host = AFCManager::afc_host();
    if (!host)
        return true;
    bool present = false;
    {
        wxBusyCursor busy;
        present = AFCManager::instance().refresh_blocking(host, 4000);
    }
    if (!present)
        return true;
    AFCPrintDialog dialog(parent);
    const bool     proceed = dialog.run(snapmaker_sends);
    if (proceed && u1_print)
        *u1_print = dialog.m_u1_print;
    return proceed;
}

std::string AFCPrintDialog::preprint_query(const AFCProtocol::U1PrintOptions &u1_print)
{
    if (!u1_print.present)
        return {};
    auto flag = [](bool on) { return on ? "1" : "0"; };
    return std::string("&afc_bed_level=") + flag(u1_print.bed_level) + "&afc_flow_calibrate=" + flag(u1_print.flow_calibrate) +
           "&afc_time_lapse_camera=" + flag(u1_print.time_lapse);
}

void AFCPrintDialog::finish_active(bool proceed, const AFCProtocol::U1PrintOptions &u1_print)
{
    if (s_active) {
        if (proceed)
            s_active->m_u1_print = u1_print;
        s_active->end(proceed);
    }
}

void AFCPrintDialog::end(bool proceed)
{
    if (m_ended || !IsModal())
        return;
    m_ended   = true;
    m_proceed = proceed;
    EndModal(proceed ? wxID_OK : wxID_CANCEL);
}

void AFCPrintDialog::OnScriptMessage(wxWebViewEvent &evt)
{
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}

void AFCPrintDialog::OnClose(wxCloseEvent &)
{
    // Ending the modal loop is enough; run()'s caller owns the dialog.
    end(false);
}

} // namespace GUI
} // namespace Slic3r
