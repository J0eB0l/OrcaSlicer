#include "AFCProtocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace Slic3r {
namespace AFCProtocol {

using json = nlohmann::json;

namespace {

std::string str_field(const json &obj, const char *key)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_number_integer())
        return std::to_string(it->get<long long>());
    if (it->is_number())
        return std::to_string(it->get<double>());
    return {};
}

double num_field(const json &obj, const char *key, double fallback = 0)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return fallback;
    if (it->is_number())
        return it->get<double>();
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        char *end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        if (end != s.c_str())
            return v;
    }
    return fallback;
}

bool bool_field(const json &obj, const char *key)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return false;
    if (it->is_boolean())
        return it->get<bool>();
    if (it->is_number())
        return it->get<double>() != 0;
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        return s == "True" || s == "true" || s == "1";
    }
    return false;
}

std::vector<std::string> string_list(const json &value)
{
    std::vector<std::string> out;
    if (value.is_array()) {
        for (const json &v : value)
            if (v.is_string() && !v.get<std::string>().empty())
                out.push_back(v.get<std::string>());
    } else if (value.is_string()) {
        // AFC saves a lane map as "T0,T5" in its vars file.
        std::string s = value.get<std::string>(), cur;
        for (char c : s) {
            if (c == ',' || c == ' ') {
                if (!cur.empty())
                    out.push_back(cur);
                cur.clear();
            } else
                cur.push_back(c);
        }
        if (!cur.empty())
            out.push_back(cur);
    }
    return out;
}

bool is_unknown(const std::string &s) { return s.empty() || s == "NONE" || s == "None" || s == "none"; }

// Fill one lane from an AFC lane status object (AFC_lane.get_status()).
void fill_lane(const json &st, Lane &lane)
{
    if (lane.name.empty())
        lane.name = str_field(st, "name");
    if (lane.unit.empty())
        lane.unit = str_field(st, "unit");
    lane.index    = int(num_field(st, "lane", 0));
    lane.extruder = str_field(st, "extruder");
    lane.extruder_index = extruder_index_from_name(lane.extruder);
    if (auto it = st.find("map"); it != st.end()) {
        for (const std::string &m : string_list(*it))
            if (m != "NONE")
                lane.maps.push_back(m);
    }
    lane.color    = normalize_color(str_field(st, "color"));
    if (auto it = st.find("multi_color_hexes"); it != st.end())
        for (const std::string &c : string_list(*it)) {
            const std::string n = normalize_color(c);
            if (!n.empty())
                lane.multi_colors.push_back(n);
        }
    lane.material      = str_field(st, "material");
    lane.sub_type      = str_field(st, "sub_type");
    lane.vendor        = str_field(st, "spool_vendor");
    lane.filament_name = str_field(st, "filament_name");
    lane.spool_id      = int(num_field(st, "spool_id", 0));
    lane.weight        = num_field(st, "weight", 0);
    lane.initial_weight = num_field(st, "initial_weight", 0);
    lane.density       = num_field(st, "density", 0);
    lane.diameter      = num_field(st, "diameter", 0);
    lane.nozzle_temp   = int(num_field(st, "extruder_temp", 0));
    lane.bed_temp      = int(num_field(st, "bed_temp", 0));
    lane.prep          = bool_field(st, "prep");
    lane.load          = bool_field(st, "load");
    lane.tool_loaded   = bool_field(st, "tool_loaded");
    lane.status        = str_field(st, "status");
    lane.td            = str_field(st, "td1_td");
}

// "Box_Turtle Turtle_1" -> {"Box_Turtle", "Turtle_1"}
std::pair<std::string, std::string> split_unit_entry(const std::string &entry)
{
    const size_t sp = entry.rfind(' ');
    if (sp == std::string::npos)
        return {"", entry};
    return {entry.substr(0, sp), entry.substr(sp + 1)};
}

void read_afc_object(const json &afc, Snapshot &out)
{
    out.version          = str_field(afc, "version");
    out.current_lane     = str_field(afc, "current_load");
    out.multiple_mapping = bool_field(afc, "multiple_tool_mapping");
    if (auto it = afc.find("spoolman"); it != afc.end()) {
        out.spoolman = (it->is_string() && !it->get<std::string>().empty()) || (it->is_boolean() && it->get<bool>());
        if (it->is_string() && it->get<std::string>().compare(0, 4, "http") == 0)
            out.spoolman_url = it->get<std::string>();
    }
    if (auto it = afc.find("extruders"); it != afc.end()) {
        if (it->is_array())
            out.extruders = string_list(*it);
        else if (it->is_object())
            for (auto e = it->begin(); e != it->end(); ++e)
                out.extruders.push_back(e.key());
    }
}

const json *unwrap_status(const json &result)
{
    const json *cur = &result;
    // AFC's webhook answers {"status:": {"AFC": {...}}} (the colon is AFC's).
    for (const char *key : {"status:", "status", "result"}) {
        auto it = cur->find(key);
        if (it != cur->end() && it->is_object())
            cur = &*it;
    }
    auto it = cur->find("AFC");
    if (it != cur->end() && it->is_object())
        return &*it;
    return nullptr;
}

bool parse_rgb(const std::string &hex, int &r, int &g, int &b)
{
    const std::string n = normalize_color(hex);
    if (n.size() != 7)
        return false;
    r = std::stoi(n.substr(1, 2), nullptr, 16);
    g = std::stoi(n.substr(3, 2), nullptr, 16);
    b = std::stoi(n.substr(5, 2), nullptr, 16);
    return true;
}

std::string upper(std::string s)
{
    for (char &c : s)
        c = char(std::toupper((unsigned char) c));
    return s;
}

// Material names AFC and Spoolman write vary in case and decoration
// ("PLA+", "pla", "PLA Basic"); compare on the leading word.
std::string material_key(const std::string &material)
{
    std::string m = upper(material);
    const size_t sp = m.find_first_of(" -_");
    if (sp != std::string::npos)
        m = m.substr(0, sp);
    while (!m.empty() && (m.back() == '+'))
        m.pop_back();
    return m;
}

bool safe_value(const std::string &v, size_t max_len)
{
    if (v.empty() || v.size() > max_len)
        return false;
    for (char c : v)
        if (!(std::isalnum((unsigned char) c) || c == '_' || c == '-' || c == '+' || c == '.'))
            return false;
    return true;
}

// "lane9" < "lane10": digit runs compare as numbers.
bool natural_less(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char) a[i]) && std::isdigit((unsigned char) b[j])) {
            size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit((unsigned char) a[ie]))
                ++ie;
            while (je < b.size() && std::isdigit((unsigned char) b[je]))
                ++je;
            const std::string na = a.substr(i, ie - i), nb = b.substr(j, je - j);
            const size_t      za = na.find_first_not_of('0'), zb = nb.find_first_not_of('0');
            const std::string ta = za == std::string::npos ? "" : na.substr(za);
            const std::string tb = zb == std::string::npos ? "" : nb.substr(zb);
            if (ta.size() != tb.size())
                return ta.size() < tb.size();
            if (ta != tb)
                return ta < tb;
            i = ie;
            j = je;
        } else {
            if (a[i] != b[j])
                return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

// The webhook arrives as a JSON object, so units and lanes come out in key
// order (lane10 before lane8). Put lanes in natural order inside each unit,
// units in the order of their first lane, and lane-less units (a toolchanger
// entry, unclaimed BridgeBox spares) last.
void order_snapshot(Snapshot &out)
{
    for (Unit &u : out.units)
        std::sort(u.lanes.begin(), u.lanes.end(), natural_less);
    std::stable_sort(out.units.begin(), out.units.end(), [](const Unit &a, const Unit &b) {
        if (a.lanes.empty() != b.lanes.empty())
            return b.lanes.empty();
        if (a.lanes.empty())
            return natural_less(a.name, b.name);
        return natural_less(a.lanes.front(), b.lanes.front());
    });
    std::vector<Lane> ordered;
    ordered.reserve(out.lanes.size());
    for (const Unit &u : out.units)
        for (const std::string &name : u.lanes)
            for (Lane &l : out.lanes)
                if (l.name == name && l.unit == u.name)
                    ordered.push_back(l);
    if (ordered.size() == out.lanes.size())
        out.lanes = std::move(ordered);
}

std::string format_number(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", v);
    std::string s = buf;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, ".0") == 0)
        s.resize(s.size() - 2);
    return s;
}

} // namespace

const Lane *Snapshot::find_lane(const std::string &name) const
{
    for (const Lane &l : lanes)
        if (l.name == name)
            return &l;
    return nullptr;
}

const Lane *Snapshot::lane_for_tool(int tool) const
{
    const std::string key = "T" + std::to_string(tool);
    for (const Lane &l : lanes)
        if (std::find(l.maps.begin(), l.maps.end(), key) != l.maps.end())
            return &l;
    return nullptr;
}

int Snapshot::max_tool() const
{
    int best = -1;
    for (const Lane &l : lanes)
        for (const std::string &m : l.maps)
            best = std::max(best, tool_number(m));
    return best;
}

int tool_number(const std::string &map)
{
    if (map.size() < 2 || (map[0] != 'T' && map[0] != 't'))
        return -1;
    int v = 0;
    for (size_t i = 1; i < map.size(); ++i) {
        if (!std::isdigit((unsigned char) map[i]) || i > 4)
            return -1;
        v = v * 10 + (map[i] - '0');
    }
    return v;
}

int extruder_index_from_name(const std::string &extruder)
{
    static const std::string prefix = "extruder";
    if (extruder.compare(0, prefix.size(), prefix) != 0)
        return -1;
    if (extruder.size() == prefix.size())
        return 0;
    int v = 0;
    for (size_t i = prefix.size(); i < extruder.size(); ++i) {
        if (!std::isdigit((unsigned char) extruder[i]))
            return -1;
        v = v * 10 + (extruder[i] - '0');
    }
    return v;
}

std::string normalize_color(const std::string &color)
{
    std::string s = color;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return {};
    for (char c : s)
        if (!std::isxdigit((unsigned char) c))
            return {};
    return "#" + upper(s.substr(0, 6));
}

bool parse_status_endpoint(const json &result, Snapshot &out)
{
    out = Snapshot();
    if (!result.is_object())
        return false;
    const json *afc = unwrap_status(result);
    if (afc == nullptr)
        return false;

    if (auto sys = afc->find("system"); sys != afc->end() && sys->is_object()) {
        read_afc_object(*sys, out);
        if (out.current_lane.empty())
            out.current_lane = str_field(*sys, "current_load");
    }

    for (auto it = afc->begin(); it != afc->end(); ++it) {
        if (it.key() == "system" || !it->is_object())
            continue;
        Unit unit;
        unit.name = it.key();
        if (auto sys = it->find("system"); sys != it->end() && sys->is_object())
            unit.type = str_field(*sys, "type");
        for (auto l = it->begin(); l != it->end(); ++l) {
            if (l.key() == "system" || !l->is_object())
                continue;
            Lane lane;
            lane.name      = l.key();
            lane.unit      = unit.name;
            lane.unit_type = unit.type;
            fill_lane(*l, lane);
            lane.unit = unit.name; // the webhook groups by unit; trust the grouping
            unit.lanes.push_back(lane.name);
            out.lanes.push_back(std::move(lane));
        }
        out.units.push_back(std::move(unit));
    }
    order_snapshot(out);
    out.present = true;
    return true;
}

std::vector<std::string> lane_object_names(const json &objects_list, const std::vector<std::string> &lane_names)
{
    std::vector<std::string> out;
    const json *list = &objects_list;
    if (objects_list.is_object()) {
        auto it = objects_list.find("objects");
        if (it == objects_list.end())
            return out;
        list = &*it;
    }
    if (!list->is_array())
        return out;
    const std::set<std::string> wanted(lane_names.begin(), lane_names.end());
    std::set<std::string> seen;
    for (const json &o : *list) {
        if (!o.is_string())
            continue;
        const std::string name = o.get<std::string>();
        if (name.compare(0, 4, "AFC_") != 0)
            continue;
        const size_t sp = name.rfind(' ');
        if (sp == std::string::npos)
            continue;
        const std::string lane = name.substr(sp + 1);
        // Lanes are registered once, as AFC_stepper/AFC_lane/AFC_BambuAMS...
        // Units and hubs share the prefix but not a lane's name.
        if (wanted.count(lane) && !seen.count(lane)) {
            seen.insert(lane);
            out.push_back(name);
        }
    }
    return out;
}

bool parse_objects_status(const json &status, Snapshot &out)
{
    out = Snapshot();
    const json *st = &status;
    if (auto it = status.find("status"); it != status.end() && it->is_object())
        st = &*it;
    auto afc_it = st->find("AFC");
    if (afc_it == st->end() || !afc_it->is_object())
        return false;
    const json &afc = *afc_it;
    read_afc_object(afc, out);

    std::map<std::string, std::string> unit_types;
    if (auto it = afc.find("units"); it != afc.end() && it->is_array())
        for (const std::string &entry : string_list(*it)) {
            auto [type, name] = split_unit_entry(entry);
            unit_types[name] = type;
            Unit unit;
            unit.name = name;
            unit.type = type;
            out.units.push_back(std::move(unit));
        }

    std::vector<std::string> lane_order;
    if (auto it = afc.find("lanes"); it != afc.end())
        lane_order = string_list(*it);

    std::map<std::string, const json *> by_lane;
    for (auto it = st->begin(); it != st->end(); ++it) {
        const std::string &key = it.key();
        const size_t sp = key.rfind(' ');
        if (key.compare(0, 4, "AFC_") != 0 || sp == std::string::npos || !it->is_object())
            continue;
        by_lane[key.substr(sp + 1)] = &*it;
    }

    for (const std::string &name : lane_order) {
        auto found = by_lane.find(name);
        if (found == by_lane.end())
            continue;
        Lane lane;
        lane.name = name;
        fill_lane(*found->second, lane);
        lane.unit_type = unit_types.count(lane.unit) ? unit_types[lane.unit] : std::string();
        bool placed = false;
        for (Unit &u : out.units)
            if (u.name == lane.unit) {
                u.lanes.push_back(name);
                placed = true;
            }
        if (!placed) {
            Unit unit;
            unit.name  = lane.unit;
            unit.type  = lane.unit_type;
            unit.lanes = {name};
            out.units.push_back(std::move(unit));
        }
        out.lanes.push_back(std::move(lane));
    }
    // Drop units with no lanes (AFC lists only units that have lanes, but be safe).
    out.units.erase(std::remove_if(out.units.begin(), out.units.end(), [](const Unit &u) { return u.lanes.empty(); }),
                    out.units.end());
    out.present = true;
    return true;
}

void merge_lane_data(const json &lane_data, Snapshot &out)
{
    const json *data = &lane_data;
    // server.database.get_item answers {"namespace":..., "key":..., "value": {...}}
    if (auto it = lane_data.find("value"); it != lane_data.end() && it->is_object())
        data = &*it;
    if (!data->is_object())
        return;
    for (auto it = data->begin(); it != data->end(); ++it) {
        if (!it->is_object())
            continue;
        // Newer AFC keys each record by the T(n) it serves, older releases by lane name.
        Lane *lane = nullptr;
        for (Lane &l : out.lanes)
            if (l.name == it.key())
                lane = &l;
        if (lane == nullptr && tool_number(it.key()) >= 0)
            for (Lane &l : out.lanes)
                if (std::find(l.maps.begin(), l.maps.end(), it.key()) != l.maps.end())
                    lane = &l;
        if (lane == nullptr)
            continue;
        const json &v = *it;
        if (lane->vendor.empty())
            lane->vendor = str_field(v, "vendor_name");
        if (lane->filament_name.empty())
            lane->filament_name = str_field(v, "name");
        if (lane->material.empty())
            lane->material = str_field(v, "material");
        if (lane->color.empty())
            lane->color = normalize_color(str_field(v, "color"));
        if (lane->spool_id == 0)
            lane->spool_id = int(num_field(v, "spool_id", 0));
        if (lane->nozzle_temp == 0)
            lane->nozzle_temp = int(num_field(v, "nozzle_temp", 0));
        if (lane->bed_temp == 0)
            lane->bed_temp = int(num_field(v, "bed_temp", 0));
        if (lane->weight == 0)
            lane->weight = num_field(v, "weight", 0);
        if (lane->initial_weight == 0)
            lane->initial_weight = num_field(v, "initial_weight", 0);
        if (lane->td.empty())
            lane->td = str_field(v, "td");
        if (lane->extruder_index < 0) {
            const double idx = num_field(v, "extruder_index", -1);
            if (idx >= 0)
                lane->extruder_index = int(idx);
        }
    }
}

namespace {

const std::vector<std::string> &ace_dryer_fields()
{
    static const std::vector<std::string> f = {"ace_connected",   "ace_temp",         "ace_humidity",       "ace_dryer",
                                               "ace_dryer_target", "ace_dryer_remain", "ace_dryer_duration", "ace_dry_rotate"};
    return f;
}

const std::vector<std::string> &bambu_dryer_fields()
{
    static const std::vector<std::string> f = {"has_heater", "dry_max_temp", "drying",      "dry_remaining", "dry_minutes",
                                               "dry_target", "dry_rotate",   "dry_state",   "dry_error",     "dry_note",
                                               "dry_blocked", "temperature", "humidity",    "ams_model",     "bridge_online"};
    return f;
}

bool is_ace_type(const std::string &type) { return upper(type).compare(0, 3, "ACE") == 0; }
bool is_bambu_type(const std::string &type) { return upper(type).compare(0, 5, "BAMBU") == 0; }

std::string note_from(const json &obj, const char *key)
{
    auto it = obj.find(key);
    if (it == obj.end() || it->is_null())
        return {};
    if (it->is_string())
        return it->get<std::string>();
    if (it->is_boolean())
        return it->get<bool>() ? std::string("Drying is blocked right now") : std::string();
    return {};
}

} // namespace

json dryer_objects_query(const Snapshot &snapshot)
{
    json q = json::object();
    for (const Unit &u : snapshot.units) {
        if (u.lanes.empty() || u.type.empty())
            continue;
        if (is_ace_type(u.type))
            q["AFC_" + u.type + " " + u.name] = ace_dryer_fields();
        else if (is_bambu_type(u.type))
            q["AFC_" + u.type + " " + u.name] = bambu_dryer_fields();
    }
    return q;
}

void merge_dryer_status(const json &status, Snapshot &out)
{
    const json *st = &status;
    if (auto it = status.find("status"); it != status.end() && it->is_object())
        st = &*it;
    if (!st->is_object())
        return;
    for (auto it = st->begin(); it != st->end(); ++it) {
        const std::string &key = it.key();
        const size_t       sp  = key.rfind(' ');
        if (key.compare(0, 4, "AFC_") != 0 || sp == std::string::npos || !it->is_object() || it->empty())
            continue;
        const std::string prefix = key.substr(4, sp - 4);
        const std::string name   = key.substr(sp + 1);
        Unit *unit = nullptr;
        for (Unit &u : out.units)
            if (u.name == name)
                unit = &u;
        if (unit == nullptr)
            continue;
        const json &o = *it;
        Dryer       d;
        if (is_ace_type(prefix)) {
            d.kind        = "ace";
            d.model       = prefix;
            d.online      = o.contains("ace_connected") ? bool_field(o, "ace_connected") : true;
            d.state       = str_field(o, "ace_dryer");
            const std::string state = upper(d.state);
            d.drying      = !state.empty() && state != "STOP" && state != "STOPPED" && state != "STOPPING" && state != "IDLE" &&
                       state.find("ERROR") == std::string::npos;
            d.target      = num_field(o, "ace_dryer_target", 0);
            d.remaining_s = int(num_field(o, "ace_dryer_remain", 0));
            // ACE 2 reports the duration in seconds, the first ACE in minutes.
            const double duration = num_field(o, "ace_dryer_duration", 0);
            d.duration_s  = int(upper(prefix) == "ACE" ? duration * 60 : duration);
            d.temperature = num_field(o, "ace_temp", -1);
            d.humidity    = num_field(o, "ace_humidity", -1);
            d.rotate      = bool_field(o, "ace_dry_rotate");
            d.max_temp    = upper(prefix) == "ACE" ? 55 : 70;
            if (state.find("ERROR") != std::string::npos)
                d.note = "Dryer reports " + d.state;
        } else if (is_bambu_type(prefix)) {
            if (o.contains("has_heater") && !bool_field(o, "has_heater"))
                continue; // AMS 1 / AMS lite: no heater
            d.kind        = "bambu";
            d.model       = str_field(o, "ams_model");
            d.online      = o.contains("bridge_online") ? bool_field(o, "bridge_online") : true;
            d.drying      = bool_field(o, "drying");
            d.target      = num_field(o, "dry_target", 0);
            d.remaining_s = int(num_field(o, "dry_remaining", 0));
            d.duration_s  = int(num_field(o, "dry_minutes", 0) * 60);
            d.temperature = num_field(o, "temperature", -1);
            d.humidity    = num_field(o, "humidity", -1);
            d.rotate      = bool_field(o, "dry_rotate");
            d.max_temp    = num_field(o, "dry_max_temp", 0);
            d.state       = str_field(o, "dry_state");
            for (const char *k : {"dry_error", "dry_blocked", "dry_note"})
                if (d.note.empty())
                    d.note = note_from(o, k);
            if (d.max_temp <= 0)
                continue; // no heater reported
        } else {
            continue;
        }
        unit->has_dryer = true;
        unit->dryer     = std::move(d);
    }
}

bool build_dry_gcode(const Snapshot &snapshot, const std::string &unit_name, const json &request, std::string &gcode, std::string &error)
{
    gcode.clear();
    const Unit *unit = nullptr;
    for (const Unit &u : snapshot.units)
        if (u.name == unit_name)
            unit = &u;
    if (unit == nullptr || !is_safe_lane_name(unit_name)) {
        error = "Unknown unit " + unit_name;
        return false;
    }
    if (!unit->has_dryer) {
        error = unit_name + " has no dryer";
        return false;
    }
    const Dryer      &d      = unit->dryer;
    const std::string action = request.is_object() ? request.value("action", std::string()) : std::string();
    if (action == "stop") {
        gcode = (d.kind == "ace" ? "ACE_DRY_STOP UNIT=" : "AFC_BAMBU_HEATER_STOP UNIT=") + unit_name;
        return true;
    }
    if (action != "start") {
        error = "Unknown dryer action";
        return false;
    }
    const json temp_v = request.value("temp", json()), minutes_v = request.value("minutes", json());
    if (!temp_v.is_number() || !minutes_v.is_number()) {
        error = "Drying needs a temperature and a time";
        return false;
    }
    const int temp    = int(std::lround(temp_v.get<double>()));
    const int minutes = int(std::lround(minutes_v.get<double>()));
    const int max     = d.max_temp > 0 ? int(d.max_temp) : 85;
    if (temp < 35 || temp > max) {
        error = "Temperature must be 35-" + std::to_string(max) + " C on " + unit_name;
        return false;
    }
    if (minutes < 1 || minutes > 48 * 60) {
        error = "Drying time must be 1 minute to 48 hours";
        return false;
    }
    const json rotate_v = request.value("rotate", json(false));
    const bool rotate   = rotate_v.is_boolean() ? rotate_v.get<bool>() : (rotate_v.is_number() && rotate_v.get<double>() != 0);
    if (d.kind == "ace")
        gcode = "ACE_DRY UNIT=" + unit_name + " TEMP=" + std::to_string(temp) + " DURATION=" + std::to_string(minutes) +
                " ROTATE=" + (rotate ? "1" : "0");
    else
        gcode = "AFC_BAMBU_HEATER_START UNIT=" + unit_name + " TEMP=" + std::to_string(temp) + " TIME=" + std::to_string(minutes) +
                " ROTATE=" + (rotate ? "1" : "0");
    return true;
}

double spool_flow_k(const json &spool)
{
    if (!spool.is_object())
        return -1;
    if (spool.contains("response"))
        return spool_flow_k(spool["response"]);
    auto extra = spool.find("extra");
    if (extra == spool.end() || !extra->is_object())
        return -1;
    // Spoolman keeps every extra field JSON-encoded, so a float reads "0.032".
    json value = extra->value("flow_k", json());
    if (value.is_string()) {
        try {
            value = json::parse(value.get<std::string>());
        } catch (const std::exception &) {
            return -1;
        }
    }
    if (!value.is_number())
        return -1;
    const double k = value.get<double>();
    return std::isfinite(k) && k >= 0 ? k : -1;
}

json to_json(const Snapshot &s)
{
    json j;
    j["present"]          = s.present;
    j["version"]          = s.version;
    j["spoolman"]         = s.spoolman;
    j["spoolman_url"]     = s.spoolman_url;
    j["multiple_mapping"] = s.multiple_mapping;
    j["current_lane"]     = s.current_lane;
    j["extruders"]        = s.extruders;
    j["units"]            = json::array();
    for (const Unit &u : s.units) {
        json unit = {{"name", u.name}, {"type", u.type}, {"lanes", u.lanes}, {"dryer", nullptr}};
        if (u.has_dryer) {
            const Dryer &d = u.dryer;
            unit["dryer"]  = {{"kind", d.kind},         {"model", d.model},       {"online", d.online},
                              {"drying", d.drying},     {"rotate", d.rotate},     {"max_temp", d.max_temp},
                              {"target", d.target},     {"temperature", d.temperature}, {"humidity", d.humidity},
                              {"remaining_s", d.remaining_s}, {"duration_s", d.duration_s}, {"state", d.state},
                              {"note", d.note}};
        }
        j["units"].push_back(std::move(unit));
    }
    j["lanes"] = json::array();
    for (const Lane &l : s.lanes) {
        j["lanes"].push_back({{"name", l.name},
                              {"unit", l.unit},
                              {"unit_type", l.unit_type},
                              {"index", l.index},
                              {"maps", l.maps},
                              {"extruder", l.extruder},
                              {"extruder_index", l.extruder_index},
                              {"color", l.color},
                              {"multi_colors", l.multi_colors},
                              {"material", l.material},
                              {"sub_type", l.sub_type},
                              {"vendor", l.vendor},
                              {"filament_name", l.filament_name},
                              {"spool_id", l.spool_id},
                              {"weight", l.weight},
                              {"initial_weight", l.initial_weight},
                              {"density", l.density},
                              {"nozzle_temp", l.nozzle_temp},
                              {"bed_temp", l.bed_temp},
                              {"prep", l.prep},
                              {"load", l.load},
                              {"tool_loaded", l.tool_loaded},
                              {"has_filament", l.has_filament()},
                              {"status", l.status},
                              {"td", l.td},
                              {"flow_k", l.flow_k >= 0 ? json(l.flow_k) : json()}});
    }
    j["max_tool"] = s.max_tool();
    return j;
}

std::vector<ToolSlot> tool_slots(const Snapshot &snapshot)
{
    std::vector<ToolSlot> out;
    const int max_tool = snapshot.max_tool();
    for (int t = 0; t <= max_tool; ++t) {
        ToolSlot slot;
        slot.tool     = t;
        slot.vendor   = "Generic";
        slot.type     = "NONE";
        slot.sub_type = "NONE";
        slot.color    = "#FFFFFF";
        if (const Lane *lane = snapshot.lane_for_tool(t)) {
            slot.mapped         = true;
            slot.lane           = lane->name;
            slot.extruder_index = lane->extruder_index;
            slot.has_filament   = lane->has_filament();
            if (slot.has_filament) {
                if (!lane->vendor.empty())
                    slot.vendor = lane->vendor;
                if (!is_unknown(lane->material))
                    slot.type = lane->material;
                if (!is_unknown(lane->sub_type))
                    slot.sub_type = lane->sub_type;
                if (!lane->color.empty())
                    slot.color = lane->color;
                slot.colors = lane->multi_colors;
                slot.flow_k = lane->flow_k;
            }
        }
        if (slot.colors.empty())
            slot.colors.push_back(slot.color);
        out.push_back(std::move(slot));
    }
    return out;
}

bool apply_spool_pressure_advance(const std::vector<double> &spool, const std::vector<int> &step_sizes,
                                  std::vector<double> &pressure_advance, std::vector<unsigned char> &enable)
{
    bool   changed = false;
    size_t start   = 0;
    for (size_t slot = 0; slot < spool.size(); ++slot) {
        const size_t size = slot < step_sizes.size() ? size_t(std::max(1, step_sizes[slot])) : 1;
        const double k    = spool[slot];
        if (std::isfinite(k) && k >= 0) {
            for (size_t i = start; i < start + size && i < pressure_advance.size(); ++i)
                if (pressure_advance[i] != k) {
                    pressure_advance[i] = k;
                    changed             = true;
                }
            for (size_t i = start; i < start + size && i < enable.size(); ++i)
                if (!enable[i]) {
                    enable[i] = 1;
                    changed   = true;
                }
        }
        start += size;
    }
    return changed;
}

std::string filament_display_name(const std::string &vendor, const std::string &type, const std::string &sub_type)
{
    if (sub_type == "Support")
        return vendor + " Support For " + type;
    return vendor + " " + type + (!is_unknown(sub_type) ? " " + sub_type : std::string());
}

namespace {

std::string lower_ascii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Single spaces, no ends, and '-' for what a preset file name or its " @" cannot hold.
std::string clean_preset_part(const std::string &in)
{
    std::string out;
    for (unsigned char c : in) {
        if (c <= ' ') {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
        } else
            out += std::strchr("<>:\"/\\|?*@", c) != nullptr ? '-' : char(c);
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// "Polymaker PolyLite PLA" without its "Polymaker " (vendor already lower-cased).
std::string without_vendor(const std::string &name, const std::string &vendor_lc)
{
    const std::string name_lc = lower_ascii(name);
    if (name_lc == vendor_lc)
        return {};
    if (name_lc.size() > vendor_lc.size() && name_lc.compare(0, vendor_lc.size(), vendor_lc) == 0 && name[vendor_lc.size()] == ' ')
        return clean_preset_part(name.substr(vendor_lc.size() + 1));
    return name;
}

// A word of `name` starts with the material: "PLA" in "PolyLite PLA" or "Tough PLA-CF".
bool names_material(const std::string &name, const std::string &material)
{
    const std::string name_lc = lower_ascii(name), material_lc = lower_ascii(material);
    for (size_t pos = 0; pos < name_lc.size();) {
        if (name_lc.compare(pos, material_lc.size(), material_lc) == 0)
            return true;
        pos = name_lc.find(' ', pos);
        if (pos == std::string::npos)
            break;
        ++pos;
    }
    return false;
}

struct SpoolNames
{
    std::string vendor;   ///< cleaned, "" for none or "Generic"
    std::string filament; ///< filament name without the vendor, cleaned
    std::string material;
};

SpoolNames spool_names(const Lane &lane)
{
    SpoolNames out;
    out.vendor = clean_preset_part(lane.vendor);
    if (is_unknown(out.vendor) || lower_ascii(out.vendor) == "generic")
        out.vendor.clear();
    out.material = is_unknown(lane.material) ? std::string() : clean_preset_part(lane.material);
    const std::string filament = clean_preset_part(lane.filament_name);
    if (!out.vendor.empty() && !is_unknown(filament))
        out.filament = without_vendor(filament, lower_ascii(out.vendor));
    return out;
}

} // namespace

std::string spool_preset_name(const Lane &lane)
{
    const SpoolNames n = spool_names(lane);
    if (n.vendor.empty())
        return {};
    if (n.filament.empty())
        return n.material.empty() ? std::string() :
                                    clean_preset_part(filament_display_name(n.vendor, n.material, clean_preset_part(lane.sub_type)));
    if (!n.material.empty() && !names_material(n.filament, n.material))
        return n.vendor + " " + n.material + " " + n.filament;
    return n.vendor + " " + n.filament;
}

std::vector<std::string> spool_preset_aliases(const Lane &lane)
{
    std::vector<std::string> out;
    const std::string name = spool_preset_name(lane);
    if (name.empty())
        return out;
    out.push_back(name);
    const SpoolNames n = spool_names(lane);
    if (!n.filament.empty() && lower_ascii(n.filament) != lower_ascii(n.material) && lower_ascii(n.filament) != lower_ascii(name))
        out.push_back(n.filament);
    return out;
}

bool is_safe_lane_name(const std::string &name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (char c : name)
        if (!(std::isalnum((unsigned char) c) || c == '_' || c == '-'))
            return false;
    return true;
}

bool build_lane_edit_gcode(const Snapshot &snapshot, const std::string &lane_name, const json &changes,
                           std::vector<std::string> &gcode, std::string &error)
{
    std::vector<std::string> out;
    if (!is_safe_lane_name(lane_name) || snapshot.find_lane(lane_name) == nullptr) {
        error = "unknown lane";
        return false;
    }
    if (!changes.is_object() || changes.empty()) {
        error = "no changes";
        return false;
    }
    const std::string lane_arg = " LANE=" + lane_name;

    // Fixed order: a Spoolman id first, since AFC then pulls colour, material
    // and weight from Spoolman and the explicit values below must win.
    static const char *order[] = {"spool_id", "map", "material", "color", "weight", "nozzle_temp", "bed_temp"};
    for (auto it = changes.begin(); it != changes.end(); ++it)
        if (std::find_if(std::begin(order), std::end(order), [&it](const char *k) { return it.key() == k; }) == std::end(order)) {
            error = "unsupported field " + it.key();
            return false;
        }

    for (const char *ordered_key : order) {
        auto it = changes.find(ordered_key);
        if (it == changes.end())
            continue;
        const std::string key = ordered_key;
        const json       &val = *it;
        if (key == "color") {
            const std::string c = val.is_string() ? normalize_color(val.get<std::string>()) : std::string();
            if (c.empty()) {
                error = "color must be #RRGGBB";
                return false;
            }
            out.push_back("SET_COLOR" + lane_arg + " COLOR=" + c.substr(1));
        } else if (key == "material") {
            const std::string m = val.is_string() ? val.get<std::string>() : std::string();
            if (!safe_value(m, 32)) {
                error = "material may only use letters, digits and + - _ .";
                return false;
            }
            out.push_back("SET_MATERIAL" + lane_arg + " MATERIAL=" + m);
        } else if (key == "weight") {
            if (!val.is_number() || val.get<double>() < 0 || val.get<double>() > 100000) {
                error = "weight must be 0-100000 g";
                return false;
            }
            out.push_back("SET_WEIGHT" + lane_arg + " WEIGHT=" + format_number(val.get<double>()));
        } else if (key == "spool_id") {
            if (!val.is_number_integer() || val.get<long long>() < 0 || val.get<long long>() > 99999999) {
                error = "spool id must be a positive whole number";
                return false;
            }
            const long long id = val.get<long long>();
            out.push_back("SET_SPOOL_ID" + lane_arg + " SPOOL_ID=" + (id > 0 ? std::to_string(id) : std::string()));
        } else if (key == "map") {
            const std::string m = val.is_string() ? upper(val.get<std::string>()) : std::string();
            if (tool_number(m) < 0) {
                error = "map must be T<number>";
                return false;
            }
            out.push_back("SET_MAP" + lane_arg + " MAP=" + m);
        } else if (key == "nozzle_temp" || key == "bed_temp") {
            if (!val.is_number() || val.get<double>() < 0 || val.get<double>() > 500) {
                error = "temperature must be 0-500";
                return false;
            }
            // AFC_SET_SPOOL_TEMP keeps the other temperature when it is omitted.
            out.push_back("AFC_SET_SPOOL_TEMP" + lane_arg + (key == "nozzle_temp" ? " EXTRUDER_TEMP=" : " BED_TEMP=") +
                          std::to_string(int(std::lround(val.get<double>()))));
        }
    }
    gcode.insert(gcode.end(), out.begin(), out.end());
    return true;
}

bool build_tool_mapping_gcode(const Snapshot &snapshot, const std::vector<std::pair<int, std::string>> &tool_to_lane,
                              std::vector<std::string> &gcode, std::string &error)
{
    std::set<int>         tools;
    std::set<std::string> lanes;
    const int             max_tool = snapshot.max_tool();
    for (const auto &[tool, lane] : tool_to_lane) {
        if (tool < 0 || tool > max_tool) {
            error = "T" + std::to_string(tool) + " is not a tool AFC knows (it has T0-T" + std::to_string(std::max(max_tool, 0)) + ")";
            return false;
        }
        if (!is_safe_lane_name(lane) || snapshot.find_lane(lane) == nullptr) {
            error = "unknown lane " + lane;
            return false;
        }
        if (!tools.insert(tool).second) {
            error = "T" + std::to_string(tool) + " is mapped twice";
            return false;
        }
        if (!lanes.insert(lane).second && !snapshot.multiple_mapping) {
            error = lane + " is used for two filaments; enable AFC multiple tool mapping to allow that";
            return false;
        }
    }

    // Replay SET_MAP's swap semantics so lanes already in place are skipped.
    std::map<std::string, std::vector<std::string>> maps;
    for (const Lane &l : snapshot.lanes)
        maps[l.name] = l.maps;
    auto holder = [&maps](const std::string &key) -> std::string {
        for (const auto &[lane, m] : maps)
            if (std::find(m.begin(), m.end(), key) != m.end())
                return lane;
        return {};
    };

    std::vector<std::string> out;
    for (const auto &[tool, lane] : tool_to_lane) {
        const std::string key = "T" + std::to_string(tool);
        const std::string cur = holder(key);
        if (cur == lane)
            continue;
        out.push_back("SET_MAP LANE=" + lane + " MAP=" + key);
        if (cur.empty())
            continue;
        if (snapshot.multiple_mapping) {
            auto &m = maps[cur];
            m.erase(std::remove(m.begin(), m.end(), key), m.end());
            maps[lane].push_back(key);
        } else {
            maps[cur]  = maps[lane];
            maps[lane] = {key};
        }
    }
    gcode.insert(gcode.end(), out.begin(), out.end());
    return true;
}

bool build_lane_action_gcode(const Snapshot &snapshot, const std::string &lane, const std::string &action,
                             std::string &gcode, std::string &error)
{
    const Lane *l = is_safe_lane_name(lane) ? snapshot.find_lane(lane) : nullptr;
    if (l == nullptr) {
        error = "unknown lane";
        return false;
    }
    if (action == "load") {
        if (!l->has_filament()) {
            error = lane + " has no filament";
            return false;
        }
        gcode = "CHANGE_TOOL LANE=" + lane;
    } else if (action == "unload") {
        if (!l->tool_loaded) {
            error = lane + " is not loaded in a toolhead";
            return false;
        }
        gcode = "TOOL_UNLOAD LANE=" + lane;
    } else if (action == "eject") {
        if (l->tool_loaded) {
            error = "unload " + lane + " from the toolhead first";
            return false;
        }
        gcode = "LANE_UNLOAD LANE=" + lane;
    } else {
        error = "unknown action " + action;
        return false;
    }
    return true;
}

double rgb_distance(const std::string &a, const std::string &b)
{
    int r1, g1, b1, r2, g2, b2;
    if (!parse_rgb(a, r1, g1, b1) || !parse_rgb(b, r2, g2, b2))
        return 1000.0;
    return std::sqrt(double((r1 - r2) * (r1 - r2) + (g1 - g2) * (g1 - g2) + (b1 - b2) * (b1 - b2)));
}

std::vector<std::string> suggest_mapping(const Snapshot &snapshot, const std::vector<ProjectFilament> &filaments,
                                         const ColorDistance &distance)
{
    std::vector<std::string> out(filaments.size());
    const ColorDistance dist = distance ? distance : ColorDistance(rgb_distance);

    std::vector<const Lane *> loaded;
    for (const Lane &l : snapshot.lanes)
        if (l.has_filament())
            loaded.push_back(&l);
    if (loaded.empty())
        return out;

    size_t used_count = 0;
    for (const ProjectFilament &f : filaments)
        used_count += f.used ? 1 : 0;
    const bool unique = used_count <= loaded.size();
    std::set<std::string> taken;

    // Two colours this close read as "the same" to the eye (CIEDE2000 ~ 10,
    // RGB ~ 40); keep the lane AFC already maps when it is that close.
    auto same_enough = [&](const ProjectFilament &f, const Lane &l) {
        const bool material_ok = is_unknown(l.material) || is_unknown(f.type) ||
                                 material_key(l.material) == material_key(f.type);
        return material_ok && !l.color.empty() && dist(f.color, l.color) <= (distance ? 10.0 : 40.0);
    };

    // Pass 1: keep the current T(n) lane when it already fits.
    for (size_t i = 0; i < filaments.size(); ++i) {
        const ProjectFilament &f = filaments[i];
        if (!f.used)
            continue;
        const Lane *cur = snapshot.lane_for_tool(f.id);
        if (cur && cur->has_filament() && same_enough(f, *cur) && (!unique || !taken.count(cur->name))) {
            out[i] = cur->name;
            taken.insert(cur->name);
        }
    }

    // Pass 2: best remaining lane, same material first.
    for (size_t i = 0; i < filaments.size(); ++i) {
        const ProjectFilament &f = filaments[i];
        if (!f.used || !out[i].empty())
            continue;
        const Lane *best      = nullptr;
        double      best_cost = std::numeric_limits<double>::max();
        for (const Lane *l : loaded) {
            if (unique && taken.count(l->name))
                continue;
            double cost = l->color.empty() ? 500.0 : dist(f.color, l->color);
            if (!is_unknown(f.type) && !is_unknown(l->material) && material_key(l->material) != material_key(f.type))
                cost += 10000.0; // other material only as a last resort
            if (f.grams > 0 && l->weight > 0 && l->weight < f.grams)
                cost += 1000.0; // not enough left on the spool
            if (cost < best_cost) {
                best_cost = cost;
                best      = l;
            }
        }
        if (best) {
            out[i] = best->name;
            taken.insert(best->name);
        }
    }
    return out;
}

} // namespace AFCProtocol
} // namespace Slic3r
