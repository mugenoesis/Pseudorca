# Resin materials

A resin material says how to expose a resin: the exposure of the normal and the bottom layers and what else the resin's maker
recommends, for a printer and a layer height. Choosing a printer and a resin fills those values in, so a print starts from the
maker's numbers instead of the printer profile's defaults.

## Using one

Pass the material file with `--load-filaments` together with the printer and process profiles of a resin printer:

```
orca-slicer --load-settings "printer.json;process.json" --load-filaments material.json --slice 0 model.stl
```

The slicer takes the row of the material that fits the printer and the process profile's layer height, prints which one it
used, and fails with a clear message when the material has nothing for the printer. Values in the process profile still win over
the material, and the material wins over the printer profile.

## The file

```json
{
  "type": "material", "name": "Siraya Tech Easy Grey", "vendor": "Siraya Tech", "resin_type": "plant-based",
  "settings": [
    { "printers": ["Elegoo Mars 3"], "layer_height": 0.05,
      "exposure_time": 2, "initial_exposure_time": 20, "bottom_layer_count": 5, "faded_layers": 5,
      "lift_distance": 5, "lift_speed": 60, "retract_speed": 200, "delay_before_exposure": 1,
      "source": "https://siraya.tech/pages/easy-user-guide", "retrieved": "2026-10-16" }
  ]
}
```

- `printers`: the names in `resources/resin/printers` (vendor and model); `"Elegoo *"` fits every printer of a vendor and `"*"` every
  printer. A row for the printer itself beats a vendor row, which beats `"*"`; a row at the asked layer height beats any row at
  another one, and when there is none the nearest layer height is used.
- The settings a row can hold: `exposure_time`, `initial_exposure_time`, `bottom_layer_count`, `faded_layers` (the transition
  layers), `lift_distance`, `lift_speed` and `retract_speed` (mm and mm/min), `delay_before_exposure`, `delay_after_exposure`,
  `light_pwm`. `material_density`, `bottle_volume`, `bottle_weight` and `bottle_cost` can sit on the resin or on a row.
- Every row of the shipped library names where its numbers come from (`source`) and when they were read (`retrieved`).

## The library

`resources/resin/materials` holds the resins whose makers publish their settings; `materials-index.json` lists them. At the
moment they are Siraya Tech's resins (read from the exposure tables in its user guides by
`scripts/pseudorca/resin-materials-siraya.pl`), plus the rest of Siraya Tech's own 50 um Chitubox profiles for the Anycubic, Elegoo and UniFormation printers, which the Open Resin Alliance ported to its DragonFruit slicer ([df-plugin-sirayatech](https://github.com/Open-Resin-Alliance/df-plugin-sirayatech), MIT, co-authored and supported by Siraya Tech; `scripts/pseudorca/resin-materials-siraya-ora.pl` adds them without replacing a row the library already had: 36 resins, among them every one for the UniFormation GKtwo) and Anycubic's own resins (the "Resin Settings for Anycubic 3D Printers" page,
`scripts/pseudorca/resin-materials-anycubic.pl`, 26 Photon printers) and Elegoo's resins (the spreadsheet of resin setting suggestions, `scripts/pseudorca/resin-materials-elegoo.pl`: 143 resin and color combinations for 20 Elegoo printers; exposure ranges are given as their middle and a two-stage lift as its total distance at the speed that takes as long). `scripts/pseudorca/resin-materials-index.pl` rebuilds
`materials-index.json`. They are the maker's recommendations as a starting point: exposure still depends on
the individual printer, the resin's age and temperature, so a calibration print remains the way to settle it.

## Your own resins

Write a file like the one above, or import a profile you already have (the importers are in `libslic3r/ResinMaterial.hpp`):

- a Chitubox resin profile (`.cfg`): `resin_material_from_chitubox_cfg()`;
- PrusaSlicer SLA material sections (`.ini`, including their inherited values): `resin_materials_from_prusaslicer_ini()`.

An imported profile is scoped to the printer you name when importing, or fits any printer when you give none.
