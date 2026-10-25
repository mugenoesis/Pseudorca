#ifndef SLA_CAVITIES_HPP
#define SLA_CAVITIES_HPP

#include <cstddef>
#include <vector>

#include "libslic3r/ExPolygon.hpp"

namespace Slic3r {
namespace sla {

// A closed cavity under this area (5 mm across) pulls too little on the film to matter. A rule of thumb of this fork, not a figure
// from any resin or printer maker; Trident's warning uses the same one.
constexpr double CLOSED_CAVITY_MIN_AREA_MM2 = 25.;

// Fills the closed cavities in the layers of a model, in place, and returns how many holes it filled.
//
// `layers` are the slices of a model from the build plate up. A hole in a layer is closed when the layer before it had no open resin
// (space that reaches the edge of the layer, or a cavity that does) where the hole is: nothing can flow into it, and as the layer
// peels off the film it seals against it and sucks (cupping). The first layer has no layer before it, so all of its holes are closed.
// A cavity with a channel to the outside is vented and stays, also in the layers where the channel is closed over. Closed holes
// smaller than `min_area_mm2` stay: they pull too little to matter.
std::size_t fill_closed_cavities(std::vector<ExPolygons> &layers, double min_area_mm2);

}} // namespace Slic3r::sla

#endif // SLA_CAVITIES_HPP
