#ifndef slic3r_GUI_AFCPrintDialog_hpp_
#define slic3r_GUI_AFCPrintDialog_hpp_

#include "libslic3r/AFCProtocol.hpp"

#include <string>

#include <boost/filesystem/path.hpp>

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
    // print popup opens next and sends the U1 switches itself. `upload_only`:
    // the file is stored on the printer, not started, so the page does not remap
    // lanes under a running print, and its U1 switches go into the file.
    bool run(bool snapmaker_sends, bool upload_only = false);

    // Called by the page bridge (sw_AFC_FinishPrintMapping).
    static void finish_active(bool proceed, const AFCProtocol::U1PrintOptions &u1_print = {});

    // Reads AFC from the printer and, when it has AFC, runs the mapping step.
    // False only when the user canceled; a printer without AFC passes straight through.
    // `u1_print` gets the U1 switches picked on the page (present only on a U1).
    static bool map_lanes_before_print(wxWindow *parent, bool snapmaker_sends = false,
                                       AFCProtocol::U1PrintOptions *u1_print = nullptr);

    // The mapping step for a file uploaded to the U1 and started later: the picks
    // are set on AFC now and hold until a print ends. False only when the user
    // canceled; `shown` is false when the printer has no AFC. `u1_print` gets the
    // switches picked there.
    static bool map_lanes_for_upload(wxWindow *parent, bool &shown, AFCProtocol::U1PrintOptions &u1_print);

    // A copy of `gcode` that sets the U1 switches before its first command, so they
    // hold however the print is started later. Empty, with `error` set, on failure.
    static boost::filesystem::path gcode_with_print_options(const boost::filesystem::path   &gcode,
                                                            const AFCProtocol::U1PrintOptions &u1_print, std::string &error);

    // Query string that opens Snapmaker's print popup with these switches on
    // (read by the flutter_web patch), empty when there are none.
    static std::string preprint_query(const AFCProtocol::U1PrintOptions &u1_print);

    enum class U1Send {
        Sent,     // the U1 took the print
        NotSent,  // nothing reached the printer, so Snapmaker's popup can still send it
        Failed,   // the start went out and was refused or never confirmed; `error` says which
        Canceled, // the user stopped it before the start went out
    };
    // Sends the sliced plate straight to the connected U1 the way Snapmaker's own
    // print popup does on the LAN: upload to the U1, then start_local_print with these
    // switches and, as its map table, the toolhead of each T(n)'s AFC lane.
    static U1Send send_u1_print(wxWindow *parent, const boost::filesystem::path &gcode,
                                const boost::filesystem::path &upload_path,
                                const AFCProtocol::U1PrintOptions &u1_print, std::string &error);

    // Stores the file on the connected U1 without starting it: the LAN upload, or
    // away from home Snapmaker's cloud with auto_start off. Sent means it is there.
    static U1Send upload_u1_file(wxWindow *parent, const boost::filesystem::path &gcode,
                                 const boost::filesystem::path &upload_path, std::string &error);

private:
    // send_u1_print and upload_u1_file; null `start_options` uploads without starting.
    static U1Send send_to_u1(wxWindow *parent, const boost::filesystem::path &gcode, const boost::filesystem::path &upload_path,
                             const AFCProtocol::U1PrintOptions *start_options, std::string &error);

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
