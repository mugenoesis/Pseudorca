#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/AnycubicPW.hpp"
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
float    f32_at(const Bytes &b, size_t at) { float v; REQUIRE(at + 4 <= b.size()); std::memcpy(&v, &b[at], 4); return v; }
std::string text_at(const Bytes &b, size_t at, size_t n) { REQUIRE(at + n <= b.size()); return std::string(reinterpret_cast<const char*>(&b[at]), strnlen(reinterpret_cast<const char*>(&b[at]), n)); }

} // namespace

TEST_CASE("The Anycubic run length codec matches the known encodings", "[Anycubic]")
{
    auto enc = [](const Bytes &px) { return anycubic::rle_encode(px.data(), px.size()); };
    CHECK(enc(Bytes(5, 0x00)) == Bytes{0x00, 0x05});
    CHECK(enc(Bytes(300, 0xFF)) == Bytes{0xF1, 0x2C});
    CHECK(enc(Bytes(3, 0x80)) == Bytes{0x83});
    CHECK(enc(Bytes(20, 0x80)) == Bytes{0x8F, 0x85});
    // Runs of black and white are cut at 4095 pixels.
    CHECK(enc(Bytes(5000, 0x00)) == Bytes{0x0F, 0xFF, 0x03, 0x89});
    // The count of lit pixels comes with it.
    uint32_t lit = 0;
    const Bytes px{0, 0, 0x40, 0x40, 0xFF, 0x0F};
    anycubic::rle_encode(px.data(), px.size(), &lit);
    CHECK(lit == 3);   // 0x0F rounds to black
}

TEST_CASE("Anycubic runs round trip at the four bit precision", "[Anycubic]")
{
    std::mt19937 rng(5);
    Bytes px;
    while (px.size() < 60000) {
        const size_t len = 1 + rng() % (rng() % 3 == 0 ? 6000 : 30);
        const uint8_t v = rng() % 3 == 0 ? 0 : rng() % 3 == 0 ? 0xFF : uint8_t(rng());
        px.insert(px.end(), len, v);
    }
    const Bytes enc = anycubic::rle_encode(px.data(), px.size());
    Bytes dec(px.size(), 0x11);
    REQUIRE(anycubic::rle_decode(enc.data(), enc.size(), dec.data(), dec.size()));
    for (size_t i = 0; i < px.size(); ++i) {
        const uint8_t hi = px[i] >> 4;
        REQUIRE(dec[i] == (hi == 0 ? 0 : hi == 0xF ? 0xFF : uint8_t((hi << 4) | hi)));
    }
    Bytes bad(px.size() - 1);
    CHECK_FALSE(anycubic::rle_decode(enc.data(), enc.size(), bad.data(), bad.size()));
}

TEST_CASE("The Anycubic formats are registered with their extensions", "[Anycubic]")
{
    for (const char *id : {"pw0", "pwx", "pwmo", "pwmx", "pwms", "pmsq", "dlp", "pwma", "pm3", "pm3m", "pwmb", "dl2p", "pmx2",
                           "pm3r", "pm3n", "pm5", "px6s", "pm5s", "m5sp"}) {
        INFO(id);
        const ArchiveEntry *entry = get_archive_entry(id);
        REQUIRE(entry != nullptr);
        CHECK(std::string(entry->ext) == id);
        CHECK(entry->wrfactoryfn);
    }
}

TEST_CASE("A cube sliced to an Anycubic format has the tables of its version and decodable layers", "[Anycubic]")
{
    struct Case { const char *id; uint32_t version; };
    const Case c = GENERATE(Case{"pwmo", 1}, Case{"pm3", 516}, Case{"pm5", 517}, Case{"pm5s", 518});
    INFO(c.id);
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("acu-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    DynamicPrintConfig machine;
    machine.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    machine.set_deserialize_strict("printer_model", "Photon Test");
    machine.set_deserialize_strict("display_width", "120");
    machine.set_deserialize_strict("display_height", "68");
    machine.set_deserialize_strict("display_orientation", "landscape");
    machine.set_deserialize_strict("display_pixels_x", "2560");
    machine.set_deserialize_strict("display_pixels_y", "1440");
    machine.set_deserialize_strict("printable_height", "150");
    machine.set_deserialize_strict("layer_height", "0.05");
    machine.set_deserialize_strict("initial_layer_height", "0.05");
    machine.set_deserialize_strict("supports_enable", "0");
    machine.set_deserialize_strict("pad_enable", "0");
    machine.set_deserialize_strict("hollowing_enable", "0");
    machine.set_deserialize_strict("exposure_time", "2.5");
    machine.set_deserialize_strict("initial_exposure_time", "30");
    machine.set_deserialize_strict("bottom_layer_count", "3");
    machine.set_deserialize_strict("faded_layers", "2");
    machine.set_deserialize_strict("lift_distance", "6");
    machine.set_deserialize_strict("lift_speed", "60");
    machine.set_deserialize_strict("retract_speed", "120");
    machine.set_deserialize_strict("sla_archive_format", c.id);
    DynamicPrintConfig process;

    Model model;
    ModelObject *mo = model.add_object();
    mo->add_volume(load_model("20mm_cube.obj"));
    mo->add_instance();
    SLAJobParams params;
    params.machine = &machine; params.process = &process; params.output_dir = dir.string();
    const SLAJobResult res = run_sla_job(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == std::string("plate_1.") + c.id);

    const Bytes f = read_file(res.archive_path);
    // The file mark
    CHECK(text_at(f, 0, 12) == "ANYCUBIC");
    CHECK(u32_at(f, 12) == c.version);
    size_t at = 20;
    const uint32_t a_header = u32_at(f, at); at += 4;
    const uint32_t a_software = u32_at(f, at); at += 4;
    const uint32_t a_preview = u32_at(f, at); at += 4;
    const uint32_t a_color = u32_at(f, at); at += 4;
    const uint32_t a_layers = u32_at(f, at); at += 4;
    at += 4;                                             // extra
    const uint32_t a_machine = c.version >= 516 ? u32_at(f, at) : 0; if (c.version >= 516) at += 4;
    const uint32_t a_images = u32_at(f, at); at += 4;
    CHECK(a_software == (c.version >= 517 ? a_software : 0u));
    CHECK(a_header == at + (c.version >= 517 ? 4u : 0u) + (c.version >= 518 ? 8u : 0u));
    CHECK((a_color != 0) == (c.version >= 515));
    REQUIRE(a_images <= f.size());

    // HEADER
    CHECK(text_at(f, a_header, 12) == "HEADER");
    const size_t h = a_header + 16;
    CHECK(f32_at(f, h + 4) == Catch::Approx(0.05f));    // layer height
    CHECK(f32_at(f, h + 8) == Catch::Approx(2.5f));
    CHECK(f32_at(f, h + 16) == Catch::Approx(30.f));
    CHECK(f32_at(f, h + 20) == Catch::Approx(3.f));     // bottom layers
    CHECK(f32_at(f, h + 24) == Catch::Approx(6.f));     // lift
    CHECK(f32_at(f, h + 28) == Catch::Approx(1.f));     // lift speed mm/s
    CHECK(f32_at(f, h + 32) == Catch::Approx(2.f));     // retract speed mm/s
    CHECK(u32_at(f, h + 44) == 2560u);
    CHECK(u32_at(f, h + 48) == 1440u);
    CHECK(u32_at(f, h + 72) == 2u);                     // transition layers
    CHECK(u32_at(f, a_header + 12) == (c.version >= 518 ? 96u : c.version >= 517 ? 92u : c.version >= 516 ? 84u : 80u));

    // PREVIEW: the table's length covers the whole table and the 224 x 168 picture.
    CHECK(text_at(f, a_preview, 12) == "PREVIEW");
    CHECK(u32_at(f, a_preview + 12) == 16u + 12u + 224u * 168u * 2u);
    CHECK(u32_at(f, a_preview + 16) == 224u);
    CHECK(u32_at(f, a_preview + 24) == 168u);

    if (c.version >= 516) {
        CHECK(text_at(f, a_machine, 12) == "MACHINE");
        CHECK(text_at(f, a_machine + 16, 96) == "Photon Test");
        CHECK(text_at(f, a_machine + 112, 16) == "pw0Img");
        CHECK(f32_at(f, a_machine + 16 + 96 + 16 + 8) == Catch::Approx(120.f));
        CHECK(u32_at(f, a_machine + 12) == (c.version >= 518 ? 224u : 156u));
    }
    if (c.version >= 517)
        CHECK(text_at(f, a_software, 32) == "Pseudorca");

    // LAYERDEF and the images it points to
    CHECK(text_at(f, a_layers, 12) == "LAYERDEF");
    const uint32_t layer_count = u32_at(f, a_layers + 16);
    CHECK(layer_count == res.layer_count);
    CHECK(u32_at(f, a_layers + 12) == 4u + 32u * layer_count);
    const size_t mid = layer_count / 2;
    const size_t def = a_layers + 20 + 32 * mid;
    const uint32_t data_at = u32_at(f, def), data_len = u32_at(f, def + 4);
    REQUIRE(data_at >= a_images);
    REQUIRE(data_at + data_len <= f.size());
    CHECK(f32_at(f, def + 8) == Catch::Approx(6.f));
    CHECK(f32_at(f, def + 12) == Catch::Approx(1.f));
    CHECK(f32_at(f, def + 16) == Catch::Approx(2.5f));
    CHECK(f32_at(f, def + 20) == Catch::Approx(0.05f));
    Bytes img(2560 * 1440);
    REQUIRE(anycubic::rle_decode(&f[data_at], data_len, img.data(), img.size()));
    const double lit = std::count_if(img.begin(), img.end(), [](uint8_t v) { return v > 127; });
    CHECK(lit == Catch::Approx((20. / (120. / 2560)) * (20. / (68. / 1440))).epsilon(0.02));
    // The stored count of lit pixels is the picture's.
    CHECK(u32_at(f, def + 24) == Catch::Approx(double(std::count_if(img.begin(), img.end(), [](uint8_t v) { return v != 0; }))).margin(1));

    // The first layer uses the bottom exposure, and the last the normal one.
    CHECK(f32_at(f, a_layers + 20 + 16) == Catch::Approx(30.f));
    CHECK(f32_at(f, a_layers + 20 + 32 * (layer_count - 1) + 16) == Catch::Approx(2.5f));
}
