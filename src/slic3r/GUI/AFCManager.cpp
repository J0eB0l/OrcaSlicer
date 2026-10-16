#include "AFCManager.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "filamentsync/FilamentSyncAlgorithm.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/MoonRaker.hpp"

#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <future>
#include <sstream>
#include <thread>

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
                self->read_afc_object();
            else
                self->read_objects();
        });
    }

    // The webhook leaves out some settings, multiple tool mapping among them.
    void read_afc_object()
    {
        auto       self   = shared_from_this();
        const json params = {{"objects", AFCProtocol::afc_object_query()}};
        host->async_jsonrpc_call("printer.objects.query", params, [self](const json &res) {
            if (res.is_object() && res.count("data") && res["data"].is_object())
                AFCProtocol::merge_afc_object(res["data"], self->snapshot);
            self->read_lane_data();
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
            params["objects"]["print_task_config"] = AFCProtocol::afc_object_query()["print_task_config"];
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

// First line of the notes of a filament preset a sync made for a spool. A later
// sync refreshes only presets that still carry it.
const std::string SPOOL_PRESET_NOTE = "Made by AFC filament sync from the spool's data. Each sync puts the spool's "
                                      "temperatures, density and diameter back in; delete this line to keep your own.";

std::string lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// "Polymaker PolyLite PLA @Voron 2.4" -> "polymaker polylite pla"
std::string preset_base_key(const std::string &name)
{
    return lower_ascii(boost::trim_copy(name.substr(0, name.find('@'))));
}

bool made_by_sync(const Preset &preset)
{
    const auto *notes = preset.config.option<ConfigOptionStrings>("filament_notes");
    return preset.is_user() && notes != nullptr && !notes->values.empty() &&
           notes->values.front().compare(0, SPOOL_PRESET_NOTE.size(), SPOOL_PRESET_NOTE) == 0;
}

template<class Option, class Value> bool set_every_value(Option *option, const Value &value, bool keep_zero = false)
{
    bool changed = false;
    if (option != nullptr)
        for (auto &v : option->values)
            if (v != value && !(keep_zero && v == Value())) {
                v       = value;
                changed = true;
            }
    return changed;
}

// Put the spool's temperatures, density and diameter into a filament config.
// A bed plate the preset rules out (0) stays ruled out. Returns whether anything changed.
bool apply_spool_values(const AFCProtocol::Lane &lane, DynamicPrintConfig &config)
{
    bool changed = false;
    if (lane.nozzle_temp > 0) {
        for (const char *key : {"nozzle_temperature", "nozzle_temperature_initial_layer"})
            changed |= set_every_value(config.option<ConfigOptionInts>(key), lane.nozzle_temp);
        // Widen the printable range to take the spool's temperature, so no warning flags it.
        if (auto *low = config.option<ConfigOptionInts>("nozzle_temperature_range_low"))
            for (int &v : low->values)
                if (v > lane.nozzle_temp) {
                    v       = lane.nozzle_temp;
                    changed = true;
                }
        if (auto *high = config.option<ConfigOptionInts>("nozzle_temperature_range_high"))
            for (int &v : high->values)
                if (v < lane.nozzle_temp) {
                    v       = lane.nozzle_temp;
                    changed = true;
                }
    }
    if (lane.bed_temp > 0)
        for (const char *plate : {"supertack_plate_temp", "cool_plate_temp", "textured_cool_plate_temp", "eng_plate_temp",
                                  "hot_plate_temp", "textured_plate_temp", "graphic_effect_plate_temp"}) {
            changed |= set_every_value(config.option<ConfigOptionInts>(plate), lane.bed_temp, true);
            changed |= set_every_value(config.option<ConfigOptionInts>(std::string(plate) + "_initial_layer"), lane.bed_temp, true);
        }
    if (lane.density > 0)
        changed |= set_every_value(config.option<ConfigOptionFloats>("filament_density"), lane.density);
    if (lane.diameter > 0)
        changed |= set_every_value(config.option<ConfigOptionFloats>("filament_diameter"), lane.diameter);
    // The spool's saved pressure advance, so the preset shows what the slot prints with.
    // A slot still prints with its own spool's K when two spools share the preset.
    if (lane.flow_k >= 0) {
        changed |= set_every_value(config.option<ConfigOptionBools>("enable_pressure_advance"), static_cast<unsigned char>(1));
        changed |= set_every_value(config.option<ConfigOptionFloats>("pressure_advance"), lane.flow_k);
    }
    return changed;
}

// Filament id for the presets made for one spool: every printer's copy shares
// it, as with Orca's own user filaments, and no other filament has it.
std::string spool_filament_id(const PresetCollection &filaments, const std::string &spool_name)
{
    const std::string key = lower_ascii(spool_name);
    for (auto it = filaments.cbegin(); it != filaments.cend(); ++it)
        if (!it->filament_id.empty() && it->filament_id != "null" && preset_base_key(it->name) == key)
            return it->filament_id;
    for (unsigned salt = 0; salt < 1000; ++salt) {
        uint32_t hash = 2166136261u; // FNV-1a
        for (unsigned char c : key + (salt > 0 ? "#" + std::to_string(salt) : std::string())) {
            hash ^= c;
            hash *= 16777619u;
        }
        char id[16];
        std::snprintf(id, sizeof(id), "P%07x", unsigned(hash >> 4));
        if (std::none_of(filaments.cbegin(), filaments.cend(), [&id](const Preset &p) { return p.filament_id == id; }))
            return id;
    }
    return {};
}

} // namespace

AFCManager &AFCManager::instance()
{
    static AFCManager manager;
    return manager;
}

std::shared_ptr<PrintHost> AFCManager::device_host()
{
    if (!wxGetApp().preset_bundle)
        return nullptr;
    // The U1 stays connected when another printer's preset is picked. Its device
    // page goes with its own preset, and so does reading AFC through it.
    const auto *model = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionString>("printer_model");
    if (model == nullptr || !boost::icontains(model->value, "Snapmaker") || !boost::icontains(model->value, "U1"))
        return nullptr;
    std::shared_ptr<PrintHost> host;
    wxGetApp().get_connect_host(host);
    return host;
}

std::shared_ptr<PrintHost> AFCManager::afc_host()
{
    if (std::shared_ptr<PrintHost> host = device_host())
        return host;
    if (!wxGetApp().preset_bundle)
        return nullptr;

    // Any other Klipper printer: Moonraker at the address in its printer settings.
    // "Octo/Klipper" hosts are usually Moonraker too; a real OctoPrint just answers 404.
    const DynamicPrintConfig &cfg  = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const auto               *type = cfg.option<ConfigOptionEnum<PrintHostType>>("host_type");
    if (type == nullptr || (type->value != htMoonRaker && type->value != htOctoPrint) || !cfg.has("print_host") ||
        cfg.opt_string("print_host").empty())
        return nullptr;

    // Not s_host: winsock defines s_host as a macro.
    static std::shared_ptr<PrintHost> s_moonraker;
    static std::string                s_moonraker_key;
    const std::string key = cfg.opt_string("print_host") + "\n" + cfg.opt_string("printhost_apikey") + "\n" +
                            cfg.opt_string("printhost_cafile");
    if (!s_moonraker || s_moonraker_key != key) {
        DynamicPrintConfig moonraker = cfg;
        s_moonraker     = std::make_shared<Moonraker>(&moonraker);
        s_moonraker_key = key;
    }
    return s_moonraker;
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
    const std::string sn = host_key(host);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing.insert(sn);
    }
    auto job  = std::make_shared<RefreshJob>();
    job->host = host;
    job->done = [this, host, sn, done](AFCProtocol::Snapshot snapshot) {
        const bool present = snapshot.present;
        finish_refresh(std::move(snapshot), sn);
        read_flow_k(host, sn);
        if (done)
            done(present);
    };
    // Off the UI thread: an MQTT host (the U1) publishes synchronously and can stall
    // for seconds on a slow connection.
    std::thread([job]() { job->start(); }).detach();
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
    m_sn      = sn;
    m_updated = std::chrono::steady_clock::now();
    m_refreshing.erase(sn);
}

void AFCManager::read_flow_k(const std::shared_ptr<PrintHost> &host, const std::string &sn)
{
    std::vector<int> spools;
    std::string      spoolman_url;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_sn != sn || !m_snapshot.present)
            return;
        spoolman_url = m_snapshot.spoolman_url;
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
    // A failed read counts as "no K" until it is due again.
    auto store = [this, sn](int spool, double k) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_sn != sn)
                return;
            m_flow_k_reading.erase(spool);
            m_flow_k[spool] = FlowK{k, std::chrono::steady_clock::now()};
        }
        m_flow_k_read.notify_all();
    };
    for (const int spool : spools) {
        // Moonraker's Spoolman proxy and AFC's own Spoolman address are asked together and
        // the first answer wins. A U1 that leaves the proxy unanswered over MQTT holds the
        // call for the link's 80 s timeout, long after a sync has gone ahead without the K.
        struct Race
        {
            std::mutex mutex;
            int        pending = 0;
            bool       done    = false;
        };
        auto race     = std::make_shared<Race>();
        race->pending = spoolman_url.empty() ? 1 : 2;
        auto answer   = [store, spool, race](double k, bool ok) {
            {
                std::lock_guard<std::mutex> lock(race->mutex);
                if (race->done)
                    return;
                --race->pending;
                if (!ok && race->pending > 0)
                    return;
                race->done = true;
            }
            store(spool, ok ? k : -1);
        };
        const json params = {{"request_method", "GET"}, {"path", "/v1/spool/" + std::to_string(spool)}, {"use_v2_response", true}};
        host->async_jsonrpc_call("server.spoolman.proxy", params, [answer, spool](const json &res) {
            if (res.is_object() && res.count("data") && res["data"].is_object()) {
                const json &data = res["data"];
                // use_v2_response: a failed Spoolman call still answers, with its error inside.
                if (!(data.contains("error") && !data["error"].is_null())) {
                    answer(AFCProtocol::spool_flow_k(data), true);
                    return;
                }
            }
            BOOST_LOG_TRIVIAL(warning) << "AFC: Moonraker could not read Spoolman spool " << spool << ": "
                                       << (res.is_object() && res.count("error") ? res["error"].dump() :
                                           res.is_object() && res.count("data") ? res["data"].value("error", json()).dump() : res.dump());
            answer(-1, false);
        });
        if (spoolman_url.empty())
            continue;
        std::string base = spoolman_url;
        while (!base.empty() && base.back() == '/')
            base.pop_back();
        Http::get(base + "/api/v1/spool/" + std::to_string(spool))
            .timeout_connect(3)
            .timeout_max(6)
            .on_complete([answer](std::string body, unsigned) {
                const json spool_json = json::parse(body, nullptr, false);
                answer(spool_json.is_object() ? AFCProtocol::spool_flow_k(spool_json) : -1, spool_json.is_object());
            })
            .on_error([answer, spool, base](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "AFC: Spoolman at " << base << " could not be read for spool " << spool << ": "
                                           << error << " (" << status << ")";
                answer(-1, false);
            })
            .perform();
    }
}

bool AFCManager::refresh_blocking(const std::shared_ptr<PrintHost> &host, int timeout_ms)
{
    if (!host)
        return false;
    const std::string sn       = host_key(host);
    const auto        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto       promise  = std::make_shared<std::promise<bool>>();
    auto       future   = promise->get_future();
    auto       fired    = std::make_shared<std::once_flag>();
    refresh_async(host, [promise, fired](bool present) { std::call_once(*fired, [&] { promise->set_value(present); }); });
    if (future.wait_until(deadline) != std::future_status::ready) {
        BOOST_LOG_TRIVIAL(warning) << "AFC: refresh timed out after " << timeout_ms << " ms";
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing.erase(sn);
        return m_snapshot.present && m_sn == sn;
    }
    future.get();
    // The sync puts each spool's flow K into its slot, so let those reads land too. They
    // start only once AFC is read, and a printer far away over MQTT can take most of the
    // time for that, so they get a budget of their own.
    std::unique_lock<std::mutex> lock(m_mutex);
    const auto flow_k_deadline = std::max(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
    if (!m_flow_k_read.wait_until(lock, flow_k_deadline, [this] { return m_flow_k_reading.empty(); }))
        BOOST_LOG_TRIVIAL(warning) << "AFC: flow K reads still running after " << 2 * timeout_ms << " ms";
    // A read that failed keeps this printer's last good snapshot; the sync uses that too.
    return m_snapshot.present && m_sn == sn;
}

void AFCManager::request_refresh(const std::shared_ptr<PrintHost> &host, std::chrono::seconds max_age)
{
    if (!host)
        return;
    {
        const std::string           sn = host_key(host);
        std::lock_guard<std::mutex> lock(m_mutex);
        // Only this printer's own read holds it back: a read still waiting on the
        // printer switched away from must not keep the new one's panel empty.
        if (m_refreshing.count(sn))
            return;
        if (m_sn == sn && std::chrono::steady_clock::now() - m_updated < max_age)
            return;
    }
    refresh_async(host, [](bool) {
        wxGetApp().CallAfter([] {
            AFCManager   &self   = AFCManager::instance();
            PresetBundle *bundle = wxGetApp().preset_bundle;
            if (!bundle)
                return;
            const std::shared_ptr<PrintHost> device = device_host();
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
    // A read still waiting on the old connection must not hold back the next one.
    m_refreshing.clear();
    m_applied_key.clear();
    m_toolheads.clear();
    m_toolheads_key.clear();
    m_flow_k.clear();
    m_flow_k_reading.clear();
}

bool AFCManager::apply_machine_filaments(PresetBundle &bundle, const std::vector<ConnectMachineInfo> &toolheads)
{
    m_toolheads     = toolheads;
    m_toolheads_key = host_key(device_host());
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
    // Toolheads belong to the printer they came from: another printer starts from its own.
    if (m_toolheads_key != host_key(host)) {
        m_toolheads.clear();
        m_toolheads_key = host_key(host);
    }
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
    m_toolheads_key.clear();
    return true;
}

AFCManager::SpoolPreset AFCManager::spool_filament_preset(PresetBundle &bundle, int tool, const std::string &base)
{
    SpoolPreset                 out;
    const AFCProtocol::Snapshot snap = snapshot();
    const AFCProtocol::Lane    *lane = snap.lane_for_tool(tool);
    if (lane == nullptr || !lane->has_filament())
        return out;
    std::vector<std::string> aliases = AFCProtocol::spool_preset_aliases(*lane);
    // No vendor (no Spoolman spool): a preset with what AFC holds for the lane, not the generic one.
    if (aliases.empty())
        if (const std::string afc_name = AFCProtocol::afc_lane_preset_name(*lane); !afc_name.empty())
            aliases.push_back(afc_name);
    if (aliases.empty())
        return out;
    PresetCollection &filaments = bundle.filaments;

    // The user's presets before the system ones, so a preset made for the spool
    // keeps winning over a system preset of that name arriving later.
    Preset *found = nullptr;
    for (size_t a = 0; a < aliases.size() && found == nullptr; ++a) {
        const std::string key = lower_ascii(aliases[a]);
        for (bool system : {false, true}) {
            for (Preset &preset : filaments)
                if (preset.is_compatible && !preset.is_default && preset.is_system == system && preset_base_key(preset.name) == key) {
                    found = &preset;
                    break;
                }
            if (found != nullptr)
                break;
        }
    }
    // A system preset cannot hold the spool's pressure advance, so a spool with a
    // saved K gets its own copy of it below and the preset shows what the slot prints with.
    const bool        copy_system = found != nullptr && found->is_system && lane->flow_k >= 0;
    const std::string found_name  = found != nullptr ? found->name : std::string();
    if (found != nullptr && !copy_system) {
        out.name = found->name;
        DynamicPrintConfig config = found->config;
        if (made_by_sync(*found) && apply_spool_values(*lane, config)) {
            // Reload it in the filament tab too, unless it has edits there that would be lost.
            const bool reload = filaments.get_selected_preset_name() == found->name && !filaments.current_is_dirty();
            found->config     = std::move(config);
            found->save(nullptr);
            if (reload)
                filaments.select_preset_by_name(found->name, true);
            out.updated = true;
        }
        return out;
    }

    const Preset     *from    = copy_system ? found : base.empty() ? nullptr : filaments.find_preset(base);
    const std::string printer = bundle.printers.get_selected_preset_name();
    const std::string name    = aliases.front() + " @" + printer;
    out.name                  = found_name;
    // A preset of that name this printer cannot use is someone else's: leave it be.
    if (from == nullptr || printer.empty() || filaments.find_preset(name) != nullptr)
        return out;
    const std::string from_name = from->name;
    const std::string filament_id = spool_filament_id(filaments, aliases.front());
    const std::string vendor      = boost::trim_copy(lane->vendor);
    const AFCProtocol::Lane spool = *lane;

    // Making a preset reselects the edited filament preset, which drops its unsaved changes; keep them.
    const bool         keep_edits = filaments.current_is_dirty();
    DynamicPrintConfig edits;
    if (keep_edits)
        edits = filaments.get_edited_preset().config;
    std::vector<std::string> failures;
    const bool made = filaments.clone_presets({from}, failures, [&](Preset &preset, Preset::Type &) {
        preset.name        = name;
        preset.filament_id = filament_id;
        preset.config.option<ConfigOptionStrings>("compatible_printers", true)->values = {printer};
        preset.config.option<ConfigOptionString>("compatible_printers_condition", true)->value.clear();
        auto set_strings = [&preset](const char *key, const std::string &value) {
            auto *option = preset.config.option<ConfigOptionStrings>(key, true);
            if (option->values.empty())
                option->values.push_back(value);
            for (std::string &v : option->values)
                v = value;
        };
        if (!vendor.empty())
            set_strings("filament_vendor", vendor);
        set_strings("filament_notes", SPOOL_PRESET_NOTE);
        apply_spool_values(spool, preset.config);
    });
    if (keep_edits) {
        filaments.get_edited_preset().config = std::move(edits);
        filaments.update_dirty();
    }
    if (!made || filaments.find_preset(name) == nullptr)
        return out;
    BOOST_LOG_TRIVIAL(warning) << "AFC sync: made filament preset \"" << name << "\" for the spool on T" << tool << " from \"" << from_name << "\"";
    out.name    = name;
    out.created = true;
    return out;
}

bool AFCManager::show_filament_preset(PresetBundle &bundle, const std::string &name)
{
    Preset *preset = name.empty() ? nullptr : bundle.filaments.find_preset(name);
    if (preset == nullptr || preset->is_visible)
        return false;
    preset->is_visible = true;
    // System presets are shown by name from the installed filaments list, so it lasts past a restart.
    if (preset->is_system)
        wxGetApp().app_config->set(AppConfig::SECTION_FILAMENTS, preset->name, "true");
    BOOST_LOG_TRIVIAL(warning) << "AFC sync: added \"" << preset->name << "\" to the filament list";
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
    const auto *colors   = project.option<ConfigOptionStrings>("filament_colour");
    const auto *types    = full.option<ConfigOptionStrings>("filament_type");
    const auto *density  = full.option<ConfigOptionFloats>("filament_density");
    const size_t count   = colors ? colors->values.size() : 0;

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
        f.color = AFCProtocol::normalize_color(colors->values[i]);
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
                                {"preset", i < presets.size() ? presets[i] : std::string()},
                                {"spool_pa", wxGetApp().preset_bundle->spool_pressure_advance(i)}});
    }
    return out;
}

void AFCManager::fill_filament_toolheads(PresetBundle &bundle) const
{
    const std::shared_ptr<PrintHost> host = afc_host();
    if (!host || printer_sn() != host_key(host))
        return;
    const AFCProtocol::Snapshot snap = snapshot();
    if (!snap.present)
        return;
    for (size_t i = 0; i < bundle.filament_presets.size(); ++i)
        if (bundle.filament_toolhead(i) < 0)
            if (const AFCProtocol::Lane *lane = snap.lane_for_tool(int(i)); lane != nullptr && lane->extruder_index >= 0)
                bundle.set_filament_toolhead(i, lane->extruder_index);
}

std::vector<int> AFCManager::mapped_toolheads() const
{
    std::vector<int>                 out;
    const std::shared_ptr<PrintHost> host = afc_host();
    if (!host || printer_sn() != host_key(host))
        return out;
    const AFCProtocol::Snapshot snap = snapshot();
    if (!snap.present)
        return out;
    for (const AFCProtocol::ProjectFilament &f : project_filaments()) {
        const AFCProtocol::Lane *lane = f.used && f.id >= 0 ? snap.lane_for_tool(f.id) : nullptr;
        if (lane == nullptr || lane->extruder_index < 0)
            continue;
        if (out.size() <= size_t(f.id))
            out.resize(size_t(f.id) + 1, -1);
        out[size_t(f.id)] = lane->extruder_index;
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
