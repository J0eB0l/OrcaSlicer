#ifndef slic3r_GUI_AFCManager_hpp_
#define slic3r_GUI_AFCManager_hpp_

#include "libslic3r/AFCProtocol.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class PrintHost;

namespace GUI {

// Live view of AFC (units, lanes, T(n) map) on the connected printer.
//
// A printer running AFC has as many filaments as AFC has T(n) maps, not one
// per toolhead. The manager reads AFC through the printer connection (the
// U1's Moonraker JSON-RPC over MQTT, or any Klipper printer's Moonraker URL
// from the printer settings), keeps the last snapshot, and turns it into the
// per-slot machine filament list the filament sync, the filament combo boxes
// and the AFC pages use.
class AFCManager
{
public:
    static AFCManager &instance();

    // The printer connection AFC is read through: the connected device (U1) while
    // its preset is selected, else a Moonraker host built from the selected printer's settings.
    // nullptr when neither exists. UI thread only.
    static std::shared_ptr<PrintHost> afc_host();
    // The connected device (U1) while its own printer preset is selected, else nullptr.
    static std::shared_ptr<PrintHost> device_host();
    // Identifies the printer behind a host: its serial number, else its address.
    static std::string host_key(const std::shared_ptr<PrintHost> &host);

    using Callback = std::function<void(bool afc_present)>;

    // Read AFC from the connected printer. `done` runs on the network thread.
    void refresh_async(const std::shared_ptr<PrintHost> &host, Callback done = nullptr);

    // Same, waiting at most `timeout_ms`, flow K reads of the linked spools
    // included. Safe on the UI thread: replies arrive on the MQTT thread.
    // Returns whether AFC is present.
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

    // Undo apply_machine_filaments for a printer that has no device page of its
    // own (the AFC was the only source of its machine filaments). UI thread only.
    bool drop_machine_filaments(PresetBundle &bundle);

    // The filament preset for the spool on T(tool), for a filament sync. A
    // compatible preset named after the spool's vendor and filament name wins.
    // Otherwise one is made from `base` (the closest preset of the spool's
    // material) with the spool's temperatures, density and diameter, saved as
    // "<spool> @<printer>", and later syncs keep its values in step with the
    // spool. `name` is empty when the spool has no vendor to go by or nothing
    // could be made. UI thread only.
    struct SpoolPreset
    {
        std::string name;
        bool        created{false};
        bool        updated{false};
    };
    SpoolPreset spool_filament_preset(PresetBundle &bundle, int tool, const std::string &base);

    // Put the filament preset a sync picked into the user's filament list, as
    // ticking it there would. A hidden preset (Orca's library ones, say) has no
    // row in the slot's combo, and Orca swaps it for the first compatible preset
    // on the next load. True when it was hidden. UI thread only.
    bool show_filament_preset(PresetBundle &bundle, const std::string &name);

    // Project filaments of the current plate, with grams used and whether the
    // sliced plate uses them. UI thread only.
    std::vector<AFCProtocol::ProjectFilament> project_filaments(nlohmann::json *details = nullptr) const;

    // Gives every filament slot the project does not know the toolhead of yet (not synced
    // from AFC) the toolhead of the lane AFC maps its T(n) to now, when the selected printer
    // runs AFC. It sticks with the project like a synced one, so later remaps do not
    // invalidate the slicing (the print popup recomputes PURGE_LENGTH for them). A printer
    // without AFC keeps every slot unknown. UI thread only.
    void fill_filament_toolheads(PresetBundle &bundle) const;

    // The toolhead of the lane AFC maps each T(n) the current plate uses to now, -1 for
    // the rest (and all of them when AFC has not answered). UI thread only.
    std::vector<int> mapped_toolheads() const;

    // Everything the AFC page shows: {"afc": snapshot, "project": {...}, "connected": bool}.
    // UI thread only (reads the plater).
    nlohmann::json page_state(bool include_project) const;

    // Send G-code over the printer connection; `done` gets an error string ("" = ok).
    static void send_gcode(const std::shared_ptr<PrintHost> &host, const std::vector<std::string> &gcode,
                           std::function<void(const std::string &error)> done);

private:
    AFCManager() = default;

    void finish_refresh(AFCProtocol::Snapshot snapshot, const std::string &sn);
    // Read the flow K of every linked spool not read lately, through Moonraker's
    // Spoolman proxy. Lanes pick it up on the next snapshot().
    void read_flow_k(const std::shared_ptr<PrintHost> &host, const std::string &sn);

    struct FlowK
    {
        double                                k{-1};   // -1: the spool has none
        std::chrono::steady_clock::time_point read_at{};
    };

    mutable std::mutex                      m_mutex;
    AFCProtocol::Snapshot                   m_snapshot;
    std::string                             m_sn;
    std::chrono::steady_clock::time_point   m_updated{};
    std::set<std::string>                   m_refreshing;     // printers (host_key) with a read on the way
    std::vector<ConnectMachineInfo>         m_toolheads;      // UI thread
    std::string                             m_toolheads_key;  // UI thread: printer (host_key) m_toolheads came from
    std::string                             m_applied_key;    // UI thread: slots last applied
    std::map<int, FlowK>                    m_flow_k;         // by Spoolman spool id, for the printer in m_sn
    std::set<int>                           m_flow_k_reading; // spool ids with a read on the way
    std::condition_variable                 m_flow_k_read;    // a flow K read finished
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AFCManager_hpp_
