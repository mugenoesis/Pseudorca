#ifndef slic3r_SLAPrintJob_hpp_
#define slic3r_SLAPrintJob_hpp_

#include <functional>
#include <string>

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
};

SLAJobResult run_sla_job(Model &model, const SLAJobParams &params);

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
