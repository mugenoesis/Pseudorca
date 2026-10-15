#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <string>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/ImageZip.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLAPrint.hpp"
#include "libslic3r/SLAPrintJob.hpp"

using namespace Slic3r;

namespace {

struct ExposedPhrozenZip : PhrozenZipArchive
{
    using PhrozenZipArchive::PhrozenZipArchive;
    void add_layer(std::vector<uint8_t> png) { m_layers.emplace_back(std::move(png), "png"); }
};

} // namespace

TEST_CASE("The Phrozen zip is registered", "[PhrozenZip]")
{
    const ArchiveEntry *entry = get_archive_entry("PHROZENZIP");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->ext) == "zip");
    CHECK(entry->wrfactoryfn);
}

TEST_CASE("The Phrozen zip holds the previews and the numbered layer images at its root, and nothing else", "[PhrozenZip]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("pz-%%%%%%%%");
    boost::filesystem::create_directories(dir);
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    ExposedPhrozenZip archive{SLAPrinterConfig{}};
    for (uint8_t i = 1; i <= 3; ++i)
        archive.add_layer({i, i, i});

    ThumbnailData thumb;
    thumb.set(64, 48);
    std::fill(thumb.pixels.begin(), thumb.pixels.end(), uint8_t(200));
    ThumbnailsList thumbs{thumb};

    SLAPrint print;
    const std::string file = (dir / "job.zip").string();
    archive.export_print(file, print, thumbs);

    ZipperArchive arch = read_zipper_archive(file, {"png", "gcode", "manifest", "json", "ini"}, {});
    std::vector<std::string> names;
    for (const EntryBuffer &e : arch.entries) names.push_back(e.fname);
    std::sort(names.begin(), names.end());
    const std::vector<std::string> expected{"1.png", "2.png", "3.png", "preview.png", "preview_cropping.png"};
    CHECK(names.size() == expected.size());
    for (const std::string &n : expected)
        CHECK(std::binary_search(names.begin(), names.end(), n));

    // The previews are real PNGs of the sizes Phrozen's software writes.
    for (const EntryBuffer &e : arch.entries) {
        if (e.fname != "preview.png" && e.fname != "preview_cropping.png")
            continue;
        png::ImageColorscale img;
        png::ReadBuf rb{e.buf.data(), e.buf.size()};
        REQUIRE(png::decode_colored_png(rb, img));
        CHECK(img.cols == (e.fname == "preview.png" ? 954u : 168u));
        CHECK(img.rows == (e.fname == "preview.png" ? 850u : 150u));
    }
}

TEST_CASE("A cube sliced for a Phrozen printer gives one image per layer at the screen's resolution", "[PhrozenZip]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("pz-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    DynamicPrintConfig machine;
    machine.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    machine.set_deserialize_strict("display_width", "40");
    machine.set_deserialize_strict("display_height", "30");
    machine.set_deserialize_strict("display_orientation", "landscape");
    machine.set_deserialize_strict("display_pixels_x", "320");
    machine.set_deserialize_strict("display_pixels_y", "240");
    machine.set_deserialize_strict("printable_height", "68");
    machine.set_deserialize_strict("layer_height", "0.1");
    machine.set_deserialize_strict("initial_layer_height", "0.1");
    machine.set_deserialize_strict("supports_enable", "0");
    machine.set_deserialize_strict("pad_enable", "0");
    machine.set_deserialize_strict("hollowing_enable", "0");
    machine.set_deserialize_strict("sla_archive_format", "PHROZENZIP");
    DynamicPrintConfig process;

    Model model;
    ModelObject *mo = model.add_object();
    mo->add_volume(load_model("20mm_cube.obj"));
    mo->add_instance();
    SLAJobParams params;
    params.machine = &machine; params.process = &process; params.output_dir = dir.string();
    const SLAJobResult res = run_sla_job(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.zip");

    ZipperArchive arch = read_zipper_archive(res.archive_path, {"png"}, {});
    REQUIRE(arch.entries.size() == res.layer_count);
    const auto it = std::find_if(arch.entries.begin(), arch.entries.end(), [](const EntryBuffer &e) { return e.fname == "100.png"; });
    REQUIRE(it != arch.entries.end());
    png::ImageGreyscale img;
    png::ReadBuf rb{it->buf.data(), it->buf.size()};
    REQUIRE(png::decode_png(rb, img));
    CHECK(img.cols == 320); CHECK(img.rows == 240);
    const double lit = std::count_if(img.buf.begin(), img.buf.end(), [](uint8_t v) { return v > 127; });
    CHECK(lit == Catch::Approx(160. * 160.).epsilon(0.03));
}
