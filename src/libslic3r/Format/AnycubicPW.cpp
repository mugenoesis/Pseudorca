#include "AnycubicPW.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#include <boost/log/trivial.hpp>

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {

namespace anycubic {

namespace {

constexpr uint32_t V1 = 1, V515 = 515, V516 = 516, V517 = 517, V518 = 518;
constexpr size_t   MarkSize = 12;
constexpr uint32_t MaxRun = 0xFFF;

class Buffer
{
public:
    size_t pos() const { return m_data.size(); }
    std::vector<uint8_t> take() { return std::move(m_data); }
    void u8(uint8_t v) { m_data.push_back(v); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void f32(float v) { put(&v, 4); }
    void bytes(const void *p, size_t n) { const auto *b = static_cast<const uint8_t*>(p); m_data.insert(m_data.end(), b, b + n); }
    void zeros(size_t n) { m_data.insert(m_data.end(), n, uint8_t(0)); }
    // Text in a fixed size, zero padded field.
    void text(const std::string &s, size_t size)
    {
        for (size_t i = 0; i < size; ++i)
            u8(i < s.size() && i + 1 < size ? uint8_t(s[i]) : uint8_t(0));
    }
    void patch(size_t at, uint32_t v) { std::memcpy(&m_data[at], &v, 4); }
    uint8_t *data(size_t at) { return &m_data[at]; }

private:
    void put(const void *p, size_t n) { bytes(p, n); }   // little endian hosts only, like the format
    std::vector<uint8_t> m_data;
};

uint32_t to_u32(size_t v)
{
    if (v > std::numeric_limits<uint32_t>::max())
        throw ExportError("The Anycubic file is too large for its 32 bit offsets.");
    return uint32_t(v);
}

// "NAME", then the length of what follows (or, for some tables, of the whole table).
void table_head(Buffer &b, const char *name, uint32_t length)
{
    b.text(name, MarkSize);
    b.u32(length);
}

void put_reps(std::vector<uint8_t> &out, uint32_t reps, int color)
{
    while (reps > 0) {
        uint32_t done = reps;
        if (color == 0 || color == 0xF) {
            done = std::min(done, MaxRun);
            const uint16_t more = uint16_t(done | (color << 12));
            out.push_back(uint8_t(more >> 8));
            out.push_back(uint8_t(more));
        } else {
            done = std::min<uint32_t>(done, 0xF);
            out.push_back(uint8_t(done | (color << 4)));
        }
        reps -= done;
    }
}

size_t table_size(uint32_t version) { return version >= V518 ? 96 : version >= V517 ? 92 : version >= V516 ? 84 : 80; }

} // namespace

std::vector<uint8_t> rle_encode(const uint8_t *pixels, size_t count, uint32_t *non_zero)
{
    std::vector<uint8_t> out;
    out.reserve(count / 32 + 16);
    int last = -1;
    uint32_t reps = 0, lit = 0;
    for (size_t i = 0; i < count;) {
        const int color = pixels[i] >> 4;
        size_t j = i + 1;
        while (j < count && (pixels[j] >> 4) == color)
            ++j;
        if (color != 0)
            lit += uint32_t(j - i);
        if (color == last) {
            reps += uint32_t(j - i);
        } else {
            put_reps(out, reps, last);
            last = color;
            reps = uint32_t(j - i);
        }
        i = j;
    }
    put_reps(out, reps, last);
    if (non_zero)
        *non_zero = lit;
    return out;
}

bool rle_decode(const uint8_t *data, size_t size, uint8_t *pixels, size_t count)
{
    size_t px = 0;
    for (size_t i = 0; i < size; ++i) {
        const uint8_t code = data[i] >> 4;
        size_t repeat = data[i] & 0xF;
        uint8_t color;
        if (code == 0 || code == 0xF) {
            color = code == 0 ? 0 : 0xFF;
            if (++i >= size)
                return false;
            repeat = (repeat << 8) + data[i];
        } else {
            color = uint8_t((code << 4) | code);
        }
        if (repeat > count - px)
            return false;
        std::fill(pixels + px, pixels + px + repeat, color);
        px += repeat;
    }
    return px == count;
}

std::vector<uint8_t> build_anycubic(const FileParams &p, const std::vector<std::vector<uint8_t>> &images)
{
    if (images.size() != p.layers.size())
        throw ExportError("The Anycubic file needs one image for each layer.");
    const uint32_t v = p.version;
    const size_t layer_count = p.layers.size();
    Buffer b;

    // The file mark: where the tables are. Filled in at the end.
    const uint32_t tables = v >= V518 ? 11 : v >= V517 ? 9 : v >= V516 ? 8 : v >= V515 ? 5 : 4;
    size_t mark_size = MarkSize + 4 * 9;                 // version, tables, header, software, preview, color, layers, extra, images
    if (v >= V516) mark_size += 4;                       // machine
    if (v >= V517) mark_size += 4;                       // model
    if (v >= V518) mark_size += 8;                       // sub layers, second preview
    b.zeros(mark_size);
    uint32_t a_software = 0, a_preview = 0, a_color = 0, a_layers = 0, a_extra = 0, a_machine = 0, a_images = 0,
             a_model = 0, a_sub = 0, a_preview2 = 0;
    const uint32_t a_header = to_u32(mark_size);

    // HEADER
    const uint32_t tl = uint32_t(table_size(v));
    table_head(b, "HEADER", tl);
    const size_t header_start = b.pos();
    b.f32(std::max(p.pixel_width_um, p.pixel_height_um));
    b.f32(p.layer_height_mm);
    b.f32(p.exposure_s);
    b.f32(p.wait_before_cure_s);
    b.f32(p.bottom_exposure_s);
    b.f32(float(p.bottom_layer_count));
    b.f32(p.lift_mm);
    b.f32(p.lift_mm_s);
    b.f32(p.retract_mm_s);
    b.f32(p.volume_ml);
    b.u32(v >= V515 ? 16 : 1);                           // anti aliasing
    b.u32(p.res_x); b.u32(p.res_y);
    b.f32(p.weight_g);
    b.f32(p.cost);
    b.u32('$');
    b.u32(0);                                            // per layer settings
    b.u32(p.print_time_s);
    b.u32(p.transition_layer_count);
    b.u32(0);                                            // transition layer type
    if (tl >= 84) b.u32(0);                              // advanced mode
    if (tl >= 92) { b.u16(0); b.u16(0); b.u32(0); }      // grey, blur level, resin type
    if (tl >= 96) b.u32(0);                              // intelligent mode
    if (b.pos() - header_start != tl)
        throw ExportError("Anycubic header size mismatch.");

    // PREVIEW
    {
        const uint32_t w = 224, h = 168;
        a_preview = to_u32(b.pos());
        table_head(b, "PREVIEW", uint32_t(MarkSize + 4 + 12 + w * h * 2));
        b.u32(w); b.text("x", 4); b.u32(h);
        std::vector<uint8_t> px = p.preview;
        px.resize(size_t(w) * h * 2, 0);
        b.bytes(px.data(), px.size());
    }

    // The gray values of the layer images' 4 bit colors.
    if (v >= V515) {
        a_color = to_u32(b.pos());
        b.u32(0);                                        // full greyscale
        b.u32(16);
        for (int i = 0; i < 16; ++i)
            b.u8(uint8_t(std::min(255.f, (i + 1) * (255.f / 16.f))));
        b.u32(0);
    }

    // LAYERDEF: the table is written once the images' positions are known.
    a_layers = to_u32(b.pos());
    const uint32_t layerdef_length = 4 + 32 * uint32_t(layer_count);
    const size_t layerdef_at = b.pos();
    b.zeros(MarkSize + 4 + layerdef_length);
    a_images = to_u32(b.pos());

    if (v >= V516) {
        a_extra = to_u32(b.pos());
        table_head(b, "EXTRA", 24);                      // what Photon Workshop writes, though the table is longer
        b.u32(2); b.f32(p.lift_mm); b.f32(p.lift_mm_s); b.f32(p.retract_mm_s);
        b.f32(0.f); b.f32(p.lift_mm_s); b.f32(p.retract_mm_s);
        b.u32(2); b.f32(p.lift_mm); b.f32(p.lift_mm_s); b.f32(p.retract_mm_s);
        b.f32(0.f); b.f32(p.lift_mm_s); b.f32(p.retract_mm_s);

        a_machine = to_u32(b.pos());
        const uint32_t machine_len = v >= V518 ? 224 : 156;
        const size_t machine_start = b.pos();
        table_head(b, "MACHINE", machine_len);
        b.text(p.machine_name, 96);
        b.text("pw0Img", 16);
        b.u32(16);                                       // the highest anti aliasing level
        b.u32(v >= V518 ? 15 : v >= V517 ? 7 : 1);       // property fields
        b.f32(p.display_width_mm); b.f32(p.display_height_mm); b.f32(p.machine_z_mm);
        b.u32(p.max_version);
        b.u32(6506241);                                  // machine background
        if (machine_len >= 224) {
            b.f32(p.pixel_width_um); b.f32(p.pixel_height_um);
            b.zeros(8 * 4);                              // paddings
            b.u32(1);                                    // display count
            b.u32(0);
            b.u16(uint16_t(p.res_x)); b.u16(uint16_t(p.res_y));
            b.zeros(4 * 4);
        }
        if (b.pos() - machine_start != machine_len)
            throw ExportError("Anycubic machine table size mismatch.");

        if (v >= V517) {
            a_software = to_u32(b.pos());
            b.text(p.software, 32);
            b.u32(164);
            b.text("1.0", 32);
            b.text("linux", 64);
            b.text("3.3-CoreProfile", 32);

            a_model = to_u32(b.pos());
            const size_t model_start = b.pos();
            table_head(b, "MODEL", 0);
            for (int i = 0; i < 3; ++i) b.f32(p.model_min[i]);
            for (int i = 0; i < 3; ++i) b.f32(p.model_max[i]);
            b.u32(0); b.f32(0.f);                        // supports
            b.patch(model_start + MarkSize, to_u32(b.pos() - model_start));

            if (v >= V518) {
                a_sub = to_u32(b.pos());
                b.zeros(MarkSize + 4 + 8 + 44 * layer_count);   // SUBIMGS, filled in below
                a_preview2 = to_u32(b.pos());
                const uint32_t w = 330, h = 190;
                table_head(b, "PREVIEW2", uint32_t(MarkSize + 4 + 12 + w * h * 2));
                b.u32(w); b.u16(4293); b.u16(0); b.u32(h);
                std::vector<uint8_t> px = p.preview2;
                px.resize(size_t(w) * h * 2, 0);
                b.bytes(px.data(), px.size());
            }
        }
        a_images = to_u32(b.pos());
    } else if (v == V515) {
        a_extra = a_images;
    }

    // The layer images.
    std::vector<uint32_t> addr(layer_count);
    for (size_t i = 0; i < layer_count; ++i) {
        addr[i] = to_u32(b.pos());
        b.bytes(images[i].data(), images[i].size());
    }

    // LAYERDEF
    {
        Buffer t;
        table_head(t, "LAYERDEF", layerdef_length);
        t.u32(uint32_t(layer_count));
        for (size_t i = 0; i < layer_count; ++i) {
            const LayerParams &l = p.layers[i];
            t.u32(addr[i]); t.u32(to_u32(images[i].size()));
            t.f32(l.lift_mm); t.f32(l.lift_mm_s);
            t.f32(l.exposure_s); t.f32(l.height_mm);
            t.u32(l.non_zero_pixels); t.u32(0);
        }
        const std::vector<uint8_t> bytes = t.take();
        std::memcpy(b.data(layerdef_at), bytes.data(), bytes.size());
    }
    // SUBIMGS
    if (v >= V518) {
        Buffer t;
        table_head(t, "SUBIMGS", uint32_t(MarkSize + 4 + 8 + 44 * layer_count));
        t.u32(uint32_t(layer_count)); t.u32(1);
        for (size_t i = 0; i < layer_count; ++i) {
            t.u32(addr[i]); t.u32(to_u32(images[i].size())); t.u32(p.layers[i].non_zero_pixels);
            t.zeros(8 * 4);
        }
        const std::vector<uint8_t> bytes = t.take();
        std::memcpy(b.data(a_sub), bytes.data(), bytes.size());
    }

    // The file mark
    Buffer m;
    m.text("ANYCUBIC", MarkSize);
    m.u32(v); m.u32(tables);
    m.u32(a_header); m.u32(a_software); m.u32(a_preview); m.u32(a_color); m.u32(a_layers); m.u32(a_extra);
    if (v >= V516) m.u32(a_machine);
    m.u32(a_images);
    if (v >= V517) m.u32(a_model);
    if (v >= V518) { m.u32(a_sub); m.u32(a_preview2); }
    const std::vector<uint8_t> mark = m.take();
    if (mark.size() != mark_size)
        throw ExportError("Anycubic file mark size mismatch.");
    std::vector<uint8_t> file = b.take();
    std::memcpy(file.data(), mark.data(), mark.size());
    return file;
}

} // namespace anycubic

namespace {

struct AnycubicPWRasterEncoder
{
    // The encoded image followed by the count of its non zero pixels (the writer cuts that off again).
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components)
    {
        uint32_t non_zero = 0;
        std::vector<uint8_t> out = anycubic::rle_encode(static_cast<const uint8_t*>(ptr), w * h * num_components, &non_zero);
        const uint8_t *c = reinterpret_cast<const uint8_t*>(&non_zero);
        out.insert(out.end(), c, c + 4);
        return sla::EncodedRaster(std::move(out), "pwimg");
    }
};

// A nearest neighbour RGB565 copy of a thumbnail (stored bottom to top), little endian.
std::vector<uint8_t> preview_565(const ThumbnailData &t, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> out(size_t(w) * h * 2);
    for (uint32_t y = 0; y < h; ++y) {
        const size_t sy = t.height - 1 - std::min<size_t>(t.height - 1, size_t(y) * t.height / h);
        for (uint32_t x = 0; x < w; ++x) {
            const size_t sx = std::min<size_t>(t.width - 1, size_t(x) * t.width / w);
            const unsigned char *s = &t.pixels[(sy * t.width + sx) * 4];
            const uint16_t px = uint16_t(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
            out[(size_t(y) * w + x) * 2]     = uint8_t(px);
            out[(size_t(y) * w + x) * 2 + 1] = uint8_t(px >> 8);
        }
    }
    return out;
}

} // namespace

std::unique_ptr<sla::RasterBase> AnycubicPWArchive::create_raster() const
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

sla::RasterEncoder AnycubicPWArchive::get_encoder() const
{
    return AnycubicPWRasterEncoder{};
}

void AnycubicPWArchive::export_print(const std::string     fname,
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

    anycubic::FileParams p;
    p.version = m_version; p.max_version = m_max_version;
    p.machine_name = m_cfg.printer_model.value.empty() ? std::string("Photon Mono") : m_cfg.printer_model.value;
    p.software = "Pseudorca";
    p.display_width_mm = float(w); p.display_height_mm = float(h); p.machine_z_mm = m_cfg.printable_height.getFloat();
    p.res_x = uint32_t(pw); p.res_y = uint32_t(ph);
    p.pixel_width_um = float(w / pw * 1000.); p.pixel_height_um = float(h / ph * 1000.);
    p.layer_height_mm = float(obj.layer_height.getFloat());
    p.exposure_s = float(mat.exposure_time.getFloat());
    p.bottom_exposure_s = float(mat.initial_exposure_time.getFloat());
    p.wait_before_cure_s = float(mat.delay_before_exposure.get_at(0));
    p.bottom_layer_count = uint32_t(std::clamp<size_t>(size_t(std::max(0, mat.bottom_layer_count.getInt())), 0, layer_count));
    p.transition_layer_count = uint32_t(std::min<size_t>(size_t(std::max(0, obj.faded_layers.getInt())), layer_count - p.bottom_layer_count));
    p.lift_mm = float(mat.lift_distance.getFloat());
    p.lift_mm_s = float(mat.lift_speed.getFloat() / 60.);
    p.retract_mm_s = float(mat.retract_speed.getFloat() / 60.);

    const SLAPrintStatistics &stats = print.print_statistics();
    p.volume_ml = float((stats.objects_used_material + stats.support_used_material) / 1000.);
    p.weight_g = p.volume_ml * float(mat.material_density.getFloat());
    const double bottle_ml = mat.bottle_volume.getFloat();
    p.cost = bottle_ml > 0. ? float(p.volume_ml * mat.bottle_cost.getFloat() / bottle_ml) : 0.f;

    // The model's box in the middle of the display.
    p.model_min[2] = 0.f;
    p.model_max[2] = layer_count ? float(unscaled<double>(levels[layer_count - 1].level())) : 0.f;

    double time_s = 0.;
    double prev_z = 0.;
    std::vector<std::vector<uint8_t>> images;
    for (size_t i = 0; i < layer_count; ++i) {
        anycubic::LayerParams l;
        const double z = unscaled<double>(levels[i].level());
        l.height_mm = float(z - prev_z);
        prev_z = z;
        if (i < p.bottom_layer_count) {
            l.exposure_s = p.bottom_exposure_s;
        } else if (i < size_t(p.bottom_layer_count) + p.transition_layer_count) {
            const double k = double(i - p.bottom_layer_count + 1) / double(p.transition_layer_count + 1);
            l.exposure_s = float(p.bottom_exposure_s + (p.exposure_s - p.bottom_exposure_s) * k);
        } else {
            l.exposure_s = p.exposure_s;
        }
        l.lift_mm = p.lift_mm; l.lift_mm_s = p.lift_mm_s;

        const auto *data = static_cast<const uint8_t*>(m_layers[i].data());
        if (m_layers[i].size() < 4)
            throw ExportError("An Anycubic layer image is too short.");
        const size_t n = m_layers[i].size() - 4;
        std::memcpy(&l.non_zero_pixels, data + n, 4);
        images.emplace_back(data, data + n);
        p.layers.push_back(l);

        time_s += l.exposure_s + p.wait_before_cure_s;
        if (p.lift_mm_s > 0.f) time_s += p.lift_mm / p.lift_mm_s;
        if (p.retract_mm_s > 0.f) time_s += p.lift_mm / p.retract_mm_s;
    }
    p.print_time_s = uint32_t(std::ceil(time_s));

    if (! thumbnails.empty() && thumbnails.front().is_valid()) {
        p.preview  = preview_565(thumbnails.front(), 224, 168);
        p.preview2 = preview_565(thumbnails.front(), 330, 190);
    }

    const std::vector<uint8_t> file = anycubic::build_anycubic(p, images);
    std::ofstream out(fname, std::ios::binary | std::ios::out | std::ios::trunc);
    if (! out)
        throw ExportError("Cannot open " + fname + " for writing.");
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    out.close();
    if (! out)
        throw ExportError("Writing " + fname + " failed.");
}

} // namespace Slic3r
