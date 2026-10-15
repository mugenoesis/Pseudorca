#ifndef SLIC3R_FORMAT_CXDLP_HPP
#define SLIC3R_FORMAT_CXDLP_HPP

// Creality CXDLP archives (the "CXSW3DV2" layout, version 3), for the Halot printers. Written from UVtools'
// CrealityCXDLPFile.cs (AGPL-3.0) and its 010 Editor template: big endian numbers, three RGB565 previews, the settings as
// whole seconds, millimetres and mm/s, and each layer as lines of constant gray along the columns of the image.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class CXDLPArchive : public SLAArchiveWriter
{
public:
    explicit CXDLPArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder               get_encoder() const override;

private:
    SLAPrinterConfig m_cfg;
};

namespace cxdlp {

struct FileParams
{
    std::string printer_model;                 // "CL-60", "CT-005", ...
    uint32_t    res_x { 0 }, res_y { 0 };
    float       display_width_mm { 0.f }, display_height_mm { 0.f };
    float       layer_height_mm { 0.f };
    float       exposure_s { 0.f }, bottom_exposure_s { 0.f };
    float       wait_before_cure_s { 0.f };
    uint32_t    bottom_layer_count { 0 };
    float       bottom_lift_mm { 0.f }, lift_mm { 0.f };
    float       bottom_lift_mm_s { 0.f }, lift_mm_s { 0.f }, retract_mm_s { 0.f };
    uint32_t    bottom_pwm { 255 }, pwm { 255 };
    std::string software, material;
    // RGB565 big endian previews of 116 x 116, 290 x 290 and 290 x 290 pixels (empty: dark).
    std::vector<uint8_t> preview[3];
};

// The printer model in a name like "Halot One CL-60": "CL-60", or an empty string.
std::string printer_model_of(const std::string &name);

// One layer from its image: the largest region's area, the number of lines and the lines, then CR LF.
std::vector<uint8_t> encode_layer(const uint8_t *pixels, size_t width, size_t height, double pixel_area_mm2);
// Decodes lines back into an image (width x height); false when a line is out of bounds.
bool decode_layer(const std::vector<uint8_t> &layer, uint8_t *pixels, size_t width, size_t height);

// The checksum the files end with: CRC-32 (polynomial 0xEDB88320, no initial or final inversion) of everything before it.
uint32_t checksum(const uint8_t *data, size_t size);

std::vector<uint8_t> build_cxdlp(const FileParams &params, const std::vector<std::vector<uint8_t>> &layers);

} // namespace cxdlp
} // namespace Slic3r

#endif // SLIC3R_FORMAT_CXDLP_HPP
