#ifndef SLIC3R_FORMAT_ANYCUBICPW_HPP
#define SLIC3R_FORMAT_ANYCUBICPW_HPP

// Anycubic Photon Workshop archives (.pwmo, .pwmx, .pwms, .pw0, .pwx, .pwma, .pwmb, .pm3, .pm3m, .pm3r, .pm3n, .pm5, .pm5s,
// .m5sp, .px6s, .pmx2, .pmsq, .dl2p, .dlp), file format versions 1 and 515 to 518. Written from the table layouts of UVtools'
// AnycubicFile.cs (AGPL-3.0); version 1 follows the layout PrusaSlicer's AnycubicSLA.cpp writes. The layer images use the
// 4 bit run length encoding Photon Workshop calls "pw0Img".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SLAArchiveFormatRegistry.hpp"
#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class AnycubicPWArchive : public SLAArchiveWriter
{
public:
    // `max_version` is the newest version the printer reads and the one that is written.
    AnycubicPWArchive(const SLAPrinterConfig &cfg, uint32_t version, uint32_t max_version)
        : m_cfg(cfg), m_version(version), m_max_version(max_version) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder               get_encoder() const override;

private:
    SLAPrinterConfig m_cfg;
    uint32_t         m_version;
    uint32_t         m_max_version;
};

namespace anycubic {

struct LayerParams
{
    float height_mm { 0.f };          // thickness of the layer
    float exposure_s { 0.f };
    float lift_mm { 0.f }, lift_mm_s { 0.f };
    uint32_t non_zero_pixels { 0 };
};

struct FileParams
{
    uint32_t version { 1 }, max_version { 1 };
    std::string machine_name;
    std::string software;
    float    display_width_mm { 0.f }, display_height_mm { 0.f }, machine_z_mm { 0.f };
    uint32_t res_x { 0 }, res_y { 0 };
    float    pixel_width_um { 0.f }, pixel_height_um { 0.f };
    float    layer_height_mm { 0.f };
    float    exposure_s { 0.f }, bottom_exposure_s { 0.f };
    float    wait_before_cure_s { 0.f };
    uint32_t bottom_layer_count { 0 };
    uint32_t transition_layer_count { 0 };
    float    lift_mm { 0.f }, lift_mm_s { 0.f }, retract_mm_s { 0.f };
    float    volume_ml { 0.f }, weight_g { 0.f }, cost { 0.f };
    uint32_t print_time_s { 0 };
    float    model_min[3] { 0, 0, 0 }, model_max[3] { 0, 0, 0 };
    // RGB565 previews (little endian), 224 x 168 and 330 x 190 pixels; empty: a dark picture.
    std::vector<uint8_t> preview, preview2;
    std::vector<LayerParams> layers;
};

// `images` are the layers' encoded images (no trailing count).
std::vector<uint8_t> build_anycubic(const FileParams &params, const std::vector<std::vector<uint8_t>> &images);
// The 4 bit run length encoding of one layer image. The second result is the number of non zero pixels.
std::vector<uint8_t> rle_encode(const uint8_t *pixels, size_t count, uint32_t *non_zero = nullptr);
bool rle_decode(const uint8_t *data, size_t size, uint8_t *pixels, size_t count);

} // namespace anycubic

inline ArchiveEntry anycubic_pw_format(const char *ext, const char *desc, uint32_t version, uint32_t max_version)
{
    ArchiveEntry entry(ext);
    entry.desc        = desc;
    entry.ext         = ext;
    entry.wrfactoryfn = [version, max_version] (const auto &cfg) { return std::make_unique<AnycubicPWArchive>(cfg, version, max_version); };
    return entry;
}
} // namespace Slic3r

#endif // SLIC3R_FORMAT_ANYCUBICPW_HPP
