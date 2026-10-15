# Where the resin (MSLA) code and data come from

Pseudorca is a fork of [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) (AGPL-3.0). The resin printing support
builds on the work below. Everything is used under a licence compatible with the AGPL-3.0 that Pseudorca is released
under; the original copyright notices stay in the files that carry them.

## Code taken or ported

| Source | Licence | What we use it for |
|---|---|---|
| [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) 2.9.6, Prusa Research | AGPL-3.0 | The SLA subsystem: supports, hollowing, pad, raster/slice pipeline, the SL1 and Anycubic archive writers, `SLAPrint`, the SLA configuration options, the sla-time-estimates library, marching squares (an SLA private copy). Ported by file, with the Prusa copyright headers kept. |
| [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) and its contributors | AGPL-3.0 | Everything else: the slicer core, CLI and build that Pseudorca is based on. Belt printing comes from the upstream pull request that OrcaSlicer merged. |

## Format knowledge (the files are written from these specifications, the code is our own)

| Format | Reference | Licence of the reference |
|---|---|---|
| CTB version 4, the GKtwo variant, CBDDLP / Photon version 2, encrypted CTB version 5, GOO, the Phrozen zip, the layer cipher and the run-length codecs | [UVtools](https://github.com/sn4k3/UVtools) by Tiago Conceição (`ChituboxFile.cs`, `CTBEncryptedFile.cs`, `CtbRleCodec.cs`, `BitPlaneRle.cs`, `GooFile.cs`, `ChituboxZipFile.cs`) and the UVtools contributors' reverse engineering | AGPL-3.0 |
| CTB versions 4 and 5, GOO | [mslicer](https://github.com/connorslade/mslicer) by Connor Slade: used to cross-check the layouts, and as an independent reader that parses our files in testing; the AES key and IV of the encrypted layout are listed there and in UVtools | GPL-3.0 |
| CBDDLP / CTB layout notes | [catibo](https://github.com/cbiffle/catibo) by Cliff Biffle | BSD-2-Clause |
| GOO | the format description by [Elegoo](https://github.com/elegooofficial/GOO) | GPL-3.0 |
| TinyMaker image folder | the firmware of the [TinyMaker open source printer](https://github.com/TinyMaker3D/TinyMaker-Open-Source-3D-Printer) was read to learn how it takes its jobs (a folder of numbered PNGs); no code or files from it are included | the repository states no licence |

`tests/data/ctb/ref_v4_mslicer.ctb` is a file written by mslicer, kept as a test reference.

## Data

The resin printer catalogue in `resources/resin` is converted from UVtools' published PrusaSlicer printer profiles
(`PrusaSlicer/printer/*.ini` in the UVtools repository, AGPL-3.0) by `scripts/pseudorca/resin-catalogue-from-uvtools.pl`:
display sizes and resolutions, build heights, default lift, speed and wait values, and the file format each printer reads.
The TinyMaker entry was measured by its owner. None of the printers is verified on hardware unless a note says so.

### Resin materials

`resources/resin/materials` holds the exposure settings Siraya Tech publishes for its resins in its user guides (https://siraya.tech/pages/...-user-guide); they are the maker's recommended values, entered with the link to their page and the date they were read (`scripts/pseudorca/resin-materials-siraya.pl`). Two test fixtures come from open sources: a Chitubox resin profile from [Photonsters/CBD-based-printers-list](https://github.com/Photonsters/CBD-based-printers-list) (The Unlicense) and a section of PrusaSlicer's `AnycubicSLA.ini` (AGPL-3.0); see `tests/data/resin/README.md`.

## Trademarks

Chitubox, CBDDLP and CTB are formats of CBD Technology; the names of the printers, vendors and formats belong to their
owners and are used only to say what a file is compatible with.
