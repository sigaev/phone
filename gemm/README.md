# GEMM benchmark

A Vulkan compute GEMM benchmark for this phone's GPU, an Arm Mali-G715 MC7 in
Google Tensor G3, at every precision the GPU supports: fp32, fp16 with fp16 or
fp32 accumulation, and int8 with int32 accumulation. It computes `C = A B^T`,
where A is M x K and B is N x K, both K-contiguous like a linear layer's
activations and weights, and C is M x N; batched shapes model mixture-of-experts
layers. Every measured result is verified, reached at a confirmed steady state,
and reported against independent hardware ceilings.

cuBLAS runs only on NVIDIA GPUs with CUDA, so it cannot serve as a baseline on
this phone; kernels are judged against Arm's published peak rates instead.

```sh
bazel run //gemm -- info            # device, cores, cooperative-matrix shapes
bazel run //gemm -- peak 0.7        # arithmetic ceilings per precision
bazel run //gemm -- loads 0.7       # load and texture throughput by access pattern
bazel run //gemm -- bandwidth 0.7   # memory bandwidth by working-set size
bazel run //gemm -- dispatch 0.7    # the fixed cost of a dispatch
bazel run //gemm -- layouts         # cooperative-matrix lane layouts
bazel run //gemm -- kernels         # compiler statistics for every kernel
bazel run //gemm -- matrix 0.7      # every shape and precision, steady state
bazel run //gemm -- gemm 0.7 fp32 "square 2048"      # one shape, every kernel
bazel run //gemm -- quick fp32 "square 2048"         # the same, fixed-length runs
bazel run //gemm -- ablate 0.7 "square 2048"         # what limits each kernel
bazel run //gemm -- soak 0.75 int8 "square 2048" "mma 6x1 2x16, 1 A tile textured" 900
bazel test //gemm:gemm_test         # every kernel on edge-case shapes
```

The number after a mode is its cooling target: before measuring, the benchmark
waits until Android reports no thermal throttling and its thermal headroom is at
most that value (1 means severe throttling). `GEMM_DESCRIBE=<label>` makes
`kernels` print everything the driver reports for matching kernels, including
thread-local storage, and `GEMM_WINDOWS=1` makes `gemm` print every measured
window. `GEMM_SPLIT=<slices>` and `GEMM_RASTER=<block rows>` override how the
tiled kernels are launched (see Kernels), for experiments.

## Ceilings

Arm publishes 256 fp32 and 512 fp16 operations per shader core per clock for
Mali-G715; the driver's `VK_ARM_shader_core_properties` reports 128 fp32 FMAs
per core per clock and 7 cores, and the GPU runs at up to 890 MHz. That gives
1.595 TFLOPS fp32 and 3.190 TFLOPS fp16. Arm publishes no int8 rate. `gemm peak`
measures what instruction streams without memory traffic reach, in operations
per core per clock at a steady 890 MHz:

| Instruction stream | Measured | Share of published |
|---|---:|---:|
| fp32 FMA | 242.5 | 95% of 256 |
| fp16 `f16vec2` FMA (fp16 accumulation) | 482.7 | 94% of 512 |
| int8 packed dot product | 504 | |
| fp32 4x4x4 cooperative matrix | 246 | 96% of 256 |
| fp16 4x8x8 cooperative matrix (fp32 accumulation) | 250 | |
| int8 4x16x16 cooperative matrix (int32 accumulation) | 944 | |

The matrix multiplier's int8 rate is close to four times its fp32 rate, about
1024 operations per core per clock, or 6.4 TOPS. The benchmark reports each GEMM
against the measured ceiling at the clock it ran at (`%ceil`), against the
published peak at 890 MHz (`%pub`), and against its roofline (`%roof`), the
lower of the compute ceiling and its minimum memory traffic at the measured
DRAM bandwidth.

These rates need loops with constant trip counts. The driver's compiler handles
loops bounded by runtime values poorly: the same kernels with a push-constant
trip count reach about half these rates, for arithmetic and loads alike. The
GEMM kernels therefore take M, N, and K as specialization constants, and the
benchmark compiles a pipeline per shape.

## What limits a GEMM here

`gemm loads`, `gemm peak`, `gemm layouts`, `gemm ablate`, and the compiler
statistics that the driver reports through `VK_KHR_pipeline_executable_properties`
(printed by `gemm kernels`) shaped the kernels. Rates are per core per clock.

- **Load/store unit.** It serves four lanes per clock for 8- and 16-byte loads
  and eight for 4-byte ones, and touches at most one 64-byte cache line per
  clock: consecutive 16-byte loads read 61 bytes per clock, 8-byte loads 31, and
  16-byte loads that give each lane its own row 15. Loads of one address by the
  whole subgroup read 105, shared memory 83. `coopMatLoad` reads element by
  element, at 15 bytes per clock, from global and shared memory alike.
- **Texture unit.** It fetches four texels per clock, in parallel with the
  load/store unit, whatever the lanes' addresses: 63 bytes per clock for 16-byte
  texels, 32 for 8-byte, 16 for 4-byte, and 62 when every lane reads its own
  image row. Uniform texel buffers go through the load/store unit instead. A
  linear-tiled image can alias a buffer's memory, so the kernels sample B, and
  optionally part of A, straight from their row-major buffers with no copy.
  Linear images need 64-byte row pitches and at most 65,536 rows; taller B
  matrices put several consecutive rows in each image row.
- **Cross-lane moves.** A subgroup shuffle or broadcast costs about as much as
  four or five FMAs: fp16 FMAs interleaved one-to-one with shuffles, broadcasts,
  or quad broadcasts issue 44 lane-instructions per clock, against 121 for FMAs
  alone. Sharing operands between lanes costs more than loading them again.
- **Matrix layouts.** The driver's element-to-lane layouts are fixed (`gemm
  layouts`) and not part of the Vulkan specification, so `gemm_test` and every
  benchmark run verify them. K is summed over, so the kernels permute it within
  each block of K, the same way for A and B, so that each lane's elements for
  several matrix steps arrive in one vector load, then assign the elements.
- **Order.** The compiler does not reorder dependent matrix multiplies: eight
  int8 accumulators each updated four times in a row run at 40% of the rate of
  the same multiplies interleaved. Kernels make the K steps the outer loop.
- **Registers.** A thread has at most 64 registers, and above 32 a core holds
  half as many threads. Tiles large enough to use the load units well need 49 to
  64, which leaves no room to load the next block of K while computing the
  current one, so load latency is exposed. The compiler also schedules eagerly:
  given 16-byte int8 vectors covering four matrix steps, it builds all four
  steps' matrices ahead of the multiplies and spills to thread-local memory,
  which `GEMM_DESCRIBE` shows, so the int8 kernel loads 8 bytes per lane.
- **Loads, not arithmetic.** `gemm ablate` times each kernel with its loads
  replaced by arithmetic, with its multiplies removed, and with every load
  hitting L1. Without loads the kernels reach 80-90% of their ceilings; with
  loads served from L1 they are within a few points of the real kernels, so the
  cost is load issue and latency, not cache or DRAM traffic, except for long
  rows of K.
- **Long rows of K.** With K = 14,336 (Llama 3 8B's down projection) the fp32
  kernel ran at 16% of its ceiling, and three times faster with every load
  served from L1: its rows of A are 56 KiB, so the eight block rows that
  workgroups sweep together span 14.7 MiB of A, more than the system cache, and
  each block column reads them again from DRAM. Sweeping four block rows doubled
  it. Splitting K alone did not help there, but it does where the grid is small:
  256 x 256 outputs over K = 16,384 make only 56 workgroups, too few to hide
  load latency, and eight slices raised fp32 from 0.47 to 0.64 TFLOPS, fp16
  from 0.60 to 0.86, and int8 from 1.8 to 2.4 TOPS.
- **Memory.** The GPU reads DRAM at about 33 GB/s once working sets exceed the
  system cache, half of LPDDR5X's 68.2 GB/s; eight CPU threads copy at about 31
  GB/s (read plus write), so the memory system, not the GPU, sets this limit.
  Working sets up to about 16 MiB are read faster, from the system cache.
- **Dispatch.** A dispatch and the barrier after it cost about 5.3 µs, plus
  about 3 ns per workgroup, which matters for the smallest shapes.

## Kernels

- `gemm_int8.comp`: int8 on 4x16x16 cooperative matrices. Per two matrix steps,
  each lane loads 8 bytes of every A row tile through the load/store unit, or
  through the texture unit for the first `A_TEXTURE` tiles, and 8 bytes of each
  of four B rows through the texture unit; each subgroup keeps WM x WN
  accumulators. Wide workgroups (2 x 16 subgroups) share A rows in L1.
- `gemm_mma4.comp`: fp32, and fp16 converted to fp32, on 4x4x4 cooperative
  matrices, with one element per lane per matrix; 16-byte loads carry four fp32
  or eight fp16 steps. Like the int8 kernel, it can read its first `A_TEXTURE`
  row tiles of A through the texture unit, which helps fp32 with long rows of K.
  fp16 with textured A needs more registers than a thread has, spills, and
  miscompiled in one variant, so only fp32 uses it.
- `gemm_mma8.comp`: fp16 on the native 4x8x8 cooperative matrices with fp32
  accumulation, with a quarter of the multiply instructions and none of the
  conversions of `gemm_mma4.comp`.
- `gemm_hfma.comp`: fp16 with fp16 accumulation on packed `f16vec2` FMAs, each
  lane computing a TM x TN tile of C. Its loads of A limit it well below the FMA
  rate, and it draws enough power to throttle the GPU clock within seconds.
- `gemv.comp`: small M, bound by streaming B: lanes read 16-byte chunks of B rows
  and a subgroup sum finishes each dot product.

The fp16 matrix kernels also serve the fp16 cells, which ask for fp16 outputs
with fp16 accumulation: fp32 accumulation is at least as accurate, and the
matrix multiplier offers fp16 inputs only with fp32 accumulators, at the fp32
rate. The fp16 FMA rate is twice that, but no kernel here can feed it: each
lane's tile would need about 64 outputs, more registers than a thread has.

The tiled kernels dispatch workgroups in one dimension and raster them over C a
few block rows at a time (`raster.glsl`), so workgroups running together share
panels of A and B in the caches; eight rows instead of one raised fp32 at 2048
cubed by about a quarter. They can also split K into slices: each workgroup
then sums one slice into 32-bit partial sums, one copy of C per slice, and
`reduce.comp` adds the copies in slice order, so results stay identical from
run to run. By default (`choose_launch` in `bench.cc`), K is split in halves
until the grid has 1,536 subgroups, keeping slices of at least 1,024, and the
raster narrows from eight block rows until the rows of A it sweeps, over one
slice, fit in 8 MiB. Both rules come from sweeps with `GEMM_SPLIT` and
`GEMM_RASTER` on the long-K shapes; splitting K is a close call on the skinny
shapes, so `matrix` also screens every kernel unsplit where its default splits.

`kernels.bzl` compiles every variant and generates the tables `bench.cc` reads;
edit the lists in `BUILD.bazel` to add one.

## Methodology

- **Steady state.** A measurement keeps two command buffers in flight, so the
  GPU never idles between them, each at least 100 ms of GPU time and four
  repeats. Windows are timed from one batch's end timestamp to the next's, which
  tile the GPU's time exactly; a batch's start timestamp can be written after its
  work has begun. A result is steady once, over the last eight windows, the
  least-squares trend and the difference between the halves' medians are both
  under 1%, the windows lie within 5% of each other, the GPU clock sampled from
  `/sys/devices/platform/1f000000.mali/cur_freq` varies by at most 1%, and at
  least a second of load has passed; the median window is reported. Otherwise the
  result is marked unsteady, and the matrix ignores it. Single long GEMMs vary by
  a few percent from run to run, so steady state is judged on drift, not on the
  windows' range.
- **Cache-cold inputs.** Repeated GEMMs rotate through copies of the operands
  until they read at least 64 MiB, beyond the GPU's L2 and the system cache,
  like consecutive layers of a model.
- **Correct results.** Each run first computes a GEMM on sparse -1, 0, and 1
  data, for which every partial sum is a small integer and even fp16 accumulation
  is exact in any order. The measured runs use full-precision random data. The
  timed output is compared with double-precision references: sampled elements
  with probabilistic rounding-error bounds for the accumulator and output
  precision, and randomized checksums of every row and column, which expose any
  wrong element; every rotating copy must produce identical output.
- **Thermal state.** Every shape in `matrix`, and every kernel in `gemm`, starts
  after cooling to the same headroom; throttling can slow memory before it
  lowers the GPU clock, so runs start at thermal status 0.
- **Choosing a kernel.** For each cell, `matrix` screens every kernel with a
  verified fixed-length run (twice where its default launch splits K: split and
  unsplit), measures the three fastest to steady state, each after cooling,
  measures the fastest twice more, and reports the median of the three with
  their spread, since the fastest of several runs is biased toward a lucky one.
- **Iterating.** `quick` runs each kernel for a fixed 0.4 s and judges it per
  clock (`%ceil` uses the clock sampled during the run), after cooling to status
  0, which ranks kernels in seconds rather than minutes. Final numbers come from
  steady-state runs.
- **Throttling.** `soak` runs one GEMM continuously, logging every window's
  throughput and clock and Android's thermal status and headroom once a second.
  It reports when throughput first falls below 97% of its unthrottled rate, when
  the GPU clock is first lowered, and the throttled steady state: the earliest
  time after which every 60-second mean stays within 3% of the final two
  minutes' mean, confirmed for three more minutes, with the range of 5-second
  medians around it. The output is verified at the end.

## Results

`gemm matrix 0.75` on 2026-09-29: every cell's kernel verified, steady, and the
median of three runs, each starting at thermal status 0 with headroom at most
0.75 and running at 890 MHz. Rates are TFLOPS (TOPS for int8) with the share of
each cell's roofline; the fp16 cells are judged against the fp16 FMA rate,
twice what their fp32-accumulating kernels can reach (see Kernels).

| Shape | fp32 TFLOPS (%roof) | fp16 TFLOPS (%roof) | fp16/fp32acc TFLOPS (%roof) | int8 TOPS (%roof) |
|---|---:|---:|---:|---:|
| square 256 | 0.599 (42%) | 0.639 (23%) | 0.638 (41%) | 1.357 (48%) |
| square 512 | 0.952 (62%) | 0.809 (27%) | 0.796 (51%) | 2.035 (36%) |
| square 1024 | 0.995 (65%) | 0.974 (32%) | 0.971 (62%) | 2.800 (48%) |
| square 2048 | 1.041 (68%) | 1.017 (34%) | 1.018 (65%) | 3.279 (56%) |
| square 4096 | 0.945 (62%) | 1.012 (34%) | 1.019 (65%) | 3.307 (56%) |
| llama3-8b decode qkv | 0.015 (89%) | 0.029 (89%) | 0.029 (89%) | 0.059 (90%) |
| llama3-8b decode gate+up | 0.015 (89%) | 0.029 (89%) | 0.030 (89%) | 0.060 (90%) |
| llama3-8b decode down | 0.016 (97%) | 0.031 (92%) | 0.030 (92%) | 0.058 (88%) |
| llama3-8b prefill qkv | 1.010 (66%) | 1.009 (34%) | 1.011 (65%) | 3.095 (53%) |
| llama3-8b prefill gate+up | 0.929 (61%) | 1.016 (34%) | 1.021 (66%) | 3.263 (56%) |
| llama3-8b prefill down | 0.605 (40%) | 0.965 (32%) | 0.966 (62%) | 2.658 (45%) |
| qwen3-30b-a3b decode gate+up | 0.015 (89%) | 0.029 (88%) | 0.030 (90%) | 0.059 (90%) |
| qwen3-30b-a3b decode down | 0.015 (89%) | 0.029 (89%) | 0.029 (89%) | 0.058 (88%) |
| qwen3-30b-a3b prefill gate+up | 0.446 (88%) | 0.843 (83%) | 0.848 (83%) | 1.629 (84%) |
| qwen3-30b-a3b prefill down | 0.442 (88%) | 0.822 (82%) | 0.866 (87%) | 1.611 (90%) |
| gpt-oss-20b decode gate+up | 0.014 (85%) | 0.030 (90%) | 0.030 (90%) | 0.059 (90%) |
| gpt-oss-20b decode down | 0.015 (90%) | 0.029 (88%) | 0.029 (89%) | 0.058 (88%) |
| gpt-oss-20b prefill gate+up | 0.829 (81%) | 0.970 (48%) | 0.970 (62%) | 2.520 (66%) |
| gpt-oss-20b prefill down | 0.824 (82%) | 0.956 (47%) | 0.957 (61%) | 2.328 (61%) |
| deepseek-v2-lite prefill gate+up | 0.649 (85%) | 0.787 (52%) | 0.783 (52%) | 2.067 (72%) |
| deepseek-v2-lite prefill down | 0.641 (86%) | 0.941 (63%) | 0.849 (57%) | 2.153 (79%) |
| mixtral-8x7b decode gate+up | 0.015 (90%) | 0.030 (92%) | 0.030 (90%) | 0.059 (90%) |
| mixtral-8x7b decode down | 0.016 (96%) | 0.032 (98%) | 0.032 (97%) | 0.063 (95%) |
| mixtral-8x7b prefill gate+up | 0.964 (63%) | 1.017 (34%) | 1.017 (65%) | 3.124 (53%) |
| mixtral-8x7b prefill down | 0.626 (41%) | 0.918 (30%) | 0.917 (59%) | 2.637 (45%) |
| tall 4096x64x4096 | 0.591 (58%) | 0.879 (43%) | 0.881 (57%) | 2.182 (56%) |
| wide 64x4096x4096 | 0.862 (84%) | 0.875 (43%) | 0.875 (56%) | 2.376 (61%) |
| deep 256x256x16384 | 0.652 (43%) | 0.854 (28%) | 0.856 (55%) | 2.373 (40%) |
| unaligned 777x1111x1024 | 0.943 (62%) | 0.941 (31%) | 0.936 (60%) | 2.678 (46%) |

- **Compute-bound peaks.** fp32 reaches 1.04 TFLOPS at 2048 cubed, 68% of the
  measured matrix ceiling and 65% of Arm's published 1.595 TFLOPS; fp16 with
  fp32 accumulation 1.02 TFLOPS (65%); int8 3.31 TOPS at 4096 cubed (56% of the
  measured 5.9 TOPS). Dense prefill layers land within a few points of these.
- **Memory-bound cells.** Every decode GEMV streams B at 28-32 GB/s, 85-98% of
  the measured DRAM bandwidth, at every precision. The Qwen3 mixture-of-experts
  prefill batches, with many small experts, run at 82-90% of their roofline.
- **Weakest cells.** fp32 with long rows of K (Llama 3 and Mixtral down
  projections, 256 x 256 x 16384) stays near 40% after the raster and split-K
  changes, and the smallest shapes are limited by too few workgroups and the
  cost of a dispatch.
- **Spread.** The median's three runs agree within 5% except in four cells:
  gpt-oss decode gate+up fp32 (8%), square 1024 fp32 (12%), and Qwen3 prefill
  gate+up int8 and down fp32, each with one run 54-60% slower than the others.

`gemm soak 0.75` ran each precision's best square-2048 kernel for 15 minutes,
starting warm (headroom 0.73-0.75), logging a 0.25-second window every five
seconds. Every output was verified at the end.

| Kernel | Unthrottled | First slower | Clock first lowered | Final two minutes |
|---|---:|---:|---:|---:|
| int8 `mma 6x1 2x16, 1 A tile textured` | 3.28 TOPS | 15 s | 20 s (850 MHz) | 2.95 TOPS, 90% |
| fp32 `mma4 4x5 4x4 16B` | 1.04 TFLOPS | 30 s | 30 s (850 MHz) | 0.85 TFLOPS, 82% |
| fp16/fp32acc `mma8 8x1 2x2` | 1.02 TFLOPS | 26 s | 36 s (850 MHz) | 0.96 TFLOPS, 95% |

From this warm start, throttling begins within 15-30 seconds of sustained load.
It does not settle into one throttled rate: Android's thermal status moves
between 0 and 2, the GPU clock between 649 and 890 MHz, and single windows also
run up to 43% slower with the GPU clock unchanged. Only the fp16 soak met the
steady-state criterion, from 688 seconds; the int8 and fp32 soaks were still
oscillating after 15 minutes. An earlier soak of a previous int8 kernel first
slowed at about 40 seconds, still at full clock, first lowered the clock at
about 320 seconds, and ended at about 74% of its unthrottled rate.

## CPU for contrast

The same Tensor G3's CPU has four Cortex-A510 cores at up to 1.70 GHz, four
Cortex-A715 at 2.37 GHz, and one Cortex-X3 at 2.91 GHz. All nine support SVE2,
but with 128-bit vectors, the same width as NEON, so SVE2 and NEON instructions
ran at the same rates, apart from the X3's throttling (below). The table shows
instruction streams without memory traffic, like `gemm peak`: 28 independent
accumulators per core, with the NEON forms where both exist. Rates are TFLOPS
(TOPS for int8), and the GPU column is the matching `gemm peak` ceiling at
890 MHz:

| Instruction stream | A510 x4 | A715 x4 | X3 | All 9 cores | GPU ceiling |
|---|---:|---:|---:|---:|---:|
| fp64 FMA | 0.027 | 0.075 | 0.031 | 0.127 | |
| fp32 FMA | 0.049 | 0.151 | 0.055 | 0.254 | 1.511 |
| fp16 FMA | 0.098 | 0.301 | 0.123 | 0.479 | 3.007 |
| bf16 `BFMMLA` (fp32 accumulation) | 0.037 | 0.603 | 0.281 | 0.839 | |
| int8 `SDOT` (int32 accumulation) | 0.196 | 0.603 | 0.234 | 0.969 | 3.140 |
| int8 `SMMLA` (int32 accumulation) | 0.397 | 1.204 | 0.497 | 2.082 | 5.881 |

- **Per core.** Each A715 issues two 128-bit FMAs per clock, and each pair of
  A510s shares one vector unit that does the same, so four A510s reach only
  twice the rate of one. The X3 has four FMA pipes and reaches four per clock in
  bursts, but it throttles its vector issue rate: in alternating windows of
  about 10 ms it issues four or two per clock, with its clock steady at
  2.91 GHz. Over the 1.5-second measurements that averaged 2.3 to 2.7 per
  clock, 0.055 TFLOPS fp32 with NEON and 0.063 with SVE2, where four per clock
  would give 0.093.
- **Against the GPU.** The GPU's ceilings are six times the whole CPU's for
  fp32 and fp16 FMAs, and 2.8 times its `SMMLA` rate. The GPU's best GEMMs,
  1.04 TFLOPS fp32 and 3.31 TOPS int8, are 4.1 and 1.6 times the CPU's
  ceilings. The GPU has no bf16. The CPU's `BFMMLA` ceiling, 0.83 TFLOPS, is
  82% of the GPU's best fp16 GEMM with fp32 accumulation; no CPU GEMM was
  measured.
- **Decode.** The decode GEMVs run at 0.015 TFLOPS fp32 and 0.06 TOPS int8
  while streaming DRAM at 28-32 GB/s, far below every CPU ceiling above, so for
  them the comparison is memory bandwidth, not arithmetic.

Read bandwidth by working-set size, in GB/s: the GPU's `read ... repeatedly`
rows from `gemm bandwidth 0.7`, steady at 890 MHz, and the CPU reading the same
sizes, split into equal slices, one per thread:

| Working set | GPU | X3 | A715 | A510 | A715 x4 | All 9 cores |
|---|---:|---:|---:|---:|---:|---:|
| 256 MiB | 32.7 | 19.6 | 22.0 | 8.5 | 30.7 | 31.7 |
| 128 MiB | 31.8 | 21.3 | 21.8 | 8.9 | 30.3 | 31.0 |
| 64 MiB | 31.9 | 21.4 | 22.1 | 8.3 | 30.4 | 30.3 |
| 32 MiB | 65.5 | 23.8 | 22.5 | 9.1 | 25.2 | 34.4 |
| 16 MiB | 129.1 | 30.3 | 28.4 | 9.1 | 34.6 | 77.2 |
| 8 MiB | 151.2 | 31.4 | 26.0 | 9.5 | 81.3 | 149.8 |
| 4 MiB | 165.2 | 31.5 | 23.2 | 10.7 | 84.2 | 192.3 |
| 1 MiB | 53.9 | 59.1 | 21.3 | 10.8 | 126.0 | 176.8 |
| 256 KiB | 171.7 | 74.7 | 27.6 | 11.4 | 185.4 | 406.2 |
| 64 KiB | 250.2 | 83.6 | 44.1 | 13.8 | 171.3 | 397.8 |
| 16 KiB | | 109.1 | 44.3 | 41.1 | 168.9 | 350.2 |

- **DRAM.** From 64 MiB up, all nine cores read 30-32 GB/s, level with the
  GPU's 32-33. Four A715 cores already reach 30.5, while one A715 or X3 reads
  20-22 GB/s and one A510 8.5. The memory system, not either processor, sets
  this limit, and the two share it: in the same `gemm bandwidth` run, the GPU
  read 19.7 GB/s while a CPU memcpy alongside it moved 15.3 GB/s, 35 GB/s
  together.
- **System cache.** The GPU reads 16 and 32 MiB working sets at 129 and 66 GB/s,
  four and two times its DRAM rate; the CPU reads them at only 77 and 34 GB/s.
  The CPU catches up at 8 MiB and passes the GPU at 4 MiB, 192 GB/s against 165.
- **Private caches.** At 256 KiB and below, each thread's slice fits its core's
  own caches, and all nine cores together read 350-406 GB/s, 1.6 to 2.4 times
  the GPU at the same sizes. From L1, an X3 reads 109 GB/s (37 bytes per clock)
  and an A715 44 GB/s (19 bytes per clock).
- **Anomaly.** The GPU's 1 MiB result, below both of its neighbours, was steady
  but has not been investigated.

These rates come from standalone inline-assembly benchmarks, not part of this
package, run on 2026-09-29 with nothing else running. Each arithmetic
measurement ran for 1.5 seconds after a 0.3-second warm-up, with 3-second pauses
between measurements but without the cooling and steady-state checks in
Methodology. The A715 and X3 cores held their maximum clocks throughout and the
A510s ran at 1.55-1.70 GHz, so the rates are unthrottled ceilings. Android lets
only the foreground app run on the X3: in the background, `sched_setaffinity` to
cpu8 fails and threads pinned there are moved off it, so the benchmarks ran with
Termux in the foreground, and the arithmetic benchmark checks that every thread
stayed on its core and repeats any measurement where one did not. Android denies
`perf_event_open` to apps, so clocks were sampled from each core's
`scaling_cur_freq`. A chain of dependent adds, one per clock, matched those
samples on the A715 and X3 cores and came within 10% on the A510s. The read
benchmark loads 256 bytes per loop iteration with 32-byte `ldp` pair loads, and
each thread reads its slice for 1 second after a 0.2-second warm-up.
