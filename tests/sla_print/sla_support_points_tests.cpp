#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <test_utils.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLA/SupportPoint.hpp"
#include "libslic3r/SLAPrintJob.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

// A 120 x 68 mm, 2560 x 1440 px display, 0.05 mm layers, supports on, no pad or hollowing.
DynamicPrintConfig resin_machine()
{
    DynamicPrintConfig cfg;
    cfg.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    cfg.set_deserialize_strict("display_width", "120");
    cfg.set_deserialize_strict("display_height", "68");
    cfg.set_deserialize_strict("display_pixels_x", "2560");
    cfg.set_deserialize_strict("display_pixels_y", "1440");
    cfg.set_deserialize_strict("printable_height", "150");
    cfg.set_deserialize_strict("layer_height", "0.05");
    cfg.set_deserialize_strict("initial_layer_height", "0.3");
    cfg.set_deserialize_strict("supports_enable", "1");
    cfg.set_deserialize_strict("pad_enable", "0");
    cfg.set_deserialize_strict("hollowing_enable", "0");
    cfg.set_deserialize_strict("sla_archive_format", "SL1");
    return cfg;
}

// The 20 mm cube from tests/data, put in the file frame at `offset` (the instance carries it, as it does for a loaded STL).
Model cube_model(const Vec3d &offset = Vec3d::Zero())
{
    Model model;
    ModelObject *mo = model.add_object();
    mo->name = "20mm_cube.obj";
    mo->add_volume(load_model("20mm_cube.obj"));
    mo->add_instance()->set_offset(offset);
    return model;
}

struct TempDir
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sla-points-%%%%%%%%");
    TempDir() { boost::filesystem::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
};

SLAJobParams job(const DynamicPrintConfig &machine, const DynamicPrintConfig &process, const TempDir &dir)
{
    SLAJobParams params;
    params.machine    = &machine;
    params.process    = &process;
    params.output_dir = dir.path.string();
    return params;
}

// The distance of a point from the surface of the axis-aligned box.
double distance_to_box_surface(const Vec3f &p, const BoundingBoxf3 &box)
{
    const Vec3d d = p.cast<double>();
    double inside = std::numeric_limits<double>::max();
    for (int a = 0; a < 3; ++a)
        inside = std::min({ inside, std::abs(d[a] - box.min[a]), std::abs(d[a] - box.max[a]) });
    const bool in_box = d.x() >= box.min.x() - 1e-6 && d.x() <= box.max.x() + 1e-6 && d.y() >= box.min.y() - 1e-6 &&
                        d.y() <= box.max.y() + 1e-6 && d.z() >= box.min.z() - 1e-6 && d.z() <= box.max.z() + 1e-6;
    if (in_box)
        return inside;
    Vec3d clamped = d.cwiseMax(box.min).cwiseMin(box.max);
    return (d - clamped).norm();
}

} // namespace

TEST_CASE("A cube gets generated support points on its surface, in the place the file has it", "[SLASupportPoints]")
{
    TempDir dir;
    const Vec3d offset(30., -10., 10.);
    Model model = cube_model(offset);
    const BoundingBoxf3 box = model.objects.front()->instance_bounding_box(0);
    const DynamicPrintConfig machine = resin_machine();
    const DynamicPrintConfig process;

    const SLASupportPointsResult res = run_sla_support_points(model, job(machine, process, dir));
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(res.error.empty());
    REQUIRE(res.objects.size() == 1);
    const sla::SupportPoints &points = res.objects.front();
    CHECK(points.size() >= 4);                       // a 20 mm face cannot hang on fewer
    for (const sla::SupportPoint &p : points) {
        CHECK_THAT(distance_to_box_surface(p.pos, box), Catch::Matchers::WithinAbs(0., 0.3));
        CHECK(p.head_front_radius > 0.f);
    }
    // No slicing happened.
    CHECK_FALSE(boost::filesystem::exists(dir.path / "plate_1.sl1"));
}

TEST_CASE("Support points come back in the file frame wherever the objects are packed", "[SLASupportPoints]")
{
    TempDir dir;
    const DynamicPrintConfig machine = resin_machine();
    const DynamicPrintConfig process;

    Model packed = cube_model(Vec3d(30., -10., 10.));
    const BoundingBoxf3 box = packed.objects.front()->instance_bounding_box(0);
    SLAJobParams params = job(machine, process, dir);
    params.keep_positions = false;                   // the job moves the cube to the middle of the display
    const SLASupportPointsResult res = run_sla_support_points(packed, params);
    REQUIRE(res.code == CLI_SUCCESS);
    REQUIRE(res.objects.size() == 1);
    REQUIRE_FALSE(res.objects.front().empty());
    for (const sla::SupportPoint &p : res.objects.front())
        CHECK_THAT(distance_to_box_surface(p.pos, box), Catch::Matchers::WithinAbs(0., 0.3));
}

TEST_CASE("Hand-placed support points are used as they are, and come back unchanged", "[SLASupportPoints]")
{
    TempDir dir;
    const DynamicPrintConfig machine = resin_machine();
    const DynamicPrintConfig process;
    Model model = cube_model(Vec3d(30., -10., 10.));
    const BoundingBoxf3 box = model.objects.front()->instance_bounding_box(0);

    // three points on the underside of the cube
    const double z = box.min.z();
    sla::SupportPoints given = {
        sla::SupportPoint(Vec3f(float(box.min.x() + 5), float(box.min.y() + 5), float(z)), 0.4f),
        sla::SupportPoint(Vec3f(float(box.max.x() - 5), float(box.min.y() + 5), float(z)), 0.5f),
        sla::SupportPoint(Vec3f(float(box.center().x()), float(box.max.y() - 5), float(z)), 0.6f),
    };
    SLAJobParams params = job(machine, process, dir);
    params.support_points = { given };

    const SLASupportPointsResult res = run_sla_support_points(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    REQUIRE(res.objects.size() == 1);
    REQUIRE(res.objects.front().size() == given.size());
    for (size_t i = 0; i < given.size(); ++i) {
        const sla::SupportPoint &got = res.objects.front()[i];
        CHECK_THAT(got.pos.x(), Catch::Matchers::WithinAbs(given[i].pos.x(), 0.01));
        CHECK_THAT(got.pos.y(), Catch::Matchers::WithinAbs(given[i].pos.y(), 0.01));
        CHECK_THAT(got.pos.z(), Catch::Matchers::WithinAbs(given[i].pos.z(), 0.01));
        CHECK_THAT(got.head_front_radius, Catch::Matchers::WithinAbs(given[i].head_front_radius, 0.001));
    }
}

TEST_CASE("Slicing with hand-placed support points builds supports for exactly those points", "[SLASupportPoints]")
{
    TempDir dir;
    const DynamicPrintConfig machine = resin_machine();
    const DynamicPrintConfig process;
    Model model = cube_model();
    const BoundingBoxf3 box = model.objects.front()->instance_bounding_box(0);
    const double z = box.min.z();

    SLAJobParams params = job(machine, process, dir);
    params.support_points = { { sla::SupportPoint(Vec3f(float(box.center().x()), float(box.center().y()), float(z)), 0.5f),
                                sla::SupportPoint(Vec3f(float(box.min.x() + 3), float(box.min.y() + 3), float(z)), 0.5f) } };
    const SLAJobResult res = run_sla_job(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(res.support_point_count == 2);
}

TEST_CASE("An empty list of hand-placed points means no supports", "[SLASupportPoints]")
{
    TempDir dir;
    const DynamicPrintConfig machine = resin_machine();
    const DynamicPrintConfig process;
    Model model = cube_model();
    SLAJobParams params = job(machine, process, dir);
    params.support_points = { sla::SupportPoints() };

    const SLASupportPointsResult res = run_sla_support_points(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    REQUIRE(res.objects.size() == 1);
    CHECK(res.objects.front().empty());
}

TEST_CASE("Support points written as JSON read back the same", "[SLASupportPoints]")
{
    const std::vector<sla::SupportPoints> objects = {
        { sla::SupportPoint(Vec3f(1.5f, -2.25f, 3.f), 0.4f), sla::SupportPoint(Vec3f(10.f, 20.f, 0.f), 0.6f, sla::SupportPointType::island) },
        {},
        { sla::SupportPoint(Vec3f(0.f, 0.f, 7.f), 0.5f, sla::SupportPointType::slope) },
    };
    std::vector<sla::SupportPoints> back;
    std::string error;
    REQUIRE(support_points_from_json(support_points_to_json(objects), back, error));
    CHECK(error.empty());
    REQUIRE(back.size() == objects.size());
    for (size_t o = 0; o < objects.size(); ++o) {
        REQUIRE(back[o].size() == objects[o].size());
        for (size_t i = 0; i < objects[o].size(); ++i) {
            CHECK(back[o][i].pos == objects[o][i].pos);
            CHECK(back[o][i].head_front_radius == objects[o][i].head_front_radius);
            CHECK(back[o][i].type == objects[o][i].type);
        }
    }
}

TEST_CASE("A support point file that is not usable is refused with a reason", "[SLASupportPoints]")
{
    std::vector<sla::SupportPoints> out;
    std::string error;
    CHECK_FALSE(support_points_from_json("not json", out, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(support_points_from_json(R"({"objects": 3})", out, error));
    CHECK_FALSE(support_points_from_json(R"({"objects": [{"points": [{"x": 1, "y": 2}]}]})", out, error));       // no z or radius
    CHECK_FALSE(support_points_from_json(R"({"objects": [{"points": [{"x": 1, "y": 2, "z": 3, "radius": -1}]}]})", out, error));
    CHECK_FALSE(support_points_from_json(R"({"objects": [{"points": [{"x": 1, "y": 2, "z": 3, "radius": 0.5, "type": "bogus"}]}]})", out, error));
}

TEST_CASE("Nothing to place points on, or no printer profile, is an error", "[SLASupportPoints]")
{
    TempDir dir;
    const DynamicPrintConfig machine = resin_machine();
    const DynamicPrintConfig process;
    Model empty;
    CHECK(run_sla_support_points(empty, job(machine, process, dir)).code == CLI_NO_SUITABLE_OBJECTS);

    Model model = cube_model();
    SLAJobParams params;
    CHECK(run_sla_support_points(model, params).code == CLI_INVALID_PARAMS);
}
