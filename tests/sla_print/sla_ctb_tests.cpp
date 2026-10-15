#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>
#include <random>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/CTB.hpp"
#include "libslic3r/Format/SLAArchiveFormatRegistry.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/SLAPrintJob.hpp"
#include "libslic3r/Utils.hpp"

using namespace Slic3r;

namespace {

using Bytes = std::vector<uint8_t>;

uint32_t u32_at(const Bytes &b, size_t at) { uint32_t v; REQUIRE(at + 4 <= b.size()); std::memcpy(&v, &b[at], 4); return v; }
uint16_t u16_at(const Bytes &b, size_t at) { uint16_t v; REQUIRE(at + 2 <= b.size()); std::memcpy(&v, &b[at], 2); return v; }
float    f32_at(const Bytes &b, size_t at) { float v; REQUIRE(at + 4 <= b.size()); std::memcpy(&v, &b[at], 4); return v; }

Bytes read_file(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A reader of the CTB v4 layout written independently of the writer: it follows the structure definitions of UVtools
// (ChituboxFile.cs) and the mslicer reference file, and is checked against that file below.
struct ParsedLayer
{
    float z, exposure, light_off, lift, lift_speed, retract_speed, rest_after_retract, pwm;
    Bytes pixels;
};

struct ParsedPreview { uint32_t w, h; std::vector<uint8_t> rgb; };

struct ParsedCTB
{
    uint32_t magic, version, res_x, res_y, layer_count, bottom_layers, print_time, projector, xor_key;
    float    bed_x, bed_y, bed_z, total_height, layer_height, exposure, bottom_exposure, light_off;
    uint16_t pwm, bottom_pwm;
    uint32_t large_off, small_off, layer_table_off, params_off, params_size, slicer_off, slicer_size;
    // print parameters
    float bottom_lift, bottom_lift_speed, lift, lift_speed, retract_speed, volume, weight, cost;
    // slicer info
    std::string machine_name;
    uint32_t transition_layers, ext_off, timestamp, software_version;
    // v4 extension
    std::string disclaimer;
    uint32_t last_layer_index;
    ParsedPreview large, small;
    std::vector<ParsedLayer> layers;
};

ParsedPreview parse_preview(const Bytes &b, size_t at)
{
    ParsedPreview p;
    p.w = u32_at(b, at);
    p.h = u32_at(b, at + 4);
    const size_t img = u32_at(b, at + 8), len = u32_at(b, at + 12);
    REQUIRE(img + len <= b.size());
    // 16 bit runs: 5 bit red (11), 5 bit green (6), 5 bit blue (0); bit 5 says that a 12 bit run length follows.
    size_t i = img;
    while (i < img + len) {
        const uint16_t px = u16_at(b, i);
        i += 2;
        size_t count = 1;
        if (px & 0x20) {
            REQUIRE(i + 2 <= img + len);
            count += (u16_at(b, i) & 0x0FFF);
            i += 2;
        }
        const uint8_t r = ((px >> 11) & 0x1F) << 3, g = ((px >> 6) & 0x1F) << 3, bl = (px & 0x1F) << 3;
        for (size_t k = 0; k < count; ++k) { p.rgb.push_back(r); p.rgb.push_back(g); p.rgb.push_back(bl); }
    }
    return p;
}

ParsedCTB parse_ctb(Bytes b)
{
    ParsedCTB c;
    c.magic = u32_at(b, 0);
    c.version = u32_at(b, 4);
    c.bed_x = f32_at(b, 8); c.bed_y = f32_at(b, 12); c.bed_z = f32_at(b, 16);
    c.total_height = f32_at(b, 28); c.layer_height = f32_at(b, 32);
    c.exposure = f32_at(b, 36); c.bottom_exposure = f32_at(b, 40); c.light_off = f32_at(b, 44);
    c.bottom_layers = u32_at(b, 48);
    c.res_x = u32_at(b, 52); c.res_y = u32_at(b, 56);
    c.large_off = u32_at(b, 60); c.layer_table_off = u32_at(b, 64); c.layer_count = u32_at(b, 68); c.small_off = u32_at(b, 72);
    c.print_time = u32_at(b, 76); c.projector = u32_at(b, 80);
    c.params_off = u32_at(b, 84); c.params_size = u32_at(b, 88);
    c.pwm = u16_at(b, 96); c.bottom_pwm = u16_at(b, 98);
    c.xor_key = u32_at(b, 100);
    c.slicer_off = u32_at(b, 104); c.slicer_size = u32_at(b, 108);

    c.large = parse_preview(b, c.large_off);
    c.small = parse_preview(b, c.small_off);

    c.bottom_lift = f32_at(b, c.params_off); c.bottom_lift_speed = f32_at(b, c.params_off + 4);
    c.lift = f32_at(b, c.params_off + 8); c.lift_speed = f32_at(b, c.params_off + 12);
    c.retract_speed = f32_at(b, c.params_off + 16);
    c.volume = f32_at(b, c.params_off + 20); c.weight = f32_at(b, c.params_off + 24); c.cost = f32_at(b, c.params_off + 28);

    const size_t s = c.slicer_off;
    const size_t name_off = u32_at(b, s + 28), name_len = u32_at(b, s + 32);
    REQUIRE(name_off + name_len <= b.size());
    c.machine_name.assign(reinterpret_cast<const char*>(&b[name_off]), name_len);
    c.timestamp = u32_at(b, s + 40);
    c.software_version = u32_at(b, s + 48);
    c.transition_layers = u32_at(b, s + 60);
    c.ext_off = u32_at(b, s + 64);

    const size_t e = c.ext_off;
    c.last_layer_index = u32_at(b, e + 52);
    const size_t dis_off = u32_at(b, e + 72), dis_len = u32_at(b, e + 76);
    REQUIRE(dis_off + dis_len <= b.size());
    c.disclaimer.assign(reinterpret_cast<const char*>(&b[dis_off]), dis_len);

    for (uint32_t i = 0; i < c.layer_count; ++i) {
        const size_t d = c.layer_table_off + size_t(i) * 36;
        const uint32_t addr = u32_at(b, d + 12), size = u32_at(b, d + 16), page = u32_at(b, d + 20);
        REQUIRE(page == 0);
        REQUIRE(u32_at(b, d + 24) == 84);
        REQUIRE(addr >= 84);
        REQUIRE(addr + size <= b.size());
        ParsedLayer l;
        l.z = f32_at(b, d); l.exposure = f32_at(b, d + 4); l.light_off = f32_at(b, d + 8);
        // The extended definition sits right before the data and repeats the layer's definition.
        const size_t x = addr - 84;
        REQUIRE(f32_at(b, x) == l.z);
        REQUIRE(u32_at(b, x + 36) == 84 + size);
        l.lift = f32_at(b, x + 40); l.lift_speed = f32_at(b, x + 44); l.retract_speed = f32_at(b, x + 56);
        l.rest_after_retract = f32_at(b, x + 76); l.pwm = f32_at(b, x + 80);
        Bytes data(b.begin() + addr, b.begin() + addr + size);
        ctb::layer_cipher(data.data(), data.size(), c.xor_key, i);
        l.pixels.assign(size_t(c.res_x) * c.res_y, 0);
        REQUIRE(ctb::rle_decode(data.data(), data.size(), l.pixels.data(), l.pixels.size()));
        c.layers.push_back(std::move(l));
    }
    return c;
}

} // namespace

TEST_CASE("CTB run length codec: known encodings", "[CTB]")
{
    // The 7 bit value is the pixel shifted right; runs longer than one pixel set the top bit and are followed by the length.
    auto encode = [](std::vector<uint8_t> px) { return ctb::rle_encode(px.data(), px.size()); };

    CHECK(encode({0, 0, 0, 255, 255, 128}) == Bytes{0x80, 3, 0xFF, 2, 0x40});
    CHECK(encode({255}) == Bytes{0x7F});
    CHECK(encode({0}) == Bytes{0x00});

    // Run length 127 fits one byte, 128 needs two (0x80 | high, low), 0x3FFF is the largest two byte run.
    CHECK(encode(std::vector<uint8_t>(127, 0)) == Bytes{0x80, 127});
    CHECK(encode(std::vector<uint8_t>(128, 0)) == Bytes{0x80, 0x80, 0x80});
    CHECK(encode(std::vector<uint8_t>(200, 0)) == Bytes{0x80, 0x80, 200});
    CHECK(encode(std::vector<uint8_t>(0x3FFF, 0)) == Bytes{0x80, 0xBF, 0xFF});
    // 0x4000 takes three bytes (0xC0 | bits 16-20, then two more).
    CHECK(encode(std::vector<uint8_t>(0x4000, 0)) == Bytes{0x80, 0xC0, 0x40, 0x00});
}

TEST_CASE("CTB run length codec: round trips", "[CTB]")
{
    std::mt19937 rng(7);
    for (size_t count : {size_t(1), size_t(2), size_t(31), size_t(4096), size_t(100000)}) {
        INFO("pixels " << count);
        // Mostly long runs with some gray, like a layer image.
        std::vector<uint8_t> px(count);
        size_t i = 0;
        while (i < count) {
            const size_t run = std::min(count - i, size_t(rng() % 5000) + 1);
            const uint8_t v = (rng() % 3 == 0) ? uint8_t(rng() % 256) : (rng() % 2 ? 255 : 0);
            std::fill(px.begin() + i, px.begin() + i + run, v);
            i += run;
        }
        const Bytes enc = ctb::rle_encode(px.data(), px.size());
        std::vector<uint8_t> out(count, 0xAA);
        REQUIRE(ctb::rle_decode(enc.data(), enc.size(), out.data(), out.size()));
        // 8 bit values come back as the 7 bit value expanded: v -> ((v >> 1) << 1) | 1, and 0 stays 0 (and 1 is 0).
        for (size_t k = 0; k < count; ++k) {
            const uint8_t v7 = px[k] >> 1;
            const uint8_t expected = v7 ? uint8_t((v7 << 1) | 1) : 0;
            if (out[k] != expected) { FAIL("pixel " << k << ": " << int(px[k]) << " came back as " << int(out[k])); }
        }
    }

    SECTION("a run longer than 0x1FFFFF, as in a mostly black 4K layer") {
        std::vector<uint8_t> px(4096 * 2160, 0);
        px[1000] = 255;
        px[px.size() - 7] = 255;
        const Bytes enc = ctb::rle_encode(px.data(), px.size());
        CHECK(enc.size() < 30);
        std::vector<uint8_t> out(px.size(), 0xAA);
        REQUIRE(ctb::rle_decode(enc.data(), enc.size(), out.data(), out.size()));
        CHECK(std::count(out.begin(), out.end(), uint8_t(255)) == 2);
        CHECK(std::count(out.begin(), out.end(), uint8_t(0)) == int(px.size()) - 2);
    }

    SECTION("malformed data is refused") {
        std::vector<uint8_t> out(16, 0);
        const Bytes truncated{0x80};
        CHECK_FALSE(ctb::rle_decode(truncated.data(), truncated.size(), out.data(), out.size()));
        const Bytes too_long{0xFF, 100};   // 100 white pixels into 16
        CHECK_FALSE(ctb::rle_decode(too_long.data(), too_long.size(), out.data(), out.size()));
        const Bytes bad_length{0x80, 0xF0, 0, 0, 0, 0};
        CHECK_FALSE(ctb::rle_decode(bad_length.data(), bad_length.size(), out.data(), out.size()));
    }
}

TEST_CASE("CTB layer cipher", "[CTB]")
{
    Bytes data(100);
    std::iota(data.begin(), data.end(), 0);
    const Bytes original = data;

    SECTION("zero seed does nothing") {
        ctb::layer_cipher(data.data(), data.size(), 0, 3);
        CHECK(data == original);
    }
    SECTION("it is symmetric, and depends on seed and layer") {
        Bytes a = data, b = data, c = data;
        ctb::layer_cipher(a.data(), a.size(), 103, 0);
        ctb::layer_cipher(b.data(), b.size(), 103, 1);
        ctb::layer_cipher(c.data(), c.size(), 104, 0);
        CHECK(a != original);
        CHECK(a != b);
        CHECK(a != c);
        ctb::layer_cipher(a.data(), a.size(), 103, 0);
        CHECK(a == original);
    }
}

TEST_CASE("The CTB reader parses a file written by mslicer", "[CTB]")
{
    const Bytes file = read_file(std::string(TEST_DATA_DIR PATH_SEPARATOR) + "ctb/ref_v4_mslicer.ctb");
    REQUIRE(file.size() == 1486);
    const ParsedCTB c = parse_ctb(file);

    CHECK(c.magic == 0x12FD0106);
    CHECK(c.version == 4);
    CHECK(c.bed_x == 120.f); CHECK(c.bed_y == 68.f); CHECK(c.bed_z == 150.f);
    CHECK(c.res_x == 8); CHECK(c.res_y == 4);
    CHECK(c.layer_count == 3);
    CHECK(c.layer_height == Catch::Approx(0.05f));
    CHECK(c.total_height == Catch::Approx(0.15f));
    CHECK(c.xor_key == 103);
    CHECK(c.pwm == 255);
    CHECK(c.slicer_size == 76);
    CHECK(c.large.w == 8); CHECK(c.large.h == 8);
    CHECK(c.large.rgb.size() == 8 * 8 * 3);
    // (10, 20, 30) reduced to the 5 bit channels.
    CHECK(c.large.rgb[0] == 8); CHECK(c.large.rgb[1] == 16); CHECK(c.large.rgb[2] == 24);
    CHECK(c.disclaimer.find("CBD Technology") != std::string::npos);
    CHECK(c.last_layer_index == 2);

    REQUIRE(c.layers.size() == 3);
    CHECK(c.layers[0].z == Catch::Approx(0.05f));
    CHECK(c.layers[1].z == Catch::Approx(0.10f));
    CHECK(c.layers[2].z == Catch::Approx(0.15f));
    CHECK(c.layers[0].exposure == 2.5f);
    CHECK(c.layers[0].light_off == 0.5f);
    CHECK(c.layers[0].lift == 6.f);
    CHECK(c.layers[0].lift_speed == 65.f);
    CHECK(c.layers[0].retract_speed == 150.f);

    // The 7 bit codec: 255 -> 255, 128 -> 129 (value 64, expanded).
    Bytes expected0(32, 0);
    std::fill(expected0.begin() + 10, expected0.begin() + 16, 255);
    std::fill(expected0.begin() + 16, expected0.begin() + 20, 129);
    CHECK(c.layers[0].pixels == expected0);
    CHECK(c.layers[1].pixels == Bytes(32, 255));
    Bytes expected2(32, 0);
    expected2[0] = 255; expected2[2] = 255;
    CHECK(c.layers[2].pixels == expected2);
}

namespace {

ctb::FileParams sample_params(CTBDialect dialect)
{
    ctb::FileParams p;
    p.dialect = dialect;
    p.bed_x_mm = 120.f; p.bed_y_mm = 68.f; p.bed_z_mm = 150.f;
    p.res_x = 8; p.res_y = 4;
    p.layer_height_mm = 0.05f;
    p.exposure_s = 2.5f; p.bottom_exposure_s = 30.f;
    p.light_off_s = 0.5f; p.bottom_light_off_s = 1.f;
    p.bottom_layer_count = 2; p.transition_layer_count = 1;
    p.print_time_s = 1234;
    p.projector_type = 1;
    p.light_pwm = 200; p.bottom_light_pwm = 255;
    p.lift_mm = 6.f; p.lift_mm_min = 65.f; p.bottom_lift_mm = 8.f; p.bottom_lift_mm_min = 50.f;
    p.retract_mm_min = 150.f; p.bottom_retract_mm_min = 100.f;
    p.rest_after_retract_s = 1.5f;
    p.volume_ml = 4.5f; p.weight_g = 5.f; p.cost = 0.7f;
    p.timestamp_minutes = 29'000'000;
    p.xor_key = 0x5A7E3C91;
    p.machine_name = "Test printer";
    p.large_preview.width = 4; p.large_preview.height = 3;
    p.large_preview.rgb.assign(4 * 3 * 3, 0);
    for (size_t i = 0; i < p.large_preview.rgb.size(); i += 3) { p.large_preview.rgb[i] = 248; p.large_preview.rgb[i + 1] = 8 * (i / 3); p.large_preview.rgb[i + 2] = 16; }
    p.small_preview.width = 2; p.small_preview.height = 2;
    p.small_preview.rgb.assign(2 * 2 * 3, 80);
    for (int i = 0; i < 4; ++i) {
        ctb::LayerParams l;
        l.z_mm = 0.05f * (i + 1);
        l.exposure_s = i < 2 ? 30.f : 2.5f;
        l.light_off_s = 0.5f;
        l.lift_mm = i < 2 ? 8.f : 6.f; l.lift_mm_min = i < 2 ? 50.f : 65.f; l.retract_mm_min = 150.f;
        l.rest_after_retract_s = 1.5f; l.light_pwm = 200.f;
        p.layers.push_back(l);
    }
    return p;
}

std::vector<Bytes> sample_images(size_t pixels)
{
    std::vector<Bytes> images;
    for (int i = 0; i < 4; ++i) {
        Bytes px(pixels, 0);
        for (size_t k = 0; k < pixels; ++k)
            if ((k + i) % 3 != 0) px[k] = (i % 2) ? 255 : 128;
        images.push_back(ctb::rle_encode(px.data(), px.size()));
    }
    return images;
}

} // namespace

TEST_CASE("A built CTB file reads back as what was put in", "[CTB]")
{
    const CTBDialect dialect = GENERATE(CTBDialect::Chitubox, CTBDialect::GKtwo);
    INFO("dialect " << (dialect == CTBDialect::GKtwo ? "GKtwo" : "Chitubox"));
    const ctb::FileParams p = sample_params(dialect);
    const Bytes file = ctb::build_ctb_v4(p, sample_images(32));
    const ParsedCTB c = parse_ctb(file);

    CHECK(c.magic == (dialect == CTBDialect::GKtwo ? 0xFF220810u : 0x12FD0106u));
    CHECK(c.version == 4);
    CHECK(c.bed_x == 120.f); CHECK(c.bed_y == 68.f); CHECK(c.bed_z == 150.f);
    CHECK(c.res_x == 8); CHECK(c.res_y == 4);
    CHECK(c.layer_count == 4);
    CHECK(c.layer_height == Catch::Approx(0.05f));
    CHECK(c.total_height == Catch::Approx(0.20f));
    CHECK(c.exposure == 2.5f); CHECK(c.bottom_exposure == 30.f);
    CHECK(c.light_off == Catch::Approx(0.5f));
    CHECK(c.bottom_layers == 2);
    CHECK(c.transition_layers == 1);
    CHECK(c.print_time == 1234);
    CHECK(c.projector == 1);
    CHECK(c.pwm == 200); CHECK(c.bottom_pwm == 255);
    CHECK(c.xor_key == 0x5A7E3C91);
    CHECK(c.slicer_size == 76);
    CHECK(c.params_size == 60);
    CHECK(c.bottom_lift == 8.f); CHECK(c.bottom_lift_speed == 50.f);
    CHECK(c.lift == 6.f); CHECK(c.lift_speed == 65.f); CHECK(c.retract_speed == 150.f);
    CHECK(c.volume == 4.5f); CHECK(c.weight == 5.f); CHECK(c.cost == Catch::Approx(0.7f));
    CHECK(c.machine_name == "Test printer");
    CHECK(c.timestamp == 29'000'000);
    CHECK(c.software_version == 0x01090000);
    CHECK(c.last_layer_index == 3);

    // The disclaimer text differs by dialect, in length as well (the GKtwo one is 323 bytes, the other 320).
    CHECK(c.disclaimer.size() == (dialect == CTBDialect::GKtwo ? 323u : 320u));
    CHECK(c.disclaimer.find("CBD Technology (China) Inc.") != std::string::npos);
    CHECK((c.disclaimer.find("jxs,ctb and cbddlp") != std::string::npos) == (dialect == CTBDialect::GKtwo));

    // Offsets are inside the file and ordered the way the sections follow each other.
    CHECK(c.large_off == 112);
    CHECK(c.small_off > c.large_off);
    CHECK(c.params_off > c.small_off);
    CHECK(c.slicer_off == c.params_off + c.params_size);
    CHECK(c.ext_off > c.slicer_off);
    CHECK(c.layer_table_off > c.ext_off);
    CHECK(c.layer_table_off + c.layer_count * 36 < file.size());

    REQUIRE(c.large.w == 4); REQUIRE(c.large.h == 3);
    CHECK(c.large.rgb.size() == 4 * 3 * 3);
    CHECK(c.large.rgb[0] == 248);
    CHECK(c.small.w == 2); CHECK(c.small.rgb == std::vector<uint8_t>(2 * 2 * 3, 80));

    REQUIRE(c.layers.size() == 4);
    const auto images_px = [](int i) { Bytes px(32, 0); for (size_t k = 0; k < 32; ++k) if ((k + i) % 3 != 0) px[k] = (i % 2) ? 255 : 129; return px; };
    for (int i = 0; i < 4; ++i) {
        INFO("layer " << i);
        CHECK(c.layers[i].z == Catch::Approx(0.05f * (i + 1)));
        CHECK(c.layers[i].exposure == (i < 2 ? 30.f : 2.5f));
        CHECK(c.layers[i].lift == (i < 2 ? 8.f : 6.f));
        CHECK(c.layers[i].lift_speed == (i < 2 ? 50.f : 65.f));
        CHECK(c.layers[i].retract_speed == 150.f);
        CHECK(c.layers[i].rest_after_retract == 1.5f);
        CHECK(c.layers[i].pwm == 200.f);
        CHECK(c.layers[i].pixels == images_px(i));
    }
}

TEST_CASE("A built CTB file is deterministic", "[CTB]")
{
    const ctb::FileParams p = sample_params(CTBDialect::GKtwo);
    CHECK(ctb::build_ctb_v4(p, sample_images(32)) == ctb::build_ctb_v4(p, sample_images(32)));
}

TEST_CASE("The CTB archives are registered", "[CTB]")
{
    for (const char *id : {"CTB", "CTBGKTWO", "CBDDLP", "CTBENCRYPTED"}) {
        INFO(id);
        const ArchiveEntry *entry = get_archive_entry(id);
        REQUIRE(entry != nullptr);
        CHECK(std::string(entry->ext) == (std::string(id) == "CBDDLP" ? "cbddlp" : "ctb"));
        CHECK(entry->wrfactoryfn);
    }
}

namespace {

SLAJobResult slice_cube_to(const std::string &format, const boost::filesystem::path &dir)
{
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
    machine.set_deserialize_strict("sla_archive_format", format);
    DynamicPrintConfig process;

    Model model;
    ModelObject *mo = model.add_object();
    mo->add_volume(load_model("20mm_cube.obj"));
    mo->add_instance();

    SLAJobParams params;
    params.machine    = &machine;
    params.process    = &process;
    params.output_dir = dir.string();
    return run_sla_job(model, params);
}

} // namespace

TEST_CASE("A cube sliced to CTB is a valid file with the print's settings", "[CTB]")
{
    const std::string format = GENERATE(as<std::string>{}, "CTB", "CTBGKTWO");
    INFO(format);
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("ctb-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    const SLAJobResult res = slice_cube_to(format, dir);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.ctb");

    const ParsedCTB c = parse_ctb(read_file(res.archive_path));
    CHECK(c.magic == (format == "CTBGKTWO" ? 0xFF220810u : 0x12FD0106u));
    CHECK(c.res_x == 2560); CHECK(c.res_y == 1440);
    CHECK(c.bed_x == Catch::Approx(120.f)); CHECK(c.bed_y == Catch::Approx(68.f)); CHECK(c.bed_z == Catch::Approx(150.f));
    CHECK(c.layer_count == res.layer_count);
    CHECK(c.layer_count == Catch::Approx(400).margin(2));
    CHECK(c.layer_height == Catch::Approx(0.05f));
    CHECK(c.exposure == Catch::Approx(2.5f));
    CHECK(c.bottom_exposure == Catch::Approx(30.f));
    CHECK(c.bottom_layers == 3);
    CHECK(c.pwm == 180);
    CHECK(c.lift == Catch::Approx(7.f)); CHECK(c.lift_speed == Catch::Approx(60.f)); CHECK(c.retract_speed == Catch::Approx(120.f));
    CHECK(c.print_time > 400 * 2);
    CHECK(c.volume == Catch::Approx(8.f).epsilon(0.05));
    CHECK(c.total_height == Catch::Approx(20.f).margin(0.1));
    CHECK(c.layers.back().z == Catch::Approx(c.total_height));

    // Bottom layers get the bottom exposure, then it fades to the normal one over the transition layers, then normal.
    REQUIRE(c.layers.size() == c.layer_count);
    CHECK(c.layers[0].exposure == Catch::Approx(30.f));
    CHECK(c.layers[2].exposure == Catch::Approx(30.f));
    CHECK(c.layers[3].exposure < 30.f);
    CHECK(c.layers[3].exposure > 2.5f);
    CHECK(c.layers[4].exposure < c.layers[3].exposure);
    CHECK(c.layers[5].exposure == Catch::Approx(2.5f));
    CHECK(c.layers[200].exposure == Catch::Approx(2.5f));

    // A middle layer of the 20 mm cube is a 20 x 20 mm square of lit pixels: 120/2560 = 0.046875 mm per pixel.
    const Bytes &mid = c.layers[200].pixels;
    const double lit = std::count_if(mid.begin(), mid.end(), [](uint8_t v) { return v > 127; });
    const double expected = (20. / 0.046875) * (20. / (68. / 1440.));
    CHECK(lit == Catch::Approx(expected).epsilon(0.02));
    // And it is in the middle of the display.
    const size_t row = 720;
    const auto first = std::find_if(mid.begin() + row * 2560, mid.begin() + (row + 1) * 2560, [](uint8_t v) { return v > 127; });
    const auto last  = std::find_if(std::make_reverse_iterator(mid.begin() + (row + 1) * 2560), std::make_reverse_iterator(mid.begin() + row * 2560), [](uint8_t v) { return v > 127; });
    const double left = first - (mid.begin() + row * 2560);
    const double right = (mid.begin() + (row + 1) * 2560) - last.base();
    CHECK(left == Catch::Approx(right).margin(3));
}

TEST_CASE("The cbddlp bit planes match the known encodings", "[CTB][Cbddlp]")
{
    // Thresholds of the four planes are 255, 63, 127, 191: a plane is lit where the pixel reaches its threshold.
    const std::vector<uint8_t> px{0, 255, 255, 255, 0, 100};
    const std::vector<uint8_t> blob = ctb::cbddlp_encode(px.data(), px.size());
    std::vector<std::vector<uint8_t>> planes;
    REQUIRE(ctb::cbddlp_split(blob, planes));
    REQUIRE(planes.size() == 4);
    CHECK(planes[0] == std::vector<uint8_t>{0x01, 0x83, 0x02});        // >= 255: off 1, on 3, off 2
    CHECK(planes[1] == std::vector<uint8_t>{0x01, 0x83, 0x01, 0x81});  // >= 63: the 100 is lit
    CHECK(planes[2] == std::vector<uint8_t>{0x01, 0x83, 0x02});        // >= 127
    CHECK(planes[3] == std::vector<uint8_t>{0x01, 0x83, 0x02});        // >= 191

    // A long run is cut at 125 pixels.
    const std::vector<uint8_t> black(300, 0);
    REQUIRE(ctb::cbddlp_split(ctb::cbddlp_encode(black.data(), black.size()), planes));
    CHECK(planes[0] == std::vector<uint8_t>{0x7D, 0x7D, 0x32});
}

TEST_CASE("A cbddlp layer decodes to its four gray levels", "[CTB][Cbddlp]")
{
    std::mt19937 rng(11);
    std::vector<uint8_t> px(5000);
    for (size_t i = 0; i < px.size(); ++i)
        px[i] = (i / 50) % 3 == 0 ? uint8_t(rng()) : ((i / 50) % 3 == 1 ? 0 : 255);
    std::vector<std::vector<uint8_t>> planes;
    REQUIRE(ctb::cbddlp_split(ctb::cbddlp_encode(px.data(), px.size()), planes));
    std::vector<uint8_t> out(px.size());
    REQUIRE(ctb::cbddlp_decode(planes, out.data(), out.size()));
    for (size_t i = 0; i < px.size(); ++i) {
        // black stays black, white stays white, the rest lands on the closest step below
        if (px[i] == 0) REQUIRE(out[i] == 0);
        else if (px[i] == 255) REQUIRE(out[i] == 255);
        else REQUIRE(int(px[i]) - int(out[i]) < 64);
    }
}

TEST_CASE("A cube sliced to cbddlp is a version 2 file with one table per bit plane", "[CTB][Cbddlp]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("cbddlp-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    const SLAJobResult res = slice_cube_to("CBDDLP", dir);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.cbddlp");

    const Bytes f = read_file(res.archive_path);
    CHECK(u32_at(f, 0) == 0x12FD0019u);
    CHECK(u32_at(f, 4) == 2u);
    CHECK(f32_at(f, 8) == Catch::Approx(120.f));
    CHECK(f32_at(f, 12) == Catch::Approx(68.f));
    CHECK(f32_at(f, 16) == Catch::Approx(150.f));
    CHECK(f32_at(f, 32) == Catch::Approx(0.05f));      // layer height
    CHECK(f32_at(f, 36) == Catch::Approx(2.5f));
    CHECK(f32_at(f, 40) == Catch::Approx(30.f));
    CHECK(u32_at(f, 48) == 3u);                        // bottom layers
    const uint32_t res_x = u32_at(f, 52), res_y = u32_at(f, 56);
    CHECK(res_x == 2560u); CHECK(res_y == 1440u);
    const uint32_t layers_at = u32_at(f, 64), layer_count = u32_at(f, 68);
    CHECK(layer_count == res.layer_count);
    CHECK(u32_at(f, 92) == 4u);                        // anti alias level = bit planes
    CHECK(u32_at(f, 100) == 0u);                       // not encrypted
    CHECK(u32_at(f, 104) == 0u);                       // no slicer info

    // The middle layer: decode its four planes into a picture of the 20 mm square.
    const size_t mid = layer_count / 2;
    std::vector<std::vector<uint8_t>> planes;
    for (uint32_t k = 0; k < 4; ++k) {
        const size_t def = layers_at + 36 * (size_t(k) * layer_count + mid);
        const uint32_t addr = u32_at(f, def + 12), size = u32_at(f, def + 16);
        REQUIRE(addr + size <= f.size());
        planes.emplace_back(f.begin() + addr, f.begin() + addr + size);
        CHECK(u32_at(f, def + 24) == 36u);
        if (k == 0) CHECK(f32_at(f, def + 4) == Catch::Approx(2.5f));
    }
    Bytes img(size_t(res_x) * res_y);
    REQUIRE(ctb::cbddlp_decode(planes, img.data(), img.size()));
    const double lit = std::count_if(img.begin(), img.end(), [](uint8_t v) { return v > 127; });
    CHECK(lit == Catch::Approx((20. / (120. / 2560)) * (20. / (68. / 1440))).epsilon(0.02));
}

TEST_CASE("The encrypted CTB layout keeps its AES round trip and rejects partial blocks", "[CTB][CTBEncrypted]")
{
    std::vector<uint8_t> plain(64);
    std::iota(plain.begin(), plain.end(), uint8_t(1));
    const std::vector<uint8_t> enc = ctb::aes_cbc(plain, true);
    REQUIRE(enc.size() == plain.size());
    CHECK(enc != plain);
    CHECK(ctb::aes_cbc(enc, false) == plain);
    // CBC chains the blocks: the same plain block twice does not give the same cipher block twice.
    std::vector<uint8_t> twice(32, 0x5A);
    const std::vector<uint8_t> twice_enc = ctb::aes_cbc(twice, true);
    CHECK(! std::equal(twice_enc.begin(), twice_enc.begin() + 16, twice_enc.begin() + 16));
    CHECK_THROWS(ctb::aes_cbc(std::vector<uint8_t>(20), true));
}

TEST_CASE("A cube sliced to encrypted CTB is a version 5 file with decryptable settings and layers", "[CTB][CTBEncrypted]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("ctbe-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    const SLAJobResult res = slice_cube_to("CTBENCRYPTED", dir);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.ctb");

    const Bytes f = read_file(res.archive_path);
    // The 48 byte header
    CHECK(u32_at(f, 0) == 0x12FD0107u);
    CHECK(u32_at(f, 4) == 288u);                       // settings size
    CHECK(u32_at(f, 8) == 48u);                        // settings offset
    CHECK(u32_at(f, 16) == 5u);                        // version
    const uint32_t sig_size = u32_at(f, 20), sig_at = u32_at(f, 24);
    CHECK(sig_size == 32u);
    REQUIRE(sig_at + sig_size <= f.size());

    // The settings decrypt to the print's values.
    const Bytes settings = ctb::aes_cbc(Bytes(f.begin() + 48, f.begin() + 48 + 288), false);
    const uint64_t checksum = uint64_t(u32_at(settings, 0)) | (uint64_t(u32_at(settings, 4)) << 32);
    CHECK(checksum == 0xCAFEBABEull);
    const uint32_t pointers_at = u32_at(settings, 8);
    CHECK(f32_at(settings, 12) == Catch::Approx(120.f));
    CHECK(f32_at(settings, 16) == Catch::Approx(68.f));
    CHECK(f32_at(settings, 20) == Catch::Approx(150.f));
    CHECK(f32_at(settings, 36) == Catch::Approx(0.05f));    // layer height
    CHECK(f32_at(settings, 40) == Catch::Approx(2.5f));
    CHECK(f32_at(settings, 44) == Catch::Approx(30.f));
    CHECK(u32_at(settings, 52) == 3u);                      // bottom layers
    const uint32_t res_x = u32_at(settings, 56), res_y = u32_at(settings, 60), layer_count = u32_at(settings, 64);
    CHECK(res_x == 2560u); CHECK(res_y == 1440u);
    CHECK(layer_count == res.layer_count);
    CHECK(u16_at(settings, 124) == 180);                    // light pwm
    const uint32_t xor_key = u32_at(settings, 128);
    CHECK(xor_key != 0u);
    CHECK(f32_at(settings, 84) == Catch::Approx(7.f));      // bottom lift

    // The signature is the encrypted SHA-256 of the checksum, which anyone can recompute.
    const Bytes sig_plain = ctb::aes_cbc(Bytes(f.begin() + sig_at, f.begin() + sig_at + 32), false);
    CHECK(sig_plain.size() == 32);
    CHECK(ctb::aes_cbc(sig_plain, true) == Bytes(f.begin() + sig_at, f.begin() + sig_at + 32));

    // A middle layer: the pointer finds a definition of 88 bytes, then an image that the key unscrambles.
    const size_t mid = layer_count / 2;
    const size_t ptr = pointers_at + 16 * mid;
    const uint32_t layer_at = u32_at(f, ptr);
    CHECK(u32_at(f, ptr + 4) == 0u);                        // page
    CHECK(u32_at(f, ptr + 8) == 88u);
    CHECK(u32_at(f, layer_at) == 88u);
    CHECK(f32_at(f, layer_at + 4) == Catch::Approx(0.05f * (mid + 1)).margin(0.01));
    CHECK(f32_at(f, layer_at + 8) == Catch::Approx(2.5f));
    const uint32_t data_at = u32_at(f, layer_at + 16), data_len = u32_at(f, layer_at + 24);
    CHECK(data_at == layer_at + 88);
    CHECK(u32_at(f, layer_at + 32) == 0u);                  // no separately encrypted part
    REQUIRE(data_at + data_len <= f.size());
    Bytes rle(f.begin() + data_at, f.begin() + data_at + data_len);
    ctb::layer_cipher(rle.data(), rle.size(), xor_key, uint32_t(mid));
    Bytes img(size_t(res_x) * res_y);
    REQUIRE(ctb::rle_decode(rle.data(), rle.size(), img.data(), img.size()));
    const double lit = std::count_if(img.begin(), img.end(), [](uint8_t v) { return v > 127; });
    CHECK(lit == Catch::Approx((20. / (120. / 2560)) * (20. / (68. / 1440))).epsilon(0.02));

    // The machine name and disclaimer sections point inside the file.
    const uint32_t name_at = u32_at(settings, 160), name_len = u32_at(settings, 164);
    REQUIRE(name_at + name_len <= f.size());
    const uint32_t disclaimer_at = u32_at(settings, 264), disclaimer_len = u32_at(settings, 268);
    CHECK(disclaimer_len == 320u);
    REQUIRE(disclaimer_at + disclaimer_len <= f.size());
    CHECK(std::string(f.begin() + disclaimer_at, f.begin() + disclaimer_at + 6) == "Layout");
}
