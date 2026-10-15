#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/AFCProtocol.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

json lane_status(const std::string &name, const std::string &unit, int index, const std::vector<std::string> &maps,
                 const std::string &extruder, const std::string &color, const std::string &material, bool loaded,
                 double weight = 1000)
{
    return json{{"name", name},
                {"unit", unit},
                {"lane", index},
                {"map", maps},
                {"extruder", extruder},
                {"color", color},
                {"material", material},
                {"spool_vendor", loaded ? "Polymaker" : ""},
                {"filament_name", loaded ? "PolyLite" : ""},
                {"spool_id", loaded ? json(12) : json(nullptr)},
                {"weight", weight},
                {"extruder_temp", 215},
                {"bed_temp", 60},
                {"prep", loaded},
                {"load", loaded},
                {"tool_loaded", false},
                {"multi_color_hexes", json::array()},
                {"status", loaded ? "Ready" : "None"}};
}

// Shape of AFC's printer/afc/status webhook: {"status:": {"AFC": {...}}}.
json webhook_fixture()
{
    json afc;
    afc["Turtle_1"]["system"] = {{"type", "Box_Turtle"}, {"hub_loaded", false}};
    afc["Turtle_1"]["lane1"]  = lane_status("lane1", "Turtle_1", 1, {"T0"}, "extruder", "#ff0000", "PLA", true);
    afc["Turtle_1"]["lane2"]  = lane_status("lane2", "Turtle_1", 2, {"T1"}, "extruder", "#00FF00", "PETG", true, 40);
    afc["Bambu_AMS_1"]["system"] = {{"type", "BambuAMS"}, {"hub_loaded", true}};
    afc["Bambu_AMS_1"]["lane5"]  = lane_status("lane5", "Bambu_AMS_1", 1, {"T4"}, "extruder1", "0000FF", "PLA", true);
    afc["Bambu_AMS_1"]["lane6"]  = lane_status("lane6", "Bambu_AMS_1", 2, {"T5"}, "extruder1", "", "", false);
    afc["system"] = {{"version", "1.0.30"},
                     {"current_load", "lane5"},
                     {"spoolman", "http://spoolman:7912"},
                     {"extruders", {{"extruder", json::object()}, {"extruder1", json::object()}}}};
    return json{{"status:", {{"AFC", afc}}}};
}

} // namespace

TEST_CASE("AFC webhook status parses units and lanes", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    CHECK(s.present);
    CHECK(s.version == "1.0.30");
    CHECK(s.spoolman);
    CHECK(s.spoolman_url == "http://spoolman:7912");
    CHECK(s.current_lane == "lane5");
    CHECK(s.units.size() == 2);
    REQUIRE(s.lanes.size() == 4);

    const AFCProtocol::Lane *l1 = s.find_lane("lane1");
    REQUIRE(l1 != nullptr);
    CHECK(l1->unit == "Turtle_1");
    CHECK(l1->unit_type == "Box_Turtle");
    CHECK(l1->color == "#FF0000");
    CHECK(l1->spool_id == 12);
    CHECK(l1->extruder_index == 0);
    CHECK(l1->has_filament());

    const AFCProtocol::Lane *l5 = s.find_lane("lane5");
    REQUIRE(l5 != nullptr);
    CHECK(l5->color == "#0000FF");
    CHECK(l5->extruder_index == 1);
    CHECK(l5->unit_type == "BambuAMS");

    CHECK(s.lane_for_tool(4) == l5);
    CHECK(s.lane_for_tool(3) == nullptr);
    CHECK(s.max_tool() == 5);
}

TEST_CASE("a lane AFC says is in the tool counts as loaded even when tool_loaded is false", "[AFCProtocol]")
{
    // A U1 toolhead lane after a reset: its extruder holds it, tool_loaded still reads false.
    json fixture = webhook_fixture();
    json &afc = fixture["status:"]["AFC"];
    afc["Turtle_1"]["lane1"]["filament_status"] = "In Tool";
    afc["Turtle_1"]["lane2"]["filament_status"] = "Ready";
    afc["Bambu_AMS_1"]["lane5"]["filament_status"] = "In Tool";
    afc["Bambu_AMS_1"]["lane5"]["tool_loaded"] = true;

    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(fixture, s));
    const AFCProtocol::Lane *l1 = s.find_lane("lane1");
    const AFCProtocol::Lane *l2 = s.find_lane("lane2");
    const AFCProtocol::Lane *l5 = s.find_lane("lane5");
    const AFCProtocol::Lane *l6 = s.find_lane("lane6");
    REQUIRE(l1 != nullptr);
    REQUIRE(l2 != nullptr);
    REQUIRE(l5 != nullptr);
    REQUIRE(l6 != nullptr);
    CHECK(l1->tool_loaded);
    CHECK_FALSE(l2->tool_loaded);
    CHECK(l5->tool_loaded);
    CHECK_FALSE(l6->tool_loaded);
    CHECK(AFCProtocol::to_json(s)["lanes"][0]["tool_loaded"] == true);
}

TEST_CASE("AFC status without the webhook wrapper and junk input", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    const json fixture = webhook_fixture();
    CHECK(AFCProtocol::parse_status_endpoint(json{{"AFC", fixture["status:"]["AFC"]}}, s));
    CHECK(s.lanes.size() == 4);
    CHECK_FALSE(AFCProtocol::parse_status_endpoint(json::array(), s));
    CHECK_FALSE(AFCProtocol::parse_status_endpoint(json{{"status", {{"toolhead", json::object()}}}}, s));
    CHECK_FALSE(s.present);
}

TEST_CASE("multiple tool mapping comes from the AFC object, which the webhook leaves out", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    CHECK_FALSE(s.multiple_mapping);
    CHECK(AFCProtocol::afc_object_query()["AFC"] == json{"multiple_tool_mapping"});

    AFCProtocol::merge_afc_object(json{{"eventtime", 1.0}, {"status", {{"AFC", {{"multiple_tool_mapping", true}}}}}}, s);
    CHECK(s.multiple_mapping);
    CHECK(s.lanes.size() == 4);

    // An answer without the field (older AFC) or without AFC leaves it alone.
    AFCProtocol::merge_afc_object(json{{"status", {{"AFC", json::object()}}}}, s);
    CHECK(s.multiple_mapping);
    AFCProtocol::merge_afc_object(json::array(), s);
    CHECK(s.multiple_mapping);
    AFCProtocol::merge_afc_object(json{{"AFC", {{"multiple_tool_mapping", false}}}}, s);
    CHECK_FALSE(s.multiple_mapping);
}

TEST_CASE("a U1 reports its own print switches beside AFC", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    CHECK_FALSE(s.u1_print.present);
    CHECK(AFCProtocol::to_json(s)["u1_print"].is_null());
    CHECK(AFCProtocol::afc_object_query()["print_task_config"] ==
          json{"auto_bed_leveling", "flow_calibrate", "time_lapse_camera"});

    // Another Klipper printer has no print_task_config: nothing changes.
    AFCProtocol::merge_afc_object(json{{"status", {{"AFC", {{"multiple_tool_mapping", true}}}}}}, s);
    CHECK_FALSE(s.u1_print.present);

    AFCProtocol::merge_afc_object(json{{"status",
                                        {{"AFC", json::object()},
                                         {"print_task_config",
                                          {{"auto_bed_leveling", true}, {"flow_calibrate", false}, {"time_lapse_camera", true}}}}}},
                                  s);
    CHECK(s.u1_print.present);
    CHECK(s.u1_print.bed_level);
    CHECK_FALSE(s.u1_print.flow_calibrate);
    CHECK(s.u1_print.time_lapse);
    CHECK(AFCProtocol::to_json(s)["u1_print"] == json{{"bed_level", true}, {"flow_calibrate", false}, {"time_lapse_camera", true}});

    AFCProtocol::U1PrintOptions options;
    options.flow_calibrate = true;
    CHECK(AFCProtocol::u1_print_options_gcode(options) == "SET_PRINT_PREFERENCE KEY=bed_level VALUE=0\n"
                                                           "SET_PRINT_PREFERENCE KEY=flow_calibrate VALUE=1\n"
                                                           "SET_PRINT_PREFERENCE KEY=time_lapse_camera VALUE=0");
}

TEST_CASE("AFC objects fallback finds lane objects and units", "[AFCProtocol]")
{
    const json list = {{"objects", {"toolhead", "AFC", "AFC_BoxTurtle Turtle_1", "AFC_stepper lane1", "AFC_lane lane2",
                                    "AFC_hub Turtle_1", "AFC_extruder extruder", "AFC_stepper lane9"}}};
    const auto names = AFCProtocol::lane_object_names(list, {"lane1", "lane2"});
    CHECK(names == std::vector<std::string>{"AFC_stepper lane1", "AFC_lane lane2"});

    json status;
    status["AFC"] = {{"units", {"Box_Turtle Turtle_1"}},
                     {"lanes", {"lane1", "lane2"}},
                     {"current_load", nullptr},
                     {"spoolman", nullptr},
                     {"multiple_tool_mapping", true},
                     {"extruders", {"extruder"}}};
    status["AFC_stepper lane1"] = lane_status("lane1", "Turtle_1", 1, {"T0", "T7"}, "extruder", "#123456", "ASA", true);
    status["AFC_lane lane2"]    = lane_status("lane2", "Turtle_1", 2, {"T1"}, "extruder", "#654321", "PLA", false);

    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_objects_status(json{{"status", status}}, s));
    CHECK_FALSE(s.spoolman);
    CHECK(s.multiple_mapping);
    REQUIRE(s.units.size() == 1);
    CHECK(s.units[0].type == "Box_Turtle");
    CHECK(s.units[0].lanes == std::vector<std::string>{"lane1", "lane2"});
    CHECK(s.max_tool() == 7);
    CHECK(s.lane_for_tool(7)->name == "lane1");
}

TEST_CASE("lane_data fills blanks only", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    const json db = {{"namespace", "lane_data"},
                     {"value",
                      {{"T5", {{"vendor_name", "Bambu"}, {"material", "PETG"}, {"color", "#AABBCC"}, {"extruder_index", 1}}},
                       {"T0", {{"vendor_name", "Other"}, {"material", "ABS"}}},
                       {"bogus", {{"material", "X"}}}}}};
    AFCProtocol::merge_lane_data(db, s);
    CHECK(s.find_lane("lane6")->vendor == "Bambu");
    CHECK(s.find_lane("lane6")->material == "PETG");
    CHECK(s.find_lane("lane6")->color == "#AABBCC");
    // live status wins
    CHECK(s.find_lane("lane1")->vendor == "Polymaker");
    CHECK(s.find_lane("lane1")->material == "PLA");
}

TEST_CASE("lane_data keyed by lane name (older AFC) fills blanks too", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    REQUIRE(s.find_lane("lane6")->vendor.empty());
    const json db = {{"namespace", "lane_data"},
                     {"value", {{"lane6", {{"vendor_name", "Bambu"}, {"material", "PETG"}, {"lane", "5"}, {"spool_id", 12}}}}}};
    AFCProtocol::merge_lane_data(db, s);
    CHECK(s.find_lane("lane6")->vendor == "Bambu");
    CHECK(s.find_lane("lane6")->material == "PETG");
    CHECK(s.find_lane("lane6")->spool_id == 12);
}

TEST_CASE("tool slots are contiguous from T0 and empty lanes read NONE", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    const auto slots = AFCProtocol::tool_slots(s);
    REQUIRE(slots.size() == 6);
    CHECK(slots[0].type == "PLA");
    CHECK(slots[0].color == "#FF0000");
    CHECK(slots[0].vendor == "Polymaker");
    CHECK(slots[1].type == "PETG");
    CHECK_FALSE(slots[2].mapped);
    CHECK(slots[2].type == "NONE");
    CHECK(slots[4].lane == "lane5");
    CHECK(slots[4].extruder_index == 1);
    CHECK(slots[5].mapped);
    CHECK_FALSE(slots[5].has_filament);
    CHECK(slots[5].type == "NONE");

    CHECK(AFCProtocol::filament_display_name("Polymaker", "PLA", "NONE") == "Polymaker PLA");
    CHECK(AFCProtocol::filament_display_name("Bambu", "PLA", "Matte") == "Bambu PLA Matte");
    CHECK(AFCProtocol::filament_display_name("Bambu", "PLA", "Support") == "Bambu Support For PLA");
}

TEST_CASE("lane edits build safe AFC commands", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    std::vector<std::string> g;
    std::string              err;
    REQUIRE(AFCProtocol::build_lane_edit_gcode(
        s, "lane2", json{{"color", "#a1b2c3"}, {"material", "PETG"}, {"weight", 812.5}, {"spool_id", 0}, {"nozzle_temp", 240}}, g, err));
    // Spoolman id first so the explicit values are not overwritten by Spoolman's.
    CHECK(g == std::vector<std::string>{"SET_SPOOL_ID LANE=lane2 SPOOL_ID=", "SET_MATERIAL LANE=lane2 MATERIAL=PETG",
                                        "SET_COLOR LANE=lane2 COLOR=A1B2C3", "SET_WEIGHT LANE=lane2 WEIGHT=812.5",
                                        "AFC_SET_SPOOL_TEMP LANE=lane2 EXTRUDER_TEMP=240"});

    g.clear();
    CHECK_FALSE(AFCProtocol::build_lane_edit_gcode(s, "lane2", json{{"material", "PLA\nM112"}}, g, err));
    CHECK_FALSE(AFCProtocol::build_lane_edit_gcode(s, "lane2", json{{"material", "PLA ;x"}}, g, err));
    CHECK_FALSE(AFCProtocol::build_lane_edit_gcode(s, "lane2 X=1", json{{"weight", 1}}, g, err));
    CHECK_FALSE(AFCProtocol::build_lane_edit_gcode(s, "lane42", json{{"weight", 1}}, g, err));
    CHECK_FALSE(AFCProtocol::build_lane_edit_gcode(s, "lane2", json{{"color", "red"}}, g, err));
    CHECK_FALSE(AFCProtocol::build_lane_edit_gcode(s, "lane2", json{{"map", "T5 M112"}}, g, err));
    CHECK_FALSE(AFCProtocol::build_lane_edit_gcode(s, "lane2", json{{"run", "M112"}}, g, err));
    CHECK(g.empty());
}

TEST_CASE("tool mapping replays SET_MAP swaps", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    std::vector<std::string> g;
    std::string              err;
    // T0 already on lane1: skipped. T1 -> lane5 swaps (lane2 takes T4). T4 -> lane2 is then in place.
    REQUIRE(AFCProtocol::build_tool_mapping_gcode(s, {{0, "lane1"}, {1, "lane5"}, {4, "lane2"}}, g, err));
    CHECK(g == std::vector<std::string>{"SET_MAP LANE=lane5 MAP=T1"});

    g.clear();
    CHECK_FALSE(AFCProtocol::build_tool_mapping_gcode(s, {{0, "lane5"}, {1, "lane5"}}, g, err));
    CHECK_FALSE(AFCProtocol::build_tool_mapping_gcode(s, {{9, "lane5"}}, g, err));
    CHECK_FALSE(AFCProtocol::build_tool_mapping_gcode(s, {{0, "nope"}}, g, err));
    CHECK(g.empty());
}

TEST_CASE("suggested mapping prefers material, then color, then current T(n)", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    std::vector<AFCProtocol::ProjectFilament> f(4);
    f[0] = {0, "#FE0101", "PLA", 10, true};   // lane1 already T0, red PLA: keep
    f[1] = {1, "#0000F0", "PLA", 10, true};   // blue PLA -> lane5
    f[2] = {2, "#00FF00", "PETG", 100, true}; // green PETG; lane2 has only 40 g but is the only PETG
    f[3] = {3, "#FFFFFF", "PLA", 0, false};   // not used on this plate
    const auto m = AFCProtocol::suggest_mapping(s, f, nullptr);
    CHECK(m == std::vector<std::string>{"lane1", "lane5", "lane2", ""});

    CHECK(AFCProtocol::tool_number("T12") == 12);
    CHECK(AFCProtocol::tool_number("T") == -1);
    CHECK(AFCProtocol::tool_number("lane1") == -1);
    CHECK(AFCProtocol::extruder_index_from_name("extruder3") == 3);
    CHECK(AFCProtocol::extruder_index_from_name("extruder_stepper x") == -1);
    CHECK(AFCProtocol::extruder_index_from_name("extruder") == 0);
    CHECK(AFCProtocol::extruder_index_from_name("e0") == 0);
    CHECK(AFCProtocol::extruder_index_from_name("e3") == 3);
    CHECK(AFCProtocol::extruder_index_from_name("e") == -1);
    CHECK(AFCProtocol::extruder_index_from_name("ebb") == -1);
    CHECK(AFCProtocol::normalize_color("ff00aaFF") == "#FF00AA");
    CHECK(AFCProtocol::normalize_color("#12345") == "");
}

TEST_CASE("lane actions check the lane state", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    std::string g, err;
    CHECK(AFCProtocol::build_lane_action_gcode(s, "lane1", "load", g, err));
    CHECK(g == "CHANGE_TOOL LANE=lane1");
    CHECK(AFCProtocol::build_lane_action_gcode(s, "lane1", "eject", g, err));
    CHECK(g == "LANE_UNLOAD LANE=lane1");
    CHECK_FALSE(AFCProtocol::build_lane_action_gcode(s, "lane6", "load", g, err));   // empty
    CHECK_FALSE(AFCProtocol::build_lane_action_gcode(s, "lane1", "unload", g, err)); // not in a toolhead
    CHECK_FALSE(AFCProtocol::build_lane_action_gcode(s, "lane1", "purge", g, err));
    CHECK_FALSE(AFCProtocol::build_lane_action_gcode(s, "lane1;M112", "load", g, err));
}

TEST_CASE("webhook units and lanes come out in natural order", "[AFCProtocol]")
{
    // Printer 1's layout: JSON key order would give AMS_1, Ace2_1, Bambu..., lane10 before lane8.
    json afc;
    afc["Ace2_1"]["system"]   = {{"type", "ACE2"}};
    afc["Ace2_1"]["lane0"]    = lane_status("lane0", "Ace2_1", 1, {"T0"}, "extruder", "", "PLA", true);
    afc["AMS_1"]["system"]    = {{"type", "OpenAMS"}};
    afc["AMS_1"]["lane4"]     = lane_status("lane4", "AMS_1", 1, {"T4"}, "extruder4", "", "", false);
    afc["Turtle_1"]["system"] = {{"type", "Box_Turtle"}};
    for (int i : {8, 9, 10, 11})
        afc["Turtle_1"]["lane" + std::to_string(i)] =
            lane_status("lane" + std::to_string(i), "Turtle_1", i - 7, {"T" + std::to_string(i)}, "extruder5", "", "", false);
    afc["Bambu_AMS_1"]["system"]    = {{"type", "BambuAMS"}};
    afc["Bambu_AMS_1"]["lane12"]    = lane_status("lane12", "Bambu_AMS_1", 1, {"T12"}, "extruder", "#0A2989", "PLA", true);
    afc["Bambu_AMS_2"]["system"]    = {{"type", "BambuAMS"}}; // unclaimed BridgeBox spare
    afc["Tools"]["system"]          = {{"type", "Toolchanger"}};
    afc["system"]                   = {{"version", "1.2.7"}};

    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(json{{"status:", {{"AFC", afc}}}}, s));
    std::vector<std::string> units;
    for (const auto &u : s.units)
        units.push_back(u.name);
    CHECK(units == std::vector<std::string>{"Ace2_1", "AMS_1", "Turtle_1", "Bambu_AMS_1", "Bambu_AMS_2", "Tools"});
    CHECK(s.units[2].lanes == std::vector<std::string>{"lane8", "lane9", "lane10", "lane11"});
    REQUIRE(s.lanes.size() == 7);
    CHECK(s.lanes[2].name == "lane8");
    CHECK(s.lanes[5].name == "lane11");
    CHECK(s.lanes[6].name == "lane12");
}

TEST_CASE("dryers are read from ACE and Bambu unit objects", "[AFCProtocol]")
{
    json afc;
    afc["Ace2_1"]["system"]         = {{"type", "ACE2"}};
    afc["Ace2_1"]["lane0"]          = lane_status("lane0", "Ace2_1", 1, {"T0"}, "extruder", "", "PLA", true);
    afc["Bambu_AMS_1"]["system"]    = {{"type", "BambuAMS"}};
    afc["Bambu_AMS_1"]["lane12"]    = lane_status("lane12", "Bambu_AMS_1", 1, {"T12"}, "extruder", "", "PLA", true);
    afc["Bambu_AMS_HT_1"]["system"] = {{"type", "BambuAMS"}};
    afc["Bambu_AMS_HT_1"]["lane20"] = lane_status("lane20", "Bambu_AMS_HT_1", 1, {"T20"}, "extruder", "", "PA", true);
    afc["Turtle_1"]["system"]       = {{"type", "Box_Turtle"}};
    afc["Turtle_1"]["lane8"]        = lane_status("lane8", "Turtle_1", 1, {"T8"}, "extruder5", "", "", false);
    afc["Bambu_AMS_2"]["system"]    = {{"type", "BambuAMS"}}; // no lanes: not queried

    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(json{{"status:", {{"AFC", afc}}}}, s));

    const json q = AFCProtocol::dryer_objects_query(s);
    CHECK(q.size() == 3);
    CHECK(q.contains("AFC_ACE2 Ace2_1"));
    CHECK(q.contains("AFC_BambuAMS Bambu_AMS_1"));
    CHECK(q.contains("AFC_BambuAMS Bambu_AMS_HT_1"));

    json status;
    status["AFC_ACE2 Ace2_1"] = {{"ace_connected", true},     {"ace_temp", 41.5},          {"ace_humidity", 18},
                                 {"ace_dryer", "keeping"},    {"ace_dryer_target", 55},    {"ace_dryer_remain", 5400},
                                 {"ace_dryer_duration", 14400}, {"ace_dry_rotate", false}};
    status["AFC_BambuAMS Bambu_AMS_1"] = {{"has_heater", true},  {"dry_max_temp", 65}, {"drying", false},
                                          {"temperature", 27.0}, {"humidity", 31.0},   {"ams_model", "ams2"},
                                          {"bridge_online", true}, {"dry_error", ""}};
    status["AFC_BambuAMS Bambu_AMS_HT_1"] = {{"has_heater", true}, {"dry_max_temp", 85}, {"drying", true},
                                             {"dry_target", 80},   {"dry_remaining", 3600}, {"dry_minutes", 720},
                                             {"ams_model", "ht"},  {"dry_note", "spools turning"}, {"dry_rotate", true}};
    AFCProtocol::merge_dryer_status(json{{"status", status}}, s);

    auto unit = [&](const std::string &name) -> const AFCProtocol::Unit & {
        for (const auto &u : s.units)
            if (u.name == name)
                return u;
        FAIL("no unit " << name);
        return s.units.front();
    };
    REQUIRE(unit("Ace2_1").has_dryer);
    CHECK(unit("Ace2_1").dryer.kind == "ace");
    CHECK(unit("Ace2_1").dryer.drying);
    CHECK(unit("Ace2_1").dryer.max_temp == 70);
    CHECK(unit("Ace2_1").dryer.duration_s == 14400);
    CHECK(unit("Ace2_1").dryer.remaining_s == 5400);
    REQUIRE(unit("Bambu_AMS_1").has_dryer);
    CHECK_FALSE(unit("Bambu_AMS_1").dryer.drying);
    CHECK(unit("Bambu_AMS_1").dryer.max_temp == 65);
    REQUIRE(unit("Bambu_AMS_HT_1").has_dryer);
    CHECK(unit("Bambu_AMS_HT_1").dryer.drying);
    CHECK(unit("Bambu_AMS_HT_1").dryer.duration_s == 720 * 60);
    CHECK(unit("Bambu_AMS_HT_1").dryer.note == "spools turning");
    CHECK_FALSE(unit("Turtle_1").has_dryer);

    // A Bambu AMS without a heater (AMS 1) has no dryer.
    AFCProtocol::merge_dryer_status(json{{"AFC_BambuAMS Bambu_AMS_1", {{"has_heater", false}}}}, s);
    CHECK(unit("Bambu_AMS_1").has_dryer); // unchanged: no-heater rows are skipped, not cleared

    const json j = AFCProtocol::to_json(s);
    CHECK(j["units"][0]["dryer"]["kind"] == "ace");
}

TEST_CASE("dry commands are validated per unit", "[AFCProtocol]")
{
    json afc;
    afc["Ace2_1"]["system"]      = {{"type", "ACE2"}};
    afc["Ace2_1"]["lane0"]       = lane_status("lane0", "Ace2_1", 1, {"T0"}, "extruder", "", "PLA", true);
    afc["Bambu_AMS_1"]["system"] = {{"type", "BambuAMS"}};
    afc["Bambu_AMS_1"]["lane12"] = lane_status("lane12", "Bambu_AMS_1", 1, {"T12"}, "extruder", "", "PLA", true);
    afc["Turtle_1"]["system"]    = {{"type", "Box_Turtle"}};
    afc["Turtle_1"]["lane8"]     = lane_status("lane8", "Turtle_1", 1, {"T8"}, "extruder5", "", "", false);
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(json{{"status:", {{"AFC", afc}}}}, s));
    json status;
    status["AFC_ACE2 Ace2_1"]          = {{"ace_dryer", "stop"}};
    status["AFC_BambuAMS Bambu_AMS_1"] = {{"has_heater", true}, {"dry_max_temp", 65}};
    AFCProtocol::merge_dryer_status(status, s);

    std::string g, err;
    CHECK(AFCProtocol::build_dry_gcode(s, "Ace2_1", json{{"action", "start"}, {"temp", 55}, {"minutes", 240}, {"rotate", true}}, g, err));
    CHECK(g == "ACE_DRY UNIT=Ace2_1 TEMP=55 DURATION=240 ROTATE=1");
    CHECK(AFCProtocol::build_dry_gcode(s, "Ace2_1", json{{"action", "stop"}}, g, err));
    CHECK(g == "ACE_DRY_STOP UNIT=Ace2_1");
    CHECK(AFCProtocol::build_dry_gcode(s, "Bambu_AMS_1", json{{"action", "start"}, {"temp", 65}, {"minutes", 480}}, g, err));
    CHECK(g == "AFC_BAMBU_HEATER_START UNIT=Bambu_AMS_1 TEMP=65 TIME=480 ROTATE=0");
    CHECK(AFCProtocol::build_dry_gcode(s, "Bambu_AMS_1", json{{"action", "stop"}}, g, err));
    CHECK(g == "AFC_BAMBU_HEATER_STOP UNIT=Bambu_AMS_1");

    CHECK_FALSE(AFCProtocol::build_dry_gcode(s, "Bambu_AMS_1", json{{"action", "start"}, {"temp", 80}, {"minutes", 480}}, g, err)); // over 65
    CHECK_FALSE(AFCProtocol::build_dry_gcode(s, "Ace2_1", json{{"action", "start"}, {"temp", 55}, {"minutes", 0}}, g, err));
    CHECK_FALSE(AFCProtocol::build_dry_gcode(s, "Ace2_1", json{{"action", "start"}, {"temp", "55;M112"}, {"minutes", 60}}, g, err));
    CHECK_FALSE(AFCProtocol::build_dry_gcode(s, "Turtle_1", json{{"action", "start"}, {"temp", 50}, {"minutes", 60}}, g, err)); // no dryer
    CHECK_FALSE(AFCProtocol::build_dry_gcode(s, "Ace2_1;M112", json{{"action", "stop"}}, g, err));
    CHECK_FALSE(AFCProtocol::build_dry_gcode(s, "Ace2_1", json{{"action", "boost"}}, g, err));
}

TEST_CASE("flow K is read from the Spoolman spool's extra field", "[AFCProtocol]")
{
    // Spoolman JSON-encodes extra values, as AFC autocal writes them.
    const json spool = {{"id", 12}, {"extra", {{"flow_k", "0.032"}, {"card_uids", "\"04A1B2\""}}}};
    CHECK_THAT(AFCProtocol::spool_flow_k(spool), WithinAbs(0.032, 1e-9));

    SECTION("through Moonraker's Spoolman proxy") {
        CHECK_THAT(AFCProtocol::spool_flow_k(json{{"response", spool}, {"error", nullptr}}), WithinAbs(0.032, 1e-9));
        CHECK(AFCProtocol::spool_flow_k(json{{"response", nullptr}, {"error", {{"status_code", 404}}}}) == -1);
    }
    SECTION("a plain number is taken as it is") {
        CHECK_THAT(AFCProtocol::spool_flow_k(json{{"extra", {{"flow_k", 0.045}}}}), WithinAbs(0.045, 1e-9));
    }
    SECTION("no K, or one that is not a pressure advance value") {
        CHECK(AFCProtocol::spool_flow_k(json{{"id", 3}, {"extra", json::object()}}) == -1);
        CHECK(AFCProtocol::spool_flow_k(json{{"id", 3}}) == -1);
        CHECK(AFCProtocol::spool_flow_k(json{{"extra", {{"flow_k", ""}}}}) == -1);
        CHECK(AFCProtocol::spool_flow_k(json{{"extra", {{"flow_k", "\"fast\""}}}}) == -1);
        CHECK(AFCProtocol::spool_flow_k(json{{"extra", {{"flow_k", "-0.1"}}}}) == -1);
        CHECK(AFCProtocol::spool_flow_k(json()) == -1);
    }
}

TEST_CASE("lanes carry flow K to the page only when the spool has one", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    s.present = true;
    s.lanes.resize(2);
    s.lanes[0].name   = "lane1";
    s.lanes[0].flow_k = 0.032;
    s.lanes[1].name   = "lane2";
    const json j = AFCProtocol::to_json(s);
    CHECK_THAT(j["lanes"][0]["flow_k"].get<double>(), WithinAbs(0.032, 1e-9));
    CHECK(j["lanes"][1]["flow_k"].is_null());
}

TEST_CASE("a slot's spool pressure advance replaces its preset value", "[AFCProtocol]")
{
    std::vector<double>        pa     = {0.02, 0.04, 0.05};
    std::vector<unsigned char> enable = {0, 1, 0};

    SECTION("one entry per slot") {
        CHECK(AFCProtocol::apply_spool_pressure_advance({0.032, -1, 0.028}, {}, pa, enable));
        CHECK_THAT(pa[0], WithinAbs(0.032, 1e-9));
        CHECK_THAT(pa[1], WithinAbs(0.04, 1e-9));
        CHECK_THAT(pa[2], WithinAbs(0.028, 1e-9));
        CHECK(enable == std::vector<unsigned char>{1, 1, 1});
    }
    SECTION("every flow variant of a slot") {
        // Slot 0 declares standard and high flow, slot 1 only standard.
        CHECK(AFCProtocol::apply_spool_pressure_advance({0.032, 0.021}, {2, 1}, pa, enable));
        CHECK_THAT(pa[0], WithinAbs(0.032, 1e-9));
        CHECK_THAT(pa[1], WithinAbs(0.032, 1e-9));
        CHECK_THAT(pa[2], WithinAbs(0.021, 1e-9));
        CHECK(enable == std::vector<unsigned char>{1, 1, 1});
    }
    SECTION("no spool values change nothing") {
        CHECK_FALSE(AFCProtocol::apply_spool_pressure_advance({-1, -1, -1}, {}, pa, enable));
        CHECK_FALSE(AFCProtocol::apply_spool_pressure_advance({}, {}, pa, enable));
        CHECK(enable == std::vector<unsigned char>{0, 1, 0});
    }
    SECTION("more spool values than slots stay inside the vectors") {
        CHECK_FALSE(AFCProtocol::apply_spool_pressure_advance({-1, -1, -1, 0.03, 0.03}, {}, pa, enable));
        CHECK(pa.size() == 3);
        CHECK_THAT(pa[0], WithinAbs(0.02, 1e-9));
    }
}

TEST_CASE("tool slots carry the lane's flow K", "[AFCProtocol]")
{
    json afc;
    afc["Turtle_1"]["system"] = {{"type", "Box_Turtle"}};
    afc["Turtle_1"]["lane1"]  = lane_status("lane1", "Turtle_1", 1, {"T0"}, "extruder", "#FF0000", "PLA", true);
    afc["Turtle_1"]["lane2"]  = lane_status("lane2", "Turtle_1", 2, {"T1"}, "extruder", "#00FF00", "PETG", true);
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(json{{"status:", {{"AFC", afc}}}}, s));
    for (AFCProtocol::Lane &l : s.lanes)
        if (l.name == "lane1")
            l.flow_k = 0.032;
    const auto slots = AFCProtocol::tool_slots(s);
    REQUIRE(slots.size() == 2);
    CHECK_THAT(slots[0].flow_k, WithinAbs(0.032, 1e-9));
    CHECK(slots[1].flow_k == -1);
}

TEST_CASE("a spool's preset name comes from its vendor and filament name", "[AFCProtocol]")
{
    auto spool = [](const std::string &vendor, const std::string &filament, const std::string &material,
                    const std::string &sub_type = "") {
        AFCProtocol::Lane l;
        l.vendor        = vendor;
        l.filament_name = filament;
        l.material      = material;
        l.sub_type      = sub_type;
        return l;
    };
    CHECK(AFCProtocol::spool_preset_name(spool("Polymaker", "PolyLite PLA", "PLA")) == "Polymaker PolyLite PLA");
    // A filament name that already carries the vendor gets it once, spelled as the vendor is.
    CHECK(AFCProtocol::spool_preset_name(spool("Polymaker", "polymaker PolyLite PLA", "PLA")) == "Polymaker PolyLite PLA");
    // The material goes in when the filament name lacks it.
    CHECK(AFCProtocol::spool_preset_name(spool("Prusament", "Galaxy Black", "PLA")) == "Prusament PLA Galaxy Black");
    CHECK(AFCProtocol::spool_preset_name(spool("Overture", "Tough PLA-CF", "PLA")) == "Overture Tough PLA-CF");
    // No filament name: vendor and material, like the U1 sync names it.
    CHECK(AFCProtocol::spool_preset_name(spool("Elegoo", "", "PETG", "Translucent")) == "Elegoo PETG Translucent");
    CHECK(AFCProtocol::spool_preset_name(spool("Elegoo", "Elegoo", "PETG")) == "Elegoo PETG");
    // Nothing to tell the spool apart from a generic one.
    CHECK(AFCProtocol::spool_preset_name(spool("", "PolyLite PLA", "PLA")).empty());
    CHECK(AFCProtocol::spool_preset_name(spool("Generic", "PLA", "PLA")).empty());
    CHECK(AFCProtocol::spool_preset_name(spool("NONE", "", "PLA")).empty());
    CHECK(AFCProtocol::spool_preset_name(spool("Elegoo", "", "")).empty());
    // File name and " @" characters are replaced, spaces tidied.
    CHECK(AFCProtocol::spool_preset_name(spool(" eSUN ", "PLA+  Silk/Gold @2kg", "PLA")) == "eSUN PLA+ Silk-Gold -2kg");
}

TEST_CASE("a spool's preset may carry its filament name without the vendor", "[AFCProtocol]")
{
    AFCProtocol::Lane l;
    l.vendor        = "Polymaker";
    l.filament_name = "Polymaker PolyLite PLA";
    l.material      = "PLA";
    CHECK(AFCProtocol::spool_preset_aliases(l) == std::vector<std::string>{"Polymaker PolyLite PLA", "PolyLite PLA"});
    // A bare material name would match a stranger's preset, so it is left out.
    l.filament_name = "PLA";
    CHECK(AFCProtocol::spool_preset_aliases(l) == std::vector<std::string>{"Polymaker PLA"});
    l.filament_name = "";
    CHECK(AFCProtocol::spool_preset_aliases(l) == std::vector<std::string>{"Polymaker PLA"});
    l.vendor = "";
    CHECK(AFCProtocol::spool_preset_aliases(l).empty());
}

TEST_CASE("the U1's LAN addresses come from system_info, wired first", "[AFCProtocol]")
{
    const json info = {{"system_info",
                        {{"network",
                          {{"wlan0",
                            {{"ip_addresses",
                              {{{"family", "ipv6"}, {"address", "fe80::1"}},
                               {{"family", "ipv4"}, {"address", "192.168.4.36"}}}}}},
                           {"lo", {{"ip_addresses", {{{"family", "ipv4"}, {"address", "127.0.0.1"}}}}}},
                           {"eth0",
                            {{"ip_addresses",
                              {{{"family", "ipv4"}, {"address", "169.254.3.4"}},
                               {{"family", "ipv4"}, {"address", "10.0.0.5"}}}}}}}}}}};
    CHECK(AFCProtocol::lan_addresses(info) == std::vector<std::string>{"10.0.0.5", "192.168.4.36"});
    CHECK(AFCProtocol::lan_addresses(info["system_info"]) == std::vector<std::string>{"10.0.0.5", "192.168.4.36"});
    CHECK(AFCProtocol::lan_addresses(json::object()).empty());
    CHECK(AFCProtocol::lan_addresses(json()).empty());
    CHECK(AFCProtocol::lan_addresses(json{{"network", {{"wlan0", "down"}}}}).empty());
}

TEST_CASE("the U1 map table pairs each used T(n) with its lane's toolhead", "[AFCProtocol]")
{
    AFCProtocol::Snapshot s;
    REQUIRE(AFCProtocol::parse_status_endpoint(webhook_fixture(), s));
    std::vector<AFCProtocol::ProjectFilament> f(3);
    f[0] = {0, "#FF0000", "PLA", 10, true};  // lane1 on extruder
    f[1] = {1, "#00FF00", "PETG", 0, false}; // not on this plate
    f[2] = {4, "#0000FF", "PLA", 10, true};  // lane5 on extruder1

    std::vector<std::pair<int, int>> table;
    std::string                      error;
    REQUIRE(AFCProtocol::u1_map_table(s, f, table, error));
    CHECK(table == std::vector<std::pair<int, int>>{{0, 0}, {4, 1}});

    f.push_back({7, "#FFFFFF", "PLA", 10, true}); // nothing maps T7
    CHECK_FALSE(AFCProtocol::u1_map_table(s, f, table, error));
    CHECK(error == "No AFC lane is mapped to T7");

    AFCProtocol::U1PrintOptions options;
    options.bed_level  = true;
    options.time_lapse = true;
    const json params  = AFCProtocol::u1_local_print_params("plate_1.gcode", options, {{5, 0}, {1, 2}});
    const json expected = json::parse(R"({"type":"gcode","path":"plate_1.gcode","print_plate":1,
        "options":{"bed_level":1,"flow_calibrate":0,"time_lapse_camera":1,"map_table":[[5,0],[1,2]]},
        "extra_info":{"print_task_id":""}})");
    CHECK(params == expected);
}

TEST_CASE("the uploaded file's path is read from Moonraker's upload answer", "[AFCProtocol]")
{
    const json answer = {{"item", {{"path", "sub/plate_1.gcode"}, {"root", "gcodes"}}}, {"action", "create_file"}};
    CHECK(AFCProtocol::uploaded_path(answer) == "sub/plate_1.gcode");
    CHECK(AFCProtocol::uploaded_path(json{{"result", answer}}) == "sub/plate_1.gcode");
    CHECK(AFCProtocol::uploaded_path(json::object()).empty());
    CHECK(AFCProtocol::uploaded_path(json("ok")).empty());
}
