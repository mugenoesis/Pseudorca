#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/SVGX.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrintJob.hpp"

using namespace Slic3r;

namespace {

using Bytes = std::vector<uint8_t>;

Bytes read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint32_t u32_at(const Bytes &b, size_t at) { uint32_t v; REQUIRE(at + 4 <= b.size()); std::memcpy(&v, &b[at], 4); return v; }

// The signed area (shoelace) of each "M ... Z" ring in a path, in mm2.
std::vector<double> ring_areas(const std::string &path)
{
    std::vector<double> areas;
    std::istringstream in(path);
    std::string tok;
    std::vector<std::pair<double, double>> pts;
    auto flush = [&] {
        if (pts.size() < 3) { pts.clear(); return; }
        double a = 0;
        for (size_t i = 0; i < pts.size(); ++i) {
            const auto &p = pts[i], &q = pts[(i + 1) % pts.size()];
            a += p.first * q.second - q.first * p.second;
        }
        areas.push_back(std::abs(a) / 2.);
        pts.clear();
    };
    std::vector<double> nums;
    while (in >> tok) {
        if (tok == "M" || tok == "L") continue;
        if (tok == "Z") { flush(); continue; }
        nums.push_back(std::stod(tok));
        if (nums.size() == 2) { pts.emplace_back(nums[0], nums[1]); nums.clear(); }
    }
    return areas;
}

} // namespace

TEST_CASE("The SVGX archive is registered", "[SVGX]")
{
    const ArchiveEntry *entry = get_archive_entry("SVGX");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->ext) == "svgx");
    CHECK(entry->wrfactoryfn);
}

TEST_CASE("A layer image is traced into outlines with holes in the same path, in millimetres around the display's middle", "[SVGX]")
{
    // A 100 x 80 pixel display of 10 x 8 mm (0.1 mm pixels): a 40 x 40 pixel square with a 10 x 10 hole, and a separate 10 x 10 square.
    const size_t w = 100, h = 80;
    Bytes px(w * h, 0);
    for (size_t y = 10; y < 50; ++y) for (size_t x = 10; x < 50; ++x) px[y * w + x] = 255;
    for (size_t y = 20; y < 30; ++y) for (size_t x = 20; x < 30; ++x) px[y * w + x] = 0;
    for (size_t y = 60; y < 70; ++y) for (size_t x = 70; x < 80; ++x) px[y * w + x] = 255;

    const svgx::Outline o = svgx::trace_layer(px.data(), w, h, 0.1, 0.1, 10., 8.);
    REQUIRE(o.any);
    REQUIRE(o.paths.size() == 2);

    // one path holds a ring of 16 mm2 and a hole of 1 mm2, the other a ring of 1 mm2
    std::vector<std::vector<double>> areas;
    for (const std::string &p : o.paths) areas.push_back(ring_areas(p));
    std::sort(areas.begin(), areas.end(), [](auto &a, auto &b) { return a.size() > b.size(); });
    REQUIRE(areas[0].size() == 2);
    REQUIRE(areas[1].size() == 1);
    CHECK(*std::max_element(areas[0].begin(), areas[0].end()) == Catch::Approx(16.).epsilon(0.1));
    CHECK(*std::min_element(areas[0].begin(), areas[0].end()) == Catch::Approx(1.).epsilon(0.3));
    CHECK(areas[1][0] == Catch::Approx(1.).epsilon(0.3));

    // The coordinates are relative to the middle of the display: x from 1 - 5 = -4 to 8 - 5 = 3, y from 1 - 4 = -3 to 7 - 4 = 3.
    CHECK(o.min_x == Catch::Approx(-4.f).margin(0.2));
    CHECK(o.max_x == Catch::Approx(3.f).margin(0.2));
    CHECK(o.min_y == Catch::Approx(-3.f).margin(0.2));
    CHECK(o.max_y == Catch::Approx(3.f).margin(0.2));

    // An empty image has no outline.
    Bytes empty(w * h, 0);
    const svgx::Outline none = svgx::trace_layer(empty.data(), w, h, 0.1, 0.1, 10., 8.);
    CHECK_FALSE(none.any);
    CHECK(none.paths.empty());
}

TEST_CASE("A BMP preview has its header and rows", "[SVGX]")
{
    Bytes rgba(2 * 2 * 4);
    for (size_t i = 0; i < 4; ++i) { rgba[i * 4] = uint8_t(10 * (i + 1)); rgba[i * 4 + 1] = 20; rgba[i * 4 + 2] = 30; rgba[i * 4 + 3] = 255; }
    const Bytes bmp = svgx::make_bmp(rgba.data(), 2, 2);
    CHECK(bmp[0] == 'B'); CHECK(bmp[1] == 'M');
    CHECK(u32_at(bmp, 2) == bmp.size());
    CHECK(u32_at(bmp, 10) == 54u);
    CHECK(u32_at(bmp, 18) == 2u); CHECK(u32_at(bmp, 22) == 2u);
    CHECK(bmp.size() == 54 + 2 * 8);        // rows of 6 bytes padded to 8
    CHECK(bmp[54] == 30); CHECK(bmp[55] == 20); CHECK(bmp[56] == 10);   // BGR of the first pixel
}

TEST_CASE("A cube sliced to SVGX has the header, the print parameters and a square outline per layer", "[SVGX]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("svgx-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    DynamicPrintConfig machine;
    machine.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    machine.set_deserialize_strict("printer_model", "Foto Test");
    machine.set_deserialize_strict("display_width", "120");
    machine.set_deserialize_strict("display_height", "68");
    machine.set_deserialize_strict("display_orientation", "landscape");
    machine.set_deserialize_strict("display_pixels_x", "1920");
    machine.set_deserialize_strict("display_pixels_y", "1080");
    machine.set_deserialize_strict("printable_height", "150");
    machine.set_deserialize_strict("layer_height", "0.2");
    machine.set_deserialize_strict("initial_layer_height", "0.2");
    machine.set_deserialize_strict("supports_enable", "0");
    machine.set_deserialize_strict("pad_enable", "0");
    machine.set_deserialize_strict("hollowing_enable", "0");
    machine.set_deserialize_strict("exposure_time", "2.5");
    machine.set_deserialize_strict("initial_exposure_time", "30");
    machine.set_deserialize_strict("bottom_layer_count", "3");
    machine.set_deserialize_strict("faded_layers", "5");
    machine.set_deserialize_strict("sla_archive_format", "SVGX");
    DynamicPrintConfig process;

    Model model;
    ModelObject *mo = model.add_object();
    mo->add_volume(load_model("20mm_cube.obj"));
    mo->add_instance();
    SLAJobParams params;
    params.machine = &machine; params.process = &process; params.output_dir = dir.string();
    const SLAJobResult res = run_sla_job(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.svgx");

    const Bytes f = read_file(res.archive_path);
    REQUIRE(f.size() > 28);
    CHECK(std::string(f.begin(), f.begin() + 11) == "DLP-II 1.1\n");
    const uint32_t doc_at = u32_at(f, 16 + 8);
    REQUIRE(doc_at < f.size());
    const std::string doc(f.begin() + doc_at, f.end());
    CHECK(doc.find("<svg version=\"1.1\"") != std::string::npos);

    auto attr = [&](const std::string &tag, const std::string &name) {
        std::smatch m;
        const std::regex re("<" + tag + "[^>]*? " + name + "=\"([^\"]*)\"");
        REQUIRE(std::regex_search(doc, m, re));
        return m[1].str();
    };
    CHECK(attr("printparams", "machinename") == "Foto Test");
    CHECK(std::stof(attr("printparams", "layerheight")) == Catch::Approx(0.2f));
    CHECK(std::stoul(attr("printparams", "layercount")) == res.layer_count);
    CHECK(std::stoul(attr("printparams", "resolutionx")) == 1920u);
    CHECK(std::stof(attr("printparams", "displaywidth")) == Catch::Approx(120.f));
    CHECK(std::stof(attr("printparams", "machinez")) == Catch::Approx(150.f));
    CHECK(std::stoul(attr("projectiontime", "attachlayer")) == 3u);
    CHECK(std::stoul(attr("projectiontime", "buildinlayer")) == 5u);
    CHECK(std::stof(attr("projectiontime", "attachtime")) == Catch::Approx(30.f));
    CHECK(std::stof(attr("projectiontime", "basetime")) == Catch::Approx(2.5f));
    CHECK(std::stof(attr("printrange", "minx")) == Catch::Approx(-10.f).margin(0.6));
    CHECK(std::stof(attr("printrange", "maxx")) == Catch::Approx(10.f).margin(0.6));

    // the middle layer's group has one path: a 20 x 20 mm square
    const std::string needle = "<g id=\"layer-" + std::to_string(res.layer_count / 2) + "\"";
    const size_t g = doc.find(needle);
    REQUIRE(g != std::string::npos);
    const size_t d = doc.find(" d=\"", g);
    REQUIRE(d != std::string::npos);
    const size_t d_end = doc.find('"', d + 4);
    const std::vector<double> areas = ring_areas(doc.substr(d + 4, d_end - d - 4));
    REQUIRE(areas.size() == 1);
    CHECK(areas[0] == Catch::Approx(400.).epsilon(0.03));
    CHECK(doc.find("<g id=\"layer-" + std::to_string(res.layer_count - 1) + "\"") != std::string::npos);
    CHECK(doc.find("<g id=\"background\">") != std::string::npos);
}
