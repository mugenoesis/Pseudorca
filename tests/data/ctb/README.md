`ref_v4_mslicer.ctb` is a CTB v4 file written by mslicer's `ctb_format` crate (GPL-3.0, connorslade/mslicer), with
synthetic content: three 8 x 4 pixel layers and an 8 x 8 preview. It is an independent writer's output, used to check
the reader, the run length codec and the layer cipher of `sla_ctb_tests.cpp`.

Layer 0: 10 x black, 6 x white (255), 4 x gray (128), 12 x black. Layer 1: 32 x white. Layer 2: white, black, white,
29 x black. Layer heights 0.05, 0.10, 0.15 mm; exposure 2.5 s; light off delay 0.5 s; lift 6 mm at 65 mm/min; retract
150 mm/min; layer cipher key 103 (0x67).
