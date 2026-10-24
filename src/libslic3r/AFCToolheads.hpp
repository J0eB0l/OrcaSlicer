#ifndef slic3r_AFCToolheads_hpp_
#define slic3r_AFCToolheads_hpp_

#include <string>
#include <vector>

namespace Slic3r {
namespace AFCProtocol {

// Apart from AFCProtocol.hpp so GCode.hpp needs no JSON; defined in AFCProtocol.cpp.

/// What each toolhead holds while a print's tool changes play out, on a
/// multi-toolhead printer whose filament slots can share a toolhead (several
/// AFC lanes feeding one extruder). `toolheads[i]` is slot i's toolhead from
/// the project option filament_toolhead, -1 when unknown.
class ToolheadLoads
{
public:
    ToolheadLoads() = default;
    explicit ToolheadLoads(std::vector<int> toolheads);

    /// Whether slot `filament` has a known toolhead.
    bool knows(unsigned int filament) const;
    /// Records a change to `filament` and returns the slot its toolhead held
    /// before when that is a lane change, else -1: a switch to a toolhead that
    /// already holds it, the toolhead's first use in the print (what it holds
    /// then is unknown), or a slot with no known toolhead.
    int change_to(unsigned int filament);

private:
    std::vector<int> m_toolheads;
    std::vector<int> m_loaded; ///< per toolhead, -1 = not known yet
};

/// Flush volume (mm³) of a lane change from slot `from` to slot `to`, from the
/// square filament-by-filament flush matrix times `multiplier`. 0 when either
/// slot is outside the matrix.
float lane_change_flush(const std::vector<double> &flush_matrix, unsigned int from, unsigned int to, double multiplier);

/// The flushing settings a print was sliced with.
struct FlushSettings
{
    std::vector<double> matrix;             ///< flush_volumes_matrix, filament by filament (mm³)
    double              multiplier{1.};     ///< flush_multiplier
    std::vector<double> filament_diameters; ///< filament_diameter
};

/// Recomputes the PURGE_LENGTH of a sliced G-code's tool changes, fed one
/// line at a time in file order, for the toolhead each slot's lane feeds at
/// print time (the print popup can map a T(n) to a lane on another toolhead
/// than the slicing assumed). A change to a slot with a known toolhead gets
/// the lane change flush from the slot that toolhead held, or no PURGE_LENGTH
/// when it is a tool switch; every other line stays as it is. A lane change
/// the slicing made from the same slot without a PURGE_LENGTH stays as it is
/// too: it was flushed on the prime tower ("Purge in prime tower"), or needed
/// no flush.
class PurgeLengths
{
public:
    /// `sliced`: filament_toolhead as sliced; `toolheads`: at print time, both by slot.
    /// `tools`: the T number each slot prints as (slot_tool_numbers), empty when
    /// slot n prints as T(n).
    PurgeLengths(std::vector<int> sliced, std::vector<int> toolheads, FlushSettings flush, const std::vector<int> &tools = {});

    /// `line` (without its newline) as this mapping needs it.
    std::string apply(const std::string &line);

private:
    ToolheadLoads    m_sliced;
    ToolheadLoads    m_loads;
    FlushSettings    m_flush;
    std::vector<int> m_slot_of; ///< slot of each T number, empty when they are the same
};

/// The T number each of `slots` filament slots prints as. `linked[i]` is the
/// AFC T number the filament sync tied slot i to (project option
/// filament_afc_tool), -1 when none: such a slot takes the lowest T number no
/// other slot has, so a project without synced lanes keeps T(n) for slot n.
/// Slots the sync's mapping put on one lane share its T number.
std::vector<int> slot_tool_numbers(const std::vector<int> &linked, size_t slots);

/// Whether every slot prints as its own index in `tools` (slot_tool_numbers).
bool tool_numbers_are_slots(const std::vector<int> &tools);

/// Rewrites the tool numbers in the whole G-code lines of `gcode` from filament
/// slots to the T numbers `tools` gives them, where AFC reads them: T<n> tool
/// changes, the T parameter of M104 and M109, and TOOL=, INITIAL_TOOL= and
/// T<n>_TEMP= of a PRINT_START or START_PRINT call. Comments stay as they are,
/// and a number past `tools` keeps its value. So does the T of an M104 or M109
/// with an A parameter: on a Snapmaker printer AFC reads that T as an extruder
/// ("M104 S0 T1 A0" in the U1's start G-code). The U1's own commands, such as
/// SM_PRINT_PREEXTRUDE_FILAMENT INDEX=, keep the slot (see u1_map_table).
void renumber_tools(std::string &gcode, const std::vector<int> &tools);

/// `sliced` (filament_toolhead) with every slot `mapped` gives a toolhead
/// (>= 0) set to it: the toolheads a print runs with after the print popup.
std::vector<int> toolheads_at_print(std::vector<int> sliced, const std::vector<int> &mapped);

/// `gcode` with AFC's PURGE_LENGTH (mm of filament) added to its first line
/// that changes to `tool` (`prefix` and the number, after any indent), so
/// AFC's poop purges that much when it loads the lane: "T5\n" becomes
/// "T5 PURGE_LENGTH=42.5\n". Works on the writer's tool change command and on
/// a custom change filament G-code alike. Unchanged when no line changes to
/// `tool`, that line already has a PURGE_LENGTH, or the length rounds to 0.
std::string with_purge_length(const std::string &gcode, const std::string &prefix, unsigned int tool, float purge_length);

} // namespace AFCProtocol
} // namespace Slic3r

#endif // slic3r_AFCToolheads_hpp_
