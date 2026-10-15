#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <test_utils.hpp>

#include <cmath>
#include <fstream>
#include <set>
#include <string>

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrintJob.hpp"

using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

nlohmann::json read_json(const fs::path &p)
{
    std::ifstream in(p.string());
    REQUIRE(in.good());
    return nlohmann::json::parse(in);
}

const fs::path catalogue_dir() { return fs::path(std::string(RESIN_DIR)); }

// The machine preset as a config: every key must be a real option with a value it accepts.
DynamicPrintConfig load_machine(const fs::path &file)
{
    const nlohmann::json j = read_json(file);
    DynamicPrintConfig cfg;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "type" || it.key() == "from" || it.key() == "name")
            continue;
        INFO(file.string() << " " << it.key());
        if (it.key() == "printer_technology") {
            cfg.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
            continue;
        }
        try {
            cfg.set_deserialize_strict(it.key(), it.value().get<std::string>());
        } catch (const std::exception &e) {
            FAIL(e.what());
        }
    }
    return cfg;
}

} // namespace

TEST_CASE("Every resin printer in the catalogue is a consistent machine preset", "[ResinCatalogue]")
{
    const nlohmann::json index = read_json(catalogue_dir() / "index.json");
    const auto &printers = index.at("printers");
    REQUIRE(printers.size() > 100);

    std::set<std::string> names;
    for (const nlohmann::json &e : printers) {
        const std::string label = e.at("vendor").get<std::string>() + " " + e.at("model").get<std::string>();
        INFO(label);
        CHECK(names.insert(label).second);
        const fs::path file = catalogue_dir() / e.at("file").get<std::string>();
        REQUIRE(fs::exists(file));
        const DynamicPrintConfig cfg = load_machine(file);

        const double w = cfg.opt_float("display_width"), h = cfg.opt_float("display_height");
        const int px = cfg.opt_int("display_pixels_x"), py = cfg.opt_int("display_pixels_y");
        CHECK(w > 10.); CHECK(h > 10.); CHECK(px > 100); CHECK(py > 100);
        CHECK(cfg.opt_float("printable_height") > 10.);
        // Pixel pitch is plausible: a display size that does not match the pixel count is the classic
        // file-rejected-by-the-printer mistake.
        const double pitch_x = w / px, pitch_y = h / py;
        CHECK(pitch_x > 0.005); CHECK(pitch_x < 0.2);
        // Some 12K and 16K panels really have rectangular pixels; the catalogue says which, so a typo in a size cannot
        // pass for one of them.
        const bool square = std::abs(pitch_x / pitch_y - 1.) <= 0.05;
        CHECK(e.at("nonsquare_pixels").get<bool>() == ! square);

        // A printer marked supported names a writer that exists, and uses it.
        const std::string format = e.at("format").get<std::string>();
        const std::string chosen = cfg.opt_string("sla_archive_format");
        CHECK(chosen == format);
        if (e.at("supported").get<bool>()) {
            CHECK(chosen == format);
            const ArchiveEntry *entry = get_archive_entry(format.c_str());
            CHECK(entry != nullptr);
            if (entry)
                CHECK(bool(entry->wrfactoryfn));
        } else {
            CHECK(get_archive_entry(format.c_str()) == nullptr);
        }
    }
}

TEST_CASE("A small cube slices for printers of different displays and formats", "[ResinCatalogue]")
{
    // The user's printer, a GKtwo; the TinyMaker; and two printers of formats written so far.
    const std::string label = GENERATE(as<std::string>{},
        "UniFormation/GKtwo", "TinyMaker/TinyMaker", "Elegoo/Saturn 3", "Anycubic/Photon Mono");
    INFO(label);
    const fs::path file = catalogue_dir() / "printers" / (label + ".json");
    REQUIRE(fs::exists(file));
    DynamicPrintConfig machine = load_machine(file);
    machine.set_deserialize_strict("layer_height", "0.2");
    machine.set_deserialize_strict("initial_layer_height", "0.2");
    machine.set_deserialize_strict("supports_enable", "0");
    machine.set_deserialize_strict("pad_enable", "0");
    machine.set_deserialize_strict("hollowing_enable", "0");
    DynamicPrintConfig process;

    fs::path dir = fs::temp_directory_path() / fs::unique_path("resin-cat-%%%%%%%%");
    struct Cleanup { fs::path p; ~Cleanup() { boost::system::error_code ec; fs::remove_all(p, ec); } } cleanup{dir};

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
    CHECK(res.layer_count == Catch::Approx(100).margin(2));      // 20 mm at 0.2 mm
    CHECK(fs::file_size(res.archive_path) > 1000);
}
