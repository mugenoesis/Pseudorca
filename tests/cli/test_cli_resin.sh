#!/usr/bin/env bash
# End-to-end check of slicing for a resin (SLA) printer through the CLI: a machine profile with
# printer_technology = SLA makes the CLI pack the objects on the printer's display, slice them and write the
# printer archive instead of G-code. result.json reports the technology and the resin figures. A model that
# does not fit the display fails with the arrange error and writes no archive.
#
# usage: test_cli_resin.sh <orca-slicer binary> <python3>
set -u

BIN="${1:-}"
PY="${2:-python3}"
# 77 is the test's SKIP_RETURN_CODE.
[ -x "$BIN" ] || { echo "SKIP: orca-slicer binary not found: $BIN"; exit 77; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/orca-cli-resin.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# box <file> <x> <y> <z>: an ASCII STL box, so the test needs no fixture.
box() {
    "$PY" - "$@" <<'PYEOF'
import sys

out, sx, sy, sz = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4])
v = [(x, y, z) for z in (0, sz) for y in (0, sy) for x in (0, sx)]
with open(out, "w") as f:
    f.write("solid box\n")
    for a, b, c, d in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
        for tri in ((v[a], v[b], v[c]), (v[a], v[c], v[d])):
            f.write("facet normal 0 0 0\nouter loop\n")
            for p in tri:
                f.write("vertex %g %g %g\n" % p)
            f.write("endloop\nendfacet\n")
    f.write("endsolid box\n")
PYEOF
}

# A 120 x 68 mm, 2560 x 1440 px display; 0.05 mm layers; no supports, pad or hollowing.
cat > "$WORK/machine.json" <<'JSON'
{
  "type": "machine", "from": "user", "name": "resin-test-printer",
  "printer_technology": "SLA",
  "display_width": "120", "display_height": "68",
  "display_pixels_x": "2560", "display_pixels_y": "1440",
  "printable_height": "150",
  "layer_height": "0.05", "initial_layer_height": "0.3",
  "exposure_time": "2.5", "initial_exposure_time": "35",
  "supports_enable": "0", "pad_enable": "0", "hollowing_enable": "0"
}
JSON
cat > "$WORK/process.json" <<'JSON'
{ "type": "process", "from": "user", "name": "resin-test-process" }
JSON

box "$WORK/cube.stl" 20 20 20
box "$WORK/wide.stl" 150 10 10

# slice <tag> <input>: -> $WORK/<tag>/{rc,log}
slice() {
    local out="$WORK/$1"
    mkdir -p "$WORK/$1.dd"
    timeout 300 "$BIN" --datadir "$WORK/$1.dd" --load-settings "$WORK/machine.json;$WORK/process.json" \
        --slice 0 --outputdir "$out" "$2" > "$WORK/$1.log" 2>&1
    echo "$?" > "$WORK/$1.rc"
}

fail() { echo "FAIL: $*"; exit 1; }
rc() { "$PY" -c "import sys; rc = int(open(sys.argv[1]).read()); print(rc - 256 if rc > 127 else rc)" "$WORK/$1.rc"; }

# 1. A cube slices, and the archive and result.json agree.
slice cube "$WORK/cube.stl"
[ "$(rc cube)" = "0" ] || fail "cube: expected 0, got $(rc cube): $(tail -n 5 "$WORK/cube.log")"
[ -s "$WORK/cube/plate_1.sl1" ] || fail "cube: no archive written"
"$PY" - "$WORK/cube/plate_1.sl1" "$WORK/cube/result.json" <<'PYEOF' || exit 1
import json, sys, zipfile

archive, result_path = sys.argv[1], sys.argv[2]
result = json.load(open(result_path))
if result["return_code"] != 0:
    sys.exit("FAIL: result.json return_code %s" % result["return_code"])
if result.get("printer_technology") != "SLA":
    sys.exit("FAIL: result.json printer_technology %r" % result.get("printer_technology"))
if result.get("archive_format") != "SL1":
    sys.exit("FAIL: result.json archive_format %r" % result.get("archive_format"))

z = zipfile.ZipFile(archive)
names = z.namelist()
pngs = [n for n in names if n.endswith(".png")]
layers = int(result["layer_count"])
if abs(layers - 395) > 3:
    sys.exit("FAIL: expected about 395 layers for a 20 mm cube, result.json says %d" % layers)
if len(pngs) < layers:
    sys.exit("FAIL: %d layer images for %d layers" % (len(pngs), layers))
if "config.ini" not in names:
    sys.exit("FAIL: no config.ini in the archive")
ini = dict(line.split(" = ", 1) for line in z.read("config.ini").decode().splitlines() if " = " in line)
if abs(float(ini["layerHeight"]) - 0.05) > 1e-6:
    sys.exit("FAIL: layerHeight %s" % ini["layerHeight"])
if int(ini["numFast"]) + int(ini["numSlow"]) != layers:
    sys.exit("FAIL: config.ini layer counts %s+%s, result.json %d" % (ini["numFast"], ini["numSlow"], layers))
ml = float(result["resin_used_ml"])
if abs(ml - 8.0) > 0.4:
    sys.exit("FAIL: a solid 20 mm cube is 8 ml, result.json says %s" % ml)
PYEOF

# 2. A model wider than the display is refused with the arrange error (-21) and writes no archive.
slice wide "$WORK/wide.stl"
[ "$(rc wide)" = "-21" ] || fail "wide: expected -21, got $(rc wide): $(tail -n 5 "$WORK/wide.log")"
[ ! -e "$WORK/wide/plate_1.sl1" ] || fail "wide: an archive was written for a model that does not fit"
"$PY" - "$WORK/wide/result.json" <<'PYEOF' || exit 1
import json, sys

result = json.load(open(sys.argv[1]))
if result["return_code"] != -21:
    sys.exit("FAIL: wide: result.json return_code %s, expected -21" % result["return_code"])
PYEOF

echo "PASS: resin slicing writes the archive and reports it, and a model that does not fit is refused"
