# scripts/pseudorca

Tooling for developing Pseudorca (this fork) without breaking what [Trident](https://github.com/mugenoesis/Trident)
depends on. Trident runs this engine headless in Docker, so the question for every change is: does everything
Trident relies on come out byte-for-byte the same?

| Script | What it does |
|---|---|
| `sync-builder.sh` | Copy this checkout into an incremental-build container and verify it by checksum (the builder image can be stale against the commit under test). |
| `sync-sources.sh` | Merge OrcaSlicer `main` into each `topic/*` branch, build the integration branch and rewrite `PSEUDORCA_SOURCES` (see the README section "Keeping up with upstream"). |
| `build-candidate.sh` | `sync-builder` + incremental build + layer the binary and `resources/` onto a Trident image as a candidate. |
| `golden.sh` / `golden_cases.py` | Slice/convert a fixed set of models (plain, tree supports, belt printer, painted multi-colour projects, convert/orient/arrange...) through Trident's own `cli_runner` inside an image and record the normalised results. |
| `golden_compare.sh` / `golden_compare.py` | Compare two golden runs (G-code and 3MF content, `result.json`, progress fields, `--help-json`). |
| `unit-tests.sh`, `unit-tests-compare.sh` | Build and run the Catch2 suites in the build container; every baseline-passing test must still pass. |
| `gate.sh` | All of the above plus Trident's `api/tests_slicer`, with a markdown report. |

Everything runs in throwaway containers with scratch state. Never point these at a running Trident container.

## Typical use

```sh
# once: a build container from the headless-orca builder image
docker run -d --name pseudorca-incr --entrypoint sleep headless-orca:builder infinity

# once per baseline (the production image, built from the commit Trident pins)
export TRIDENT_API=/path/to/Trident/api
scripts/pseudorca/golden.sh mugenoesis/trident:latest baseline/golden
scripts/pseudorca/unit-tests.sh pseudorca-incr baseline/unit-tests.txt   # with the container synced to that commit

# per change
BASELINE=baseline scripts/pseudorca/gate.sh pseudorca-incr trident:candidate .
```

The golden normalisation removes only what differs between two runs of the same engine: the G-code timestamp
line, per-build object ids, `.3mf` timestamps and wall-clock timings in `result.json`.
