#include "GOO.hpp"

#include "libslic3r_version.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

namespace goo {

namespace {

constexpr size_t small_preview_side = 116;
constexpr size_t big_preview_side   = 290;

const std::array<uint8_t, 8> magic_tag  = { 0x07, 0x00, 0x00, 0x00, 0x44, 0x4C, 0x50, 0x00 };
const std::array<uint8_t, 2> delimiter  = { 0x0D, 0x0A };
const std::array<uint8_t, 11> ending    = { 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x44, 0x4C, 0x50, 0x00 };

class Writer
{
public:
    std::vector<uint8_t> out;

    void bytes(const uint8_t *p, size_t n) { out.insert(out.end(), p, p + n); }
    template<size_t N> void bytes(const std::array<uint8_t, N> &a) { bytes(a.data(), N); }
    void u8(uint8_t v) { out.push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
    void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
    void f32(float v) { uint32_t b; std::memcpy(&b, &v, 4); u32(b); }
    // Zero padded, fixed size text field.
    void text(const std::string &s, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
            u8(i < s.size() ? uint8_t(s[i]) : uint8_t(0));
    }
    void preview(const std::vector<uint16_t> &px, size_t side)
    {
        for (size_t i = 0; i < side * side; ++i)
            u16(i < px.size() ? px[i] : uint16_t(0));
    }
};

void add_run(std::vector<uint8_t> &out, uint32_t length, uint8_t value)
{
    // byte 0: aabbcccc. a: 0 black, 1 gray (value follows), 3 white. b: how many bytes of length follow
    // the low nibble (0 to 3, each holding 8 more bits). c: the low 4 bits of the length.
    while (length > 0) {
        const uint32_t n = std::min<uint32_t>(length, 0xFFFFFFF);
        const uint8_t kind = value == 0 ? 0b00 : value == 0xFF ? 0b11 : 0b01;
        const uint8_t extra = n <= 0xF ? 0 : n <= 0xFFF ? 1 : n <= 0xFFFFF ? 2 : 3;
        out.push_back(uint8_t((kind << 6) | (extra << 4) | (n & 0xF)));
        if (kind == 0b01)
            out.push_back(value);
        if (extra >= 3) out.push_back(uint8_t(n >> 20));
        if (extra >= 2) out.push_back(uint8_t(n >> 12));
        if (extra >= 1) out.push_back(uint8_t(n >> 4));
        length -= n;
    }
}

} // namespace

std::vector<uint8_t> rle_encode(const uint8_t *pixels, size_t count)
{
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < count) {
        size_t j = i + 1;
        while (j < count && pixels[j] == pixels[i])
            ++j;
        add_run(out, uint32_t(j - i), pixels[i]);
        i = j;
    }
    return out;
}

bool rle_decode(const uint8_t *data, size_t size, uint8_t *pixels, size_t count)
{
    size_t at = 0, px = 0;
    uint8_t color = 0;
    while (at < size) {
        const uint8_t head = data[at++];
        const uint8_t kind = head >> 6;
        uint32_t length = 0;
        if (kind == 0b10) {
            // A difference to the previous run's color, which the writer never produces but readers must accept.
            const uint8_t diff = head & 0xF;
            if (head & 0x10) {
                if (at >= size) return false;
                length = data[at++];
            } else {
                length = 1;
            }
            color = (head & 0x20) ? uint8_t(color - diff) : uint8_t(color + diff);
        } else {
            color = kind == 0 ? 0 : kind == 3 ? 0xFF : 0;
            if (kind == 0b01) {
                if (at >= size) return false;
                color = data[at++];
            }
            const size_t extra = (head >> 4) & 3;
            if (at + extra > size) return false;
            length = head & 0xF;
            for (size_t k = 0; k < extra; ++k)
                length |= uint32_t(data[at + k]) << (4 + 8 * (extra - 1 - k));
            at += extra;
        }
        if (length == 0 || length > count - px)
            return false;
        std::fill(pixels + px, pixels + px + length, color);
        px += length;
    }
    return px == count;
}

uint8_t checksum(const uint8_t *data, size_t size)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < size; ++i)
        sum = uint8_t(sum + data[i]);
    return uint8_t(~sum);
}

std::vector<uint8_t> build_goo(const FileParams &p, const std::vector<std::vector<uint8_t>> &images)
{
    Writer w;
    w.out.reserve(header_size + images.size() * 4096);

    w.text("V3.0", 4);
    w.bytes(magic_tag);
    w.text(p.software, 32);
    w.text(p.software_version, 24);
    w.text(p.file_time, 24);
    w.text(p.printer_name, 32);
    w.text(p.printer_type, 32);
    w.text(p.profile_name, 32);
    w.u16(0);                       // anti aliasing level
    w.u16(0);                       // grey level
    w.u16(0);                       // blur level
    w.preview(p.small_preview, small_preview_side);
    w.bytes(delimiter);
    w.preview(p.big_preview, big_preview_side);
    w.bytes(delimiter);
    w.u32(uint32_t(p.layers.size()));
    w.u16(uint16_t(p.res_x));
    w.u16(uint16_t(p.res_y));
    w.boolean(p.mirror_x);
    w.boolean(p.mirror_y);
    w.f32(p.bed_x_mm);
    w.f32(p.bed_y_mm);
    w.f32(p.bed_z_mm);
    w.f32(p.layer_height_mm);
    w.f32(p.exposure_s);
    w.u8(1);                        // exposure delay mode: static times (before lift, after lift, after retract)
    w.f32(p.wait_after_cure_s);     // light off time, used by the other delay mode
    w.f32(p.wait_after_cure_s);     // bottom: wait before lift
    w.f32(0.f);                     // bottom: wait after lift
    w.f32(p.wait_after_retract_s);  // bottom: wait after retract
    w.f32(p.wait_after_cure_s);
    w.f32(0.f);
    w.f32(p.wait_after_retract_s);
    w.f32(p.bottom_exposure_s);
    w.u32(p.bottom_layer_count);
    w.f32(p.lift_mm);               // bottom lift
    w.f32(p.lift_mm_min);
    w.f32(p.lift_mm);               // lift
    w.f32(p.lift_mm_min);
    w.f32(p.retract_mm);            // bottom retract
    w.f32(p.retract_mm_min);
    w.f32(p.retract_mm);            // retract
    w.f32(p.retract_mm_min);
    for (int i = 0; i < 8; ++i)     // the second lift and retract of the bottom and normal layers: unused
        w.f32(0.f);
    w.u16(p.light_pwm);             // bottom
    w.u16(p.light_pwm);
    w.boolean(true);                // the layers carry their own settings
    w.u32(p.print_time_s);
    w.f32(p.volume_mm3);
    w.f32(p.weight_g);
    w.f32(p.cost);
    w.text("$", 8);
    w.u32(header_size);
    w.boolean(true);                // gray values use the whole 0 to 255 range
    w.u16(p.transition_layer_count);

    for (size_t i = 0; i < p.layers.size(); ++i) {
        const LayerParams &l = p.layers[i];
        const std::vector<uint8_t> &img = images[i];
        w.u16(0);                   // no pause
        w.f32(p.bed_z_mm);          // the Z to move to when paused
        w.f32(l.z_mm);
        w.f32(l.exposure_s);
        w.f32(l.wait_before_lift_s);    // light off time
        w.f32(l.wait_before_lift_s);
        w.f32(l.wait_after_lift_s);
        w.f32(l.wait_after_retract_s);
        w.f32(l.lift_mm);
        w.f32(l.lift_mm_min);
        w.f32(0.f);
        w.f32(0.f);
        w.f32(l.retract_mm);
        w.f32(l.retract_mm_min);
        w.f32(0.f);
        w.f32(0.f);
        w.u16(l.light_pwm);
        w.bytes(delimiter);
        w.u32(uint32_t(img.size() + 2));
        w.u8(0x55);
        w.bytes(img.data(), img.size());
        w.u8(checksum(img.data(), img.size()));
        w.bytes(delimiter);
    }
    w.bytes(ending);
    return std::move(w.out);
}

} // namespace goo

namespace {

struct GOORasterEncoder
{
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components)
    {
        return sla::EncodedRaster(goo::rle_encode(static_cast<const uint8_t*>(ptr), w * h * num_components), "goo");
    }
};

// A nearest neighbour RGB565 copy of a thumbnail (which is stored bottom to top) at side x side pixels.
std::vector<uint16_t> preview_565(const ThumbnailData &t, size_t side)
{
    std::vector<uint16_t> out(side * side);
    for (size_t y = 0; y < side; ++y) {
        const size_t sy = t.height - 1 - std::min<size_t>(t.height - 1, y * t.height / side);
        for (size_t x = 0; x < side; ++x) {
            const size_t sx = std::min<size_t>(t.width - 1, x * t.width / side);
            const unsigned char *s = &t.pixels[(sy * t.width + sx) * 4];
            out[y * side + x] = uint16_t(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
        }
    }
    return out;
}

} // namespace

std::unique_ptr<sla::RasterBase> GOOArchive::create_raster() const
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

sla::RasterEncoder GOOArchive::get_encoder() const
{
    return GOORasterEncoder{};
}

void GOOArchive::export_print(const std::string     fname,
                              const SLAPrint       &print,
                              const ThumbnailsList &thumbnails,
                              const std::string    &/*projectname*/)
{
    const SLAMaterialConfig    &mat = print.material_config();
    const SLAPrintObjectConfig &obj = print.default_object_config();
    const auto &levels = print.print_layers();
    const size_t layer_count = std::min(m_layers.size(), levels.size());

    double w = m_cfg.display_width.getFloat(), h = m_cfg.display_height.getFloat();
    size_t pw = size_t(m_cfg.display_pixels_x.getInt()), ph = size_t(m_cfg.display_pixels_y.getInt());
    if (m_cfg.display_orientation.getInt() == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }

    goo::FileParams p;
    p.software = "Pseudorca";
    p.software_version = SLIC3R_VERSION;
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    p.file_time = stamp;
    p.printer_name = m_cfg.printer_model.value.empty() ? std::string("standard") : m_cfg.printer_model.value;
    p.printer_type = "Default";
    p.profile_name = "Pseudorca";
    p.res_x = uint32_t(pw); p.res_y = uint32_t(ph);
    p.mirror_x = m_cfg.display_mirror_x.getBool();
    p.mirror_y = m_cfg.display_mirror_y.getBool();
    p.bed_x_mm = float(w); p.bed_y_mm = float(h); p.bed_z_mm = m_cfg.printable_height.getFloat();
    p.layer_height_mm = float(obj.layer_height.getFloat());
    p.exposure_s = float(mat.exposure_time.getFloat());
    p.bottom_exposure_s = float(mat.initial_exposure_time.getFloat());
    p.bottom_layer_count = uint32_t(std::clamp<size_t>(size_t(std::max(0, mat.bottom_layer_count.getInt())), 0, layer_count));
    p.transition_layer_count = uint16_t(std::min<size_t>(size_t(std::max(0, obj.faded_layers.getInt())), layer_count - p.bottom_layer_count));
    p.wait_after_cure_s = float(mat.delay_after_exposure.get_at(0));
    p.wait_after_retract_s = float(mat.delay_before_exposure.get_at(0));
    p.lift_mm = float(mat.lift_distance.getFloat());
    p.lift_mm_min = float(mat.lift_speed.getFloat());
    p.retract_mm = p.lift_mm;
    p.retract_mm_min = float(mat.retract_speed.getFloat());
    p.light_pwm = uint16_t(std::clamp(mat.light_pwm.getInt(), 0, 255));

    const SLAPrintStatistics &stats = print.print_statistics();
    const double volume_ml = (stats.objects_used_material + stats.support_used_material) / 1000.;
    p.volume_mm3 = float(volume_ml * 1000.);
    p.weight_g = float(volume_ml * mat.material_density.getFloat());
    const double bottle_ml = mat.bottle_volume.getFloat();
    p.cost = bottle_ml > 0. ? float(volume_ml * mat.bottle_cost.getFloat() / bottle_ml) : 0.f;

    // The bottom layers, then a linear fade from the bottom to the normal exposure.
    double time_s = 0.;
    for (size_t i = 0; i < layer_count; ++i) {
        goo::LayerParams l;
        l.z_mm = float(unscaled<double>(levels[i].level()));
        if (i < p.bottom_layer_count) {
            l.exposure_s = p.bottom_exposure_s;
        } else if (i < size_t(p.bottom_layer_count) + p.transition_layer_count) {
            const double k = double(i - p.bottom_layer_count + 1) / double(p.transition_layer_count + 1);
            l.exposure_s = float(p.bottom_exposure_s + (p.exposure_s - p.bottom_exposure_s) * k);
        } else {
            l.exposure_s = p.exposure_s;
        }
        l.wait_before_lift_s = p.wait_after_cure_s;
        l.wait_after_retract_s = p.wait_after_retract_s;
        l.lift_mm = p.lift_mm; l.lift_mm_min = p.lift_mm_min;
        l.retract_mm = p.retract_mm; l.retract_mm_min = p.retract_mm_min;
        l.light_pwm = p.light_pwm;
        p.layers.push_back(l);

        time_s += l.exposure_s + l.wait_before_lift_s + l.wait_after_retract_s;
        if (l.lift_mm_min > 0.f)
            time_s += l.lift_mm / l.lift_mm_min * 60.;
        if (l.retract_mm_min > 0.f)
            time_s += l.retract_mm / l.retract_mm_min * 60.;
    }
    p.print_time_s = uint32_t(std::ceil(time_s));

    if (! thumbnails.empty() && thumbnails.front().is_valid()) {
        p.small_preview = preview_565(thumbnails.front(), 116);
        p.big_preview   = preview_565(thumbnails.front(), 290);
    }

    std::vector<std::vector<uint8_t>> images;
    images.reserve(layer_count);
    for (size_t i = 0; i < layer_count; ++i) {
        const auto *data = static_cast<const uint8_t*>(m_layers[i].data());
        images.emplace_back(data, data + m_layers[i].size());
    }

    const std::vector<uint8_t> file = goo::build_goo(p, images);
    std::ofstream out(fname, std::ios::binary | std::ios::out | std::ios::trunc);
    if (! out)
        throw ExportError("Cannot open " + fname + " for writing.");
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    out.close();
    if (! out)
        throw ExportError("Writing " + fname + " failed.");
}

} // namespace Slic3r
