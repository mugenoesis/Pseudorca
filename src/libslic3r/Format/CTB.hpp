#ifndef SLIC3R_FORMAT_CTB_HPP
#define SLIC3R_FORMAT_CTB_HPP

// Chitubox CTB archives (version 4), written for printers with a Chitu mainboard.
//
// The file layout, the run-length codec and the layer cipher follow UVtools (AGPL-3.0, sn4k3/UVtools,
// UVtools.Core/FileFormats/ChituboxFile.cs, CtbRleCodec.cs) and mslicer (GPL-3.0, connorslade/mslicer,
// format/ctb_format), which in turn rely on the reverse engineering work of the UVtools contributors.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

// The magic number selects the dialect: the UniFormation GKtwo wants its own magic and disclaimer text.
// Cbddlp is the older layout of the Chitubox, Photon and Elegoo Mars families (.cbddlp, .photon): version 2, unencrypted,
// with the anti aliasing done by several one bit images per layer.
enum class CTBDialect { Chitubox, GKtwo, Cbddlp, Encrypted };

class CTBArchive : public SLAArchiveWriter
{
public:
    explicit CTBArchive(const SLAPrinterConfig &cfg, CTBDialect dialect = CTBDialect::Chitubox)
        : m_cfg(cfg), m_dialect(dialect) {}

    void export_print(const std::string     fname,
                      const SLAPrint       &print,
                      const ThumbnailsList &thumbnails,
                      const std::string    &projectname = "") override;

protected:
    std::unique_ptr<sla::RasterBase> create_raster() const override;
    sla::RasterEncoder               get_encoder() const override;

private:
    SLAPrinterConfig m_cfg;
    CTBDialect       m_dialect;
};

// The parameters of one CTB file. build_ctb_v4() turns them and the encoded layer images into the file's bytes.
// Exposed for the tests.
namespace ctb {

struct LayerParams
{
    float z_mm            { 0.f };   // top of the layer
    float exposure_s      { 0.f };
    float light_off_s     { 0.f };   // light off delay
    float lift_mm         { 0.f };
    float lift_mm_min     { 0.f };
    float retract_mm_min  { 0.f };
    float rest_before_lift_s { 0.f };
    float rest_after_lift_s  { 0.f };
    float rest_after_retract_s { 0.f };
    float light_pwm       { 255.f };
};

struct Preview
{
    uint32_t             width  { 0 };
    uint32_t             height { 0 };
    std::vector<uint8_t> rgb;        // width * height * 3
};

struct FileParams
{
    CTBDialect  dialect { CTBDialect::Chitubox };
    float       bed_x_mm { 0.f }, bed_y_mm { 0.f }, bed_z_mm { 0.f };
    uint32_t    res_x { 0 }, res_y { 0 };
    float       layer_height_mm { 0.f };
    float       exposure_s { 0.f }, bottom_exposure_s { 0.f };
    float       light_off_s { 0.f }, bottom_light_off_s { 0.f };
    uint32_t    bottom_layer_count { 0 };
    uint32_t    transition_layer_count { 0 };
    uint32_t    print_time_s { 0 };
    uint32_t    projector_type { 0 };
    uint16_t    light_pwm { 255 }, bottom_light_pwm { 255 };
    float       lift_mm { 0.f }, lift_mm_min { 0.f }, bottom_lift_mm { 0.f }, bottom_lift_mm_min { 0.f };
    float       retract_mm_min { 0.f }, bottom_retract_mm_min { 0.f };
    float       rest_after_retract_s { 0.f };
    float       volume_ml { 0.f }, weight_g { 0.f }, cost { 0.f };
    uint32_t    timestamp_minutes { 0 };
    uint32_t    xor_key { 0 };
    std::string machine_name;
    Preview     large_preview, small_preview;
    std::vector<LayerParams> layers;
};

// The number of one bit images that make up a cbddlp layer; they give it that many gray levels.
constexpr uint32_t cbddlp_planes = 4;

// One layer of a cbddlp file as `cbddlp_planes` run length encoded bit planes (a pixel is lit in plane k when it reaches
// that plane's threshold). The result is the planes one after the other, each preceded by its size as a u32.
std::vector<uint8_t> cbddlp_encode(const uint8_t *pixels, size_t count);
// Splits the result of cbddlp_encode() into its planes; false when it is malformed.
bool cbddlp_split(const std::vector<uint8_t> &blob, std::vector<std::vector<uint8_t>> &planes);
// Rebuilds the gray image from the planes: each lit plane adds one gray step (the writer's coarse levels).
bool cbddlp_decode(const std::vector<std::vector<uint8_t>> &planes, uint8_t *pixels, size_t count);

// AES-256-CBC without padding, with the key and IV Chitubox uses for the encrypted layout (the length must be a multiple of 16).
std::vector<uint8_t> aes_cbc(const std::vector<uint8_t> &data, bool encrypt);

// `images` are rle_encode()d layers, like for build_ctb_v4(). Writes version 5 of the encrypted layout, the one the
// Elegoo Mars 3 / Saturn and similar printers read: the settings and the signature are AES encrypted.
std::vector<uint8_t> build_ctb_encrypted(const FileParams &params, const std::vector<std::vector<uint8_t>> &images);

// `images` are cbddlp_encode()d layers. Writes version 2 of the layout.
std::vector<uint8_t> build_cbddlp(const FileParams &params, const std::vector<std::vector<uint8_t>> &images);

// The 7 bit grayscale run length encoding of one layer image (rows are not separated, runs may span them).
std::vector<uint8_t> rle_encode(const uint8_t *pixels, size_t count);
// Decodes into `pixels` (count bytes, black where nothing is written). Returns false on malformed data.
bool rle_decode(const uint8_t *data, size_t size, uint8_t *pixels, size_t count);

// XOR cipher over a layer image (symmetric). A zero seed does nothing.
void layer_cipher(uint8_t *data, size_t size, uint32_t seed, uint32_t layer_index);

// `images` are the RLE encoded layer images, one per entry of params.layers, before the cipher.
std::vector<uint8_t> build_ctb_v4(const FileParams &params, const std::vector<std::vector<uint8_t>> &images);

} // namespace ctb
} // namespace Slic3r

#endif // SLIC3R_FORMAT_CTB_HPP
