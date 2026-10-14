#include <catch2/catch_test_macros.hpp>

#include "libslic3r/AFCProtocol.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;
using namespace Slic3r;

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

TEST_CASE("suggested mapping prefers material, then colour, then current T(n)", "[AFCProtocol]")
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
