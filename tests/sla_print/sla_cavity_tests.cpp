#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/SLA/Cavities.hpp"

using namespace Slic3r;

namespace {

// A counter-clockwise rectangle in mm.
Polygon rect(double x0, double y0, double x1, double y1)
{
    return Polygon({Point::new_scale(x0, y0), Point::new_scale(x1, y0), Point::new_scale(x1, y1), Point::new_scale(x0, y1)});
}

// A 30 x 30 mm layer, solid.
ExPolygons disk() { return {ExPolygon(rect(0, 0, 30, 30))}; }

// The same layer with a closed square hole of `hole` mm in the middle.
ExPolygons ring(double hole)
{
    Polygon h = rect(15 - hole / 2, 15 - hole / 2, 15 + hole / 2, 15 + hole / 2);
    h.reverse();
    return {ExPolygon(rect(0, 0, 30, 30), h)};
}

// The same layer with a channel from the hole to the edge: one contour, no hole.
ExPolygons ring_with_channel(double hole)
{
    return diff_ex(ExPolygon(rect(0, 0, 30, 30)), rect(15 - hole / 2, 15 - hole / 2, 31, 15 + hole / 2));
}

size_t holes(const std::vector<ExPolygons> &layers)
{
    size_t n = 0;
    for (const ExPolygons &layer : layers)
        for (const ExPolygon &p : layer)
            n += p.holes.size();
    return n;
}

} // namespace

TEST_CASE("A cavity closed at the plate side is filled", "[SLA][Cavities]")
{
    std::vector<ExPolygons> layers{disk(), disk(), ring(6), ring(6), ring(6)};
    REQUIRE(sla::fill_closed_cavities(layers, 1.) == 3);
    REQUIRE(holes(layers) == 0);
    REQUIRE(area(layers[2]) == Catch::Approx(double(scaled(30.)) * double(scaled(30.))));
}

TEST_CASE("Solid layers are left alone", "[SLA][Cavities]")
{
    std::vector<ExPolygons> layers{disk(), disk(), disk()};
    REQUIRE(sla::fill_closed_cavities(layers, 1.) == 0);
}

TEST_CASE("A cavity with a channel to the edge is vented and stays", "[SLA][Cavities]")
{
    std::vector<ExPolygons> layers{disk(), disk(), ring_with_channel(6), ring(6)};
    REQUIRE(sla::fill_closed_cavities(layers, 1.) == 0);
    REQUIRE(holes(layers) == 1);
}

TEST_CASE("A cavity under the minimum area stays", "[SLA][Cavities]")
{
    std::vector<ExPolygons> layers{disk(), ring(2), ring(2)};  // 4 mm2
    REQUIRE(sla::fill_closed_cavities(layers, 25.) == 0);
    REQUIRE(holes(layers) == 2);
}

TEST_CASE("No layers is fine", "[SLA][Cavities]")
{
    std::vector<ExPolygons> layers;
    REQUIRE(sla::fill_closed_cavities(layers, 25.) == 0);
}
