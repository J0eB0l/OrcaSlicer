#include "AFCPrintDialog.hpp"

#include "AFCCloudUpload.hpp"
#include "AFCManager.hpp"
#include "GUI_App.hpp"
#include "HttpServer.hpp"
#include "I18N.hpp"
#include "SSWCP.hpp"
#include "Widgets/ProgressDialog.hpp"
#include "Widgets/WebView.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <wx/sizer.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {

namespace {

using json = nlohmann::json;

// One JSON-RPC call over the U1's link, waited for here. Moonraker_Mqtt answers
// on a thread of its own, never this one.
json call_and_wait(const std::shared_ptr<PrintHost> &host, const std::string &method, const json &params, int timeout_ms)
{
    struct Wait
    {
        std::mutex              mtx;
        std::condition_variable cv;
        bool                    done{false};
        json                    res;
    };
    auto wait = std::make_shared<Wait>();
    host->async_jsonrpc_call(method, params, [wait](const json &res) {
        std::lock_guard<std::mutex> lock(wait->mtx);
        wait->res  = res;
        wait->done = true;
        wait->cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(wait->mtx);
    if (!wait->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&wait] { return wait->done; }))
        return json{{"error", "timeout"}};
    return wait->res;
}

// The error of a JSON-RPC answer as text, "" when it has none.
std::string rpc_error(const json &res)
{
    if (res.is_null())
        return "send failed";
    if (!res.is_object() || !res.contains("error"))
        return {};
    const json &err = res["error"];
    if (err.is_string())
        return err.get<std::string>();
    if (err.is_object() && err.contains("message") && err["message"].is_string())
        return err["message"].get<std::string>();
    return err.dump();
}

bool method_missing(const json &res)
{
    if (!res.is_object() || !res.contains("error"))
        return false;
    const json &err = res["error"];
    if (err.is_object() && err.contains("code") && err["code"].is_number_integer() && err["code"].get<int>() == -32601)
        return true;
    return boost::algorithm::to_lower_copy(rpc_error(res)).find("method not found") != std::string::npos;
}

std::string u8_path(const boost::filesystem::path &path) { return boost::nowide::narrow(path.generic_wstring()); }

} // namespace

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

AFCPrintDialog::U1Send AFCPrintDialog::send_u1_print(wxWindow *parent, const boost::filesystem::path &gcode,
                                                     const boost::filesystem::path &upload_path,
                                                     const AFCProtocol::U1PrintOptions &u1_print, std::string &error)
{
    error.clear();
    const std::shared_ptr<PrintHost> host = AFCManager::device_host();
    if (!host)
        return U1Send::NotSent;

    // The map table comes from AFC as the lane screen just left it.
    std::vector<std::pair<int, int>> table;
    {
        wxBusyCursor busy;
        AFCManager &afc = AFCManager::instance();
        if (!afc.refresh_blocking(host, 4000) || !AFCProtocol::u1_map_table(afc.snapshot(), afc.project_filaments(), table, error)) {
            BOOST_LOG_TRIVIAL(warning) << "AFC print: not sending straight to the U1: " << (error.empty() ? "AFC did not answer" : error);
            error.clear();
            return U1Send::NotSent;
        }
    }

    // stage: 0 finding the U1 on the LAN, 1 uploading, 2 starting; -1 canceled.
    // The worker moves 0 -> 1 -> 2 and the dialog 0/1 -> -1, each only from the
    // stage it saw, so a cancel either lands before the start goes out or not at all.
    struct Job
    {
        std::atomic<int>  stage{0};
        std::atomic<int>  percent{0};
        std::atomic<bool> cloud{false}; // uploading through Snapmaker's cloud, not the LAN
        std::atomic<bool> finished{false};
        U1Send            result{U1Send::NotSent};
        std::string       error;
    };
    auto job = std::make_shared<Job>();

    // Away from the U1's network the upload goes through Snapmaker's cloud with the
    // account the slicer is signed in to, as Snapmaker's popup does.
    AFCCloudUpload::Account account;
    const bool              signed_in = AFCCloudUpload::signed_in_account(account);
    const std::string       serial    = host->get_sn();

    const std::string name   = u8_path(upload_path.filename());
    const std::string folder = u8_path(upload_path.parent_path());
    std::thread([job, host, gcode, name, folder, u1_print, table, account, signed_in, serial]() {
        auto finish = [&job](U1Send result, const std::string &error) {
            job->result = result;
            job->error  = error;
            job->finished = true;
        };
        auto advance = [&job](int from, int to) { return job->stage.compare_exchange_strong(from, to); };
        // Starts the uploaded file. A start the U1 may have taken is shown, never retried, so it cannot print twice.
        auto start = [&](const std::string &method, const json &params, int timeout_ms) {
            const json        res = call_and_wait(host, method, params, timeout_ms);
            const std::string err = rpc_error(res);
            if (err.empty())
                return finish(U1Send::Sent, "");
            BOOST_LOG_TRIVIAL(warning) << "AFC print: the U1 answered " << method << " with " << res.dump();
            if (err == "send failed" || method_missing(res))
                return finish(U1Send::NotSent, "");
            if (err == "timeout")
                return finish(U1Send::Failed, "The printer did not confirm the print start. Check the printer before sending again.");
            return finish(U1Send::Failed, "The printer refused the print: " + err);
        };

        const json               info = call_and_wait(host, "machine.system_info", json::object(), 8000);
        const std::vector<std::string> addresses = info.is_object() && info.contains("data") ? AFCProtocol::lan_addresses(info["data"]) :
                                                                                         std::vector<std::string>();
        if (addresses.empty())
            BOOST_LOG_TRIVIAL(warning) << "AFC print: the U1 gave no LAN address (" << rpc_error(info) << ")";
        if (!advance(0, 1))
            return finish(U1Send::Canceled, "");

        std::string stored, failure;
        for (const std::string &address : addresses) {
            bool        done = false;
            unsigned    http_status = 0;
            std::string answer;
            const std::string url = "http://" + address + "/server/files/upload";
            auto http = Http::post(url);
            http.timeout_connect(5);
            if (!folder.empty())
                http.form_add("path", folder);
            http.form_add_file("file", gcode, name)
                .on_complete([&](std::string body, unsigned) {
                    done   = true;
                    answer = std::move(body);
                })
                .on_error([&](std::string body, std::string err, unsigned status) {
                    http_status = status;
                    failure     = url + ": " + (err.empty() ? "HTTP " + std::to_string(status) : err) + (body.empty() ? "" : " " + body);
                })
                .on_progress([&](Http::Progress progress, bool &cancel) {
                    if (progress.ultotal > 0)
                        job->percent = int(progress.ulnow * 100 / progress.ultotal);
                    cancel = job->stage.load() < 0;
                })
                .perform_sync();
            if (job->stage.load() < 0)
                return finish(U1Send::Canceled, "");
            if (done) {
                stored = AFCProtocol::uploaded_path(json::parse(answer, nullptr, false));
                if (stored.empty())
                    stored = folder.empty() ? name : folder + "/" + name;
                break;
            }
            if (http_status != 0)
                break; // the U1 answered and refused it; another address will not change that
        }
        if (!stored.empty()) {
            if (!advance(1, 2))
                return finish(U1Send::Canceled, "");
            const json params = AFCProtocol::u1_local_print_params(stored, u1_print, table);
            BOOST_LOG_TRIVIAL(warning) << "AFC print: starting " << stored << " on the U1 with " << params["options"].dump();
            return start("server.files.start_local_print", params, 20000);
        }
        if (!addresses.empty())
            BOOST_LOG_TRIVIAL(warning) << "AFC print: upload to the U1 failed (" << failure << ")";

        if (!signed_in) {
            BOOST_LOG_TRIVIAL(warning) << "AFC print: no Snapmaker account signed in for a cloud upload, leaving the send to Snapmaker's popup";
            return finish(U1Send::NotSent, "");
        }
        job->percent = 0;
        job->cloud   = true;
        AFCCloudUpload::Result uploaded;
        std::string            cloud_error;
        const bool             ok = AFCCloudUpload::upload(
            account, gcode, name, serial,
            [&job](size_t done, size_t total) {
                if (total > 0)
                    job->percent = int(done * 100 / total);
                return job->stage.load() >= 0;
            },
            uploaded, cloud_error);
        if (job->stage.load() < 0)
            return finish(U1Send::Canceled, "");
        if (!ok) {
            BOOST_LOG_TRIVIAL(warning) << "AFC print: cloud upload failed (" << cloud_error << "), leaving the send to Snapmaker's popup";
            return finish(U1Send::NotSent, "");
        }
        if (!advance(1, 2))
            return finish(U1Send::Canceled, "");
        const json params = AFCProtocol::u1_cloud_print_params(uploaded.url, uploaded.checksum, uploaded.size, u1_print, table);
        BOOST_LOG_TRIVIAL(warning) << "AFC print: starting " << name << " on the U1 from the cloud with " << params["options"].dump();
        // The U1 may fetch the file before it answers.
        return start("server.files.start_cloud_print", params, 90000);
    }).detach();

    {
        ProgressDialog dlg(_L("Send to printer"), _L("Finding the printer on the network"), 100, parent,
                           wxPD_APP_MODAL | wxPD_AUTO_HIDE | wxPD_CAN_ABORT);
        bool canceling = false;
        while (!job->finished) {
            const int stage = job->stage.load();
            bool      go    = true;
            if (stage == 0)
                go = dlg.Pulse(_L("Finding the printer on the network"));
            else if (stage == 1)
                go = dlg.Update(std::min(99, job->percent.load()),
                                job->cloud ? _L("Uploading through Snapmaker's cloud") : _L("Uploading to the printer"));
            else if (stage == 2)
                go = dlg.Pulse(_L("Starting the print"));
            if (!go && !canceling) {
                canceling = true;
                // Too late once the start went out: then its answer still decides.
                int seen = job->stage.load();
                while (seen >= 0 && seen < 2)
                    if (job->stage.compare_exchange_weak(seen, -1))
                        return U1Send::Canceled;
            }
            wxMilliSleep(100);
        }
    }
    error = job->error;
    return job->result;
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
