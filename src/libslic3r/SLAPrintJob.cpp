#include "SLAPrintJob.hpp"

#include <algorithm>
#include <exception>
#include <limits>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "BoundingBox.hpp"
#include "Format/SLAArchiveFormatRegistry.hpp"
#include "Model.hpp"
#include "PrintConfig.hpp"
#include "SLAPrint.hpp"
#include "SLA/Hollowing.hpp"
#include "TriangleMesh.hpp"
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

// Where the objects were in the file, to translate hand-placed support points to the object frame the print uses and the generated
// ones back. The job moves an object (centre_around_origin shifts its mesh and records the shift in origin_translation, the instance
// goes to a slot on the display), and the points of a ModelObject are in the object frame, so they do not follow it by themselves.
struct FileFrame
{
    Transform3d instance { Transform3d::Identity() };   // object -> file, as loaded
    Vec3d       origin   { Vec3d::Zero() };             // origin_translation as loaded
};

std::vector<FileFrame> record_file_frames(Model &model)
{
    std::vector<FileFrame> frames;
    for (ModelObject *mo : model.objects) {
        if (mo->instances.empty())
            mo->add_instance();
        frames.push_back({ mo->instances.front()->get_matrix(), mo->origin_translation });
    }
    return frames;
}

// Applies the configured process on top of the machine (and material) values, with the display as the build area.
int build_sla_config(const SLAJobParams &params, SLAFullPrintConfig &sla_cfg, std::string &error)
{
    sla_cfg.apply(*params.machine, true);
    if (params.material != nullptr)
        sla_cfg.apply(*params.material, true);
    sla_cfg.apply(*params.process, true);

    const double width  = sla_cfg.display_width.getFloat();
    const double height = sla_cfg.display_height.getFloat();
    if (width <= 0. || height <= 0. || sla_cfg.display_pixels_x.getInt() <= 0 || sla_cfg.display_pixels_y.getInt() <= 0) {
        error = "The printer profile has no usable display size and resolution.";
        return CLI_CONFIG_FILE_ERROR;
    }

    // The display is the build area.
    sla_cfg.printable_area.values = { Vec2d(0, 0), Vec2d(width, 0), Vec2d(width, height), Vec2d(0, height) };
    sla_cfg.printer_technology.value = ptSLA;
    if (sla_cfg.sla_archive_format.value.empty())
        sla_cfg.sla_archive_format.value = "SL1";
    return CLI_SUCCESS;
}

// Hand-placed points to the object frame of the packed model; an object with an entry is then "user modified" and gets no generated ones.
void apply_support_points(Model &model, const std::vector<FileFrame> &frames, const std::vector<sla::SupportPoints> &given)
{
    for (size_t i = 0; i < model.objects.size() && i < given.size(); ++i) {
        ModelObject *mo = model.objects[i];
        const Transform3d to_object = frames[i].instance.inverse();
        const Vec3d delta = mo->origin_translation - frames[i].origin;
        mo->sla_support_points.clear();
        for (const sla::SupportPoint &p : given[i]) {
            sla::SupportPoint q = p;
            q.pos = (to_object * p.pos.cast<double>() + delta).cast<float>();
            mo->sla_support_points.push_back(q);
        }
        mo->sla_points_status = sla::PointsStatus::UserModified;
    }
}

// Hand-placed points that are empty for every object: the user took all the supports away.
bool no_supports_wanted(const Model &model, const std::vector<sla::SupportPoints> &given)
{
    return given.size() >= model.objects.size() && std::all_of(given.begin(), given.end(), [](const sla::SupportPoints &p) { return p.empty(); });
}

// The print works on its own copy of the model objects; the copies keep the ids.
size_t object_index(const Model &model, const ModelObject *printed)
{
    const auto it = std::find_if(model.objects.begin(), model.objects.end(), [printed](const ModelObject *mo) { return mo->id() == printed->id(); });
    return size_t(it - model.objects.begin());
}

} // namespace

std::string support_points_to_json(const std::vector<sla::SupportPoints> &objects)
{
    nlohmann::json out = { { "objects", nlohmann::json::array() } };
    for (const sla::SupportPoints &points : objects) {
        nlohmann::json list = nlohmann::json::array();
        for (const sla::SupportPoint &p : points)
            list.push_back({ { "x", p.pos.x() }, { "y", p.pos.y() }, { "z", p.pos.z() }, { "radius", p.head_front_radius },
                             { "type", p.type == sla::SupportPointType::island ? "island" : p.type == sla::SupportPointType::slope ? "slope" : "manual" } });
        out["objects"].push_back({ { "points", list } });
    }
    return out.dump();
}

bool support_points_from_json(const std::string &text, std::vector<sla::SupportPoints> &objects, std::string &error)
{
    objects.clear();
    try {
        const nlohmann::json in = nlohmann::json::parse(text);
        if (! in.is_object() || ! in.contains("objects") || ! in["objects"].is_array()) {
            error = "The support points file needs an \"objects\" list.";
            return false;
        }
        for (const nlohmann::json &object : in["objects"]) {
            if (! object.is_object() || ! object.contains("points") || ! object["points"].is_array()) {
                error = "Every entry of \"objects\" needs a \"points\" list.";
                return false;
            }
            sla::SupportPoints points;
            for (const nlohmann::json &p : object["points"]) {
                for (const char *key : { "x", "y", "z", "radius" })
                    if (! p.is_object() || ! p.contains(key) || ! p[key].is_number()) {
                        error = std::string("A support point needs a number for \"") + key + "\".";
                        return false;
                    }
                const float radius = p["radius"].get<float>();
                if (! (radius > 0.f)) {
                    error = "A support point needs a radius above 0.";
                    return false;
                }
                sla::SupportPointType type = sla::SupportPointType::manual_add;
                if (p.contains("type")) {
                    const std::string name = p["type"].is_string() ? p["type"].get<std::string>() : std::string();
                    if (name == "island")      type = sla::SupportPointType::island;
                    else if (name == "slope")  type = sla::SupportPointType::slope;
                    else if (name != "manual") {
                        error = "A support point type is \"manual\", \"island\" or \"slope\".";
                        return false;
                    }
                }
                points.emplace_back(Vec3f(p["x"].get<float>(), p["y"].get<float>(), p["z"].get<float>()), radius, type);
            }
            objects.push_back(std::move(points));
        }
    } catch (const std::exception &ex) {
        error = std::string("The support points file is not valid JSON: ") + ex.what();
        objects.clear();
        return false;
    }
    return true;
}

SLASupportPointsResult run_sla_support_points(Model &model, const SLAJobParams &params)
{
    SLASupportPointsResult res;
    auto fail = [&res](int code, std::string msg) {
        res.code  = code;
        res.error = std::move(msg);
        res.objects.clear();
        return res;
    };

    if (params.machine == nullptr || params.process == nullptr)
        return fail(CLI_INVALID_PARAMS, "A resin printer needs both a machine and a process profile.");
    if (model.objects.empty())
        return fail(CLI_NO_SUITABLE_OBJECTS, "There is no object to place support points on.");

    SLAFullPrintConfig sla_cfg;
    std::string error;
    if (const int code = build_sla_config(params, sla_cfg, error); code != CLI_SUCCESS)
        return fail(code, error);
    if (no_supports_wanted(model, params.support_points)) {
        res.objects.assign(model.objects.size(), sla::SupportPoints());
        return res;
    }
    sla_cfg.supports_enable.value = true;   // the points are what was asked for, whatever the profile does when slicing

    const std::vector<FileFrame> frames = record_file_frames(model);
    if (! place_on_display(model, sla_cfg.display_width.getFloat(), sla_cfg.display_height.getFloat(), params.keep_positions))
        return fail(CLI_OBJECT_ARRANGE_FAILED, "The objects do not fit on the printer's display.");
    apply_support_points(model, frames, params.support_points);

    try {
        SLAPrint print;
        print.set_status_callback([&params](const PrintBase::SlicingStatus &s) {
            if (params.progress)
                params.progress(s.percent, s.text);
        });
        DynamicPrintConfig dyn;
        dyn.apply(sla_cfg);
        print.apply(model, dyn);

        const std::string err = print.validate_sla();
        if (! err.empty())
            return fail(CLI_VALIDATE_ERROR, err);

        print.stop_after_object_step(slaposSupportPoints);
        print.process();

        res.objects.assign(model.objects.size(), sla::SupportPoints());
        for (const SLAPrintObject *po : print.objects()) {
            const size_t i = object_index(model, po->model_object());
            if (i >= model.objects.size())
                continue;
            const Transform3d to_object = po->trafo().inverse();
            const Transform3d to_file   = frames[i].instance;
            const Vec3d       delta     = model.objects[i]->origin_translation - frames[i].origin;
            for (sla::SupportPoint p : po->get_support_points()) {
                p.pos = (to_file * (to_object * p.pos.cast<double>() - delta)).cast<float>();
                res.objects[i].push_back(p);
            }
        }
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Placing support points failed: " << ex.what();
        return fail(CLI_SLICING_ERROR, ex.what());
    }
    return res;
}

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
    std::string config_error;
    if (const int code = build_sla_config(params, sla_cfg, config_error); code != CLI_SUCCESS)
        return fail(code, config_error);

    res.archive_format = sla_cfg.sla_archive_format.value;
    const char *ext = get_default_extension(res.archive_format.c_str());
    const auto &registry = registered_sla_archives();
    if (registry.find(ArchiveEntry{res.archive_format.c_str()}) == registry.end())
        return fail(CLI_CONFIG_FILE_ERROR, "Unknown resin archive format: " + res.archive_format + " (no writer for it yet)");

    if (no_supports_wanted(model, params.support_points))
        sla_cfg.supports_enable.value = false;

    const std::vector<FileFrame> frames = record_file_frames(model);
    if (! place_on_display(model, sla_cfg.display_width.getFloat(), sla_cfg.display_height.getFloat(), params.keep_positions))
        return fail(CLI_OBJECT_ARRANGE_FAILED, "The objects do not fit on the printer's display.");
    apply_support_points(model, frames, params.support_points);

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
        for (const SLAPrintObject *po : print.objects())
            res.support_point_count += po->get_support_points().size();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Resin slicing failed: " << ex.what();
        return fail(CLI_SLICING_ERROR, ex.what());
    }
    return res;
}

} // namespace Slic3r

namespace Slic3r {

SLAHollowResult run_sla_hollow(Model &model, const SLAHollowParams &params)
{
    SLAHollowResult res;
    auto fail = [&res](int code, std::string msg) {
        res.code  = code;
        res.error = std::move(msg);
        return res;
    };
    auto progress = [&params](int percent, const char *text) {
        if (params.progress)
            params.progress(percent, text);
    };

    if (params.process == nullptr || params.output_path.empty())
        return fail(CLI_INVALID_PARAMS, "Hollowing needs a process profile and a file to write.");
    if (model.objects.empty())
        return fail(CLI_NO_SUITABLE_OBJECTS, "There is no object to hollow.");

    SLAFullPrintConfig sla_cfg;
    sla_cfg.apply(*params.process, true);
    sla::HollowingConfig hollowing;
    hollowing.min_thickness    = sla_cfg.hollowing_min_thickness.getFloat();
    hollowing.quality          = sla_cfg.hollowing_quality.getFloat();
    hollowing.closing_distance = sla_cfg.hollowing_closing_distance.getFloat();

    try {
        indexed_triangle_set all;
        size_t instances = 0;
        for (const ModelObject *mo : model.objects) {
            for (const ModelInstance *inst : mo->instances) {
                TriangleMesh mesh = mo->raw_mesh();
                inst->transform_mesh(&mesh);
                if (mesh.empty())
                    continue;
                ++instances;
                res.volume_before += its_volume(mesh.its);
                const size_t solid_triangles = mesh.its.indices.size();
                progress(10, "Hollowing model");
                sla::hollow_mesh(mesh, hollowing);
                if (mesh.its.indices.size() == solid_triangles)
                    return fail(CLI_SLICING_ERROR, "The model is too thin for a wall of " + std::to_string(hollowing.min_thickness) + " mm: there is no room for a cavity.");
                its_merge(all, mesh.its);
            }
        }
        if (instances == 0)
            return fail(CLI_NO_SUITABLE_OBJECTS, "There is no object to hollow.");
        res.volume_after = its_volume(all);
        res.triangles    = all.indices.size();

        progress(90, "Writing the hollow model");
        boost::system::error_code ec;
        boost::filesystem::create_directories(boost::filesystem::path(params.output_path).parent_path(), ec);
        if (! its_write_stl_binary(params.output_path.c_str(), "hollow", all))
            return fail(CLI_EXPORT_STL_ERROR, "Cannot write " + params.output_path);
        res.path = params.output_path;
        progress(100, "Hollow model written");
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(error) << "Hollowing failed: " << ex.what();
        return fail(CLI_SLICING_ERROR, ex.what());
    }
    return res;
}

} // namespace Slic3r
