#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <test_utils.hpp>

#include <boost/filesystem.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SLAPrintJob.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

struct TempDir
{
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("sla-hollow-%%%%%%%%");
    TempDir() { boost::filesystem::create_directories(path); }
    ~TempDir() { boost::system::error_code ec; boost::filesystem::remove_all(path, ec); }
};

Model cube_model(const Vec3d &offset = Vec3d::Zero())
{
    Model model;
    ModelObject *mo = model.add_object();
    mo->name = "20mm_cube.obj";
    mo->add_volume(load_model("20mm_cube.obj"));
    ModelInstance *inst = mo->add_instance();
    inst->set_offset(offset);
    return model;
}

DynamicPrintConfig hollowing(double thickness)
{
    DynamicPrintConfig cfg;
    cfg.set_deserialize_strict("hollowing_min_thickness", std::to_string(thickness));
    return cfg;
}

SLAHollowResult hollow(Model &model, const DynamicPrintConfig &process, const std::string &path)
{
    SLAHollowParams params;
    params.process     = &process;
    params.output_path = path;
    return run_sla_hollow(model, params);
}

TriangleMesh read_stl(const std::string &path)
{
    TriangleMesh mesh;
    REQUIRE(mesh.ReadSTLFile(path.c_str()));
    return mesh;
}

} // namespace

TEST_CASE("A cube is hollowed to a mesh with a cavity, in the place it was", "[SLAHollow]")
{
    TempDir dir;
    const std::string out = (dir.path / "hollow.stl").string();
    Model model = cube_model(Vec3d(30., -10., 10.));
    const BoundingBoxf3 before = model.objects.front()->instance_bounding_box(0);

    const SLAHollowResult res = hollow(model, hollowing(2.), out);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(res.error.empty());
    CHECK(res.path == out);
    REQUIRE(boost::filesystem::exists(out));

    TriangleMesh mesh = read_stl(out);
    const double solid = before.size().x() * before.size().y() * before.size().z();
    // the cavity leaves the 2 mm walls: a 16 mm cube inside a 20 mm one is 4096 mm3 of the 8000, so about 3900 of material
    CHECK(mesh.volume() > 3000.);
    CHECK(mesh.volume() < 5000.);
    CHECK(mesh.volume() < solid * 0.7);
    CHECK(mesh.its.indices.size() > 12);                 // an outer shell and an inner one
    const BoundingBoxf3 after = mesh.bounding_box();
    CHECK_THAT(after.min.x(), Catch::Matchers::WithinAbs(before.min.x(), 0.05));
    CHECK_THAT(after.max.x(), Catch::Matchers::WithinAbs(before.max.x(), 0.05));
    CHECK_THAT(after.min.z(), Catch::Matchers::WithinAbs(before.min.z(), 0.05));
    CHECK_THAT(after.max.z(), Catch::Matchers::WithinAbs(before.max.z(), 0.05));
    CHECK_THAT(res.volume_before, Catch::Matchers::WithinRel(solid, 0.01));
    CHECK_THAT(res.volume_after, Catch::Matchers::WithinRel(double(mesh.volume()), 0.01));
}

TEST_CASE("A thicker wall leaves a smaller cavity", "[SLAHollow]")
{
    TempDir dir;
    Model thin = cube_model();
    Model thick = cube_model();
    REQUIRE(hollow(thin, hollowing(1.5), (dir.path / "thin.stl").string()).code == CLI_SUCCESS);
    REQUIRE(hollow(thick, hollowing(4.), (dir.path / "thick.stl").string()).code == CLI_SUCCESS);
    CHECK(read_stl((dir.path / "thick.stl").string()).volume() > read_stl((dir.path / "thin.stl").string()).volume());
}

TEST_CASE("Every object of the model is hollowed into the one file", "[SLAHollow]")
{
    TempDir dir;
    Model one = cube_model();
    Model two = cube_model();
    ModelObject *second = two.add_object();
    second->name = "second.obj";
    second->add_volume(load_model("20mm_cube.obj"));
    second->add_instance()->set_offset(Vec3d(50., 0., 0.));

    REQUIRE(hollow(one, hollowing(2.), (dir.path / "one.stl").string()).code == CLI_SUCCESS);
    REQUIRE(hollow(two, hollowing(2.), (dir.path / "two.stl").string()).code == CLI_SUCCESS);
    CHECK_THAT(double(read_stl((dir.path / "two.stl").string()).volume()), Catch::Matchers::WithinRel(2. * read_stl((dir.path / "one.stl").string()).volume(), 0.02));
}

TEST_CASE("Nothing to hollow, or no process profile, is an error and writes no file", "[SLAHollow]")
{
    TempDir dir;
    const std::string out = (dir.path / "none.stl").string();
    Model empty;
    CHECK(hollow(empty, hollowing(2.), out).code == CLI_NO_SUITABLE_OBJECTS);

    Model model = cube_model();
    SLAHollowParams params;
    params.output_path = out;
    CHECK(run_sla_hollow(model, params).code == CLI_INVALID_PARAMS);
    CHECK_FALSE(boost::filesystem::exists(out));
}
