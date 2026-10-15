#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <test_utils.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/Format/CXDLP.hpp"
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

struct Be
{
    const Bytes &b;
    size_t at { 0 };
    uint32_t u32() { REQUIRE(at + 4 <= b.size()); uint32_t v = (uint32_t(b[at]) << 24) | (uint32_t(b[at+1]) << 16) | (uint32_t(b[at+2]) << 8) | b[at+3]; at += 4; return v; }
    uint16_t u16() { REQUIRE(at + 2 <= b.size()); uint16_t v = uint16_t((b[at] << 8) | b[at+1]); at += 2; return v; }
    uint8_t  u8()  { REQUIRE(at < b.size()); return b[at++]; }
    std::string cstring() { const uint32_t n = u32(); REQUIRE(at + n <= b.size()); std::string s(b.begin() + at, b.begin() + at + n); at += n; return s.substr(0, s.find('\0')); }
    std::string utf16() { const uint32_t n = u32(); REQUIRE(at + n <= b.size()); std::string s; for (uint32_t i = 0; i + 1 < n; i += 2) s += char(b[at + i + 1]); at += n; return s; }
    void crlf() { REQUIRE(u8() == 0x0D); REQUIRE(u8() == 0x0A); }
};

// The checksum, bit by bit.
uint32_t slow_checksum(const Bytes &d)
{
    uint32_t sum = 0;
    for (uint8_t byte : d) {
        sum ^= byte;
        for (int i = 0; i < 8; ++i)
            sum = (sum & 1) ? (sum >> 1) ^ 0xEDB88320u : sum >> 1;
    }
    return sum;
}

} // namespace

TEST_CASE("The CXDLP printer model is found in a printer name", "[CXDLP]")
{
    CHECK(cxdlp::printer_model_of("Halot One CL-60") == "CL-60");
    CHECK(cxdlp::printer_model_of("Halot Ray CL925") == "CL925");
    CHECK(cxdlp::printer_model_of("CT-005 Pro") == "CT-005");
    CHECK(cxdlp::printer_model_of("Elegoo Mars 3").empty());
}

TEST_CASE("The CXDLP checksum is the plain CRC-32 table walk", "[CXDLP]")
{
    std::mt19937 rng(3);
    Bytes data(5000);
    for (auto &b : data) b = uint8_t(rng());
    CHECK(cxdlp::checksum(data.data(), data.size()) == slow_checksum(data));
    CHECK(cxdlp::checksum(data.data(), 0) == 0u);
}

TEST_CASE("A CXDLP layer round trips and reports its largest region", "[CXDLP]")
{
    const size_t w = 64, h = 48;
    Bytes px(w * h, 0);
    // a 20 x 20 square with a 6 x 6 hole, a nested 2 x 2 island in the hole, and a separate 5 x 5 square
    for (size_t y = 5; y < 25; ++y) for (size_t x = 5; x < 25; ++x) px[y * w + x] = 255;
    for (size_t y = 10; y < 16; ++y) for (size_t x = 10; x < 16; ++x) px[y * w + x] = 0;
    for (size_t y = 12; y < 14; ++y) for (size_t x = 12; x < 14; ++x) px[y * w + x] = 128;
    for (size_t y = 30; y < 35; ++y) for (size_t x = 40; x < 45; ++x) px[y * w + x] = 255;

    const Bytes layer = cxdlp::encode_layer(px.data(), w, h, 0.01);
    // the big square's outline encloses 400 pixels: 400 * 0.01 mm2 * 1000
    const uint32_t area = (uint32_t(layer[0]) << 24) | (uint32_t(layer[1]) << 16) | (uint32_t(layer[2]) << 8) | layer[3];
    CHECK(area == 4000u);
    CHECK(layer[layer.size() - 2] == 0x0D);
    CHECK(layer[layer.size() - 1] == 0x0A);

    Bytes back(w * h, 0x77);
    REQUIRE(cxdlp::decode_layer(layer, back.data(), w, h));
    CHECK(back == px);

    // an empty layer is no lines at all
    Bytes empty(w * h, 0);
    const Bytes none = cxdlp::encode_layer(empty.data(), w, h, 0.01);
    CHECK(none.size() == 10);
}

TEST_CASE("The CXDLP archive is registered", "[CXDLP]")
{
    const ArchiveEntry *entry = get_archive_entry("CXDLP");
    REQUIRE(entry != nullptr);
    CHECK(std::string(entry->ext) == "cxdlp");
    CHECK(entry->wrfactoryfn);
}

TEST_CASE("A cube sliced to CXDLP has the file's sections in order, a valid checksum and decodable layers", "[CXDLP]")
{
    boost::filesystem::path dir = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("cx-job-%%%%%%%%");
    struct Cleanup { boost::filesystem::path p; ~Cleanup() { boost::system::error_code ec; boost::filesystem::remove_all(p, ec); } } cleanup{dir};

    DynamicPrintConfig machine;
    machine.set_key_value("printer_technology", new ConfigOptionEnum<PrinterTechnology>(ptSLA));
    machine.set_deserialize_strict("printer_model", "Halot Test CL-60");
    machine.set_deserialize_strict("display_width", "120");
    machine.set_deserialize_strict("display_height", "68");
    machine.set_deserialize_strict("display_orientation", "landscape");
    machine.set_deserialize_strict("display_pixels_x", "1920");
    machine.set_deserialize_strict("display_pixels_y", "1080");
    machine.set_deserialize_strict("printable_height", "150");
    machine.set_deserialize_strict("layer_height", "0.1");
    machine.set_deserialize_strict("initial_layer_height", "0.1");
    machine.set_deserialize_strict("supports_enable", "0");
    machine.set_deserialize_strict("pad_enable", "0");
    machine.set_deserialize_strict("hollowing_enable", "0");
    machine.set_deserialize_strict("exposure_time", "2.5");
    machine.set_deserialize_strict("initial_exposure_time", "30");
    machine.set_deserialize_strict("bottom_layer_count", "4");
    machine.set_deserialize_strict("lift_distance", "6");
    machine.set_deserialize_strict("lift_speed", "120");
    machine.set_deserialize_strict("retract_speed", "180");
    machine.set_deserialize_strict("delay_before_exposure", "2");
    machine.set_deserialize_strict("light_pwm", "200");
    machine.set_deserialize_strict("sla_archive_format", "CXDLP");
    DynamicPrintConfig process;

    Model model;
    ModelObject *mo = model.add_object();
    mo->add_volume(load_model("20mm_cube.obj"));
    mo->add_instance();
    SLAJobParams params;
    params.machine = &machine; params.process = &process; params.output_dir = dir.string();
    const SLAJobResult res = run_sla_job(model, params);
    REQUIRE(res.code == CLI_SUCCESS);
    CHECK(boost::filesystem::path(res.archive_path).filename() == "plate_1.cxdlp");

    const Bytes f = read_file(res.archive_path);
    // the checksum covers all but its own 4 bytes
    REQUIRE(f.size() > 4);
    const uint32_t stored = (uint32_t(f[f.size()-4]) << 24) | (uint32_t(f[f.size()-3]) << 16) | (uint32_t(f[f.size()-2]) << 8) | f[f.size()-1];
    CHECK(stored == slow_checksum(Bytes(f.begin(), f.end() - 4)));

    Be r{f};
    CHECK(r.u32() == 9u);
    r.at = 4;
    CHECK(std::string(f.begin() + 4, f.begin() + 12) == "CXSW3DV2");
    r.at = 13;
    CHECK(r.u16() == 3);
    CHECK(r.cstring() == "CL-60");
    const uint32_t layer_count = r.u16();
    CHECK(layer_count == res.layer_count);
    CHECK(r.u16() == 1920); CHECK(r.u16() == 1080);
    r.at += 64;
    r.at += 116 * 116 * 2; r.crlf();
    r.at += 290 * 290 * 2; r.crlf();
    r.at += 290 * 290 * 2; r.crlf();
    CHECK(r.utf16() == "120");
    CHECK(r.utf16() == "68");
    CHECK(r.utf16() == "0.1");
    CHECK(r.u16() == 25);          // exposure in tenths of seconds
    CHECK(r.u16() == 2);           // wait before cure
    CHECK(r.u16() == 30);          // bottom exposure
    CHECK(r.u16() == 4);           // bottom layers
    CHECK(r.u16() == 6); CHECK(r.u16() == 2);     // bottom lift, speed in mm/s
    CHECK(r.u16() == 6); CHECK(r.u16() == 2);     // lift, speed
    CHECK(r.u16() == 3);           // retract speed
    CHECK(r.u16() == 200); CHECK(r.u16() == 200);
    std::vector<uint32_t> areas;
    for (uint32_t i = 0; i < layer_count; ++i) areas.push_back(r.u32());
    r.crlf();
    CHECK(r.cstring() == "Pseudorca");
    r.cstring();                   // material
    r.at += 1 + 8 + 1 + 2 + 1 + 2 + 3 + 2;   // the compensation settings of version 3
    r.crlf();

    const size_t w = 1920, h = 1080;
    Bytes img(w * h);
    for (uint32_t i = 0; i < layer_count; ++i) {
        const size_t start = r.at;
        r.u32();
        const uint32_t lines = r.u32();
        r.at += size_t(lines) * 6;
        r.crlf();
        if (i == layer_count / 2) {
            const Bytes layer(f.begin() + start, f.begin() + r.at);
            REQUIRE(cxdlp::decode_layer(layer, img.data(), w, h));
            const double lit = std::count_if(img.begin(), img.end(), [](uint8_t v) { return v > 127; });
            CHECK(lit == Catch::Approx((20. / (120. / 1920)) * (20. / (68. / 1080))).epsilon(0.03));
            // the area table and the layer agree, and a 20 x 20 mm square is 400 mm2
            const uint32_t area = (uint32_t(layer[0]) << 24) | (uint32_t(layer[1]) << 16) | (uint32_t(layer[2]) << 8) | layer[3];
            CHECK(area == areas[i]);
            CHECK(area == Catch::Approx(400. * 1000.).epsilon(0.03));
        }
    }
    // the footer, then the checksum
    CHECK(r.cstring() == "CXSW3DV2");
    CHECK(f.size() - r.at == 4);
}
