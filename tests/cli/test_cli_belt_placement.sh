#!/usr/bin/env bash
# End-to-end checks of where the CLI puts an object on a belt printer. The placement code lives inline in
# CLI::run(), so only the binary can reach it.
#
# - Without --keep-positions an object is moved next to the prime lines at the start of the belt.
# - --belt-shift-y N moves the start of the print N mm further along the belt, whichever way the object was
#   placed: by the automatic arrangement of a plain mesh, or by the single-object recentre of a project that
#   is not arranged (--arrange=0).
# - --keep-positions leaves an object where the file put it, and then --belt-shift-y has nothing to move.
#
# The belt axis is machine Z with the remap used below, so "along the belt" is the Z of the extruding moves.
#
# usage: test_cli_belt_placement.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-belt.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/datadir"

# Standalone presets: without "inherits" the CLI loads them as-is, with no preset bundle.
cat > "$WORK/machine.json" <<'EOF'
{
    "type": "machine",
    "from": "User",
    "name": "CLI belt placement test printer",
    "printable_area": ["0x0", "200x0", "200x2000", "0x2000"],
    "printable_height": "300",
    "layer_change_gcode": "G92 E0",
    "belt_printer": "1",
    "belt_printer_infinite_y": "1",
    "belt_slice_rotation": "x",
    "belt_slice_rotation_angle": "45",
    "gcode_remap_x": "rev_x",
    "gcode_remap_y": "pos_z",
    "gcode_remap_z": "pos_y",
    "z_hop": "0"
}
EOF
cat > "$WORK/process.json" <<'EOF'
{
    "type": "process",
    "from": "User",
    "name": "CLI belt placement test process",
    "skirt_loops": "0"
}
EOF
cat > "$WORK/filament.json" <<'EOF'
{
    "type": "filament",
    "from": "User",
    "name": "CLI belt placement test filament"
}
EOF

# A 10 mm cube as an STL (no position of its own) and as a 3MF that places it 500 mm along the belt.
"$PY" - "$WORK" <<'EOF'
import sys
import zipfile

work = sys.argv[1]
verts = [(x, y, z) for z in (0, 10) for y in (0, 10) for x in (0, 10)]
# Faces wound counter-clockwise seen from outside: -z, +z, -y, +y, -x, +x.
tris = []
for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
    tris += [(a, b, c), (a, c, d)]

with open(work + "/cube.stl", "w") as f:
    f.write("solid cube\n")
    for tri in tris:
        f.write("facet normal 0 0 0\nouter loop\n")
        for i in tri:
            f.write("vertex %g %g %g\n" % verts[i])
        f.write("endloop\nendfacet\n")
    f.write("endsolid cube\n")

vertices = "".join('<vertex x="%g" y="%g" z="%g"/>' % v for v in verts)
triangles = "".join('<triangle v1="%d" v2="%d" v3="%d"/>' % t for t in tris)
model = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
    '<resources><object id="1" type="model"><mesh><vertices>%s</vertices><triangles>%s</triangles></mesh></object></resources>'
    '<build><item objectid="1" transform="1 0 0 0 1 0 0 0 1 100 500 0"/></build></model>' % (vertices, triangles)
)
content_types = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
    '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
    '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>'
)
rels = (
    '<?xml version="1.0" encoding="UTF-8"?>'
    '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
    '<Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>'
    '</Relationships>'
)
with zipfile.ZipFile(work + "/placed.3mf", "w") as z:
    z.writestr("[Content_Types].xml", content_types)
    z.writestr("_rels/.rels", rels)
    z.writestr("3D/3dmodel.model", model)
EOF

fails=0
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

# start_z <tag> <input> [option...]: slice <input> into $WORK/<tag> and print the lowest Z (along the belt) of
# an extruding move, or nothing when the slice failed.
start_z() {
    local out="$WORK/$1" input="$2"; shift 2
    mkdir -p "$out"
    timeout 300 "$BIN" --datadir "$WORK/datadir" --outputdir "$out" \
        --load-settings "$WORK/machine.json;$WORK/process.json" --load-filaments "$WORK/filament.json" \
        --slice 0 "$@" "$input" > "$out/log" 2>&1 || return 0
    local gcode; gcode="$(ls "$out"/*.gcode 2>/dev/null | head -1)"
    [ -n "$gcode" ] || return 0
    "$PY" - "$gcode" <<'EOF'
import re
import sys

lowest = None
with open(sys.argv[1]) as f:
    for line in f:
        if line.startswith("G1 ") and re.search(r" E[0-9.]", line):
            m = re.search(r" Z(-?[0-9.]+)", line)
            if m:
                z = float(m.group(1))
                lowest = z if lowest is None else min(lowest, z)
if lowest is not None:
    print("%.3f" % lowest)
EOF
}

# expect_near <what> <got> <want> <tolerance>
expect_near() {
    [ -n "$2" ] || { fail "$1: the slice produced no extruding moves"; return; }
    "$PY" -c 'import sys; sys.exit(0 if abs(float(sys.argv[1]) - float(sys.argv[2])) <= float(sys.argv[3]) else 1)' "$2" "$3" "$4" \
        || fail "$1: lowest belt position $2, want $3 (+/- $4)"
}

# expect_below <what> <got> <limit>
expect_below() {
    [ -n "$2" ] || { fail "$1: the slice produced no extruding moves"; return; }
    "$PY" -c 'import sys; sys.exit(0 if float(sys.argv[1]) < float(sys.argv[2]) else 1)' "$2" "$3" \
        || fail "$1: lowest belt position $2, want below $3"
}

# expect_above <what> <got> <limit>
expect_above() {
    [ -n "$2" ] || { fail "$1: the slice produced no extruding moves"; return; }
    "$PY" -c 'import sys; sys.exit(0 if float(sys.argv[1]) > float(sys.argv[2]) else 1)' "$2" "$3" \
        || fail "$1: lowest belt position $2, want above $3"
}

SHIFT=10

# A plain mesh is arranged by the CLI.
mesh_base="$(start_z mesh_base "$WORK/cube.stl")"
mesh_shift="$(start_z mesh_shift "$WORK/cube.stl" --belt-shift-y "$SHIFT")"
expect_below "mesh near the prime lines" "$mesh_base" 100
expect_near "mesh with --belt-shift-y $SHIFT" "$mesh_shift" "$("$PY" -c "print(float('${mesh_base:-0}') + $SHIFT)")" 0.05

# A project the caller has already placed and does not want arranged: the single object is recentred.
placed_base="$(start_z placed_base "$WORK/placed.3mf" --arrange=0)"
placed_shift="$(start_z placed_shift "$WORK/placed.3mf" --arrange=0 --belt-shift-y "$SHIFT")"
expect_below "placed project near the prime lines" "$placed_base" 100
expect_near "placed project with --belt-shift-y $SHIFT" "$placed_shift" "$("$PY" -c "print(float('${placed_base:-0}') + $SHIFT)")" 0.05

# --keep-positions leaves the object 500 mm along the belt, and the shift has nothing to move.
keep="$(start_z keep "$WORK/placed.3mf" --arrange=0 --keep-positions=1)"
keep_shift="$(start_z keep_shift "$WORK/placed.3mf" --arrange=0 --keep-positions=1 --belt-shift-y "$SHIFT")"
expect_above "kept position far along the belt" "$keep" 400
expect_near "kept positions with --belt-shift-y $SHIFT" "$keep_shift" "${keep:-0}" 0.05

[ "$fails" -eq 0 ] || exit 1
echo "PASS"
