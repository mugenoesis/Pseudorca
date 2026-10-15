#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrintJob.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

// A 120 x 68 mm, 2560 x 1440 px display, 0.05 mm layers.
DynamicPrintConfig resin_machine(const std::string &archive_format = "SL1")
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    // Some formats (CXDLP) name the printer model in the file and refuse a name without a model code.
    cfg.set_deserialize_strict("printer_model", "Halot One CL-60");
    cfg.set_deserialize_strict("display_width", "120");
    cfg.set_deserialize_strict("display_height", "68");
    cfg.set_deserialize_strict("display_pixels_x", "2560");
    cfg.set_deserialize_strict("display_pixels_y", "1440");
    cfg.set_deserialize_strict("printable_height", "150");
    cfg.set_deserialize_strict("layer_height", "0.05");
    cfg.set_deserialize_strict("initial_layer_height", "0.3");
    cfg.set_deserialize_strict("supports_enable", "0");
    cfg.set_deserialize_strict("pad_enable", "0");
    cfg.set_deserialize_strict("hollowing_enable", "0");
    cfg.set_deserialize_strict("sla_archive_format", archive_format);
    return cfg;
}

Model cube_model(const std::string &obj = "20mm_cube.obj")
{
    Model model;
    ModelObject *mo = model.add_object();
    mo->name = obj;
    mo->add_volume(load_model(obj));
    mo->add_instance();
    return model;
}

struct TempDir
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sla-job-%%%%%%%%");
    ~TempDir() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
};

SLAJobResult run(Model &model, const DynamicPrintConfig &machine, const TempDir &dir, bool keep_positions = false)
{
    DynamicPrintConfig process;
    SLAJobParams params;
    params.machine        = &machine;
    params.process        = &process;
    params.output_dir     = dir.path.string();
    params.keep_positions = keep_positions;
    return run_sla_job(model, params);
}

} // namespace

TEST_CASE("A cube is sliced to an archive in the output directory", "[SLAJob]")
{
    TempDir dir;
    Model model = cube_model();
    const SLAJobResult res = run(model, resin_machine(), dir);

    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(res.error.empty());
    CHECK(res.archive_format == "SL1");
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.sl1");
    CHECK(boost::filesystem::file_size(res.archive_path) > 0);
    // 20 mm: the 0.3 mm first layer, then 0.05 mm layers.
    CHECK(res.layer_count == Catch::Approx(395).margin(3));
    CHECK(res.layer_height == Catch::Approx(0.05));
    CHECK(res.print_time_s > 0.);
    // A solid 20 mm cube is 8 ml.
    CHECK(res.resin_used_ml == Catch::Approx(8.).epsilon(0.05));
}

TEST_CASE("The archive extension follows the format", "[SLAJob]")
{
    for (const ArchiveEntry &entry : registered_sla_archives()) {
        if (! entry.wrfactoryfn)
            continue;
        TempDir dir;
        Model model = cube_model();
        const SLAJobResult res = run(model, resin_machine(entry.id), dir);
        INFO("format " << entry.id << ": " << res.error);
        REQUIRE(res.code == CLI_SUCCESS);
        CHECK(boost::filesystem::path(res.archive_path).extension() == std::string(".") + entry.ext);
    }
}

TEST_CASE("Hollowing and supports change the resin used", "[SLAJob]")
{
    TempDir dir_solid, dir_hollow;
    Model solid = cube_model(), hollow = cube_model();
    DynamicPrintConfig hollow_machine = resin_machine();
    hollow_machine.set_deserialize_strict("hollowing_enable", "1");
    hollow_machine.set_deserialize_strict("hollowing_min_thickness", "2");

    const SLAJobResult a = run(solid, resin_machine(), dir_solid);
    const SLAJobResult b = run(hollow, hollow_machine, dir_hollow);
    REQUIRE(a.code == CLI_SUCCESS);
    REQUIRE(b.code == CLI_SUCCESS);
    CHECK(b.resin_used_ml < a.resin_used_ml * 0.7);
}

TEST_CASE("Several objects are packed on the display", "[SLAJob]")
{
    TempDir dir;
    Model model = cube_model();
    for (int i = 0; i < 3; ++i) {
        ModelObject *mo = model.add_object();
        mo->add_volume(load_model("20mm_cube.obj"));
        mo->add_instance();
    }
    const SLAJobResult res = run(model, resin_machine(), dir);
    REQUIRE(res.code == CLI_SUCCESS);

    // Every instance ends up inside the 120 x 68 mm display.
    for (const ModelObject *mo : model.objects) {
        const BoundingBoxf3 bb = mo->instance_bounding_box(0);
        CHECK(bb.min.x() >= -EPSILON);
        CHECK(bb.min.y() >= -EPSILON);
        CHECK(bb.max.x() <= 120. + EPSILON);
        CHECK(bb.max.y() <= 68. + EPSILON);
    }
    // And the solid volume is four cubes, not one.
    CHECK(res.resin_used_ml == Catch::Approx(32.).epsilon(0.05));
}

TEST_CASE("Failures come back as CLI codes, with nothing written", "[SLAJob]")
{
    SECTION("an empty model") {
        TempDir dir;
        Model model;
        const SLAJobResult res = run(model, resin_machine(), dir);
        CHECK(res.code == CLI_NO_SUITABLE_OBJECTS);
        CHECK(! boost::filesystem::exists(dir.path));
    }
    SECTION("a printer without a display") {
        TempDir dir;
        Model model = cube_model();
        DynamicPrintConfig machine = resin_machine();
        machine.set_deserialize_strict("display_pixels_x", "0");
        CHECK(run(model, machine, dir).code == CLI_CONFIG_FILE_ERROR);
        CHECK(! boost::filesystem::exists(dir.path));
    }
    SECTION("an unknown archive format") {
        TempDir dir;
        Model model = cube_model();
        const SLAJobResult res = run(model, resin_machine("no-such-format"), dir);
        CHECK(res.code == CLI_CONFIG_FILE_ERROR);
        CHECK(res.error.find("no-such-format") != std::string::npos);
        CHECK(! boost::filesystem::exists(dir.path));
    }
    SECTION("an object larger than the display") {
        TempDir dir;
        Model model = cube_model();
        model.objects.front()->scale(Vec3d(7., 1., 1.)); // 140 mm wide on a 120 mm display
        CHECK(run(model, resin_machine(), dir).code == CLI_OBJECT_ARRANGE_FAILED);
        CHECK(! boost::filesystem::exists(dir.path));
    }
    SECTION("missing profiles") {
        SLAJobParams params;
        Model model = cube_model();
        CHECK(run_sla_job(model, params).code == CLI_INVALID_PARAMS);
    }
}
