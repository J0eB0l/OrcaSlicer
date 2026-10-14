#ifndef slic3r_GUI_AFCManager_hpp_
#define slic3r_GUI_AFCManager_hpp_

#include "libslic3r/AFCProtocol.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

class PrintHost;

namespace GUI {

// Live view of AFC (units, lanes, T(n) map) on the connected printer.
//
// A U1 running AFC has as many filaments as AFC has T(n) maps, not four.
// The manager reads AFC through the printer connection the device page
// already made (Moonraker JSON-RPC over MQTT), keeps the last snapshot,
// and turns it into the per-slot machine filament list the filament sync,
// the filament combo boxes and the AFC pages use.
class AFCManager
{
public:
    static AFCManager &instance();

    using Callback = std::function<void(bool afc_present)>;

    // Read AFC from the connected printer. `done` runs on the network thread.
    void refresh_async(const std::shared_ptr<PrintHost> &host, Callback done = nullptr);

    // Same, waiting at most `timeout_ms`. Safe on the UI thread: replies
    // arrive on the MQTT thread. Returns whether AFC is present.
    bool refresh_blocking(const std::shared_ptr<PrintHost> &host, int timeout_ms = 4000);

    // Refresh in the background unless the snapshot is younger than `max_age`.
    // When the T(n) slots change, the machine filament list is rebuilt on the
    // UI thread.
    void request_refresh(const std::shared_ptr<PrintHost> &host, std::chrono::seconds max_age = std::chrono::seconds(10));

    AFCProtocol::Snapshot snapshot() const;
    bool                  has_afc() const;
    std::string           printer_sn() const;
    void                  clear();

    // Replace the toolhead slots the device page reported (one per U1 tool)
    // with one slot per AFC T(n). `toolheads` keeps the nozzle info per
    // physical toolhead. Returns false (and leaves the bundle alone) when the
    // connected printer has no AFC. UI thread only.
    bool apply_machine_filaments(PresetBundle &bundle, const std::vector<ConnectMachineInfo> &toolheads);

    // Re-apply the last snapshot using the toolheads from the last apply. UI thread only.
    bool reapply_machine_filaments(PresetBundle &bundle);

    // Project filaments of the current plate, with grams used and whether the
    // sliced plate uses them. UI thread only.
    std::vector<AFCProtocol::ProjectFilament> project_filaments(nlohmann::json *details = nullptr) const;

    // Everything the AFC page shows: {"afc": snapshot, "project": {...}, "connected": bool}.
    // UI thread only (reads the plater).
    nlohmann::json page_state(bool include_project) const;

    // Send G-code over the printer connection; `done` gets an error string ("" = ok).
    static void send_gcode(const std::shared_ptr<PrintHost> &host, const std::vector<std::string> &gcode,
                           std::function<void(const std::string &error)> done);

private:
    AFCManager() = default;

    void finish_refresh(AFCProtocol::Snapshot snapshot, const std::string &sn);

    mutable std::mutex                      m_mutex;
    AFCProtocol::Snapshot                   m_snapshot;
    std::string                             m_sn;
    std::chrono::steady_clock::time_point   m_updated{};
    bool                                    m_refreshing{false};
    std::vector<ConnectMachineInfo>         m_toolheads;      // UI thread
    std::string                             m_applied_key;    // UI thread: slots last applied
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AFCManager_hpp_
