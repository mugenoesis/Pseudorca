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
        ${3:+--load-filaments "$3"} --slice 0 --outputdir "$out" "$2" > "$WORK/$1.log" 2>&1
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

# 3. A resin material (--load-filaments) sets the exposure; one with no settings for the printer is refused (-5).
cat > "$WORK/material.json" <<'JSON'
{ "type": "material", "name": "test resin", "vendor": "Test",
  "settings": [ { "printers": ["*"], "layer_height": 0.05, "exposure_time": 7, "initial_exposure_time": 41,
                  "source": "https://example.test/", "retrieved": "2026-10-16" } ] }
JSON
cat > "$WORK/material_other.json" <<'JSON'
{ "type": "material", "name": "other printer resin", "vendor": "Test",
  "settings": [ { "printers": ["Some Other Printer"], "layer_height": 0.05, "exposure_time": 7, "initial_exposure_time": 41 } ] }
JSON
slice material "$WORK/cube.stl" "$WORK/material.json"
[ "$(rc material)" = "0" ] || fail "material: expected 0, got $(rc material): $(tail -n 5 "$WORK/material.log")"
grep -q "resin material test resin" "$WORK/material.log" || fail "material: the log does not name the resin"
"$PY" - "$WORK/material/plate_1.sl1" <<'PYEOF' || exit 1
import sys, zipfile

ini = dict(line.split(" = ", 1) for line in zipfile.ZipFile(sys.argv[1]).read("config.ini").decode().splitlines() if " = " in line)
if abs(float(ini["expTime"]) - 7) > 1e-6 or abs(float(ini["expTimeFirst"]) - 41) > 1e-6:
    sys.exit("FAIL: expTime %s expTimeFirst %s, the material says 7 and 41" % (ini["expTime"], ini["expTimeFirst"]))
PYEOF
slice material_other "$WORK/cube.stl" "$WORK/material_other.json"
[ "$(rc material_other)" = "-5" ] || fail "material_other: expected -5, got $(rc material_other): $(tail -n 5 "$WORK/material_other.log")"
[ ! -e "$WORK/material_other/plate_1.sl1" ] || fail "material_other: an archive was written without usable resin settings"

# 4. --sla-hollow-output hollows the model into an STL (no slicing) in the place the file has it, and result.json says so.
cat > "$WORK/hollow_process.json" <<'JSON'
{ "type": "process", "from": "user", "name": "resin-hollow-process", "hollowing_min_thickness": "2" }
JSON
mkdir -p "$WORK/hollow.dd"
timeout 300 "$BIN" --datadir "$WORK/hollow.dd" --load-settings "$WORK/machine.json;$WORK/hollow_process.json" --slice 0 \
    --sla-hollow-output "$WORK/hollow/hollow.stl" --outputdir "$WORK/hollow" "$WORK/cube.stl" > "$WORK/hollow.log" 2>&1
echo "$?" > "$WORK/hollow.rc"
[ "$(rc hollow)" = "0" ] || fail "hollow: expected 0, got $(rc hollow): $(tail -n 5 "$WORK/hollow.log")"
[ -s "$WORK/hollow/hollow.stl" ] || fail "hollow: no STL written"
[ ! -e "$WORK/hollow/plate_1.sl1" ] || fail "hollow: the model was sliced as well"
"$PY" - "$WORK/hollow/hollow.stl" "$WORK/hollow/result.json" <<'PYEOF' || exit 1
import json, struct, sys

data = open(sys.argv[1], "rb").read()
n = struct.unpack_from("<I", data, 80)[0]
if 84 + 50 * n != len(data):
    sys.exit("FAIL: hollow: not a binary STL")
lo, hi, volume = [1e9] * 3, [-1e9] * 3, 0.0
for i in range(n):
    p = [struct.unpack_from("<3f", data, 84 + 50 * i + 12 + 12 * k) for k in range(3)]
    for v in p:
        for k in range(3):
            lo[k], hi[k] = min(lo[k], v[k]), max(hi[k], v[k])
    (ax, ay, az), (bx, by, bz), (cx, cy, cz) = p
    volume += (ax * (by * cz - bz * cy) - ay * (bx * cz - bz * cx) + az * (bx * cy - by * cx)) / 6.0
# 20 mm cube with 2 mm walls: about 3900 mm3 of material, not 8000
if not 3000 < volume < 5000:
    sys.exit("FAIL: hollow: volume %.1f mm3, expected the walls of a 20 mm cube (about 3900)" % volume)
if n <= 12:
    sys.exit("FAIL: hollow: %d triangles, no cavity" % n)
# the model stays where the file puts it: the cube spans 0 to 20 on every axis
if any(abs(lo[k]) > 0.1 or abs(hi[k] - 20) > 0.1 for k in range(3)):
    sys.exit("FAIL: hollow: spans %s to %s, expected 0 to 20 mm on every axis" % (lo, hi))
result = json.load(open(sys.argv[2]))
if result["return_code"] != 0 or not result.get("hollow_file", "").endswith("hollow.stl"):
    sys.exit("FAIL: hollow: result.json %r" % result)
if abs(float(result["volume_before_mm3"]) - 8000) > 80:
    sys.exit("FAIL: hollow: volume_before_mm3 %s" % result["volume_before_mm3"])
PYEOF

# 5. A model too thin for the wall cannot be hollowed: the error is reported and no STL is written.
box "$WORK/thin.stl" 3 3 3
timeout 300 "$BIN" --datadir "$WORK/hollow.dd" --load-settings "$WORK/machine.json;$WORK/hollow_process.json" --slice 0 \
    --sla-hollow-output "$WORK/thin/thin_hollow.stl" --outputdir "$WORK/thin" "$WORK/thin.stl" > "$WORK/thin.log" 2>&1
echo "$?" > "$WORK/thin.rc"
[ "$(rc thin)" != "0" ] || fail "thin: a 3 mm cube was hollowed with a 2 mm wall"
[ ! -e "$WORK/thin/thin_hollow.stl" ] || fail "thin: an STL was written for a model that cannot be hollowed"

# 6. --sla-support-points-output writes the generated support points (no slicing) in the file's own frame, and
#    --sla-support-points-input slices with exactly the points given.
cat > "$WORK/support_process.json" <<'JSON'
{ "type": "process", "from": "user", "name": "resin-support-process", "supports_enable": "1" }
JSON
mkdir -p "$WORK/points.dd"
timeout 300 "$BIN" --datadir "$WORK/points.dd" --load-settings "$WORK/machine.json;$WORK/support_process.json" --slice 0 \
    --sla-support-points-output "$WORK/points/points.json" --outputdir "$WORK/points" "$WORK/cube.stl" > "$WORK/points.log" 2>&1
echo "$?" > "$WORK/points.rc"
[ "$(rc points)" = "0" ] || fail "points: expected 0, got $(rc points): $(tail -n 5 "$WORK/points.log")"
[ -s "$WORK/points/points.json" ] || fail "points: no points file written"
[ ! -e "$WORK/points/plate_1.sl1" ] || fail "points: the model was sliced as well"
"$PY" - "$WORK/points/points.json" "$WORK/points/result.json" <<'PYEOF' || exit 1
import json, sys

pts = json.load(open(sys.argv[1]))["objects"][0]["points"]
result = json.load(open(sys.argv[2]))
if len(pts) < 4:
    sys.exit("FAIL: points: %d points for a 20 mm cube" % len(pts))
for p in pts:
    on_face = any(abs(p[a] - c) < 0.3 for a in "xyz" for c in (0, 20))
    inside = all(-0.3 <= p[a] <= 20.3 for a in "xyz")
    if not (on_face and inside) or p["radius"] <= 0:
        sys.exit("FAIL: points: %r is not on the cube in the file's own frame" % p)
if result["return_code"] != 0 or int(result.get("support_point_count", -1)) != len(pts):
    sys.exit("FAIL: points: result.json %r" % result)
PYEOF

cat > "$WORK/given.json" <<'JSON'
{ "objects": [ { "points": [
  { "x": 5,  "y": 5,  "z": 0, "radius": 0.4 },
  { "x": 15, "y": 5,  "z": 0, "radius": 0.4 },
  { "x": 10, "y": 15, "z": 0, "radius": 0.5 } ] } ] }
JSON
timeout 300 "$BIN" --datadir "$WORK/points.dd" --load-settings "$WORK/machine.json;$WORK/support_process.json" --slice 0 \
    --sla-support-points-input "$WORK/given.json" --sla-support-points-output "$WORK/given_back/back.json" \
    --outputdir "$WORK/given_back" "$WORK/cube.stl" > "$WORK/given_back.log" 2>&1
echo "$?" > "$WORK/given_back.rc"
[ "$(rc given_back)" = "0" ] || fail "points input: expected 0, got $(rc given_back): $(tail -n 5 "$WORK/given_back.log")"
"$PY" - "$WORK/given_back/back.json" <<'PYEOF' || exit 1
import json, sys

pts = json.load(open(sys.argv[1]))["objects"][0]["points"]
want = [(5, 5, 0, 0.4), (15, 5, 0, 0.4), (10, 15, 0, 0.5)]
got = sorted((round(p["x"], 2), round(p["y"], 2), round(p["z"], 2), round(p["radius"], 2)) for p in pts)
if got != sorted(want):
    sys.exit("FAIL: points input: %r came back as %r" % (want, got))
PYEOF

timeout 300 "$BIN" --datadir "$WORK/points.dd" --load-settings "$WORK/machine.json;$WORK/support_process.json" --slice 1 \
    --sla-support-points-input "$WORK/given.json" --outputdir "$WORK/given_slice" "$WORK/cube.stl" > "$WORK/given_slice.log" 2>&1
echo "$?" > "$WORK/given_slice.rc"
[ "$(rc given_slice)" = "0" ] || fail "points slice: expected 0, got $(rc given_slice): $(tail -n 5 "$WORK/given_slice.log")"
[ -s "$WORK/given_slice/plate_1.sl1" ] || fail "points slice: no archive"
"$PY" - "$WORK/given_slice/result.json" <<'PYEOF' || exit 1
import json, sys

result = json.load(open(sys.argv[1]))
if int(result.get("support_point_count", -1)) != 3:
    sys.exit("FAIL: points slice: support_point_count %r, expected the 3 given" % result.get("support_point_count"))
PYEOF

# 7. A points file that is not usable is refused before anything is sliced.
echo '{"objects": 3}' > "$WORK/bad_points.json"
timeout 300 "$BIN" --datadir "$WORK/points.dd" --load-settings "$WORK/machine.json;$WORK/support_process.json" --slice 1 \
    --sla-support-points-input "$WORK/bad_points.json" --outputdir "$WORK/bad_points" "$WORK/cube.stl" > "$WORK/bad_points.log" 2>&1
echo "$?" > "$WORK/bad_points.rc"
[ "$(rc bad_points)" != "0" ] || fail "bad points: an unusable file was accepted"
[ ! -e "$WORK/bad_points/plate_1.sl1" ] || fail "bad points: an archive was written"
grep -q "objects" "$WORK/bad_points.log" || fail "bad points: the reason was not reported: $(tail -n 3 "$WORK/bad_points.log")"

echo "PASS: resin slicing writes the archive and reports it, a resin material sets the exposure, a model that does not fit is refused, --sla-hollow-output hollows a model (and refuses one too thin), and the support points can be written and given back"
