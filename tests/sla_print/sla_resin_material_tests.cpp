#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Format/ZipperArchiveImport.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ResinMaterial.hpp"
#include "libslic3r/SLAPrintJob.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;
using json = nlohmann::json;

namespace {

std::string read_text(const fs::path &p)
{
    std::ifstream in(p.string(), std::ios::binary);
    REQUIRE(in.good());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

fs::path resin_dir() { return fs::path(std::string(RESIN_DIR)); }
fs::path fixture_dir() { return fs::path(std::string(TEST_DATA_DIR)) / "resin"; }

const char *two_row_material = R"({
  "type": "material", "name": "Test resin", "vendor": "Test", "resin_type": "tough", "material_density": 1.1,
  "settings": [
    { "printers": ["Elegoo Mars 3"], "layer_height": 0.05, "exposure_time": 2.5, "initial_exposure_time": 30, "source": "https://example.test/a" },
    { "printers": ["Elegoo Mars 3"], "layer_height": 0.1,  "exposure_time": 4,   "initial_exposure_time": 40, "source": "https://example.test/b" },
    { "printers": ["Elegoo *"],      "layer_height": 0.05, "exposure_time": 3,   "initial_exposure_time": 35, "source": "https://example.test/c" },
    { "printers": ["*"],             "layer_height": 0.05, "exposure_time": 5,   "initial_exposure_time": 45, "source": "https://example.test/d" },
    { "printers": ["Anycubic Photon Mono"], "layer_height": 0.07, "exposure_time": 6, "initial_exposure_time": 50 }
  ] })";

} // namespace

TEST_CASE("A resin material picks the row of the printer and the layer height", "[ResinMaterial]")
{
    using PM = ResinMaterialChoice::PrinterMatch;

    // The printer's own row, by its name or by its model alone
    ResinMaterialChoice c = choose_resin_material(two_row_material, { "Mars 3", "Elegoo Mars 3" }, 0.05);
    REQUIRE(c.ok);
    CHECK(c.settings.at("exposure_time") == "2.5");
    CHECK(c.settings.at("initial_exposure_time") == "30");
    CHECK(c.printer_match == PM::Exact);
    CHECK(c.exact_layer_height);
    CHECK(c.source == "https://example.test/a");
    CHECK(c.settings.at("material_density") == "1.1");        // from the resin, not the row

    c = choose_resin_material(two_row_material, { "Mars 3" }, 0.1);
    REQUIRE(c.ok);
    CHECK(c.settings.at("exposure_time") == "4");

    // Another printer of the vendor, then any printer
    c = choose_resin_material(two_row_material, { "Saturn", "Elegoo Saturn" }, 0.05);
    REQUIRE(c.ok);
    CHECK(c.settings.at("exposure_time") == "3");
    CHECK(c.printer_match == PM::Vendor);
    c = choose_resin_material(two_row_material, { "Halot One", "Creality Halot One CL-60" }, 0.05);
    REQUIRE(c.ok);
    CHECK(c.settings.at("exposure_time") == "5");
    CHECK(c.printer_match == PM::Any);

    // A row at the asked layer height beats the printer's row at another one; with none, the nearest layer height is used
    c = choose_resin_material(two_row_material, { "Mars 3" }, 0.07);
    REQUIRE(c.ok);
    CHECK_FALSE(c.exact_layer_height);
    CHECK(c.layer_height == Catch::Approx(0.05).margin(1e-9));
    c = choose_resin_material(two_row_material, { "Photon Mono", "Anycubic Photon Mono" }, 0.07);
    REQUIRE(c.ok);
    CHECK(c.settings.at("exposure_time") == "6");
    CHECK(c.exact_layer_height);

    // A model name that only ends the same does not match another printer
    c = choose_resin_material(R"({"type":"material","name":"x","settings":[{"printers":["Elegoo Mars 3 Pro"],"layer_height":0.05,"exposure_time":9}]})",
                              { "Mars 3", "Elegoo Mars 3" }, 0.05);
    CHECK_FALSE(c.ok);
}

TEST_CASE("A resin material that is not usable says why", "[ResinMaterial]")
{
    CHECK_FALSE(choose_resin_material("not json", { "x" }, 0.05).ok);
    ResinMaterialChoice c = choose_resin_material(R"({"type":"machine"})", { "x" }, 0.05);
    CHECK_FALSE(c.ok);
    CHECK_FALSE(c.error.empty());
    c = choose_resin_material(R"({"type":"material","name":"y","settings":[{"printers":["Anycubic Photon"],"layer_height":0.05,"exposure_time":9}]})",
                              { "Mars 3" }, 0.05);
    CHECK_FALSE(c.ok);
    CHECK(c.error.find("no settings for this printer") != std::string::npos);
}

TEST_CASE("The chosen values go into the configuration", "[ResinMaterial]")
{
    const ResinMaterialChoice c = choose_resin_material(two_row_material, { "Mars 3" }, 0.05);
    REQUIRE(c.ok);
    DynamicPrintConfig cfg;
    apply_resin_material(c, cfg);
    CHECK(cfg.opt_float("exposure_time") == Catch::Approx(2.5));
    CHECK(cfg.opt_float("initial_exposure_time") == Catch::Approx(30.));
    CHECK(cfg.opt_float("material_density") == Catch::Approx(1.1));
}

TEST_CASE("A Chitubox resin profile imports as a material", "[ResinMaterial]")
{
    std::string error;
    const std::string text = read_text(fixture_dir() / "chitubox_siraya_blu_50um.cfg");
    const std::string material = resin_material_from_chitubox_cfg(text, "Peopoly Phenom", error);
    INFO(error);
    REQUIRE_FALSE(material.empty());

    const json j = json::parse(material);
    CHECK(j["name"] == "Siraya Blu 50um");
    CHECK(j["resin_type"] == "tough");
    CHECK(j["material_density"].get<double>() == Catch::Approx(1.1));
    CHECK(j["bottle_volume"].get<double>() == Catch::Approx(1000.));
    CHECK(j["bottle_cost"].get<double>() == Catch::Approx(30.));

    const ResinMaterialChoice c = choose_resin_material(material, { "Phenom", "Peopoly Phenom" }, 0.05);
    REQUIRE(c.ok);
    CHECK(c.settings.at("exposure_time") == "15");
    CHECK(c.settings.at("initial_exposure_time") == "65");
    CHECK(c.settings.at("bottom_layer_count") == "5");
    CHECK(c.settings.at("lift_distance") == "10");
    CHECK(c.settings.at("lift_speed") == "48");
    CHECK(c.settings.at("retract_speed") == "150");
    CHECK(c.settings.at("light_pwm") == "255");
    CHECK(c.settings.at("delay_before_exposure") == "0");
    // scoped to the printer it was made for
    CHECK_FALSE(choose_resin_material(material, { "Mars 3" }, 0.05).ok);

    // and with no printer, for any
    const std::string any = resin_material_from_chitubox_cfg(text, "", error);
    CHECK(choose_resin_material(any, { "Mars 3" }, 0.05).ok);

    CHECK(resin_material_from_chitubox_cfg("layerHeight:0.05\n", "", error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("PrusaSlicer SLA materials import with their inherited values", "[ResinMaterial]")
{
    std::string error;
    const std::vector<std::string> materials = resin_materials_from_prusaslicer_ini(read_text(fixture_dir() / "prusaslicer_anycubic_material.ini"), "", error);
    INFO(error);
    REQUIRE(materials.size() == 3);        // the two "*common*" sections are not resins

    const ResinMaterialChoice blue = choose_resin_material(materials[0], { "Photon Mono" }, 0.05);
    REQUIRE(blue.ok);
    CHECK(blue.name == "Generic Blue Resin @MONO 0.05");
    CHECK(blue.resin_type == "tough");
    CHECK(blue.settings.at("exposure_time") == "2.5");
    CHECK(blue.settings.at("initial_exposure_time") == "40");
    CHECK(blue.settings.at("lift_distance") == "8");          // from the inherited notes
    CHECK(blue.settings.at("lift_speed") == "150");           // 2.5 mm/s
    CHECK(blue.settings.at("retract_speed") == "180");        // 3 mm/s

    const ResinMaterialChoice grey = choose_resin_material(materials[1], { "Photon Mono" }, 0.05);
    REQUIRE(grey.ok);
    CHECK(grey.settings.at("exposure_time") == "2.3");
    CHECK(grey.settings.at("initial_exposure_time") == "23");
    CHECK(grey.settings.at("lift_speed") == "120");           // the section's own notes replace the inherited ones
    CHECK_FALSE(grey.settings.count("lift_distance"));

    CHECK(resin_materials_from_prusaslicer_ini("[sla_material:x]\nfoo = 1\n", "", error).empty());
    CHECK_FALSE(error.empty());
}

TEST_CASE("Every resin material in the library is complete and refers to printers that exist", "[ResinMaterial]")
{
    // the printers of the catalogue
    std::set<std::string> printers;
    {
        const json index = json::parse(read_text(resin_dir() / "index.json"));
        for (const json &p : index.at("printers"))
            printers.insert(p.at("vendor").get<std::string>() + " " + p.at("model").get<std::string>());
    }

    const json index = json::parse(read_text(resin_dir() / "materials-index.json"));
    REQUIRE(index.at("materials").size() >= 5);
    const std::set<std::string> keys = { "printers", "layer_height", "exposure_time", "initial_exposure_time", "bottom_layer_count",
        "faded_layers", "lift_distance", "lift_speed", "retract_speed", "delay_before_exposure", "delay_after_exposure", "light_pwm",
        "material_density", "bottle_volume", "bottle_weight", "bottle_cost", "source", "retrieved", "notes" };
    std::set<std::string> names;
    for (const json &entry : index.at("materials")) {
        const std::string label = entry.at("name").get<std::string>();
        INFO(label);
        CHECK(names.insert(label).second);
        const json m = json::parse(read_text(resin_dir() / entry.at("file").get<std::string>()));
        CHECK(m.at("type") == "material");
        CHECK(m.at("name") == label);
        CHECK_FALSE(m.at("vendor").get<std::string>().empty());
        REQUIRE(m.at("settings").size() == entry.at("rows").get<size_t>());
        REQUIRE(m.at("settings").size() > 0);
        for (const json &row : m.at("settings")) {
            for (auto it = row.begin(); it != row.end(); ++it) { INFO(it.key()); CHECK(keys.count(it.key()) == 1); }
            // where the numbers come from, and when they were read
            const std::string source = row.at("source").get<std::string>();
            CHECK(source.rfind("https://", 0) == 0);
            CHECK(row.at("retrieved").get<std::string>().size() == 10);
            // plausible values
            const double lh = row.at("layer_height").get<double>();
            CHECK(lh >= 0.01); CHECK(lh <= 0.2);
            const double e = row.at("exposure_time").get<double>(), b = row.at("initial_exposure_time").get<double>();
            CHECK(e >= 0.5); CHECK(e <= 60.);
            CHECK(b >= e); CHECK(b <= 200.);
            if (row.contains("lift_distance")) { CHECK(row["lift_distance"].get<double>() >= 1.); CHECK(row["lift_distance"].get<double>() <= 30.); }
            if (row.contains("lift_speed")) { CHECK(row["lift_speed"].get<double>() >= 5.); CHECK(row["lift_speed"].get<double>() <= 400.); }
            for (const json &p : row.at("printers")) {
                const std::string name = p.get<std::string>();
                INFO(name);
                CHECK(printers.count(name) == 1);
            }
        }
    }
}

TEST_CASE("A resin material sets the exposure of a sliced cube, and the process profile still wins", "[ResinMaterial]")
{
    fs::path dir = fs::temp_directory_path() / fs::unique_path("rm-job-%%%%%%%%");
    struct Cleanup { fs::path p; ~Cleanup() { boost::system::error_code ec; fs::remove_all(p, ec); } } cleanup{dir};

    // the printer from the catalogue, written as a Chitubox zip so that its settings can be read in the G-code
    DynamicPrintConfig machine;
    {
        const json j = json::parse(read_text(resin_dir() / "printers" / "Elegoo" / "Mars 3.json"));
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.key() == "type" || it.key() == "from" || it.key() == "name") continue;
            if (it.key() == "printer_technology") { machine.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA)); continue; }
            machine.set_deserialize_strict(it.key(), it.value().get<std::string>());
        }
    }
    machine.set_deserialize_strict("sla_archive_format", "CHITUBOXZIP");
    machine.set_deserialize_strict("layer_height", "0.05");
    machine.set_deserialize_strict("initial_layer_height", "0.05");
    machine.set_deserialize_strict("supports_enable", "0");
    machine.set_deserialize_strict("pad_enable", "0");
    machine.set_deserialize_strict("hollowing_enable", "0");

    auto printer_names = [&]() {
        return std::vector<std::string>{ machine.opt_string("printer_model"), machine.opt_string("printer_settings_id") };
    };
    const ResinMaterialChoice choice = choose_resin_material(read_text(resin_dir() / "materials" / "Siraya Tech" / "Easy Grey.json"), printer_names(), 0.05);
    REQUIRE(choice.ok);
    CHECK(choice.printer_match == ResinMaterialChoice::PrinterMatch::Exact);
    DynamicPrintConfig material;
    apply_resin_material(choice, material);

    auto slice = [&](DynamicPrintConfig &process, const std::string &sub) {
        Model model;
        ModelObject *mo = model.add_object();
        mo->add_volume(load_model("20mm_cube.obj"));
        mo->add_instance();
        mo->instances.front()->set_scaling_factor(Vec3d(0.25, 0.25, 0.25));     // 5 mm: 100 layers
        SLAJobParams params;
        params.machine = &machine; params.material = &material; params.process = &process;
        params.output_dir = (dir / sub).string();
        const SLAJobResult res = run_sla_job(model, params);
        REQUIRE(res.code == CLI_SUCCESS);
        ZipperArchive arch = read_zipper_archive(res.archive_path, { "gcode" }, {});
        std::string gcode;
        for (const EntryBuffer &e : arch.entries)
            if (e.fname == "run.gcode") gcode.assign(e.buf.begin(), e.buf.end());
        REQUIRE_FALSE(gcode.empty());
        return gcode;
    };

    DynamicPrintConfig process;
    const std::string g = slice(process, "a");
    // Siraya Tech's Easy Grey on a Mars 3 at 0.05 mm: 2 s, 20 s for the bottom layers, 5 of them, lifting 5 mm at 60 mm/min
    CHECK(g.find(";normalExposureTime:2\n") != std::string::npos);
    CHECK(g.find(";bottomLayExposureTime:20\n") != std::string::npos);
    CHECK(g.find(";bottomLayerCount:5\n") != std::string::npos);
    CHECK(g.find(";normalLayerLiftHeight:5\n") != std::string::npos);
    CHECK(g.find(";normalLayerLiftSpeed:60\n") != std::string::npos);
    CHECK(g.find(";normalDropSpeed:200\n") != std::string::npos);

    // a value in the process profile wins over the material
    DynamicPrintConfig override_process;
    override_process.set_deserialize_strict("exposure_time", "3.5");
    const std::string h = slice(override_process, "b");
    CHECK(h.find(";normalExposureTime:3.5\n") != std::string::npos);
    CHECK(h.find(";bottomLayExposureTime:20\n") != std::string::npos);
}
