#include "Cavities.hpp"

#include <algorithm>
#include <cmath>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

namespace Slic3r {
namespace sla {

namespace {

// A rectangle that holds every layer with room to spare: what is inside it and not in a layer is empty space.
ExPolygon frame_around(const std::vector<ExPolygons> &layers)
{
    BoundingBox bb;
    for (const ExPolygons &layer : layers)
        bb.merge(get_extents(layer));
    bb.offset(scaled(1.));
    return ExPolygon(Polygon({bb.min, Point(bb.max.x(), bb.min.y()), bb.max, Point(bb.min.x(), bb.max.y())}));
}

// The hole as a solid region.
ExPolygon region_of(const Polygon &hole)
{
    Polygon region = hole;
    region.make_counter_clockwise();
    return ExPolygon(std::move(region));
}

double area_mm2(const Polygon &hole)
{
    return std::abs(hole.area()) * SCALING_FACTOR * SCALING_FACTOR;
}

} // namespace

std::size_t fill_closed_cavities(std::vector<ExPolygons> &layers, double min_area_mm2)
{
    if (layers.empty())
        return 0;

    const ExPolygon frame = frame_around(layers);
    std::size_t     filled = 0;
    // Open resin of the layer before: the empty space around it and its vented cavities, nothing else.
    ExPolygons open_before;

    for (ExPolygons &layer : layers) {
        ExPolygons small_closed;
        for (ExPolygon &part : layer) {
            Polygons kept;
            for (const Polygon &hole : part.holes) {
                const ExPolygon region = region_of(hole);
                const bool      vented = !intersection_ex(ExPolygons{region}, open_before).empty();
                if (vented) {
                    kept.push_back(hole);
                } else if (area_mm2(hole) < min_area_mm2) {
                    kept.push_back(hole);
                    small_closed.push_back(region);
                } else {
                    ++filled;
                }
            }
            part.holes = std::move(kept);
        }
        open_before = diff_ex(diff_ex(ExPolygons{frame}, layer), small_closed);
    }
    return filled;
}

}} // namespace Slic3r::sla
