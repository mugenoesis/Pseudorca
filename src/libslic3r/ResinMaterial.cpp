#include "ResinMaterial.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "PrintConfig.hpp"

namespace Slic3r {

namespace {

using json = nlohmann::json;

// The options a material may set: exposure and what the maker says about moving the plate and the resin.
const std::set<std::string> &settings_keys()
{
    static const std::set<std::string> keys = {
        "exposure_time", "initial_exposure_time", "bottom_layer_count", "faded_layers", "lift_distance", "lift_speed",
        "retract_speed", "delay_before_exposure", "delay_after_exposure", "light_pwm", "material_density", "bottle_volume",
        "bottle_weight", "bottle_cost",
    };
    return keys;
}

std::string normalise(const std::string &s)
{
    std::string out;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string value_text(const json &v)
{
    if (v.is_string())
        return v.get<std::string>();
    std::ostringstream o;
    o.imbue(std::locale::classic());
    if (v.is_boolean()) return v.get<bool>() ? "1" : "0";
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    o << v.get<double>();
    return o.str();
}

// 3: the printer's name, 2: "Vendor *", 1: "*", 0: no match.
int printer_score(const std::string &entry, const std::vector<std::string> &names)
{
    const std::string e = normalise(entry);
    if (entry == "*")
        return 1;
    if (! entry.empty() && entry.back() == '*') {
        const std::string prefix = normalise(entry.substr(0, entry.size() - 1));
        for (const std::string &n : names)
            if (! prefix.empty() && normalise(n).compare(0, prefix.size(), prefix) == 0)
                return 2;
        return 0;
    }
    for (const std::string &n : names) {
        const std::string nn = normalise(n);
        if (nn.empty())
            continue;
        // "Mars 3" is the printer's model and the row says "Elegoo Mars 3": the row ends with the model
        if (nn == e || (e.size() > nn.size() && e.compare(e.size() - nn.size(), nn.size(), nn) == 0) ||
            (nn.size() > e.size() && nn.compare(nn.size() - e.size(), e.size(), e) == 0))
            return 3;
    }
    return 0;
}

std::string to_json_text(const json &j) { return j.dump(2) + "\n"; }

std::vector<std::pair<std::string, std::string>> key_value_lines(const std::string &text, char sep)
{
    std::vector<std::pair<std::string, std::string>> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (! line.empty() && line.back() == '\r') line.pop_back();
        const size_t at = line.find(sep);
        if (at == std::string::npos || line.empty() || line[0] == '#' || line[0] == ';')
            continue;
        auto trim = [](std::string s) {
            const char *ws = " \t";
            s.erase(0, s.find_first_not_of(ws));
            s.erase(s.find_last_not_of(ws) + 1);
            return s;
        };
        out.emplace_back(trim(line.substr(0, at)), trim(line.substr(at + 1)));
    }
    return out;
}

bool to_double(const std::string &s, double &v)
{
    char *end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end != s.c_str() && std::isfinite(v);
}

} // namespace

ResinMaterialChoice choose_resin_material(const std::string &material_json, const std::vector<std::string> &printer_names,
                                          double layer_height)
{
    ResinMaterialChoice c;
    json j;
    try {
        j = json::parse(material_json);
    } catch (const std::exception &e) {
        c.error = std::string("The resin material file is not valid JSON: ") + e.what();
        return c;
    }
    if (! j.is_object() || j.value("type", std::string()) != "material" || ! j.contains("settings") || ! j["settings"].is_array()) {
        c.error = "The file is not a resin material (it needs \"type\": \"material\" and a \"settings\" list).";
        return c;
    }
    c.name       = j.value("name", std::string());
    c.vendor     = j.value("vendor", std::string());
    c.resin_type = j.value("resin_type", std::string());

    // The best row: the closest layer height among the rows that fit the printer, preferring the better printer match.
    int          best_score = 0;
    double       best_gap = 0.;
    const json  *best = nullptr;
    for (const json &row : j["settings"]) {
        if (! row.is_object() || ! row.contains("layer_height"))
            continue;
        int score = 0;
        if (row.contains("printers") && row["printers"].is_array()) {
            for (const json &p : row["printers"])
                if (p.is_string())
                    score = std::max(score, printer_score(p.get<std::string>(), printer_names));
        } else {
            score = 1;
        }
        if (score == 0)
            continue;
        const double gap = std::abs(row["layer_height"].get<double>() - layer_height);
        // a row at the right layer height beats any other; then the better printer match; then the closer layer height
        const bool exact_layer = gap < 0.0005;
        const bool best_exact_layer = best && best_gap < 0.0005;
        bool better = false;
        if (! best) better = true;
        else if (exact_layer != best_exact_layer) better = exact_layer;
        else if (score != best_score) better = score > best_score;
        else better = gap < best_gap;
        if (better) { best = &row; best_score = score; best_gap = gap; }
    }
    if (! best) {
        c.error = "The resin material \"" + c.name + "\" has no settings for this printer.";
        return c;
    }

    for (auto it = best->begin(); it != best->end(); ++it)
        if (settings_keys().count(it.key()))
            c.settings[it.key()] = value_text(it.value());
    // resin wide values apply unless the row says otherwise
    for (const std::string &key : { "material_density", "bottle_volume", "bottle_weight", "bottle_cost" })
        if (! c.settings.count(key) && j.contains(key))
            c.settings[key] = value_text(j[key]);
    c.source = best->value("source", std::string());
    c.layer_height = (*best)["layer_height"].get<double>();
    c.exact_layer_height = best_gap < 0.0005;
    c.printer_match = best_score == 3 ? ResinMaterialChoice::PrinterMatch::Exact :
                      best_score == 2 ? ResinMaterialChoice::PrinterMatch::Vendor : ResinMaterialChoice::PrinterMatch::Any;
    c.ok = true;
    return c;
}

void apply_resin_material(const ResinMaterialChoice &choice, DynamicPrintConfig &config)
{
    for (const auto &[key, value] : choice.settings)
        config.set_deserialize_strict(key, value);
}

std::string resin_material_from_chitubox_cfg(const std::string &text, const std::string &printer_name, std::string &error)
{
    std::map<std::string, std::string> kv;
    for (auto &[k, v] : key_value_lines(text, ':'))
        kv[k] = v;
    if (! kv.count("normalExposureTime") || ! kv.count("layerHeight")) {
        error = "This does not look like a Chitubox resin profile (normalExposureTime or layerHeight is missing).";
        return {};
    }

    auto num = [&](const char *key, double &v) { auto it = kv.find(key); return it != kv.end() && to_double(it->second, v); };
    json row = json::object();
    row["printers"] = json::array({ printer_name.empty() ? std::string("*") : printer_name });
    double v = 0.;
    if (! num("layerHeight", v)) { error = "The layer height is not a number."; return {}; }
    row["layer_height"] = v;
    if (num("normalExposureTime", v))       row["exposure_time"] = v;
    if (num("bottomLayExposureTime", v))    row["initial_exposure_time"] = v;
    if (num("bottomLayCount", v))           row["bottom_layer_count"] = int(v);
    if (num("normalLayerLiftHeight", v))    row["lift_distance"] = v;
    if (num("normalLayerLiftSpeed", v))     row["lift_speed"] = v;
    if (num("normalDropSpeed", v))          row["retract_speed"] = v;
    if (num("lightOffTime", v))             row["delay_before_exposure"] = v;
    if (num("normalLightIntensityPWM", v))  row["light_pwm"] = int(v);
    row["source"] = "Chitubox resin profile";

    json j = json::object();
    j["type"] = "material";
    j["name"] = kv.count("currProfile") ? kv["currProfile"] : std::string("Imported resin");
    j["vendor"] = "";
    if (kv.count("__resin")) j["resin_type"] = kv["__resin"];
    if (num("resinDensity", v) && v > 0.) j["material_density"] = v;
    // a price per litre is a bottle of one litre
    if (kv.count("resinUnit") && kv["resinUnit"].size() >= 2 && kv["resinUnit"].substr(kv["resinUnit"].size() - 2) == "/L" &&
        num("resinPrice", v) && v > 0.) {
        j["bottle_volume"] = 1000.;
        j["bottle_cost"] = v;
    }
    j["settings"] = json::array({ row });
    return to_json_text(j);
}

std::vector<std::string> resin_materials_from_prusaslicer_ini(const std::string &text, const std::string &printer_name,
                                                              std::string &error)
{
    using Section = std::map<std::string, std::string>;
    std::map<std::string, Section> sections;
    std::vector<std::string> order;
    {
        std::istringstream in(text);
        std::string line, current;
        while (std::getline(in, line)) {
            if (! line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("[sla_material:", 0) == 0 && line.back() == ']') {
                current = line.substr(14, line.size() - 15);
                if (! sections.count(current)) order.push_back(current);
                sections[current];
            } else if (line.rfind("[", 0) == 0) {
                current.clear();
            } else if (! current.empty()) {
                const auto kv = key_value_lines(line, '=');
                if (! kv.empty()) sections[current][kv.front().first] = kv.front().second;
            }
        }
    }

    // The values of a section with its parents' underneath (a loop of "inherits" is cut).
    std::function<Section(const std::string &, int)> flatten = [&](const std::string &name, int depth) {
        Section out;
        const auto it = sections.find(name);
        if (it == sections.end() || depth > 8)
            return out;
        const auto parents = it->second.find("inherits");
        if (parents != it->second.end()) {
            std::istringstream p(parents->second);
            std::string parent;
            while (std::getline(p, parent, ',')) {
                parent.erase(0, parent.find_first_not_of(" \t"));
                parent.erase(parent.find_last_not_of(" \t") + 1);
                for (auto &[k, v] : flatten(parent, depth + 1)) out[k] = v;
            }
        }
        for (auto &[k, v] : it->second) out[k] = v;
        return out;
    };

    std::vector<std::string> materials;
    std::string last_error;
    for (const std::string &name : order) {
        if (name.empty() || name[0] == '*')
            continue;
        const Section kv = flatten(name, 0);
        if (! kv.count("exposure_time") || ! kv.count("initial_exposure_time"))
            continue;
        json row = json::object();
        row["printers"] = json::array({ printer_name.empty() ? std::string("*") : printer_name });
        double v = 0.;
        double layer_height = 0.;
        const auto cond = kv.find("compatible_prints_condition");
        if (cond != kv.end()) {
            const size_t eq = cond->second.find("layer_height ==");
            if (eq != std::string::npos)
                to_double(cond->second.substr(eq + 15), layer_height);
        }
        if (layer_height <= 0. && kv.count("initial_layer_height"))
            to_double(kv.at("initial_layer_height"), layer_height);
        if (layer_height <= 0.) { last_error = "The layer height of \"" + name + "\" could not be found."; continue; }
        row["layer_height"] = layer_height;
        if (to_double(kv.at("exposure_time"), v)) row["exposure_time"] = v;
        if (to_double(kv.at("initial_exposure_time"), v)) row["initial_exposure_time"] = v;

        // The Anycubic profiles keep the lift in the notes: "LIFT_DISTANCE=8.0\nLIFT_SPEED=2.5" (mm and mm/s)
        const auto notes_it = kv.find("material_notes");
        if (notes_it != kv.end()) {
            std::string notes = notes_it->second;
            for (size_t at; (at = notes.find("\\n")) != std::string::npos;) notes.replace(at, 2, "\n");
            std::map<std::string, std::string> nk;
            for (auto &[k, val] : key_value_lines(notes, '='))
                nk[k] = val;
            if (nk.count("LIFT_DISTANCE") && to_double(nk["LIFT_DISTANCE"], v)) row["lift_distance"] = v;
            if (nk.count("LIFT_SPEED") && to_double(nk["LIFT_SPEED"], v)) row["lift_speed"] = v * 60.;
            if (nk.count("RETRACT_SPEED") && to_double(nk["RETRACT_SPEED"], v)) row["retract_speed"] = v * 60.;
            if (nk.count("DELAY_BEFORE_EXPOSURE") && to_double(nk["DELAY_BEFORE_EXPOSURE"], v)) row["delay_before_exposure"] = v;
        }
        row["source"] = "PrusaSlicer material profile";

        json j = json::object();
        j["type"] = "material";
        j["name"] = name;
        j["vendor"] = kv.count("material_vendor") ? kv.at("material_vendor") : std::string();
        if (kv.count("material_type")) {
            std::string t = kv.at("material_type");
            std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            j["resin_type"] = t;
        }
        for (const char *key : { "bottle_volume", "bottle_weight", "bottle_cost" })
            if (kv.count(key) && to_double(kv.at(key), v) && v > 0.) j[key] = v;
        j["settings"] = json::array({ row });
        materials.push_back(to_json_text(j));
    }
    if (materials.empty())
        error = last_error.empty() ? std::string("No PrusaSlicer SLA material with an exposure time was found.") : last_error;
    return materials;
}

} // namespace Slic3r
