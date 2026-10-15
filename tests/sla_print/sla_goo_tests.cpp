#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
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

#include "libslic3r/Format/GOO.hpp"
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

// A reader written from the format description (UVtools GooFile.cs), independent of the writer.
struct Cursor
{
    const Bytes &b;
    size_t at { 0 };
    uint32_t u32() { REQUIRE(at + 4 <= b.size()); uint32_t v = (uint32_t(b[at]) << 24) | (uint32_t(b[at+1]) << 16) | (uint32_t(b[at+2]) << 8) | b[at+3]; at += 4; return v; }
    uint16_t u16() { REQUIRE(at + 2 <= b.size()); uint16_t v = uint16_t((b[at] << 8) | b[at+1]); at += 2; return v; }
    uint8_t  u8()  { REQUIRE(at < b.size()); return b[at++]; }
    float    f32() { uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    std::string text(size_t n) { REQUIRE(at + n <= b.size()); std::string s(b.begin() + at, b.begin() + at + n); at += n; return s.substr(0, s.find('\0')); }
    void skip(size_t n) { at += n; }
};

struct ParsedLayer
{
    float z, exposure, lift, lift_speed, retract, retract_speed, wait_before_lift, wait_after_retract;
    uint16_t pwm;
    Bytes pixels;
};

struct ParsedGoo
{
    std::string version, software, printer;
    uint32_t layer_count, header_size_field, bottom_layers, print_time;
    uint16_t res_x, res_y, transition;
    float x, y, z, layer_height, exposure, bottom_exposure, lift, lift_speed, retract_speed, volume;
    uint16_t pwm;
    std::vector<ParsedLayer> layers;
};

ParsedGoo parse_goo(const Bytes &file)
{
    ParsedGoo g;
    Cursor c{file};
    g.version = c.text(4);
    const Bytes tag{0x07, 0x00, 0x00, 0x00, 0x44, 0x4C, 0x50, 0x00};
    REQUIRE(std::equal(tag.begin(), tag.end(), file.begin() + c.at));
    c.skip(8);
    g.software = c.text(32);
    c.skip(24 + 24);                    // software version, time
    g.printer = c.text(32);
    c.skip(32 + 32);                    // printer type, profile
    c.skip(6);                          // aa, grey, blur
    c.skip(116 * 116 * 2);
    REQUIRE(c.u8() == 0x0D); REQUIRE(c.u8() == 0x0A);
    c.skip(290 * 290 * 2);
    REQUIRE(c.u8() == 0x0D); REQUIRE(c.u8() == 0x0A);
    g.layer_count = c.u32();
    g.res_x = c.u16(); g.res_y = c.u16();
    c.skip(2);                          // mirrors
    g.x = c.f32(); g.y = c.f32(); g.z = c.f32();
    g.layer_height = c.f32();
    g.exposure = c.f32();
    c.skip(1 + 4 + 6 * 4);              // delay mode, turn off time, the six waits
    g.bottom_exposure = c.f32();
    g.bottom_layers = c.u32();
    c.skip(8);
    g.lift = c.f32(); g.lift_speed = c.f32();
    c.skip(8);
    c.skip(4); g.retract_speed = c.f32();
    c.skip(8 * 4);                      // second lifts and retracts
    c.skip(2);
    g.pwm = c.u16();
    c.skip(1);                          // per layer settings
    g.print_time = c.u32();
    g.volume = c.f32();
    c.skip(8 + 8);                      // weight, cost, currency
    g.header_size_field = c.u32();
    c.skip(1);
    g.transition = c.u16();
    REQUIRE(c.at == goo::header_size);

    const size_t px = size_t(g.res_x) * g.res_y;
    for (uint32_t i = 0; i < g.layer_count; ++i) {
        ParsedLayer l;
        c.skip(2);                      // pause
        c.skip(4);                      // pause position
        l.z = c.f32(); l.exposure = c.f32();
        c.skip(4);                      // light off time
        l.wait_before_lift = c.f32();
        c.skip(4);
        l.wait_after_retract = c.f32();
        l.lift = c.f32(); l.lift_speed = c.f32();
        c.skip(8);
        l.retract = c.f32(); l.retract_speed = c.f32();
        c.skip(8);
        l.pwm = c.u16();
        REQUIRE(c.u8() == 0x0D); REQUIRE(c.u8() == 0x0A);
        const uint32_t len = c.u32();
        REQUIRE(len >= 2);
        REQUIRE(c.u8() == 0x55);
        REQUIRE(c.at + len - 1 <= file.size());
        const uint8_t *data = &file[c.at];
        c.skip(len - 2);
        const uint8_t sum = c.u8();
        CHECK(sum == goo::checksum(data, len - 2));
        REQUIRE(c.u8() == 0x0D); REQUIRE(c.u8() == 0x0A);
        l.pixels.assign(px, 0xAA);
        REQUIRE(goo::rle_decode(data, len - 2, l.pixels.data(), px));
        g.layers.push_back(std::move(l));
    }
    const Bytes ending{0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x44, 0x4C, 0x50, 0x00};
    REQUIRE(file.size() - c.at == ending.size());
    CHECK(std::equal(ending.begin(), ending.end(), file.begin() + c.at));
    return g;
}

} // namespace

TEST_CASE("The GOO run length codec matches the format's known encodings", "[GOO]")
{
    auto enc = [](const Bytes &px) { return goo::rle_encode(px.data(), px.size()); };
    CHECK(enc(Bytes(10, 0x00)) == Bytes{0x0A});
    CHECK(enc(Bytes(5, 0xFF)) == Bytes{0xC5});
    CHECK(enc(Bytes(3, 0x80)) == Bytes{0x43, 0x80});
    CHECK(enc(Bytes(300, 0xFF)) == Bytes{0xDC, 0x12});
    CHECK(enc(Bytes(0x12345, 0x00)) == Bytes{0x25, 0x12, 0x34});

    // A decoder accepts the format's difference chunks as well: +3 from the previous gray, then five of those.
    Bytes diff{0x43, 0x80, 0x83, 0x93, 0x05};
    Bytes out(3 + 1 + 5);
    REQUIRE(goo::rle_decode(diff.data(), diff.size(), out.data(), out.size()));
    CHECK(out == Bytes{0x80, 0x80, 0x80, 0x83, 0x86, 0x86, 0x86, 0x86, 0x86});
}

TEST_CASE("GOO runs round trip, including very long ones", "[GOO]")
{
    std::mt19937 rng(7);
    Bytes px;
    while (px.size() < 100000) {
        const size_t len = 1 + rng() % (rng() % 4 == 0 ? 5000 : 20);
        const uint8_t v = rng() % 3 == 0 ? 0 : rng() % 3 == 0 ? 0xFF : uint8_t(rng());
        px.insert(px.end(), len, v);
    }
    px.insert(px.end(), 0x1000100, 0xFF);   // longer than a 28 bit run field holds
    const Bytes enc = goo::rle_encode(px.data(), px.size());
    Bytes dec(px.size(), 0x11);
    REQUIRE(goo::rle_decode(enc.data(), enc.size(), dec.data(), dec.size()));
    CHECK(dec == px);

    Bytes bad(px.size() - 1);
    CHECK_FALSE(goo::rle_decode(enc.data(), enc.size(), bad.data(), bad.size()));
    CHECK(goo::checksum(enc.data(), enc.size()) == goo::checksum(enc.data(), enc.size()));
}

TEST_CASE("The GOO archive is registered", "[GOO]")
{
    const ArchiveEntry *entry = get_archive_entry("GOO");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->ext) == "goo");
    CHECK(entry->wrfactoryfn);
}

TEST_CASE("A cube sliced to GOO is a valid file with the print's settings", "[GOO]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("goo-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    DynamicPrintConfig machine;
    machine.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
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
    machine.set_deserialize_strict("lift_distance", "7");
    machine.set_deserialize_strict("lift_speed", "60");
    machine.set_deserialize_strict("retract_speed", "120");
    machine.set_deserialize_strict("light_pwm", "180");
    machine.set_deserialize_strict("sla_archive_format", "GOO");
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
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.goo");

    const ParsedGoo g = parse_goo(read_file(res.archive_path));
    CHECK(g.version == "V3.0");
    CHECK(g.header_size_field == goo::header_size);
    CHECK(g.res_x == 2560); CHECK(g.res_y == 1440);
    CHECK(g.x == Catch::Approx(120.f)); CHECK(g.y == Catch::Approx(68.f)); CHECK(g.z == Catch::Approx(150.f));
    CHECK(g.layer_count == res.layer_count);
    CHECK(g.layer_count == Catch::Approx(400).margin(2));
    CHECK(g.layer_height == Catch::Approx(0.05f));
    CHECK(g.exposure == Catch::Approx(2.5f));
    CHECK(g.bottom_exposure == Catch::Approx(30.f));
    CHECK(g.bottom_layers == 3);
    CHECK(g.transition == 2);
    CHECK(g.pwm == 180);
    CHECK(g.lift == Catch::Approx(7.f)); CHECK(g.lift_speed == Catch::Approx(60.f)); CHECK(g.retract_speed == Catch::Approx(120.f));
    CHECK(g.volume == Catch::Approx(8000.f).epsilon(0.05));
    CHECK(g.print_time > 400 * 2);

    REQUIRE(g.layers.size() == g.layer_count);
    CHECK(g.layers[0].exposure == Catch::Approx(30.f));
    CHECK(g.layers[3].exposure < 30.f);
    CHECK(g.layers[3].exposure > 2.5f);
    CHECK(g.layers[5].exposure == Catch::Approx(2.5f));
    CHECK(g.layers[200].pwm == 180);
    CHECK(g.layers[200].lift == Catch::Approx(7.f));
    CHECK(g.layers.back().z == Catch::Approx(20.f).margin(0.1));

    // A middle layer is a 20 x 20 mm square of lit pixels, centered on the display.
    const Bytes &mid = g.layers[200].pixels;
    const double lit = std::count_if(mid.begin(), mid.end(), [](uint8_t v) { return v > 127; });
    CHECK(lit == Catch::Approx((20. / (120. / 2560)) * (20. / (68. / 1440))).epsilon(0.02));
    const size_t row = 720;
    const auto first = std::find_if(mid.begin() + row * 2560, mid.begin() + (row + 1) * 2560, [](uint8_t v) { return v > 127; });
    const auto last  = std::find_if(std::make_reverse_iterator(mid.begin() + (row + 1) * 2560), std::make_reverse_iterator(mid.begin() + row * 2560), [](uint8_t v) { return v > 127; });
    CHECK(double(first - (mid.begin() + row * 2560)) == Catch::Approx(double((mid.begin() + (row + 1) * 2560) - last.base())).margin(3));
}
