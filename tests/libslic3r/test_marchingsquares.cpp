#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "libslic3r/Point.hpp"
#include <cstddef>
#include "libslic3r/libslic3r.h"
#include <utility>
#include <algorithm>
#include <iterator>
#include <vector>
#include <cmath>
#include <string>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <catch2/benchmark/catch_benchmark.hpp>

#include <catch2/catch_all.hpp>
#include "test_utils.hpp"


#include <libslic3r/MarchingSquares.hpp>

#include <libslic3r/MTUtils.hpp>

#include <catch2/interfaces/catch_interfaces_capture.hpp>
using namespace Slic3r;

// Benchmarks the (FDM) marching squares of MarchingSquares.hpp, as the gyroid infill uses it. The resin code has its own
// copy of the algorithm, tested in test_marchingsquares_sla.cpp.

namespace marchsq {

static constexpr float  layerf = 0.20;  // layer height in mm (used for z values).
static constexpr float  gsizef = 100.0; // grid size in mm (box volume side length).
static constexpr float  wsizef = 0.50;  // grid window size in mm (roughly line segment length).
static constexpr float  psizef = 0.01;  // raster pixel size in mm (roughly point accuracy).
static constexpr float  isoval = 0.0;   // iso value threshold to use.
static const     long   wsize  = std::round(wsizef / psizef);

static float period = 10.0;            // gyroid "wavelength" in mm (2x line spacing).
static float freq   = 2 * PI / period; // gyroid frequency in waves per mm.

void set_period(float len = 10.0)
{
    period = len;
    freq   = 2 * PI / period;
}

static size_t layer_n;
static size_t ring_n;
static size_t point_n;
static size_t get_n;

void reset_stats()
{
    layer_n = 0;
    ring_n  = 0;
    point_n = 0;
    get_n   = 0;
}

using Rings = std::vector<Ring>;

template<> struct _RasterTraits<size_t>
{
    // using Rst = Slic3r::sla::RasterGrayscaleAA;
    //  The type of pixel cell in the raster
    using ValueType = float;

    // Value at a given position
    static float get(const size_t& layer, size_t row, size_t col)
    {
        get_n++;
        const float x = col * psizef * freq;
        const float y = row * psizef * freq;
        const float z = layer * psizef * freq;

        return sinf(x) * cosf(y) + sinf(y) * cosf(z) + sinf(z) * cosf(x);
    }

    // Number of rows and cols of the raster
    static size_t rows(const size_t& layer) { return std::round(gsizef / psizef); }
    static size_t cols(const size_t& layer) { return std::round(gsizef / psizef); }
};

Rings get_gyroids(size_t l)
{
    size_t layer = l;
    Rings  rings = execute(layer, isoval, {wsize, wsize});
    layer_n++;
    ring_n += rings.size();
    for (auto r : rings)
        point_n += r.size();
    return rings;
}

}; // namespace marchsq

void benchmark_gyroid(float period)
{
    marchsq::reset_stats();
    marchsq::set_period(period);
    INFO("grid size: " << marchsq::gsizef << "mm\nlayer height: " << marchsq::layerf << "mm\n");
    INFO("window size: " << marchsq::wsizef << "mm\npoint size: " << marchsq::psizef << "mm\n");
    INFO("gyroid period: " << marchsq::period << "mm\n");
    BENCHMARK("indexed", i) { return marchsq::get_gyroids(i); };
    INFO("output avg rings/layer: " << float(marchsq::ring_n) / float(marchsq::layer_n) << "\n");
    INFO("output avg points/layer: " << float(marchsq::point_n) / float(marchsq::layer_n) << "\n");
    INFO("output avg gets/layer: " << float(marchsq::get_n) / float(marchsq::layer_n) << "\n");

    REQUIRE(marchsq::layer_n > 0);
}

TEST_CASE("Benchmark gyroid cube period 10.0mm", "[MarchingSquares]") { benchmark_gyroid(10.0); }

TEST_CASE("Benchmark gyroid cube period 5.0mm", "[MarchingSquares]") { benchmark_gyroid(5.0); }
