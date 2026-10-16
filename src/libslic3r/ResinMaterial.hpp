#ifndef slic3r_ResinMaterial_hpp_
#define slic3r_ResinMaterial_hpp_

// Resin materials: what the resin maker (or the user) says about exposing a resin on a printer.
//
// A material file is JSON with the resin's name and a list of settings rows, each for some printers and one layer height:
//
//   { "type": "material", "name": "Siraya Tech Blu", "vendor": "Siraya Tech", "resin_type": "tough",
//     "settings": [ { "printers": ["Elegoo Mars 3", "Anycubic *", "*"], "layer_height": 0.05,
//                     "exposure_time": 2.5, "initial_exposure_time": 30, "bottom_layer_count": 6,
//                     "source": "https://...", "retrieved": "2026-10-16" } ] }
//
// choose_resin_material() picks the row that fits the printer and layer height best; the chosen values are then applied to
// the slicing configuration between the printer profile and the process profile.

#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

struct ResinMaterialChoice
{
    bool        ok { false };
    std::string error;
    std::string name, vendor, resin_type;
    // option key -> value, for the whitelisted resin options (exposure_time, ...)
    std::map<std::string, std::string> settings;
    std::string source;                 // where the numbers come from
    double      layer_height { 0. };    // the layer height of the chosen row
    // How well the row fits: a row for exactly this printer, for the printer's vendor, or for any printer; and the layer
    // height as asked or the nearest one.
    enum class PrinterMatch { Exact, Vendor, Any } printer_match { PrinterMatch::Any };
    bool        exact_layer_height { false };
};

// `printer_names` are the names the printer is known by (its model, its preset name, ...), compared without case or
// punctuation. A row's printer entry "Vendor *" matches every printer whose name starts with the vendor, "*" every printer.
ResinMaterialChoice choose_resin_material(const std::string &material_json, const std::vector<std::string> &printer_names,
                                          double layer_height);

// Sets the chosen values on `config` (as the profile files would).
void apply_resin_material(const ResinMaterialChoice &choice, DynamicPrintConfig &config);

// Imports: both return the material JSON, or an empty string with `error` set. `printer_name` narrows the settings row to
// one printer (empty: any printer).
// A Chitubox resin profile (.cfg: "key:value" lines).
std::string resin_material_from_chitubox_cfg(const std::string &text, const std::string &printer_name, std::string &error);
// A PrusaSlicer profile file with [sla_material:...] sections: one material for each concrete section (not the "*common*"
// ones), with what it inherits filled in. Returns an empty list with `error` set when there is none.
std::vector<std::string> resin_materials_from_prusaslicer_ini(const std::string &text, const std::string &printer_name,
                                                              std::string &error);

} // namespace Slic3r

#endif // slic3r_ResinMaterial_hpp_
