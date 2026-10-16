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
/// when it is a tool switch; every other line stays as it is.
class PurgeLengths
{
public:
    PurgeLengths(std::vector<int> toolheads, FlushSettings flush);

    /// `line` (without its newline) as this mapping needs it.
    std::string apply(const std::string &line);

private:
    ToolheadLoads m_loads;
    FlushSettings m_flush;
};

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
