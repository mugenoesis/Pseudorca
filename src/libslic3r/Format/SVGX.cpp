#include "SVGX.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/SLA/MarchingSquaresSLA.hpp"
#include "libslic3r/SLA/RasterBase.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/libslic3r.h"

namespace Slic3r {
namespace svgx {

namespace {

struct GrayImage
{
    const uint8_t *data;
    size_t         rows, cols;
};

} // namespace
} // namespace svgx
} // namespace Slic3r

namespace marchsq_sla {

template<> struct _RasterTraits<Slic3r::svgx::GrayImage> {
    using ValueType = uint8_t;
    static uint8_t get(const Slic3r::svgx::GrayImage &r, size_t row, size_t col) { return r.data[row * r.cols + col]; }
    static size_t rows(const Slic3r::svgx::GrayImage &r) { return r.rows; }
    static size_t cols(const Slic3r::svgx::GrayImage &r) { return r.cols; }
};

} // namespace marchsq_sla

namespace Slic3r {
namespace svgx {

namespace {

// The shortest text of a number in millimetres, three decimals at most.
std::string num(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string xml_escape(const std::string &s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

void put_u32(std::vector<uint8_t> &out, uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back(uint8_t(v >> (8 * i))); }

} // namespace

Outline trace_layer(const uint8_t *pixels, size_t width, size_t height, double pw, double ph, double dw, double dh)
{
    Outline o;
    const GrayImage img{pixels, height, width};
    const std::vector<marchsq_sla::Ring> rings = marchsq_sla::execute(img, uint8_t(128), marchsq_sla::Coord{2, 2});
    if (rings.empty())
        return o;

    Polygons polys;
    polys.reserve(rings.size());
    for (const marchsq_sla::Ring &ring : rings) {
        Polygon poly;
        poly.points.reserve(ring.size());
        for (const marchsq_sla::Coord &c : ring)
            poly.points.emplace_back(scaled(double(c.c)), scaled(double(c.r)));   // pixel units
        polys.emplace_back(std::move(poly));
    }
    const ExPolygons shapes = union_ex(polys);

    auto point_text = [&](const Point &p, std::string &out) {
        const double x = unscaled(p.x()) * pw - dw / 2., y = unscaled(p.y()) * ph - dh / 2.;
        const double rx = std::round(x * 1000.) / 1000., ry = std::round(y * 1000.) / 1000.;
        out += num(rx); out += ' '; out += num(ry);
        if (! o.any) { o.min_x = o.max_x = float(rx); o.min_y = o.max_y = float(ry); o.any = true; }
        o.min_x = std::min(o.min_x, float(rx)); o.max_x = std::max(o.max_x, float(rx));
        o.min_y = std::min(o.min_y, float(ry)); o.max_y = std::max(o.max_y, float(ry));
    };
    auto ring_text = [&](const Points &pts, std::string &out) {
        if (pts.empty()) return;
        out += "M ";
        point_text(pts.front(), out);
        out += " L";
        for (size_t i = 1; i < pts.size(); ++i) {
            out += ' ';
            point_text(pts[i], out);
        }
        out += " Z";
    };

    const double pixel_um = std::max(pw, ph) * 1000.;
    for (const ExPolygon &s : shapes) {
        std::string path;
        ring_text(s.contour.points, path);
        for (const Polygon &hole : s.holes) {
            path += ' ';
            ring_text(hole.points, path);
        }
        o.paths.push_back(std::move(path));
        // As UVtools does: the last outer outline's area in pixels divided by the pixel size in microns, cube rooted.
        const double area_px = std::abs(unscaled(unscaled(s.contour.area())));
        o.area = float(std::round(std::cbrt(area_px / pixel_um) * 1000.) / 1000.);
        o.perimeter = float(std::round(unscaled(s.contour.length()) / pixel_um * 1000.) / 1000.);
    }
    return o;
}

std::vector<uint8_t> make_bmp(const uint8_t *rgba, uint32_t w, uint32_t h)
{
    const uint32_t row = (w * 3 + 3) & ~3u;
    std::vector<uint8_t> out;
    out.reserve(54 + size_t(row) * h);
    out.push_back('B'); out.push_back('M');
    put_u32(out, 54 + row * h);
    put_u32(out, 0);
    put_u32(out, 54);
    put_u32(out, 40);
    put_u32(out, w); put_u32(out, h);
    put_u32(out, 0x180001);
    put_u32(out, 0);
    put_u32(out, row * h);
    put_u32(out, 3780); put_u32(out, 3780);
    put_u32(out, 0); put_u32(out, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t *s = rgba + (size_t(y) * w + x) * 4;
            out.push_back(s[2]); out.push_back(s[1]); out.push_back(s[0]);
        }
        for (uint32_t pad = w * 3; pad < row; ++pad)
            out.push_back(0);
    }
    return out;
}

std::vector<uint8_t> build_svgx(const FileParams &p)
{
    std::ostringstream svg;
    svg.imbue(std::locale::classic());
    auto attr = [&](const char *name, const std::string &v) { svg << ' ' << name << "=\"" << xml_escape(v) << '"'; };

    float min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (const Outline &l : p.layers) {
        if (! l.any) continue;
        min_x = std::min(min_x, l.min_x); min_y = std::min(min_y, l.min_y);
        max_x = std::max(max_x, l.max_x); max_y = std::max(max_y, l.max_y);
    }

    svg << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    svg << "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\">\n";
    svg << "<printparams";
    attr("machinename", p.machine_name);
    attr("materialname", p.material_name);
    attr("layerheight", num(p.layer_height_mm));
    attr("volume", num(p.volume_ml));
    attr("layercount", std::to_string(p.layers.size()));
    attr("lightintensity", "1");
    attr("resolutionx", std::to_string(p.res_x));
    attr("resolutiony", std::to_string(p.res_y));
    attr("displaywidth", num(p.display_width_mm));
    attr("displayheight", num(p.display_height_mm));
    attr("machinez", num(p.machine_z_mm));
    svg << ">\n";
    svg << "<projectiontime";
    attr("attachlayer", std::to_string(p.bottom_layer_count));
    attr("buildinlayer", std::to_string(p.transition_layer_count));
    attr("attachtime", num(p.bottom_exposure_s));
    attr("basetime", num(p.exposure_s));
    svg << " />\n<projectionadjust x=\"100\" y=\"100\" />\n<printrange";
    attr("minx", num(min_x)); attr("miny", num(min_y)); attr("minz", "0");
    attr("maxx", num(max_x)); attr("maxy", num(max_y)); attr("maxz", num(p.print_height_mm));
    svg << " />\n</printparams>\n<g id=\"background\">\n</g>\n";
    for (size_t i = 0; i < p.layers.size(); ++i) {
        const Outline &l = p.layers[i];
        svg << "<g id=\"layer-" << i << "\"";
        attr("area", num(l.area)); attr("perimeter", num(l.perimeter));
        svg << ">\n";
        for (const std::string &d : l.paths)
            svg << "<path d=\"" << d << "\" style=\"fill:white\" fill-rule=\"evenodd\" />\n";
        svg << "</g>\n";
    }
    svg << "</svg>\n";

    std::vector<uint8_t> out;
    const char ident[16] = { 'D','L','P','-','I','I',' ','1','.','1','\n', 0, 0, 0, 0, 0 };
    out.insert(out.end(), ident, ident + 16);
    const size_t addr_at = out.size();
    out.resize(out.size() + 12, 0);
    auto patch = [&](size_t at, uint32_t v) { std::memcpy(&out[at], &v, 4); };
    if (! p.preview1.empty()) { patch(addr_at, uint32_t(out.size())); out.insert(out.end(), p.preview1.begin(), p.preview1.end()); }
    if (! p.preview2.empty()) { patch(addr_at + 4, uint32_t(out.size())); out.insert(out.end(), p.preview2.begin(), p.preview2.end()); }
    patch(addr_at + 8, uint32_t(out.size()));
    const std::string text = svg.str();
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

} // namespace svgx

namespace {

// Per layer: the outline as text, kept in the raster slot until the file is written.
struct SVGXRasterEncoder
{
    double pw, ph, dw, dh;
    sla::EncodedRaster operator()(const void *ptr, size_t w, size_t h, size_t num_components)
    {
        (void) num_components;
        const svgx::Outline o = svgx::trace_layer(static_cast<const uint8_t*>(ptr), w, h, pw, ph, dw, dh);
        // area, perimeter, min x, min y, max x, max y, any; then one path per line
        std::ostringstream s;
        s.imbue(std::locale::classic());
        s << o.area << ' ' << o.perimeter << ' ' << o.min_x << ' ' << o.min_y << ' ' << o.max_x << ' ' << o.max_y << ' ' << (o.any ? 1 : 0) << '\n';
        for (const std::string &p : o.paths) s << p << '\n';
        const std::string t = s.str();
        return sla::EncodedRaster(std::vector<uint8_t>(t.begin(), t.end()), "svgx");
    }
};

svgx::Outline parse_outline(const sla::EncodedRaster &r)
{
    svgx::Outline o;
    const std::string t(reinterpret_cast<const char*>(r.data()), r.size());
    std::istringstream in(t);
    in.imbue(std::locale::classic());
    int any = 0;
    in >> o.area >> o.perimeter >> o.min_x >> o.min_y >> o.max_x >> o.max_y >> any;
    o.any = any != 0;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line))
        if (! line.empty()) o.paths.push_back(line);
    return o;
}

// A bottom to top RGBA thumbnail scaled (nearest) to w x h, still bottom to top.
std::vector<uint8_t> scaled_rgba(const ThumbnailData &t, uint32_t w, uint32_t h)
{
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const size_t sy = std::min<size_t>(t.height - 1, size_t(y) * t.height / h);
        for (uint32_t x = 0; x < w; ++x) {
            const size_t sx = std::min<size_t>(t.width - 1, size_t(x) * t.width / w);
            std::copy_n(&t.pixels[(sy * t.width + sx) * 4], 4, &out[(size_t(y) * w + x) * 4]);
        }
    }
    return out;
}

} // namespace

std::unique_ptr<sla::RasterBase> SVGXArchive::create_raster() const
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
    // The outlines are cut at half the gray range: a plain black and white raster is all that is needed.
    return sla::create_raster_grayscale_aa(sla::Resolution{pw, ph}, sla::PixelDim{w / pw, h / ph},
                                           m_cfg.gamma_correction.getFloat(), tr);
}

sla::RasterEncoder SVGXArchive::get_encoder() const
{
    double w = m_cfg.display_width.getFloat(), h = m_cfg.display_height.getFloat();
    double pw = m_cfg.display_pixels_x.getInt(), ph = m_cfg.display_pixels_y.getInt();
    if (m_cfg.display_orientation.getInt() == sla::RasterBase::roPortrait) {
        std::swap(w, h);
        std::swap(pw, ph);
    }
    return SVGXRasterEncoder{ w / pw, h / ph, w, h };
}

void SVGXArchive::export_print(const std::string     fname,
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

    svgx::FileParams p;
    p.machine_name = m_cfg.printer_model.value.empty() ? std::string("Unknown") : m_cfg.printer_model.value;
    p.material_name = "Unknown";
    p.layer_height_mm = float(obj.layer_height.getFloat());
    const SLAPrintStatistics &stats = print.print_statistics();
    p.volume_ml = float((stats.objects_used_material + stats.support_used_material) / 1000.);
    p.res_x = uint32_t(pw); p.res_y = uint32_t(ph);
    p.display_width_mm = float(w); p.display_height_mm = float(h);
    p.machine_z_mm = m_cfg.printable_height.getFloat();
    p.bottom_layer_count = uint32_t(std::clamp<size_t>(size_t(std::max(0, mat.bottom_layer_count.getInt())), 0, layer_count));
    p.transition_layer_count = uint32_t(std::min<size_t>(size_t(std::max(0, obj.faded_layers.getInt())), layer_count - p.bottom_layer_count));
    p.bottom_exposure_s = float(mat.initial_exposure_time.getFloat());
    p.exposure_s = float(mat.exposure_time.getFloat());
    p.print_height_mm = layer_count ? float(unscaled<double>(levels[layer_count - 1].level())) : 0.f;

    for (size_t i = 0; i < layer_count; ++i)
        p.layers.push_back(parse_outline(m_layers[i]));

    if (! thumbnails.empty() && thumbnails.front().is_valid()) {
        const ThumbnailData &t = thumbnails.front();
        p.preview1 = svgx::make_bmp(scaled_rgba(t, 128, 128).data(), 128, 128);
        p.preview2 = svgx::make_bmp(scaled_rgba(t, 200, 240).data(), 200, 240);
    }

    const std::vector<uint8_t> file = svgx::build_svgx(p);
    std::ofstream out(fname, std::ios::binary | std::ios::out | std::ios::trunc);
    if (! out)
        throw ExportError("Cannot open " + fname + " for writing.");
    out.write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
    out.close();
    if (! out)
        throw ExportError("Writing " + fname + " failed.");
}

} // namespace Slic3r
