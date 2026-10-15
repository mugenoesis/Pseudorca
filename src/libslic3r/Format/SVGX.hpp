#ifndef SLIC3R_FORMAT_SVGX_HPP
#define SLIC3R_FORMAT_SVGX_HPP

// FlashForge SVGX archives ("DLP-II 1.1"), for the Foto, Focus, Hunter and Explorer MAX printers. Written from UVtools'
// FlashForgeSVGXFile.cs (AGPL-3.0): a small header, two BMP previews and an SVG document whose layers are the outlines of the
// layer images in millimetres around the middle of the display.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "SLAArchiveWriter.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class SVGXArchive : public SLAArchiveWriter
{
public:
    explicit SVGXArchive(const SLAPrinterConfig &cfg) : m_cfg(cfg) {}

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

namespace svgx {

// What the writer extracts from one layer image.
struct Outline
{
    std::vector<std::string> paths;    // "M x y L x y ... Z", the holes of a shape in the same path
    float area { 0.f }, perimeter { 0.f };
    float min_x { 0.f }, min_y { 0.f }, max_x { 0.f }, max_y { 0.f };
    bool  any { false };
};

struct FileParams
{
    std::string machine_name, material_name;
    float    layer_height_mm { 0.f };
    float    volume_ml { 0.f };
    uint32_t res_x { 0 }, res_y { 0 };
    float    display_width_mm { 0.f }, display_height_mm { 0.f }, machine_z_mm { 0.f };
    uint32_t bottom_layer_count { 0 }, transition_layer_count { 0 };
    float    bottom_exposure_s { 0.f }, exposure_s { 0.f };
    float    print_height_mm { 0.f };
    // BMP files (with their 54 byte header) of 128 x 128 and 200 x 240 pixels; empty: no preview.
    std::vector<uint8_t> preview1, preview2;
    std::vector<Outline> layers;
};

// Traces the lit pixels (>= 128) of an image into outlines. pixel_w_mm and pixel_h_mm are the pixel pitch.
Outline trace_layer(const uint8_t *pixels, size_t width, size_t height, double pixel_w_mm, double pixel_h_mm,
                    double display_w_mm, double display_h_mm);
// A 24 bit BMP file from RGBA pixels in the bottom to top order the thumbnails come in.
std::vector<uint8_t> make_bmp(const uint8_t *rgba, uint32_t width, uint32_t height);
// The header, previews and SVG document.
std::vector<uint8_t> build_svgx(const FileParams &params);

} // namespace svgx
} // namespace Slic3r

#endif // SLIC3R_FORMAT_SVGX_HPP
