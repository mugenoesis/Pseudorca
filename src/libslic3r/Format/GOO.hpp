#ifndef SLIC3R_FORMAT_GOO_HPP
#define SLIC3R_FORMAT_GOO_HPP

// Elegoo GOO archives (format version 3.0), for the Elegoo Mars/Saturn/Jupiter family.
//
// The layout follows the official format description (github.com/elegooofficial/GOO) as implemented by
// UVtools (sn4k3/UVtools, UVtools.Core/FileFormats/GooFile.cs) and mslicer (GPL-3.0, connorslade/mslicer,
// format/goo_format). All numbers are big endian.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class GOOArchive : public SLAArchiveWriter
{
public:
    explicit GOOArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}

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

// Exposed for the tests.
namespace goo {

constexpr uint32_t header_size = 0x2FB95;   // also the offset of the first layer, and stored in the header

struct LayerParams
{
    float z_mm { 0.f };
    float exposure_s { 0.f };
    float wait_before_lift_s { 0.f }, wait_after_lift_s { 0.f }, wait_after_retract_s { 0.f };
    float lift_mm { 0.f }, lift_mm_min { 0.f };
    float retract_mm { 0.f }, retract_mm_min { 0.f };
    uint16_t light_pwm { 255 };
};

struct FileParams
{
    std::string software, software_version, file_time, printer_name, printer_type, profile_name;
    uint32_t    res_x { 0 }, res_y { 0 };
    bool        mirror_x { false }, mirror_y { false };
    float       bed_x_mm { 0.f }, bed_y_mm { 0.f }, bed_z_mm { 0.f };
    float       layer_height_mm { 0.f };
    float       exposure_s { 0.f }, bottom_exposure_s { 0.f };
    uint32_t    bottom_layer_count { 0 };
    uint16_t    transition_layer_count { 0 };
    float       wait_after_cure_s { 0.f }, wait_after_retract_s { 0.f };
    float       lift_mm { 0.f }, lift_mm_min { 0.f }, retract_mm { 0.f }, retract_mm_min { 0.f };
    uint16_t    light_pwm { 255 };
    uint32_t    print_time_s { 0 };
    float       volume_mm3 { 0.f }, weight_g { 0.f }, cost { 0.f };
    // RGB565 previews, 116 x 116 and 290 x 290 pixels (empty: black).
    std::vector<uint16_t> small_preview, big_preview;
    std::vector<LayerParams> layers;
};

// The run length encoding of one 8 bit layer image: runs of black, white or one gray value.
std::vector<uint8_t> rle_encode(const uint8_t *pixels, size_t count);
// Decodes a layer's data (without the leading 0x55 and the checksum). `pixels` must hold `count` bytes.
bool rle_decode(const uint8_t *data, size_t size, uint8_t *pixels, size_t count);
// The negated wrapping sum of the bytes.
uint8_t checksum(const uint8_t *data, size_t size);

// `images` are the rle_encode()d layers, one per entry of params.layers.
std::vector<uint8_t> build_goo(const FileParams &params, const std::vector<std::vector<uint8_t>> &images);

} // namespace goo
} // namespace Slic3r

#endif // SLIC3R_FORMAT_GOO_HPP
