#include "SLAPrintJob.hpp"

#include <algorithm>
#include <exception>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "BoundingBox.hpp"
#include "Format/SLAArchiveFormatRegistry.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"
#include "SLAPrint.hpp"
#include "Utils.hpp"

namespace Slic3r {

namespace {

constexpr double ObjectSpacing = 5.; // mm between packed objects

// Packs the objects of the model on the display in rows, left to right, and centres the pack on the display.
// Returns false when they do not fit.
bool place_on_display(Model &model, double width, double height, bool keep_positions)
{
    std::vector<ModelObject*> objects;
    for (ModelObject *mo : model.objects) {
        if (mo->instances.empty())
            mo->add_instance();
        objects.emplace_back(mo);
    }

    if (keep_positions) {
        for (ModelObject *mo : objects)
            mo->ensure_on_bed();
        return true;
    }

    // Shelf packing of the bounding boxes, in the plane of the display.
    struct Slot { ModelObject *obj; Vec2d size; Vec2d pos; };
    std::vector<Slot> slots;
    double x = 0., y = 0., row_h = 0., used_w = 0.;
    for (ModelObject *mo : objects) {
        mo->center_around_origin();
        const BoundingBoxf3 bb = mo->instance_bounding_box(0);
        const Vec2d size(bb.size().x(), bb.size().y());
        if (x > 0. && x + size.x() > width) {
            x = 0.;
            y += row_h + ObjectSpacing;
            row_h = 0.;
        }
        slots.push_back({ mo, size, Vec2d(x, y) });
        x += size.x() + ObjectSpacing;
        row_h  = std::max(row_h, size.y());
        used_w = std::max(used_w, x - ObjectSpacing);
    }
    const double used_h = y + row_h;
    if (used_w > width + EPSILON || used_h > height + EPSILON)
        return false;

    const Vec2d shift((width - used_w) / 2., (height - used_h) / 2.);
    for (Slot &s : slots) {
        // center_around_origin() put the mesh centre at the origin, so the centre of the slot is the offset.
        const Vec2d centre = shift + s.pos + s.size / 2.;
        for (ModelInstance *inst : s.obj->instances)
            inst->set_offset(Vec3d(centre.x(), centre.y(), 0.));
        s.obj->ensure_on_bed();
    }
    return true;
}

} // namespace

SLAJobResult run_sla_job(Model &model, const SLAJobParams &params)
{
    SLAJobResult res;
    auto fail = [&res](int code, std::string msg) {
        res.code  = code;
        res.error = std::move(msg);
        return res;
    };

    if (params.machine == nullptr || params.process == nullptr)
        return fail(CLI_INVALID_PARAMS, "A resin printer needs both a machine and a process profile.");
    if (model.objects.empty())
        return fail(CLI_NO_SUITABLE_OBJECTS, "There is no object to slice.");

    SLAFullPrintConfig sla_cfg;
    sla_cfg.apply(*params.machine, true);
    sla_cfg.apply(*params.process, true);

    const double width  = sla_cfg.display_width.getFloat();
    const double height = sla_cfg.display_height.getFloat();
    if (width <= 0. || height <= 0. || sla_cfg.display_pixels_x.getInt() <= 0 || sla_cfg.display_pixels_y.getInt() <= 0)
        return fail(CLI_CONFIG_FILE_ERROR, "The printer profile has no usable display size and resolution.");

    // The display is the build area.
    sla_cfg.printable_area.values = { Vec2d(0, 0), Vec2d(width, 0), Vec2d(width, height), Vec2d(0, height) };
    sla_cfg.printer_technology.value = ptSLA;
    if (sla_cfg.sla_archive_format.value.empty())
        sla_cfg.sla_archive_format.value = "SL1";

    res.archive_format = sla_cfg.sla_archive_format.value;
    const char *ext = get_default_extension(res.archive_format.c_str());
    const auto &registry = registered_sla_archives();
    if (registry.find(ArchiveEntry{res.archive_format.c_str()}) == registry.end())
        return fail(CLI_CONFIG_FILE_ERROR, "Unknown resin archive format: " + res.archive_format);

    if (! place_on_display(model, width, height, params.keep_positions))
        return fail(CLI_OBJECT_ARRANGE_FAILED, "The objects do not fit on the printer's display.");

    SLAPrint print;
    print.set_status_callback([&params](const PrintBase::SlicingStatus &s) {
        if (params.progress)
            params.progress(s.percent, s.text);
    });

    try {
        DynamicPrintConfig dyn;
        dyn.apply(sla_cfg);
        print.apply(model, dyn);

        const std::string err = print.validate_sla();
        if (! err.empty())
            return fail(CLI_VALIDATE_ERROR, err);

        print.process();

        boost::filesystem::path out(params.output_dir.empty() ? "." : params.output_dir);
        boost::system::error_code ec;
        boost::filesystem::create_directories(out, ec);
        out /= std::string("plate_1.") + (ext ? ext : "zip");
        print.export_print(out.string());

        const SLAPrintStatistics &stats = print.print_statistics();
        res.archive_path  = out.string();
        res.layer_count   = stats.fast_layers_count + stats.slow_layers_count;
        res.print_time_s  = stats.estimated_print_time;
        res.resin_used_ml = (stats.objects_used_material + stats.support_used_material) / 1000.; // the statistics are in mm^3
        res.layer_height  = print.default_object_config().layer_height.getFloat();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Resin slicing failed: " << ex.what();
        return fail(CLI_SLICING_ERROR, ex.what());
    }
    return res;
}

} // namespace Slic3r
