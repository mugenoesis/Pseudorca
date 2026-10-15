#ifndef SLIC3R_FORMAT_IMAGEZIP_HPP
#define SLIC3R_FORMAT_IMAGEZIP_HPP

// The Phrozen zip: the layer images 1.png, 2.png, ... and the two previews in a zip, and nothing else. The printer
// holds the exposure, lift and layer settings itself (as UVtools writes it, ChituboxZipFile.cs without run.gcode).

#include <string>
#include <vector>

#include "SL1.hpp"

namespace Slic3r {

class PhrozenZipArchive : public SL1Archive
{
public:
    using SL1Archive::SL1Archive;

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

// The Chitubox zip: a Phrozen zip plus run.gcode, which holds the print settings and the move, light and image commands of
// every layer (UVtools' ChituboxZipFile.cs and GCodeBuilder.cs). Chitu mainboard printers that take a zip read it.
namespace chituzip {

struct LayerParams
{
    float z_mm { 0.f };
    float exposure_s { 0.f }, wait_before_cure_s { 0.f }, wait_after_cure_s { 0.f };
    float lift_mm { 0.f }, lift_mm_min { 0.f }, retract_mm_min { 0.f };
    uint32_t pwm { 255 };
};

struct GCodeParams
{
    std::string file_name, machine_type;
    float    print_time_s { 0.f }, volume_ml { 0.f }, weight_g { 0.f }, price { 0.f };
    float    layer_height_mm { 0.f };
    uint32_t res_x { 0 }, res_y { 0 };
    float    machine_x_mm { 0.f }, machine_y_mm { 0.f }, machine_z_mm { 0.f };
    bool     mirror { false };
    float    exposure_s { 0.f }, bottom_exposure_s { 0.f };
    float    retract_mm_min { 0.f }, lift_mm_min { 0.f }, lift_mm { 0.f };
    uint32_t bottom_layer_count { 0 };
    float    bottom_lift_mm { 0.f }, bottom_lift_mm_min { 0.f };
    float    bottom_light_off_s { 0.f }, light_off_s { 0.f };
    uint32_t bottom_pwm { 255 }, pwm { 255 };
    std::vector<LayerParams> layers;
};

// The text of run.gcode.
std::string build_gcode(const GCodeParams &params);

} // namespace chituzip

class ChituboxZipArchive : public SL1Archive
{
public:
    using SL1Archive::SL1Archive;

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;
};

} // namespace Slic3r

#endif // SLIC3R_FORMAT_IMAGEZIP_HPP
