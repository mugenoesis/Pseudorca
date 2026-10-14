"""Golden FDM outputs for the Pseudorca no-regression gate.

Runs INSIDE a Trident image (see golden.sh): it slices and converts a fixed set of models through Trident's own
`app.cli_runner`, exactly the way the API starts a job, and records what the engine produced, so a candidate
engine can be compared with the baseline engine (golden_compare.py).

    python golden_cases.py <outdir> [case-name-substring ...]

What is recorded per case (summary.json): the process return code and result.json, the progress-message keys, and
for every output file a sha256 of its *normalised* content. Normalising removes only what legitimately differs
between two runs of the same engine: the G-code timestamp line and the per-build object ids. The normalised
G-code is also kept (gzip) so a failing case can be diffed.
"""

from __future__ import annotations

import gzip
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import zipfile
from pathlib import Path

OUT = Path(sys.argv[1])
ONLY = sys.argv[2:]
OUT.mkdir(parents=True, exist_ok=True)
WORK = Path(tempfile.mkdtemp(prefix="golden-"))

# Isolated scratch state: the real /data is never touched, and a mounted /data would be ignored.
for key, sub in (
    ("MODELS_DIR", "models"),
    ("OUTPUT_DIR", "output"),
    ("JOBSTORE_PATH", "output/jobstore.sqlite3"),
    ("USERS_DB_PATH", "output/users.sqlite3"),
    ("PRINTERS_DB_PATH", "output/printers.sqlite3"),
    ("ORCASLICER_DATADIR", "datadir"),
):
    os.environ[f"ORCA_API_{key}"] = str(WORK / sub)

sys.path.insert(0, "/tests_slicer")  # project_fixtures.py (mounted by golden.sh)

from app import cli_runner, profiles  # noqa: E402
from app.config import settings  # noqa: E402

profiles.catalog.load()

PRINTERS = {
    "ender3": {
        "printer": "Creality Ender-3 0.4 nozzle",
        "process": "0.20mm Standard @Creality Ender3",
        "material": "CR-PLA @Ender-3-all",
        "nozzles": 1,
    },
    "u1": {
        "printer": "Snapmaker U1 (0.4 nozzle)",
        "process": "0.20 Standard @Snapmaker U1 (0.4 nozzle)",
        "material": "Snapmaker PLA Basic @U1",
        "nozzles": 4,
    },
    "ir3belt": {
        "printer": "IdeaFormer IR3 V2 0.4 nozzle",
        "process": None,
        "material": "Generic PLA @IdeaFormer IR3 V2",
        "nozzles": 1,
    },
}
HANDY = Path("/opt/orcaslicer/resources/handy_models")


def process_for(p: dict) -> str:
    if p["process"]:
        return p["process"]
    return profiles.catalog.get_by_name("machine", p["printer"], None).data["default_print_profile"]


# --- generated models (binary STL) -------------------------------------------------------------------------------


def _box(x0, y0, z0, x1, y1, z1):
    v = [
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    ]  # fmt: skip
    quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
    tris = []
    for a, b, c, d in quads:
        tris += [(v[a], v[b], v[c]), (v[a], v[c], v[d])]
    return tris


def _write_stl(path: Path, tris) -> Path:
    with path.open("wb") as f:
        f.write(b"golden".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            f.write(struct.pack("<12fH", 0, 0, 0, *[c for p in t for c in p], 0))
    return path


MODELS = {
    "cube": _write_stl(WORK / "cube.stl", _box(0, 0, 0, 20, 20, 20)),
    # a pillar with a wide slab on top: the slab overhangs on both sides and needs support
    "tshape": _write_stl(WORK / "tshape.stl", _box(15, 0, 0, 25, 10, 12) + _box(0, 0, 12, 40, 10, 18)),
    # two separate shells with empty layers between them (a gap in the layers)
    "gap": _write_stl(WORK / "gap.stl", _box(0, 0, 0, 10, 10, 10) + _box(0, 0, 20, 10, 10, 30)),
}

# --- normalisation -----------------------------------------------------------------------------------------------

_OBJECT_ID = re.compile(rb"(id:\s*)\d+")
_LABEL_ID = re.compile(rb"(label id:\s*)\d+")


def normalise_gcode(raw: bytes) -> bytes:
    lines = raw.split(b"\n")
    if len(lines) > 1:
        lines[1] = b"; <timestamp>"  # line 2 carries the generation time
    text = b"\n".join(lines)
    text = _LABEL_ID.sub(rb"\1N", text)
    return _OBJECT_ID.sub(rb"\1N", text)


_3MF_STAMP = re.compile(rb'(<metadata name="(?:CreationDate|ModificationDate)">)[^<]*(</metadata>)')


def normalise_3mf_member(name: str, data: bytes) -> bytes:
    data = _3MF_STAMP.sub(rb"\1<date>\2", data)
    if name == "Metadata/model_settings.config":
        # The engine lists objects, parts and assemble items in an order that depends on pointer values, so it changes
        # with any rebuild of the binary (and so do the ids handed out in that order). Compare them as a multiset of
        # lines with the ids masked: the content must match, the order need not.
        text = data.decode("utf-8", "replace")
        text = re.sub(r'\b((?:object_|source_object_|identify_)?id)="\d+"', r'\1="#"', text)
        text = re.sub(r'(key="source_object_id" value=")\d+', r"\1#", text)
        data = "\n".join(sorted(text.splitlines())).encode()
    return data


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def describe_outputs(case: str, folder: Path) -> dict:
    files = {}
    for path in sorted(folder.iterdir()):
        if path.name in ("progress.pipe", "result.json"):
            continue
        if path.suffix == ".gcode":
            norm = normalise_gcode(path.read_bytes())
            (OUT / case).mkdir(parents=True, exist_ok=True)
            with gzip.open(OUT / case / (path.name + ".gz"), "wb") as f:
                f.write(norm)
            files[path.name] = {"sha256": sha(norm), "bytes": len(norm)}
        elif path.suffix == ".3mf":
            files[path.name] = describe_3mf(path)
        elif path.is_file():
            files[path.name] = {"sha256": sha(path.read_bytes()), "bytes": path.stat().st_size}
    return files


def describe_3mf(path: Path) -> dict:
    members = {}
    with zipfile.ZipFile(path) as z:
        for info in sorted(z.infolist(), key=lambda i: i.filename):
            if info.is_dir() or info.filename.lower().endswith((".png", ".jpg", ".gcode")):
                members[info.filename] = {"bytes": info.file_size}  # thumbnails/plate gcode: presence + size only
                continue
            members[info.filename] = {"sha256": sha(normalise_3mf_member(info.filename, z.read(info)))}
    return {"members": members}


# --- cases -------------------------------------------------------------------------------------------------------

summary: dict[str, dict] = {}


def wanted(name: str) -> bool:
    return not ONLY or any(o in name for o in ONLY)


_TIMING = {"prepare_time", "export_time", "sliced_time", "sliced_time_with_cache"}


def scrub(value):
    """result.json with the work path replaced and the wall-clock timings removed (they differ run to run)."""

    def strip(node):
        if isinstance(node, dict):
            return {k: strip(v) for k, v in node.items() if k not in _TIMING}
        if isinstance(node, list):
            return [strip(v) for v in node]
        return node

    return strip(json.loads(json.dumps(value, default=str).replace(str(WORK), "<work>")))


def slice_case(name, model, printer, *, overrides=None, filaments=1, arrange=True, **kw):
    if not wanted(name):
        return
    p = PRINTERS[printer]
    folder = WORK / "cases" / name
    progress_keys: set[str] = set()
    warnings: set[str] = set()

    def on_progress(msg):
        data = msg.model_dump() if hasattr(msg, "model_dump") else dict(msg)
        progress_keys.update(k for k, v in data.items() if v is not None and k != "warning")
        if data.get("warning"):
            warnings.add(str(data["warning"]))

    result = cli_runner.run_slice(
        model_path=Path(model),
        output_dir=folder,
        printer_profile=p["printer"],
        process_profile=process_for(p),
        filament_profiles=[p["material"]] * filaments,
        setting_overrides=dict(overrides or {}),
        arrange=arrange,
        on_progress=on_progress,
        timeout_s=1500,
        **kw,
    )
    summary[name] = {
        "kind": "slice",
        "return_code": result.return_code,
        "result_json": scrub(result.result_json),
        "progress_keys": sorted(progress_keys),
        "progress_warnings": sorted(warnings),
        "files": describe_outputs(name, folder),
    }
    print(f"{name}: rc={result.return_code} files={sorted(summary[name]['files'])}", flush=True)


def convert_case(name, model, **kw):
    if not wanted(name):
        return
    folder = WORK / "cases" / name
    dest = folder / "out.3mf"
    try:
        cli_runner.convert_model(Path(model), dest, **kw)
        outcome = "ok"
    except Exception as exc:  # the error message is part of the contract too
        outcome = f"{type(exc).__name__}: {exc}"
    summary[name] = {
        "kind": "convert",
        "outcome": outcome,
        "files": describe_outputs(name, folder) if folder.is_dir() else {},
    }
    print(f"{name}: {outcome}", flush=True)


def project_case(name, *, printer, colours, authored_on, style, remap=""):
    if not wanted(name):
        return
    from project_fixtures import make_project

    authors = {
        "x1c": ("Bambu Lab X1 Carbon 0.4 nozzle", "0.20mm Standard @BBL X1C", "Bambu PLA Basic @BBL X1C"),
        "u1": ("Snapmaker U1 (0.4 nozzle)", "0.20 Standard @Snapmaker U1 (0.4 nozzle)", "Snapmaker PLA Basic @U1"),
    }[authored_on]
    path = make_project(
        WORK / f"{name}.3mf",
        authoring_printer=authors[0],
        authoring_process=authors[1],
        authoring_filament=authors[2],
        colours=colours,
        bin_path=settings.orcaslicer_bin,
        datadir=str(settings.orcaslicer_datadir),
        resolved_dir=Path(tempfile.mkdtemp(prefix="golden-resolved-")),
        style=style,
    )
    p = PRINTERS[printer]
    overrides = {
        "nozzle_diameter": ",".join(["0.4"] * p["nozzles"]),
        "nozzle_type": ",".join(["hardened_steel"] * p["nozzles"]),
    }
    if colours > 1:
        overrides["enable_prime_tower"] = "1"
    if remap:
        overrides["remap_filament_extruder"] = remap
    slice_case(name, path, printer, overrides=overrides, filaments=colours)


def main():
    tree = {"enable_support": "1", "support_type": "tree(auto)", "support_on_build_plate_only": "0"}
    normal = {"enable_support": "1", "support_type": "normal(auto)"}

    slice_case("badge_ender3", HANDY / "OrcaBadge.3mf", "ender3")
    for printer in PRINTERS:
        slice_case(f"cube_{printer}", MODELS["cube"], printer)
    slice_case("tshape_ender3_tree", MODELS["tshape"], "ender3", overrides=tree)
    slice_case("tshape_ender3_normal", MODELS["tshape"], "ender3", overrides=normal)
    slice_case("tshape_ir3belt_tree", MODELS["tshape"], "ir3belt", overrides=tree)
    slice_case("tshape_ir3belt_tree_keep", MODELS["tshape"], "ir3belt", overrides=tree, arrange=False, keep_positions=True)
    slice_case("tshape_ir3belt_shift", MODELS["tshape"], "ir3belt", overrides=tree, belt_shift_y=25.0)
    slice_case("gap_ender3", MODELS["gap"], "ender3")
    slice_case("gap_ir3belt", MODELS["gap"], "ir3belt")
    slice_case("gap_ir3belt_tree", MODELS["gap"], "ir3belt", overrides=tree)
    slice_case("benchy_ender3", HANDY / "3DBenchy.drc", "ender3")

    project_case("proj_u1_painted4", printer="u1", colours=4, authored_on="x1c", style="painted")
    project_case("proj_u1_objects2_remap", printer="u1", colours=2, authored_on="u1", style="objects", remap="1:3,2:4")
    project_case("proj_ender3_painted1", printer="ender3", colours=1, authored_on="x1c", style="painted")
    project_case("proj_ir3belt_objects1", printer="ir3belt", colours=1, authored_on="x1c", style="objects")

    p = PRINTERS["ender3"]
    ender = {"printer_profile": p["printer"], "process_profile": process_for(p)}
    convert_case("convert_plain", MODELS["cube"])
    convert_case("convert_arrange", MODELS["tshape"], arrange=True, **ender)
    convert_case("convert_orient", MODELS["tshape"], orient=True, **ender)
    convert_case("convert_rotx", MODELS["tshape"], transforms=[Step("rotate_x", degrees=45)])
    convert_case("convert_rotz", MODELS["tshape"], transforms=[Step("rotate_z", degrees=30)])
    convert_case("convert_layflat", MODELS["tshape"], transforms=[Step("lay_flat")])
    convert_case("convert_normal", MODELS["tshape"], transforms=[Step("face_normal", normal=(0.0, 1.0, 0.0))])
    convert_case("convert_badge", HANDY / "OrcaBadge.3mf", arrange=True, **ender)

    if wanted("help_json"):
        help_json = cli_runner.fetch_help_json()
        (OUT / "help.json").write_text(json.dumps(sorted(help_json or [], key=lambda i: i["key"]), indent=1, sort_keys=True))
        summary["help_json"] = {"kind": "help", "options": len(help_json or [])}
        print(f"help_json: {len(help_json or [])} options", flush=True)

    out = OUT / "summary.json"
    merged = json.loads(out.read_text()) if ONLY and out.exists() else {}
    merged.update(summary)
    out.write_text(json.dumps(merged, indent=1, sort_keys=True))
    shutil.rmtree(WORK, ignore_errors=True)


class Step:
    """The few fields transform_args reads (the API's TransformStep, without importing the router)."""

    def __init__(self, op, degrees=0.0, normal=None):
        self.op, self.degrees, self.normal = op, degrees, normal


if __name__ == "__main__":
    main()
