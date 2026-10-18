#ifndef slic3r_SLAPrintJob_hpp_
#define slic3r_SLAPrintJob_hpp_

#include <functional>
#include <string>
#include <vector>

#include "SLA/SupportPoint.hpp"

namespace Slic3r {

class DynamicPrintConfig;
class Model;

// Slices a Model for a resin (SLA/MSLA) printer and writes the printer archive. This is what the command line runs
// for a machine profile with printer_technology = SLA.
struct SLAJobParams
{
    // The machine and process (print) settings as loaded from the profile files. SLA keys are taken straight from
    // them; keys that are not SLA options are ignored.
    const DynamicPrintConfig *machine { nullptr };
    const DynamicPrintConfig *process { nullptr };
    // Optional: the resin material's values (see ResinMaterial.hpp). They sit between the machine and the process profile: a
    // value in the process profile still wins.
    const DynamicPrintConfig *material { nullptr };
    // Directory the archive is written to. The file is named plate_1.<extension of sla_archive_format>.
    std::string               output_dir;
    // Leave the instances where they are (the caller already placed them) instead of packing the objects on the display.
    bool                      keep_positions { false };
    // Hand-placed support points, one entry per object in the order the objects were loaded, in the place the objects are in the
    // file (instances applied, before the job packs them on the display). An entry replaces the generated points of that object, an
    // empty one means no supports; objects past the end of the list get generated points.
    std::vector<sla::SupportPoints> support_points;
    // Called with a percentage and a message while slicing.
    std::function<void(int, const std::string &)> progress;
};

struct SLAJobResult
{
    // One of the CLI_* codes of libslic3r/Utils.hpp; CLI_SUCCESS when the archive was written.
    int         code { 0 };
    std::string error;
    std::string archive_path;
    std::string archive_format;
    size_t      layer_count { 0 };
    double      print_time_s { 0. };
    double      resin_used_ml { 0. };
    double      layer_height { 0. };
    size_t      support_point_count { 0 };  // all objects
};

SLAJobResult run_sla_job(Model &model, const SLAJobParams &params);

// Generates the support points of the objects (or takes the hand-placed ones of `params.support_points`) and returns them, in the same
// frame as `params.support_points`, without building the supports or slicing. A profile with supports off still gets its points.
struct SLASupportPointsResult
{
    // One of the CLI_* codes of libslic3r/Utils.hpp; CLI_SUCCESS when the points were produced.
    int                             code { 0 };
    std::string                     error;
    std::vector<sla::SupportPoints> objects;   // per object, in load order
};

SLASupportPointsResult run_sla_support_points(Model &model, const SLAJobParams &params);

// The JSON the command line reads and writes: {"objects": [{"points": [{"x", "y", "z", "radius", "type"}]}]} where type is
// "manual", "island" or "slope" (default "manual"). The reader says why in `error` when it refuses a text.
std::string support_points_to_json(const std::vector<sla::SupportPoints> &objects);
bool        support_points_from_json(const std::string &text, std::vector<sla::SupportPoints> &objects, std::string &error);

// Hollows the objects of a Model and writes the result as one binary STL, in the place the objects are (instances applied): the
// outer surface and, inside it, a cavity left by a wall of at least `hollowing_min_thickness` (with `hollowing_quality` and
// `hollowing_closing_distance`, all from the process profile). No supports, pad or slicing. Used to give a design a hollow body that a
// resin printer can print with less material.
struct SLAHollowParams
{
    const DynamicPrintConfig *process { nullptr };
    std::string               output_path;
    std::function<void(int, const std::string &)> progress;
};

struct SLAHollowResult
{
    // One of the CLI_* codes of libslic3r/Utils.hpp; CLI_SUCCESS when the file was written.
    int         code { 0 };
    std::string error;
    std::string path;
    size_t      triangles { 0 };
    double      volume_before { 0. };   // mm3, as a solid
    double      volume_after { 0. };    // mm3, the material left
};

SLAHollowResult run_sla_hollow(Model &model, const SLAHollowParams &params);

} // namespace Slic3r

#endif // slic3r_SLAPrintJob_hpp_
