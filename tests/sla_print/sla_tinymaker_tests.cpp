#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <string>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PNGReadWrite.hpp"
#include "libslic3r/SLAPrintJob.hpp"

using namespace Slic3r;

TEST_CASE("The TinyMaker archive is registered", "[TinyMaker]")
{
    const ArchiveEntry *entry = get_archive_entry("TINYMAKER");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->ext) == "zip");
    CHECK(entry->wrfactoryfn);
}

TEST_CASE("A cube sliced for the TinyMaker is a folder of numbered PNGs at the screen's resolution", "[TinyMaker]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("tm-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    DynamicPrintConfig machine;
    machine.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    machine.set_deserialize_strict("display_width", "40");
    machine.set_deserialize_strict("display_height", "30");
    machine.set_deserialize_strict("display_orientation", "landscape");
    machine.set_deserialize_strict("display_pixels_x", "320");
    machine.set_deserialize_strict("display_pixels_y", "240");
    machine.set_deserialize_strict("printable_height", "68");
    machine.set_deserialize_strict("layer_height", "0.05");
    machine.set_deserialize_strict("initial_layer_height", "0.05");
    machine.set_deserialize_strict("supports_enable", "0");
    machine.set_deserialize_strict("pad_enable", "0");
    machine.set_deserialize_strict("hollowing_enable", "0");
    machine.set_deserialize_strict("sla_archive_format", "TINYMAKER");
    DynamicPrintConfig process;

    Model model;
    ModelObject *mo = model.add_object();
    mo->add_volume(load_model("20mm_cube.obj"));
    mo->add_instance();

    SLAJobParams params;
    params.machine    = &machine;
    params.process    = &process;
    params.output_dir = dir.string();
    const SLAJobResult res = run_sla_job(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.zip");
    CHECK(res.layer_count == Catch::Approx(400).margin(2));

    ZipperArchive arch = read_zipper_archive(res.archive_path, {"png"}, {});
    REQUIRE(arch.entries.size() == res.layer_count);

    // Every image sits in the job folder and is numbered from 1 without gaps.
    std::vector<std::string> names;
    for (const EntryBuffer &e : arch.entries) names.push_back(e.fname);
    std::sort(names.begin(), names.end());
    for (size_t i = 1; i <= res.layer_count; ++i) {
        const std::string want = "plate_1/" + std::to_string(i) + ".png";
        CHECK(std::binary_search(names.begin(), names.end(), want));
    }

    // A middle layer is a 20 x 20 mm square on a 40 x 30 mm, 320 x 240 pixel screen: 160 x 160 lit pixels.
    const auto it = std::find_if(arch.entries.begin(), arch.entries.end(),
                                 [](const EntryBuffer &e) { return e.fname == "plate_1/200.png"; });
    REQUIRE(it != arch.entries.end());
    png::ImageGreyscale img;
    png::ReadBuf rb{it->buf.data(), it->buf.size()};
    REQUIRE(png::decode_png(rb, img));
    CHECK(img.cols == 320);
    CHECK(img.rows == 240);
    const double lit = std::count_if(img.buf.begin(), img.buf.end(), [](uint8_t v) { return v > 127; });
    CHECK(lit == Catch::Approx(160. * 160.).epsilon(0.03));
}
