#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/ImageZip.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrintJob.hpp"

using namespace Slic3r;

namespace {

std::vector<std::string> lines_of(const std::string &s)
{
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string l;
    while (std::getline(in, l)) out.push_back(l);
    return out;
}

bool has_line(const std::vector<std::string> &lines, const std::string &line)
{
    return std::find(lines.begin(), lines.end(), line) != lines.end();
}

} // namespace

TEST_CASE("The Chitubox zip is registered", "[ChituboxZip]")
{
    const ArchiveEntry *entry = get_archive_entry("CHITUBOXZIP");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->ext) == "zip");
    CHECK(entry->wrfactoryfn);
}

TEST_CASE("run.gcode lifts, retracts, shows the image and exposes for each layer", "[ChituboxZip]")
{
    chituzip::GCodeParams p;
    p.file_name = "job"; p.machine_type = "Test"; p.layer_height_mm = 0.05f;
    p.res_x = 2560; p.res_y = 1440; p.machine_x_mm = 120; p.machine_y_mm = 68; p.machine_z_mm = 150;
    p.exposure_s = 2.5f; p.bottom_exposure_s = 30.f; p.bottom_layer_count = 1;
    p.lift_mm = 6; p.lift_mm_min = 60; p.retract_mm_min = 120; p.pwm = 200; p.bottom_pwm = 200;
    for (int i = 0; i < 2; ++i) {
        chituzip::LayerParams l;
        l.z_mm = 0.05f * float(i + 1); l.exposure_s = i == 0 ? 30.f : 2.5f; l.wait_before_cure_s = 1.f; l.wait_after_cure_s = 0.5f;
        l.lift_mm = 6; l.lift_mm_min = 60; l.retract_mm_min = 120; l.pwm = 200;
        p.layers.push_back(l);
    }
    const std::vector<std::string> g = lines_of(chituzip::build_gcode(p));

    CHECK(has_line(g, ";fileName:job"));
    CHECK(has_line(g, ";layerHeight:0.05"));
    CHECK(has_line(g, ";resolutionX:2560"));
    CHECK(has_line(g, ";normalExposureTime:2.5"));
    CHECK(has_line(g, ";bottomLayerCount:1"));
    CHECK(has_line(g, ";totalLayer:2"));
    CHECK(has_line(g, ";PWMLight:200"));
    CHECK(has_line(g, ";START_GCODE_BEGIN"));
    CHECK(has_line(g, "G90;Absolute positioning"));
    CHECK(has_line(g, "G28 Z0;Home Z"));

    // layer 0: lift to z + 6, back to z, wait 1 s, 30 s of light at pwm 200, the image removed, wait 0.5 s
    const auto at = std::find(g.begin(), g.end(), ";LAYER_START:0");
    REQUIRE(at != g.end());
    const std::vector<std::string> layer0(at, std::find(g.begin(), g.end(), ";LAYER_END"));
    const std::vector<std::string> want{
        ";LAYER_START:0", ";PositionZ:0.05mm", "M6054 \"1.png\";Show image", "G0 Z6.05 F60;Z Lift (1)", "G0 Z0.05 F120;Retract (1)",
        "G4 P1000;Wait before cure", "M106 S200;Turn LED ON", "G4 P30000;Cure time/delay", "M106 S0;Turn LED OFF", ";<Slice> Blank",
        "G4 P500;Wait after cure" };
    CHECK(layer0 == want);
    CHECK(has_line(g, "M6054 \"2.png\";Show image"));
    CHECK(has_line(g, "G4 P2500;Cure time/delay"));
    CHECK(has_line(g, ";END_GCODE_END"));
    CHECK(has_line(g, ";<Completed>"));
    CHECK(has_line(g, "M18;Disable motors"));
}

TEST_CASE("A cube sliced to the Chitubox zip holds run.gcode and one image per layer", "[ChituboxZip]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("cz-job-%%%%%%%%");
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
    machine.set_deserialize_strict("exposure_time", "3");
    machine.set_deserialize_strict("initial_exposure_time", "20");
    machine.set_deserialize_strict("bottom_layer_count", "2");
    machine.set_deserialize_strict("lift_distance", "5");
    machine.set_deserialize_strict("lift_speed", "60");
    machine.set_deserialize_strict("retract_speed", "150");
    machine.set_deserialize_strict("sla_archive_format", "CHITUBOXZIP");
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

    ZipperArchive arch = read_zipper_archive(res.archive_path, {"png", "gcode"}, {});
    size_t pngs = 0;
    std::string gcode;
    for (const EntryBuffer &e : arch.entries) {
        if (e.fname == "run.gcode") gcode.assign(e.buf.begin(), e.buf.end());
        else if (e.fname.size() > 4 && e.fname.substr(e.fname.size() - 4) == ".png" && e.fname[0] != 'p') ++pngs;
    }
    CHECK(pngs == res.layer_count);
    REQUIRE_FALSE(gcode.empty());
    const std::vector<std::string> g = lines_of(gcode);
    CHECK(has_line(g, ";totalLayer:" + std::to_string(res.layer_count)));
    CHECK(has_line(g, ";normalExposureTime:3"));
    CHECK(has_line(g, ";bottomLayExposureTime:20"));
    CHECK(has_line(g, "M6054 \"" + std::to_string(res.layer_count) + ".png\";Show image"));
    CHECK(has_line(g, "G4 P20000;Cure time/delay"));    // the first layer, at the bottom exposure
    CHECK(has_line(g, "G4 P3000;Cure time/delay"));
}
