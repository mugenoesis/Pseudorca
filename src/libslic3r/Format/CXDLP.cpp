#include "CXDLP.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <regex>

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

namespace cxdlp {

namespace {

constexpr const char *Magic = "CXSW3DV2";
constexpr uint16_t    Version = 3;

class Writer
{
public:
    std::vector<uint8_t> out;
    void u8(uint8_t v) { out.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
    void le32(uint32_t v) { for (int i = 0; i < 4; ++i) u8(uint8_t(v >> (8 * i))); }
    void le16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
    void bytes(const uint8_t *p, size_t n) { out.insert(out.end(), p, p + n); }
    void crlf() { u8(0x0D); u8(0x0A); }
    // A length (counting the terminating zero) and the zero terminated text.
    void cstring(const std::string &s) { u32(uint32_t(s.size() + 1)); bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size()); u8(0); }
    // A length in bytes and the text as UTF-16 big endian.
    void utf16(const std::string &s)
    {
        u32(uint32_t(s.size() * 2));
        for (char c : s) { u8(0); u8(uint8_t(c)); }
    }
};

uint16_t whole(double v) { return uint16_t(std::clamp<long>(std::lround(v), 0, 65535)); }

std::string number(double v, int decimals)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    return s;
}

} // namespace

std::string printer_model_of(const std::string &name)
{
    static const std::regex re(R"((CL|CT)-?[0-9]+[a-zA-Z]?)", std::regex::icase);
    std::smatch m;
    return std::regex_search(name, m, re) ? m.str() : std::string();
}

std::vector<uint8_t> encode_layer(const uint8_t *px, size_t w, size_t h, double pixel_area_mm2)
{
    // The area of the largest region: the lit pixels, the holes in them and what is inside the holes. The outside is the
    // background that reaches the border; everything else belongs to a region, found with a flood fill.
    std::vector<uint8_t> exterior(w * h, 0);
    std::vector<uint32_t> stack;
    auto seed = [&](size_t x, size_t y) {
        const size_t i = y * w + x;
        if (px[i] == 0 && ! exterior[i]) { exterior[i] = 1; stack.push_back(uint32_t(i)); }
    };
    for (size_t x = 0; x < w; ++x) { seed(x, 0); seed(x, h - 1); }
    for (size_t y = 0; y < h; ++y) { seed(0, y); seed(w - 1, y); }
    while (! stack.empty()) {
        const size_t i = stack.back(); stack.pop_back();
        const size_t x = i % w, y = i / w;
        if (x > 0)     seed(x - 1, y);
        if (x + 1 < w) seed(x + 1, y);
        if (y > 0)     seed(x, y - 1);
        if (y + 1 < h) seed(x, y + 1);
    }
    std::vector<uint8_t> seen(w * h, 0);
    uint64_t largest = 0;
    for (size_t start = 0; start < w * h; ++start) {
        if (exterior[start] || seen[start])
            continue;
        uint64_t count = 0;
        seen[start] = 1;
        stack.push_back(uint32_t(start));
        while (! stack.empty()) {
            const size_t i = stack.back(); stack.pop_back();
            ++count;
            const size_t x = i % w, y = i / w;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const long nx = long(x) + dx, ny = long(y) + dy;
                    if (nx < 0 || ny < 0 || nx >= long(w) || ny >= long(h))
                        continue;
                    const size_t n = size_t(ny) * w + size_t(nx);
                    if (! exterior[n] && ! seen[n]) { seen[n] = 1; stack.push_back(uint32_t(n)); }
                }
        }
        largest = std::max(largest, count);
    }
    const uint32_t area = uint32_t(std::min<double>(4294967295.0, double(largest) * pixel_area_mm2 * 1000.));

    // The lines: for each column, the runs of one non zero gray from top to bottom.
    std::vector<uint8_t> body;
    uint32_t lines = 0;
    for (size_t x = 0; x < w; ++x) {
        size_t y = 0;
        while (y < h) {
            const uint8_t gray = px[y * w + x];
            size_t end = y + 1;
            while (end < h && px[end * w + x] == gray)
                ++end;
            if (gray != 0) {
                const uint16_t sy = uint16_t(y), ey = uint16_t(end - 1), sx = uint16_t(x);
                body.push_back(uint8_t((sy >> 5) & 0xFF));
                body.push_back(uint8_t(((sy << 3) + (ey >> 10)) & 0xFF));
                body.push_back(uint8_t((ey >> 2) & 0xFF));
                body.push_back(uint8_t(((ey << 6) + (sx >> 8)) & 0xFF));
                body.push_back(uint8_t(sx));
                body.push_back(gray);
                ++lines;
            }
            y = end;
        }
    }
    std::vector<uint8_t> out;
    out.reserve(body.size() + 10);
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(area >> shift));
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(uint8_t(lines >> shift));
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(0x0D); out.push_back(0x0A);
    return out;
}

bool decode_layer(const std::vector<uint8_t> &layer, uint8_t *px, size_t w, size_t h)
{
    if (layer.size() < 10)
        return false;
    const uint32_t lines = (uint32_t(layer[4]) << 24) | (uint32_t(layer[5]) << 16) | (uint32_t(layer[6]) << 8) | layer[7];
    if (layer.size() != 8 + size_t(lines) * 6 + 2)
        return false;
    std::fill(px, px + w * h, uint8_t(0));
    for (uint32_t i = 0; i < lines; ++i) {
        const uint8_t *c = &layer[8 + size_t(i) * 6];
        const size_t sy = ((c[0] << 8) + c[1]) >> 3 & 0x1FFF;
        const size_t ey = ((c[1] << 16) + (c[2] << 8) + c[3]) >> 6 & 0x1FFF;
        const size_t sx = ((c[3] << 8) + c[4]) & 0x3FFF;
        if (ey < sy || ey >= h || sx >= w)
            return false;
        for (size_t y = sy; y <= ey; ++y)
            px[y * w + sx] = c[5];
    }
    return true;
}

uint32_t checksum(const uint8_t *data, size_t size)
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t v = i;
            for (int j = 0; j < 8; ++j)
                v = (v & 1) ? (v >> 1) ^ 0xEDB88320u : v >> 1;
            t[i] = v;
        }
        return t;
    }();
    uint32_t sum = 0;
    for (size_t i = 0; i < size; ++i)
        sum = table[uint8_t(sum) ^ data[i]] ^ (sum >> 8);
    return sum;
}

std::vector<uint8_t> build_cxdlp(const FileParams &p, const std::vector<std::vector<uint8_t>> &layers)
{
    Writer w;
    w.cstring(Magic);                      // the length counts the zero, as in the file's first 4 bytes: 9
    w.u16(Version);
    w.cstring(p.printer_model);
    w.u16(uint16_t(layers.size()));
    w.u16(uint16_t(p.res_x)); w.u16(uint16_t(p.res_y));
    w.out.insert(w.out.end(), 64, uint8_t(0));

    const uint32_t sides[3] = { 116, 290, 290 };
    for (int i = 0; i < 3; ++i) {
        std::vector<uint8_t> px = p.preview[i];
        px.resize(size_t(sides[i]) * sides[i] * 2, 0);
        w.bytes(px.data(), px.size());
        w.crlf();
    }

    // The slicer info: the display size and layer height as text, the rest whole numbers.
    w.utf16(number(p.display_width_mm, 3));
    w.utf16(number(p.display_height_mm, 3));
    w.utf16(number(p.layer_height_mm, 3));
    w.u16(whole(p.exposure_s * 10));
    w.u16(std::max<uint16_t>(1, whole(p.wait_before_cure_s)));     // a wait of 0 does not print
    w.u16(whole(p.bottom_exposure_s));
    w.u16(uint16_t(p.bottom_layer_count));
    w.u16(whole(p.bottom_lift_mm));
    w.u16(whole(p.bottom_lift_mm_s));
    w.u16(whole(p.lift_mm));
    w.u16(whole(p.lift_mm_s));
    w.u16(whole(p.retract_mm_s));
    w.u16(uint16_t(p.bottom_pwm));
    w.u16(uint16_t(p.pwm));

    // The area of the largest region of each layer.
    for (const std::vector<uint8_t> &l : layers) {
        if (l.size() < 4)
            throw ExportError("A CXDLP layer is too short.");
        w.bytes(l.data(), 4);
    }
    w.crlf();

    // Slicer info version 3
    w.cstring(p.software);
    w.cstring(p.material);
    w.u8(0);                               // distortion compensation off
    w.le32(600); w.le32(300000);
    w.u8(1);                               // xy axis profile compensation
    w.le16(0);
    w.u8(0);                               // z penetration compensation
    w.le16(1000);
    w.u8(1); w.u8(1); w.u8(255);           // anti aliasing on, between 1 and 255
    w.u8(0); w.u8(2);                      // blur off, level 2
    w.crlf();

    for (const std::vector<uint8_t> &l : layers)
        w.bytes(l.data(), l.size());

    w.cstring(Magic);                      // the footer
    w.u32(checksum(w.out.data(), w.out.size()));
    return std::move(w.out);
}

} // namespace cxdlp

namespace {

struct CXDLPRasterEncoder
{
    double pixel_area_mm2;
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components)
    {
        (void) num_components;
        return sla::EncodedRaster(cxdlp::encode_layer(static_cast<const uint8_t*>(ptr), w, h, pixel_area_mm2), "cxdlp");
    }
};

// Big endian RGB565 copy of a thumbnail (stored bottom to top) of side x side pixels.
std::vector<uint8_t> preview_565_be(const ThumbnailData &t, uint32_t side)
{
    std::vector<uint8_t> out(size_t(side) * side * 2);
    for (uint32_t y = 0; y < side; ++y) {
        const size_t sy = t.height - 1 - std::min<size_t>(t.height - 1, size_t(y) * t.height / side);
        for (uint32_t x = 0; x < side; ++x) {
            const size_t sx = std::min<size_t>(t.width - 1, size_t(x) * t.width / side);
            const unsigned char *s = &t.pixels[(sy * t.width + sx) * 4];
            const uint16_t px = uint16_t(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
            out[(size_t(y) * side + x) * 2]     = uint8_t(px >> 8);
            out[(size_t(y) * side + x) * 2 + 1] = uint8_t(px);
        }
    }
    return out;
}

} // namespace

std::unique_ptr<sla::RasterBase> CXDLPArchive::create_raster() const
{
    double w = m_cfg.display_width.getFloat(), h = m_cfg.display_height.getFloat();
    size_t pw = size_t(m_cfg.display_pixels_x.getInt()), ph = size_t(m_cfg.display_pixels_y.getInt());
    const bool portrait = m_cfg.display_orientation.getInt() == sla::RasterBase::roPortrait;
    if (portrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }
    const std::array<bool, 2> mirror{ m_cfg.display_mirror_x.getBool(), m_cfg.display_mirror_y.getBool() };
    const sla::RasterBase::Trafo tr{ portrait ? sla::RasterBase::roPortrait : sla::RasterBase::roLandscape, mirror };
    return sla::create_raster_grayscale_aa(sla::Resolution{pw, ph}, sla::PixelDim{w / pw, h / ph},
                                           m_cfg.gamma_correction.getFloat(), tr);
}

sla::RasterEncoder CXDLPArchive::get_encoder() const
{
    double w = m_cfg.display_width.getFloat(), h = m_cfg.display_height.getFloat();
    double pw = m_cfg.display_pixels_x.getInt(), ph = m_cfg.display_pixels_y.getInt();
    return CXDLPRasterEncoder{ (w / pw) * (h / ph) };
}

void CXDLPArchive::export_print(const std::string     fname,
                                const SLAPrint       &print,
                                const ThumbnailsList &thumbnails,
                                const std::string    &/*projectname*/)
{
    const SLAMaterialConfig    &mat = print.material_config();
    const SLAPrintObjectConfig &obj = print.default_object_config();

    cxdlp::FileParams p;
    p.printer_model = cxdlp::printer_model_of(m_cfg.printer_model.value);
    if (p.printer_model.empty())
        throw ExportError("The CXDLP format needs the printer's model code (like CL-60) in the printer model, got \"" +
                          m_cfg.printer_model.value + "\".");
    double w = m_cfg.display_width.getFloat(), h = m_cfg.display_height.getFloat();
    size_t pw = size_t(m_cfg.display_pixels_x.getInt()), ph = size_t(m_cfg.display_pixels_y.getInt());
    if (m_cfg.display_orientation.getInt() == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }
    p.res_x = uint32_t(pw); p.res_y = uint32_t(ph);
    p.display_width_mm = float(w); p.display_height_mm = float(h);
    p.layer_height_mm = float(obj.layer_height.getFloat());
    p.exposure_s = float(mat.exposure_time.getFloat());
    p.bottom_exposure_s = float(mat.initial_exposure_time.getFloat());
    p.wait_before_cure_s = float(mat.delay_before_exposure.get_at(0));
    p.bottom_layer_count = uint32_t(std::clamp<size_t>(size_t(std::max(0, mat.bottom_layer_count.getInt())), 0, m_layers.size()));
    p.bottom_lift_mm = p.lift_mm = float(mat.lift_distance.getFloat());
    p.bottom_lift_mm_s = p.lift_mm_s = float(mat.lift_speed.getFloat() / 60.);
    p.retract_mm_s = float(mat.retract_speed.getFloat() / 60.);
    p.bottom_pwm = p.pwm = uint32_t(std::clamp(mat.light_pwm.getInt(), 0, 255));
    p.software = "Pseudorca";
    p.material = "";

    if (! thumbnails.empty() && thumbnails.front().is_valid()) {
        p.preview[0] = preview_565_be(thumbnails.front(), 116);
        p.preview[1] = preview_565_be(thumbnails.front(), 290);
        p.preview[2] = p.preview[1];
    }

    std::vector<std::vector<uint8_t>> layers;
    layers.reserve(m_layers.size());
    for (const sla::EncodedRaster &r : m_layers) {
        const auto *data = static_cast<const uint8_t*>(r.data());
        layers.emplace_back(data, data + r.size());
    }
    const std::vector<uint8_t> file = cxdlp::build_cxdlp(p, layers);
    std::ofstream out(fname, std::ios::binary | std::ios::out | std::ios::trunc);
    if (! out)
        throw ExportError("Cannot open " + fname + " for writing.");
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    out.close();
    if (! out)
        throw ExportError("Writing " + fname + " failed.");
}

} // namespace Slic3r
