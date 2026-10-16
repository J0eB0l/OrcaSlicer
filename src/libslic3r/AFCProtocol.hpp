#ifndef slic3r_AFCProtocol_hpp_
#define slic3r_AFCProtocol_hpp_

#include <nlohmann/json.hpp>

#include "AFCToolheads.hpp"

#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

/// Pure helpers for printers running AFC (Armored Turtle's Klipper filament
/// changer add-on: Box Turtle, Night Owl, ACE, OpenAMS, Bambu AMS via
/// BridgeBox, ...). AFC exposes every unit and lane through Klipper, so a
/// printer with AFC can have many more filaments than physical toolheads.
///
/// Data sources understood here, all reachable over Moonraker JSON-RPC:
///   - printer.afc.status           (AFC webhook: units -> lanes -> status)
///   - printer.objects.query        (AFC object + per-lane objects; fallback)
///   - server.database.get_item     (namespace "lane_data", keyed by T(n) or lane name)
///
/// No wxWidgets dependency, so everything here is unit-tested.
namespace AFCProtocol {

struct Lane
{
    std::string              name;           ///< Klipper lane name, e.g. "lane4"
    std::string              unit;           ///< unit name, e.g. "Turtle_1", "Bambu_AMS_1"
    std::string              unit_type;      ///< unit type, e.g. "Box_Turtle", "BambuAMS"
    int                      index{0};       ///< 1-based position inside the unit, as AFC reports it
    std::vector<std::string> maps;           ///< T(n) commands mapped to this lane, e.g. {"T4"}
    std::string              extruder;       ///< AFC extruder this lane feeds, e.g. "extruder1"
    int                      extruder_index{-1}; ///< physical toolhead index (extruder=0, extruder1=1, ...)
    std::string              color;          ///< "#RRGGBB", empty when unknown
    std::vector<std::string> multi_colors;   ///< "#RRGGBB" list for multi color spools
    std::string              material;
    std::string              sub_type;
    std::string              vendor;
    std::string              filament_name;
    int                      spool_id{0};    ///< Spoolman spool id, 0 = none
    double                   weight{0};      ///< remaining grams
    double                   initial_weight{0};
    double                   density{0};
    double                   diameter{0};
    int                      nozzle_temp{0};
    int                      bed_temp{0};
    bool                     prep{false};
    bool                     load{false};
    bool                     tool_loaded{false};
    std::string              status;         ///< AFC lane status text ("Ready", "Loaded", ...)
    std::string              td;             ///< TD-1 transmission distance, when scanned
    double                   flow_k{-1};     ///< pressure advance K saved on the Spoolman spool, -1 when none

    /// A lane holds filament when its prep or load sensor sees it.
    bool has_filament() const { return prep || load || tool_loaded; }
};

/// A unit's filament dryer (ACE / ACE 2 Pro, Bambu AMS 2 Pro and AMS HT
/// through BridgeBox). AFC's webhook carries none of this; it comes from the
/// unit's own Klipper object (see dryer_objects_query()).
struct Dryer
{
    std::string kind;              ///< "ace" or "bambu": which commands drive it
    std::string model;             ///< e.g. "ACE2", "ams2", "ht"
    bool        online{true};
    bool        drying{false};
    bool        rotate{false};     ///< spools turn while drying
    double      max_temp{0};       ///< hottest the unit allows, C
    double      target{0};         ///< target C while drying
    double      temperature{-1};   ///< inside the unit, C; -1 unknown
    double      humidity{-1};      ///< %RH; -1 unknown
    int         remaining_s{0};
    int         duration_s{0};
    std::string state;             ///< the unit's own dryer state text
    std::string note;              ///< why it cannot dry right now, or what it reported
};

struct Unit
{
    std::string              name;
    std::string              type;
    std::vector<std::string> lanes; ///< lane names in AFC order
    bool                     has_dryer{false};
    Dryer                    dryer;
};

/// The Snapmaker U1's own print-start switches, read from its
/// print_task_config object. Other printers have no such object, so
/// `present` stays false there.
struct U1PrintOptions
{
    bool present{false};
    bool bed_level{false};      ///< auto bed leveling before the print
    bool flow_calibrate{false}; ///< extrusion flow calibration
    bool time_lapse{false};     ///< time-lapse camera
};

struct Snapshot
{
    bool                     present{false};   ///< AFC answered
    std::string              version;
    bool                     spoolman{false};  ///< AFC has Spoolman configured
    std::string              spoolman_url;     ///< Spoolman base URL when AFC reports one
    bool                     multiple_mapping{false};
    std::string              current_lane;     ///< lane loaded in the active toolhead
    std::vector<std::string> extruders;
    std::vector<Unit>        units;
    std::vector<Lane>        lanes;            ///< every lane, unit order
    U1PrintOptions           u1_print;         ///< the U1's print switches as the printer has them now

    const Lane *find_lane(const std::string &name) const;
    /// Lane currently mapped to T(tool), or nullptr.
    const Lane *lane_for_tool(int tool) const;
    /// Highest T(n) mapped to any lane, -1 when none.
    int max_tool() const;
};

/// "T12" -> 12, anything else -> -1.
int tool_number(const std::string &map);

/// "extruder" -> 0, "extruder3" -> 3, AFC's "e1" (U1 toolheads) -> 1, anything else -> -1.
int extruder_index_from_name(const std::string &extruder);

/// "#ff00aa", "ff00aa", "FF00AAFF" -> "#FF00AA". Empty when not a color.
std::string normalize_color(const std::string &color);

/// Parse the result of the `printer.afc.status` JSON-RPC call (the AFC
/// webhook). Accepts the raw result, or a result wrapped in "status"/"status:"
/// (AFC sends the latter). Returns false when no AFC data is present.
bool parse_status_endpoint(const nlohmann::json &result, Snapshot &out);

/// Klipper AFC object fields the webhook leaves out, plus the U1's
/// print_task_config switches, for afc_object_query().
nlohmann::json afc_object_query();

/// Read a `printer.objects.query` status map for afc_object_query() into a
/// snapshot parsed from the webhook. A field the answer lacks keeps its value.
void merge_afc_object(const nlohmann::json &status, Snapshot &out);

/// G-code that sets the U1's print-start switches the way its own print
/// popup does on U1 firmware 1.3.0 and later, one line per switch:
/// "SET_PRINT_PREFERENCE KEY=bed_level VALUE=1\n...KEY=flow_calibrate...\n...KEY=time_lapse_camera...".
std::string u1_print_options_gcode(const U1PrintOptions &options);

/// The line a file uploaded to a U1 carries so it starts with these switches,
/// however it is started later: "SET_PRINT_PREFERENCES BED_LEVEL=1
/// FLOW_CALIBRATE=0 TIME_LAPSE_CAMERA=1 FORCE=1". The U1 refuses those
/// preferences while a print runs unless FORCE=1 is given.
std::string u1_file_print_options_gcode(const U1PrintOptions &options);

/// Names of the per-lane Klipper objects in a `printer.objects.list` result
/// ({"objects": [...]}) for the given lane names, e.g. "AFC_stepper lane1".
std::vector<std::string> lane_object_names(const nlohmann::json &objects_list, const std::vector<std::string> &lane_names);

/// Build a snapshot from a `printer.objects.query` status map holding the
/// "AFC" object and the per-lane objects named by lane_object_names().
bool parse_objects_status(const nlohmann::json &status, Snapshot &out);

/// Klipper objects to query for unit dryers: {"AFC_ACE2 Ace2_1": [fields], ...}
/// for every ACE / Bambu unit with lanes. Empty object when there are none.
nlohmann::json dryer_objects_query(const Snapshot &snapshot);

/// Read a `printer.objects.query` status map for the objects named by
/// dryer_objects_query() into the units' dryer info.
void merge_dryer_status(const nlohmann::json &status, Snapshot &out);

/// G-code to start or stop a unit's dryer. `request` holds action
/// ("start"/"stop"), temp (C), minutes and rotate (bool). Validated against
/// the unit's limits; returns false and sets `error` otherwise.
bool build_dry_gcode(const Snapshot &snapshot, const std::string &unit, const nlohmann::json &request, std::string &gcode,
                     std::string &error);

/// Fill blanks (vendor, temps, Spoolman id, extruder index, weights) from the
/// Moonraker "lane_data" database namespace ({"T0": {...}, ...} from newer AFC,
/// {"lane1": {...}, ...} from older releases). Values the live status already
/// carries are kept.
void merge_lane_data(const nlohmann::json &lane_data, Snapshot &out);

/// Flow K (pressure advance) saved on a Spoolman spool: the "flow_k" extra
/// field AFC's autocal writes. Takes the spool record, or Moonraker's Spoolman
/// proxy answer around it ({"response": spool, "error": ...}). -1 when the
/// spool has none.
double spool_flow_k(const nlohmann::json &spool);

/// Snapshot as JSON for the AFC web page and the cache.
nlohmann::json to_json(const Snapshot &snapshot);

/// One slicer filament slot per T(n): slot i is whatever lane T(i) maps to.
struct ToolSlot
{
    int                      tool{0};
    bool                     mapped{false};   ///< some lane carries T(tool)
    bool                     has_filament{false};
    std::string              lane;
    std::string              vendor;          ///< "Generic" when the lane has none
    std::string              type;            ///< material, "NONE" when unknown
    std::string              sub_type;        ///< "NONE" when unknown
    std::string              color;           ///< "#RRGGBB"
    std::vector<std::string> colors;
    int                      extruder_index{-1};
    double                   flow_k{-1};      ///< the spool's saved pressure advance, -1 when none
};

/// Slots T0..T(max_tool), contiguous; unmapped or empty tools get NONE.
std::vector<ToolSlot> tool_slots(const Snapshot &snapshot);

/// The same display name the U1 sync builds from print_task_config:
/// "<vendor> <type>[ <sub_type>]", or "<vendor> Support For <type>".
std::string filament_display_name(const std::string &vendor, const std::string &type, const std::string &sub_type);

/// Base name of the filament preset that stands for a lane's spool, the part
/// before " @<printer>": "<vendor> <filament name>" ("Polymaker PolyLite PLA"),
/// with the material put in when the filament name lacks it ("Prusament PLA
/// Galaxy Black"), or "<vendor> <material>" for a spool with no filament name.
/// Characters a preset file name cannot hold become '-'. Empty for a spool
/// with no vendor (or "Generic"): nothing tells it apart from a generic one.
std::string spool_preset_name(const Lane &lane);

/// Base names an existing filament preset may carry for this spool, best
/// first: spool_preset_name, then the filament name without the vendor when it
/// says more than the material (Snapmaker ships Polymaker's "PolyLite PLA").
/// Empty when spool_preset_name is.
std::vector<std::string> spool_preset_aliases(const Lane &lane);

/// Base name of the filament preset for a lane AFC knows without a vendor (no
/// Spoolman spool), built from what AFC holds for it: "AFC <material>[ <sub_type>]
/// <nozzle>C[ bed <bed>C]", e.g. "AFC PLA 210C bed 60C". Empty when AFC has no
/// material or no temperature for it, since the generic preset then says as much.
std::string afc_lane_preset_name(const Lane &lane);

/// Put each filament slot's spool pressure advance (`spool`, one value per
/// slot, -1 = keep the filament preset's) into a composed config's
/// pressure_advance and enable_pressure_advance vectors. Every flow variant of
/// a slot gets it: slot i owns `step_sizes[i]` entries (1 when missing), after
/// the entries of the slots before it. Returns whether any value changed.
bool apply_spool_pressure_advance(const std::vector<double> &spool, const std::vector<int> &step_sizes,
                                  std::vector<double> &pressure_advance, std::vector<unsigned char> &enable);

/// True for names AFC accepts as a lane (letters, digits, '_' and '-').
bool is_safe_lane_name(const std::string &name);

/// G-code for a lane edit coming from the AFC page. `changes` may hold
/// color ("#RRGGBB"), material, weight (g), spool_id (0 clears), map ("T5"),
/// nozzle_temp, bed_temp. Values are validated and never interpolated raw.
/// Returns false and sets `error` when anything is invalid; nothing is built then.
bool build_lane_edit_gcode(const Snapshot &snapshot, const std::string &lane, const nlohmann::json &changes,
                           std::vector<std::string> &gcode, std::string &error);

/// G-code that makes each T(tool) load the given lane, using AFC's SET_MAP
/// (which swaps with the lane that held the T(n) before). Pairs whose lane
/// already carries the tool are skipped. Two tools on one lane need AFC's
/// multiple mapping enabled.
bool build_tool_mapping_gcode(const Snapshot &snapshot, const std::vector<std::pair<int, std::string>> &tool_to_lane,
                              std::vector<std::string> &gcode, std::string &error);

/// Copies the G-code file `src` to `dst` with `lines` inserted before its first
/// command, after the slicer's comment header and thumbnails. False, with `error`
/// set, when a file cannot be read or written.
bool write_gcode_with_prelude(const std::string &src, const std::string &dst, const std::vector<std::string> &lines,
                              std::string &error);

/// Copies the G-code file `src` to `dst` with every line passed through
/// `purge`, so its tool changes carry the PURGE_LENGTH the print-time lane map
/// needs. False, with `error` set, when a file cannot be read or written.
bool write_gcode_with_purge_lengths(const std::string &src, const std::string &dst, PurgeLengths &purge, std::string &error);

/// G-code for a physical lane action from the device panel:
///   "load"   -> CHANGE_TOOL LANE=<lane>  (swap this lane into its toolhead)
///   "unload" -> TOOL_UNLOAD LANE=<lane>  (pull it back out of the toolhead)
///   "eject"  -> LANE_UNLOAD LANE=<lane>  (retract it out of the unit)
/// The caller must only send these while the printer is idle.
bool build_lane_action_gcode(const Snapshot &snapshot, const std::string &lane, const std::string &action,
                             std::string &gcode, std::string &error);

/// Project filament as the print popup sees it.
struct ProjectFilament
{
    int         id{0};      ///< the T number the G-code uses for it (slot_tool_numbers)
    std::string color;      ///< "#RRGGBB"
    std::string type;       ///< e.g. "PLA"
    double      grams{0};   ///< grams this print uses, 0 when unknown
    bool        used{true}; ///< the sliced plate uses this filament
    int         slot{0};    ///< 0-based filament slot
};

/// Color distance callback, lower = closer (the GUI passes CIEDE2000).
using ColorDistance = std::function<double(const std::string &, const std::string &)>;

/// Suggest a lane for every used project filament. Prefers the lane already
/// mapped to the filament's T(n) when its material and color agree, then the
/// same material with the closest color, then the closest color on any
/// loaded lane. Each lane is used once while enough loaded lanes exist.
/// Returns lane names aligned with `filaments` ("" = no suggestion).
std::vector<std::string> suggest_mapping(const Snapshot &snapshot, const std::vector<ProjectFilament> &filaments,
                                         const ColorDistance &distance);

/// Simple sRGB Euclidean distance, used when no better metric is available.
double rgb_distance(const std::string &a, const std::string &b);

// --- Sending a print straight to the U1 -------------------------------------

/// The printer's LAN IPv4 addresses from Moonraker's machine.system_info
/// answer (with or without its "system_info" wrapper), wired interfaces
/// first. Loopback and link-local addresses are left out.
std::vector<std::string> lan_addresses(const nlohmann::json &system_info);

/// The U1's map_table for this print: every used filament's slot paired with
/// the toolhead of the lane AFC maps its T number to. The U1 reads the file's
/// filaments by slot (its filament_used_g list, SM_PRINT_PREEXTRUDE_FILAMENT
/// INDEX=), while AFC runs the T commands. False, with `error` set, when a
/// used filament has no lane or its lane has no toolhead.
bool u1_map_table(const Snapshot &snapshot, const std::vector<ProjectFilament> &filaments,
                  std::vector<std::pair<int, int>> &table, std::string &error);

/// Params for the U1's server.files.start_local_print, as Snapmaker's own
/// print popup sends them on firmware 1.3.0 and later.
nlohmann::json u1_local_print_params(const std::string &path, const U1PrintOptions &options,
                                     const std::vector<std::pair<int, int>> &map_table);

/// The stored file's path from Moonraker's upload answer ("" when missing).
std::string uploaded_path(const nlohmann::json &answer);

// --- Sending through Snapmaker's cloud (away from the U1's network) ----------

/// Params for the U1's server.files.start_cloud_print: the U1 downloads what
/// Snapmaker's cloud holds at `url` and starts it, as Snapmaker's popup does.
nlohmann::json u1_cloud_print_params(const std::string &url, const std::string &checksum, size_t size,
                                     const U1PrintOptions &options, const std::vector<std::pair<int, int>> &map_table);

/// Params for server.files.start_cloud_print that only store the file on the U1
/// (auto_start off), as Snapmaker's popup sends an upload without a print.
nlohmann::json u1_cloud_upload_params(const std::string &url, const std::string &checksum, size_t size);

/// The cloud id of the device with serial `sn` in /user/device/list's data,
/// null when it is not there.
nlohmann::json cloud_device_id(const nlohmann::json &devices, const std::string &sn);

/// Where Snapmaker's cloud stored an upload, from /user/device/upload/completed's
/// data. Snapmaker's popup reads filePath first after a multipart upload and url
/// first after a single one.
std::string cloud_upload_location(const nlohmann::json &completed, bool multipart);

} // namespace AFCProtocol
} // namespace Slic3r

#endif // slic3r_AFCProtocol_hpp_
