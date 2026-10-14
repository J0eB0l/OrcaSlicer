#include "AFCManager.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "filamentsync/FilamentSyncAlgorithm.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/MoonRaker.hpp"

#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/log/trivial.hpp>

#include <future>
#include <sstream>

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

namespace {

// One AFC read: the webhook first, Klipper objects as the fallback, then the
// lane_data database to fill blanks.
struct RefreshJob : std::enable_shared_from_this<RefreshJob>
{
    std::shared_ptr<PrintHost>                       host;
    AFCProtocol::Snapshot                            snapshot;
    std::function<void(AFCProtocol::Snapshot)>       done;

    void start()
    {
        auto self = shared_from_this();
        host->async_jsonrpc_call("printer.afc.status", json::object(), [self](const json &res) {
            if (res.is_object() && res.count("data") && AFCProtocol::parse_status_endpoint(res["data"], self->snapshot))
                self->read_lane_data();
            else
                self->read_objects();
        });
    }

    void read_objects()
    {
        auto self = shared_from_this();
        json params = {{"objects", {{"AFC", nullptr}}}};
        host->async_jsonrpc_call("printer.objects.query", params, [self](const json &res) {
            const json *status = nullptr;
            if (res.is_object() && res.count("data") && res["data"].is_object() && res["data"].count("status"))
                status = &res["data"]["status"];
            if (status == nullptr || !status->count("AFC") || !(*status)["AFC"].is_object() || (*status)["AFC"].empty()) {
                self->finish(); // no AFC on this printer
                return;
            }
            self->read_lane_objects((*status)["AFC"]);
        });
    }

    void read_lane_objects(json afc)
    {
        auto self = shared_from_this();
        host->async_jsonrpc_call("printer.objects.list", json::object(), [self, afc](const json &res) {
            std::vector<std::string> lanes;
            if (afc.count("lanes") && afc["lanes"].is_array())
                for (const json &l : afc["lanes"])
                    if (l.is_string())
                        lanes.push_back(l.get<std::string>());
            const json list = res.is_object() && res.count("data") ? res["data"] : json::object();
            json       params;
            params["objects"]["AFC"] = nullptr;
            for (const std::string &name : AFCProtocol::lane_object_names(list, lanes))
                params["objects"][name] = nullptr;
            self->host->async_jsonrpc_call("printer.objects.query", params, [self](const json &res) {
                if (res.is_object() && res.count("data") && res["data"].is_object())
                    AFCProtocol::parse_objects_status(res["data"], self->snapshot);
                self->read_lane_data();
            });
        });
    }

    void read_lane_data()
    {
        auto self = shared_from_this();
        host->async_jsonrpc_call("server.database.get_item", json{{"namespace", "lane_data"}}, [self](const json &res) {
            if (res.is_object() && res.count("data"))
                AFCProtocol::merge_lane_data(res["data"], self->snapshot);
            self->read_dryers();
        });
    }

    // Unit dryers (ACE, AMS 2 Pro, AMS HT) live on the units' own Klipper objects.
    void read_dryers()
    {
        const json objects = AFCProtocol::dryer_objects_query(snapshot);
        if (objects.empty()) {
            finish();
            return;
        }
        auto self = shared_from_this();
        host->async_jsonrpc_call("printer.objects.query", json{{"objects", objects}}, [self](const json &res) {
            if (res.is_object() && res.count("data") && res["data"].is_object())
                AFCProtocol::merge_dryer_status(res["data"], self->snapshot);
            self->finish();
        });
    }

    void finish()
    {
        if (done) {
            auto cb = std::move(done);
            done    = nullptr;
            cb(std::move(snapshot));
        }
    }
};

// How long a spool's flow K is trusted before it is read again. AFC autocal
// only writes it after a calibration, so this mostly bounds how soon a new one shows.
constexpr std::chrono::seconds k_flow_k_max_age{120};

// Signature of the slots the slicer sees; a change means the combo boxes need rebuilding.
std::string slots_key(const AFCProtocol::Snapshot &s)
{
    std::string key;
    for (const AFCProtocol::ToolSlot &slot : AFCProtocol::tool_slots(s))
        key += std::to_string(slot.tool) + slot.lane + slot.vendor + slot.type + slot.sub_type + slot.color + "|";
    return key;
}

double ciede2000(const std::string &a, const std::string &b)
{
    const std::string ca = AFCProtocol::normalize_color(a), cb = AFCProtocol::normalize_color(b);
    if (ca.size() != 7 || cb.size() != 7)
        return 1000.0;
    auto byte = [](const std::string &c, size_t at) { return uint8_t(std::stoi(c.substr(at, 2), nullptr, 16)); };
    return delta_e_ciede2000(byte(ca, 1), byte(ca, 3), byte(ca, 5), byte(cb, 1), byte(cb, 3), byte(cb, 5));
}

// One slot per toolhead from the printer settings, for printers whose device
// page does not report toolheads (anything but the U1).
std::vector<ConnectMachineInfo> preset_toolheads(const PresetBundle &bundle, const AFCProtocol::Snapshot &snap)
{
    const DynamicPrintConfig &cfg     = bundle.printers.get_edited_preset().config;
    const auto               *nozzles = cfg.option<ConfigOptionFloats>("nozzle_diameter");
    size_t                    count   = nozzles ? nozzles->values.size() : 0;
    count                             = std::max(count, snap.extruders.size());
    for (const AFCProtocol::Lane &lane : snap.lanes)
        if (lane.extruder_index >= 0)
            count = std::max(count, size_t(lane.extruder_index) + 1);
    std::vector<ConnectMachineInfo> heads;
    for (size_t i = 0; i < count; ++i) {
        ConnectMachineInfo head;
        head.index = int(i);
        if (nozzles && !nozzles->values.empty()) {
            std::ostringstream diameter;
            diameter << nozzles->get_at(i);
            head.nozzle_info = diameter.str();
        }
        heads.push_back(std::move(head));
    }
    return heads;
}

} // namespace

AFCManager &AFCManager::instance()
{
    static AFCManager manager;
    return manager;
}

std::shared_ptr<PrintHost> AFCManager::afc_host()
{
    std::shared_ptr<PrintHost> host;
    wxGetApp().get_connect_host(host);
    if (host || !wxGetApp().preset_bundle)
        return host;

    // Any other Klipper printer: Moonraker at the address in its printer settings.
    // "Octo/Klipper" hosts are usually Moonraker too; a real OctoPrint just answers 404.
    const DynamicPrintConfig &cfg  = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto               *type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
    if (type == nullptr || (type->value != htMoonRaker && type->value != htOctoPrint) || !cfg.has("print_host") ||
        cfg.opt_string("print_host").empty())
        return nullptr;

    static std::shared_ptr<PrintHost> s_host;
    static std::string                s_key;
    const std::string key = cfg.opt_string("print_host") + "\n" + cfg.opt_string("printhost_apikey") + "\n" +
                            cfg.opt_string("printhost_cafile");
    if (!s_host || s_key != key) {
        DynamicPrintConfig moonraker = cfg;
        s_host = std::make_shared<Moonraker>(&moonraker);
        s_key  = key;
    }
    return s_host;
}

std::string AFCManager::host_key(const std::shared_ptr<PrintHost> &host)
{
    if (!host)
        return {};
    const std::string sn = host->get_sn();
    return sn.empty() ? host->get_host() : sn;
}

void AFCManager::refresh_async(const std::shared_ptr<PrintHost> &host, Callback done)
{
    if (!host) {
        if (done)
            done(false);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing = true;
    }
    const std::string sn  = host_key(host);
    auto              job = std::make_shared<RefreshJob>();
    job->host             = host;
    job->done             = [this, host, sn, done](AFCProtocol::Snapshot snapshot) {
        const bool present = snapshot.present;
        finish_refresh(std::move(snapshot), sn);
        read_flow_k(host, sn);
        if (done)
            done(present);
    };
    job->start();
}

void AFCManager::finish_refresh(AFCProtocol::Snapshot snapshot, const std::string &sn)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_sn != sn) {
        m_flow_k.clear();
        m_flow_k_reading.clear();
    }
    if (snapshot.present || m_sn != sn || !m_snapshot.present)
        m_snapshot = std::move(snapshot);
    else
        // A failed read on the same printer keeps the last good snapshot,
        // so one MQTT hiccup does not collapse the slots back to four.
        BOOST_LOG_TRIVIAL(warning) << "AFC: refresh failed, keeping the previous snapshot";
    m_sn         = sn;
    m_updated    = std::chrono::steady_clock::now();
    m_refreshing = false;
}

void AFCManager::read_flow_k(const std::shared_ptr<PrintHost> &host, const std::string &sn)
{
    std::vector<int> spools;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sn != sn || !m_snapshot.present)
            return;
        const auto now = std::chrono::steady_clock::now();
        for (const AFCProtocol::Lane &lane : m_snapshot.lanes) {
            if (lane.spool_id <= 0 || m_flow_k_reading.count(lane.spool_id))
                continue;
            auto known = m_flow_k.find(lane.spool_id);
            if (known != m_flow_k.end() && now - known->second.read_at < k_flow_k_max_age)
                continue;
            m_flow_k_reading.insert(lane.spool_id);
            spools.push_back(lane.spool_id);
        }
    }
    for (const int spool : spools) {
        const json params = {{"request_method", "GET"}, {"path", "/v1/spool/" + std::to_string(spool)}, {"use_v2_response", true}};
        host->async_jsonrpc_call("server.spoolman.proxy", params, [this, sn, spool](const json &res) {
            // A failed read counts as "no K" until it is due again.
            const double k = res.is_object() && res.count("data") ? AFCProtocol::spool_flow_k(res["data"]) : -1;
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_sn != sn)
                return;
            m_flow_k_reading.erase(spool);
            m_flow_k[spool] = FlowK{k, std::chrono::steady_clock::now()};
        });
    }
}

bool AFCManager::refresh_blocking(const std::shared_ptr<PrintHost> &host, int timeout_ms)
{
    if (!host)
        return false;
    auto promise = std::make_shared<std::promise<bool>>();
    auto future  = promise->get_future();
    auto fired   = std::make_shared<std::once_flag>();
    refresh_async(host, [promise, fired](bool present) { std::call_once(*fired, [&] { promise->set_value(present); }); });
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        BOOST_LOG_TRIVIAL(warning) << "AFC: refresh timed out after " << timeout_ms << " ms";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing = false;
        return m_snapshot.present && m_sn == host_key(host);
    }
    return future.get();
}

void AFCManager::request_refresh(const std::shared_ptr<PrintHost> &host, std::chrono::seconds max_age)
{
    if (!host)
        return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_refreshing)
            return;
        if (m_sn == host_key(host) && std::chrono::steady_clock::now() - m_updated < max_age)
            return;
    }
    refresh_async(host, [](bool) {
        wxGetApp().CallAfter([] {
            AFCManager   &self   = AFCManager::instance();
            PresetBundle *bundle = wxGetApp().preset_bundle;
            if (!bundle)
                return;
            std::shared_ptr<PrintHost> device;
            wxGetApp().get_connect_host(device);
            // The U1's device page pushes its toolheads first; until it has, leave its slots alone.
            if (device && self.m_toolheads.empty())
                return;
            const std::string before = self.m_applied_key;
            if (self.reapply_machine_filaments(*bundle)) {
                if (self.m_applied_key != before)
                    wxGetApp().load_current_presets();
            } else if (!device && self.drop_machine_filaments(*bundle)) {
                // AFC gone from a printer with no device page of its own: no machine filaments left.
                wxGetApp().load_current_presets();
            }
        });
    });
}

AFCProtocol::Snapshot AFCManager::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    AFCProtocol::Snapshot out = m_snapshot;
    for (AFCProtocol::Lane &lane : out.lanes) {
        auto known = m_flow_k.find(lane.spool_id);
        if (lane.spool_id > 0 && known != m_flow_k.end())
            lane.flow_k = known->second.k;
    }
    return out;
}

bool AFCManager::has_afc() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_snapshot.present && !m_snapshot.lanes.empty();
}

std::string AFCManager::printer_sn() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sn;
}

void AFCManager::clear()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_snapshot = AFCProtocol::Snapshot();
    m_sn.clear();
    m_updated     = {};
    m_applied_key.clear();
    m_toolheads.clear();
    m_flow_k.clear();
    m_flow_k_reading.clear();
}

bool AFCManager::apply_machine_filaments(PresetBundle &bundle, const std::vector<ConnectMachineInfo> &toolheads)
{
    m_toolheads = toolheads;
    return reapply_machine_filaments(bundle);
}

bool AFCManager::reapply_machine_filaments(PresetBundle &bundle)
{
    const std::shared_ptr<PrintHost> host = afc_host();
    const AFCProtocol::Snapshot      snap = snapshot();
    if (!host || !snap.present || snap.lanes.empty() || printer_sn() != host_key(host))
        return false;

    const std::vector<AFCProtocol::ToolSlot> slots = AFCProtocol::tool_slots(snap);
    if (slots.empty())
        return false;
    // First AFC apply without a device-page push: the bundle still holds the toolhead slots.
    if (m_toolheads.empty() && m_applied_key.empty())
        m_toolheads = bundle.m_connect_machine_info_list;
    // No device page reporting toolheads (any printer but the U1): take them from the printer settings.
    if (m_toolheads.empty())
        m_toolheads = preset_toolheads(bundle, snap);

    std::vector<ConnectMachineInfo>                    infos;
    std::map<int, std::pair<std::string, std::string>> filaments;
    for (const AFCProtocol::ToolSlot &slot : slots) {
        ConnectMachineInfo info;
        info.index         = slot.tool;
        info.filament_type = slot.type;
        info.filament_info = AFCProtocol::filament_display_name(slot.vendor, slot.type, slot.sub_type);
        info.color_info    = slot.color;
        info.multiColors   = slot.colors;
        info.colorMode     = FilamentColorMode::Segment;
        // Nozzle of the toolhead the lane feeds; the device page reports one per toolhead.
        const int head = slot.extruder_index >= 0 && size_t(slot.extruder_index) < m_toolheads.size() ? slot.extruder_index : 0;
        if (!m_toolheads.empty()) {
            info.nozzle_info        = m_toolheads[head].nozzle_info;
            info.nozzle_volume_type = m_toolheads[head].nozzle_volume_type;
        }
        filaments.insert({slot.tool, {info.filament_info, slot.color}});
        infos.push_back(std::move(info));
    }
    bundle.m_connect_machine_info_list = std::move(infos);
    bundle.machine_filaments           = std::move(filaments);
    m_applied_key                      = slots_key(snap);
    return true;
}

bool AFCManager::drop_machine_filaments(PresetBundle &bundle)
{
    if (m_applied_key.empty())
        return false;
    bundle.machine_filaments.clear();
    bundle.m_connect_machine_info_list.clear();
    m_applied_key.clear();
    m_toolheads.clear();
    return true;
}

std::vector<AFCProtocol::ProjectFilament> AFCManager::project_filaments(json *details) const
{
    std::vector<AFCProtocol::ProjectFilament> out;
    Plater *plater = wxGetApp().plater();
    if (!plater || !wxGetApp().preset_bundle)
        return out;
    PartPlate *plate = plater->get_partplate_list().get_curr_plate();
    if (!plate || !plate->fff_print())
        return out;

    const DynamicPrintConfig &project = wxGetApp().preset_bundle->project_config;
    const DynamicPrintConfig  full    = wxGetApp().preset_bundle->full_config();
    const auto *colours  = project.option<ConfigOptionStrings>("filament_colour");
    const auto *types    = full.option<ConfigOptionStrings>("filament_type");
    const auto *density  = full.option<ConfigOptionFloats>("filament_density");
    const size_t count   = colours ? colours->values.size() : 0;

    const GCodeProcessorResult *result  = plate->get_slice_result();
    const bool                  sliced  = result != nullptr && plate->is_slice_result_valid();
    std::map<size_t, double>    volumes;
    if (sliced)
        volumes = result->print_statistics.total_volumes_per_extruder;

    const std::vector<std::string> &presets = wxGetApp().preset_bundle->filament_presets;
    if (details)
        *details = json::array();
    for (size_t i = 0; i < count; ++i) {
        AFCProtocol::ProjectFilament f;
        f.id    = int(i);
        f.color = AFCProtocol::normalize_color(colours->values[i]);
        f.type  = types ? types->get_at(int(i)) : std::string();
        auto v  = volumes.find(i);
        f.used  = !sliced || (v != volumes.end() && v->second > 0);
        if (v != volumes.end() && density)
            f.grams = density->get_at(int(i)) * v->second * 0.001;
        out.push_back(f);
        if (details)
            details->push_back({{"id", f.id},
                                {"color", f.color},
                                {"type", f.type},
                                {"grams", f.grams},
                                {"used", f.used},
                                {"preset", i < presets.size() ? presets[i] : std::string()}});
    }
    return out;
}

json AFCManager::page_state(bool include_project) const
{
    const std::shared_ptr<PrintHost> host = afc_host();

    const AFCProtocol::Snapshot snap = snapshot();
    json state;
    state["connected"] = host != nullptr;
    state["afc"]       = AFCProtocol::to_json(snap);
    if (!host || printer_sn() != host_key(host))
        state["afc"]["present"] = false;

    if (include_project) {
        json details;
        const auto filaments = project_filaments(&details);
        Plater    *plater    = wxGetApp().plater();
        PartPlate *plate     = plater ? plater->get_partplate_list().get_curr_plate() : nullptr;
        state["project"]["filaments"]  = details;
        state["project"]["sliced"]     = plate != nullptr && plate->is_slice_result_valid();
        state["project"]["suggestion"] = AFCProtocol::suggest_mapping(snap, filaments, ciede2000);
    }
    return state;
}

void AFCManager::send_gcode(const std::shared_ptr<PrintHost> &host, const std::vector<std::string> &gcode,
                            std::function<void(const std::string &error)> done)
{
    if (!host) {
        done("The printer is not connected");
        return;
    }
    if (gcode.empty()) {
        done("");
        return;
    }
    std::string script;
    for (const std::string &line : gcode)
        script += (script.empty() ? "" : "\n") + line;
    host->async_jsonrpc_call("printer.gcode.script", json{{"script", script}}, [done](const json &res) {
        if (res.is_null()) {
            done("The printer did not take the command");
        } else if (res.is_object() && res.count("error")) {
            const json &err = res["error"];
            if (err.is_string())
                done(err.get<std::string>());
            else if (err.is_object() && err.contains("message") && err["message"].is_string())
                done(err["message"].get<std::string>());
            else
                done(err.dump());
        } else {
            done("");
        }
    });
}

} // namespace GUI
} // namespace Slic3r
