# Streaming probes

For the hardware two-player gate, `run_tv_split_probe.py --device tv --fps 30`
cross-compiles `tv_split_probe.c` using the existing Moonlight SDK. It feeds two
synthetic H264 fixtures through the TV's installed SMP module, starts the second
after3seconds, and checks continued frame-ready callbacks for both. Use60 only
as a separate load experiment. It temporarily foregrounds a removable test app,
then removes it and relaunches Moonlight. This is video-only: it does not establish
audio mixing, real display FPS, controller isolation, or two-PC gameplay. No new
downloads, game access, privileges or persistent decoder setting changes are used.
The receipt and prior attempt summaries are ignored under`code-review/tv-test/`.

The host-only programs below use the real GPU with synthetic frames. They do not emulate the
LG decoder, capture a game, or measure input-to-photon latency. Nothing is installed
as a service. Individual experiments use hard deadlines and clean up their temporary files and processes.

Game-process access is excluded: use normal capture outputs or owned synthetic
producers. Do not inject into games, add game render hooks/layers, read game memory,
or obtain engine-only depth/motion data. Anti-cheat enforcement stays intact.
Diagnostic shims and process-targeted capture probes apply only to owned test
programs. These boundaries do not constitute a vendor anti-cheat certification.

Prepare Sunshine `019b1ba661d829e334706557a3e2bdc3d2374591`, apply
patches `0001` through `0005` in order, and initialize its nanors submodule. Then run
from the repository root:

```sh
python host/tools/run_offline_benches.py --sunshine-source /path/to/patched/Sunshine
```

The runner downloads nothing, uses an automatically cleaned `/tmp` build directory,
kills its process group on timeout, and retains ignored logs under
`code-review/engine-bench/`. It needs installed CUDA 13.4, GCC14, Clang, FFmpeg
development libraries and the RTX4090. It links system FFmpeg; the original pinned
host-library measurements are separately identified in `latency-validation.json`.
Do not mix their results without identifying the library/context configuration.

* `nvenc_latency.cpp`: P1/ULL/CBR23Mb/s, no B frames/lookahead, one surface,120FPS;
  20 warm-up +100 timed frames per case, matching output PTS required. The default
  runs HEVC/AV1,1080p/4K,quarter-resolution/single-pass,auto/forced split in two
  reversed rounds. Current code uses a private CUDA context and fences the input
  update before encode timing. It reports source readiness and combined time
  separately. The readiness fence is diagnostic, not a Sunshine optimization.
  `--contention-probe` restricts the matrix to1080p baseline/experimental ABBA.
  `--stream-probe` tests legacy, host-style blocking, nonblocking and high-priority
  I/O streams. `--write-streams EXISTING_DIRECTORY` writes decoder-validation
  fixtures; use a temporary directory and remove them after validation.
* `gpu_conversion.cu`: CUDA-only separate passes/copies versus fused NV12 output,
  with full output equality and independent CPU sample checks. It omits EGL/KMS,
  cursor, HDR and real game load. `--contention-seconds N` instead runs a separate
  context with approximately4ms compute kernels, bounded to1–20seconds. Wait for
  its ready output before a loaded probe; verify it remains alive throughout,
  then terminate/reap it. This is synthetic interference, not game simulation.
* `fec_latency.cpp`: fresh versus cached Reed–Solomon matrices, setup-only and full
  parity timings, bounded sender-owned cache. Large setup-only percentage gains
  are not full FEC or host latency gains.
* `nvenc_api_trace.cpp`: benchmark-only shim, one serial encoder session. CPU wall
  timing of actual map/submit/bitstream-lock/unlock/unmap calls, not GPU hardware
  execution time. Samples include warm-up and initialization where applicable.
  Link with `-Wl,-z,nodelete` and SONAME `libnvidia-encode.so.1`; otherwise FFmpeg
  unload/reload cycles can bypass the shim after the first session. Build against
  the pinned `third-party/build-deps/third-party/FFmpeg/nv-codec-headers/include`.
  Use a private TemporaryDirectory and set `LD_LIBRARY_PATH` only on the probe's
  subprocess. The shim forwards `/usr/lib/libnvidia-encode.so.1` by default;
  `NVENC_REAL_LIBRARY` may select another absolute driver path. Never replace the
  system library or inject this diagnostic into the live Sunshine service.

The retained API trace predates the final input-fenced/private-context probe.
Its call attribution and the final corrected service measurements have different
boundaries. The report labels them explicitly. Current highest-priority stream
measurements showed no loaded improvement; production stream flags stay unchanged.


The follow-up `run_import_benches.py` extracts the new cache header and GTest
regression from patch0006. It downloads nothing and automatically cleans temporary
builds. Its standalone regression entrypoint exercises eight invariants under
ASan/UBSan; `dmabuf_import_probe.cpp` uses that actual header with GBM/EGL imports,
FD identity/layout validation and changed-pixel checks. It requires a usable
NVIDIA `/dev/dri/renderD128`, GBM/EGL/OpenGL development headers and Clang.
`--contention` adds the bounded CUDA load. Results are synthetic import/readback
measurements; there was no loaded completion improvement. Main README has commands.

`nvenc_gl_probe.cpp` is a standalone native-NVENC backend/ordering probe. Build
with Clang C++17, CUDA headers, the nv-codec-headers n13.0.19.0 `nvEncodeAPI.h`,
and `-lEGL -lGL -lcuda -ldl`. Use a TemporaryDirectory for headers/builds/fixtures.
Exact compile commands/header URL/checksum are recorded in experiment manifests.
Default: direct CUDA/OpenGL and granted normal/high GL priorities.
`--pipelined-probe`: explicit CPU-ready waits versus stream-ordered input.
`--interop-probe`: direct CUDA versus GL map/copy/unmap; EGL event bridge is
capability checked and skipped if absent. `--split-probe`: disabled/auto/forced
split policy with moving versus static ready input. Optional `--write-streams`
requires an existing temporary directory; remove generated streams after checks.
The prototype's I/O stream setter takes pointers to CUstream handles, as required
by NVENC. No probe library, driver flag or backend is installed in Sunshine.

New `--rgb-probe` compares native NV12 versus packed RGBA/ABGR inputs through
CUDA and OpenGL, using gray ramps and a moving gray band in reversed rounds.
It covers static ready inputs separately from ordered source updates, checks
ABGR support through the session's input-format enumeration, and labels input
format in every result. HEVC only; optional raw fixtures include input format
in their names. NV12 gray codes use limited-range luma; encoded color/range and
quality equivalence for arbitrary colors still requires decoder checks. The
mode compiled and ran on the4090;16 HEVC streams each decoded120 frames. Gray
streams were byte-identical across input formats/backends for matching motion
state. CUDA took about0.52ms idle and2.24ms under synthetic load for either input
format. OpenGL remained slower. This is not a full-host latency improvement.
Earlier measurements refer to the source hash in their individual manifests. This mode
does not model imported game RGB, cursor/scale/HDR or cross-process fences.

`--4k-policy-probe` compares default CBR, minimum CU16/CU32, and constant QP28 on
static ready3840x2160 HEVC input in reversed order. None produced a substantial
speedup beyond approximately1.66ms; these policies can change quality and bitrate.
`--resolution-probe` compares1080p/1440p/4K with automatic versus forced two-way
split encoding. It uses identical native API policy at each size, not a running
Sunshine service. Every retained fixture decoded120 frames. Forcing split helped
1080p/1440p;4K automatic/forced results were similar. Neither mode proves game or
TV image quality. Compile/header setup remains the same as the other native modes.

`--4k-array-probe` compares a mapped GL array copied into linear memory with
direct array registration. It preserves map ownership through completed encoding
and unregisters before returning the surface to GL. Per-frame registration costs
made its timing mixed. `cycle_us` includes input retirement and the optional
packet memory copy; fixture file I/O is outside that span.

`vulkan_shared_surface.h` and `shared_nv12.comp` add an optional owned Vulkan
producer using one persistent external allocation and ready/reusable semaphores.
Compile the probe with `-DSUNSHINE_VULKAN_SHARED_PROBE -lvulkan`; compile the shader
to temporary SPIR-V with `glslangValidator -V`. `--4k-shared-probe --shared-shader
PATH` compares persistent direct reads with a copy. `--4k-shared-paired-probe`
alternates the two paths within each encoder session and reverses parity next
round. These are synthetic NV12 producers, not game rendering or RGB conversion.

`--4k-native-array-probe` compares linear input, generic R8 arrays and codec-native
NV12 arrays with `CUDA_ARRAY3D_VIDEO_ENCODE_DECODE`. It requires current CUDA
headers. NV12's logical channel count is3; plane views have1/2 packed channels.
Initialization reads both native planes back and requires exact source equality.
Native output passed source checks but differed from generic output with split
encoding; its timing was worse in clean forced-split tests. With splitting
disabled all paths produced identical streams. No production backend is selected.

`--4k-native-paired-probe` alternates native/linear input inside one session;
`--pace-fps 120` spaces submissions before the measured span. This isolates real
cadence from maximum-rate submission. The loaded paced control showed no native
latency win; reference/coding decisions also depend on which input type is first.

Use the bounded runner for these experiments (installed Clang/CUDA/FFmpeg and
NumPy required; shared mode also needs Vulkan/glslang):

```sh
python host/tools/run_native_surface_bench.py
python host/tools/run_native_surface_bench.py --graphics-contention
python host/tools/run_native_surface_bench.py --kind shared --sdk-version 13.1
python host/tools/run_native_surface_bench.py --kind shared --sdk-version 13.1 --paced-120
python host/tools/run_native_surface_bench.py --kind native-paired --graphics-contention --paced-120
python host/tools/run_native_surface_bench.py --graphics-contention --split-mode disabled
python host/tools/run_native_surface_bench.py --graphics-contention --profile-nsys /opt/cuda-12.8/bin/nsys
```

The runner downloads only one checksum-pinned API header to a TemporaryDirectory,
then removes headers, builds, streams and raw profiler reports. Each run has a
120-second deadline and owns/reaps subprocess groups before cleanup. Text/JSON
receipts remain under `code-review/engine-bench/native-surface-*`. The normal host
service is not touched. `--sdk-version 13.0` tests the older API explicitly.
`--io-stream legacy` is a ready-static-input diagnostic; it is rejected for moving
producers and did not improve the loaded test. Profiling is opt-in and its timings
are separate from clean comparisons. Only referenced profiler strings are retained;
environment/process inventories are excluded.

Shared mode requires complete stream equality and checks decoded first/last
frames against the changing producer pattern. The final120FPS control decoded240
distinct moving4K frames with identical output across routes and order. It saved
about5–51us in paired rounds. `--unpaired` selects independent copy/direct sessions.
An attempted external multi-planar native mapping failed GPU address validation
and was removed from runnable tools; its unapplied diagnostic patch remains in
the ignored evidence directory. Format/export support alone did not validate that
mapping, and the failure is not proof of a driver limitation.

Nsight ranges cover100 measured frames per case. The retained traces show two
internal `Convert_PL2BL`/`Convert_BL2BL` kernels per ordinary forced-split frame,
about12–13us each at idle. Codec-native NV12 removed those kernels. Under graphics
load, ordinary kernels waited about2.2ms to start, while the native path still
waited milliseconds for completion. The installed Nsight2024.6 traced CUDA GPU
activity and NVENC CPU calls; it did not export hardware NVENC-engine intervals.
No80% host-latency reduction follows from removing these kernels alone.

`kms_metadata_probe.cpp` reads DRM plane/property metadata without framebuffer
export, modesetting or changing global DRM state. Build with Clang C++17,
`-I/usr/include/libdrm -ldrm`; optional argument selects the DRM card (default
`/dev/dri/card1`). Its property-descriptor versus value-only comparison measures
microsecond bookkeeping, not the privileged capture/readback path.

`vulkan_video_latency.cpp` uploads one static NV12 frame before measurement, then
requires 120 completed packets with matching PTS (20 warm-up, 100 timed). Build
with Clang C++17 and `pkg-config --cflags --libs libavcodec libavutil vulkan`.
Keep `vulkan_video_trace.h` alongside the source. Arguments select
`hevc_vulkan`/`av1_vulkan` and an optional temporary raw output path. Set
`SUNSHINE_VK_TRACE=1` only on this probe for bounded CPU API attribution; no new
GPU work is introduced. `SUNSHINE_VK_POLL_FENCES=1` additionally replaces blocking
fence waits with status polling and50us sleeps, returning completion only after
the real fence signals. It is diagnostic, not a production policy. Four reversed
native/polling cases remained about7.9ms, with identical120-frame HEVC outputs.
The installed FFmpeg 9.0.2 HEVC result was about7.9ms,
dominated by fence waits. HEVC output decoded120 frames; the separate AV1 CLI
smoke output failed software decoding, so AV1 is not validated. None of these
numbers include capture, conversion, TV decode or display.

`nvfbc_direct_probe.cpp` targets an explicitly supplied, owned Vulkan process PID;
optional `--gl` selects GL output instead of CUDA. Build with Clang C++17, installed
CUDA headers, a separately supplied NVIDIA NvFBC1.9 `NvFBC.h`, and
`-lGL -lcuda -ldl`. The exact tested header URL/hash and compile commands are in
the retained experiment manifests. No SDK header or downloaded executable is
vendored. Use a temporary private D-Bus daemon and set both
`DBUS_SYSTEM_BUS_ADDRESS` and `DBUS_SESSION_BUS_ADDRESS` on the owned producer and
capture subprocesses only. Wait for `nvidia.nvfbc.pid_<producer PID>` before
capture. The installed driver allowed session creation, but capture stalled in
the driver's EGL wait path. This is a failure reproducer, not a usable backend.

All three standalone tools require an external hard deadline: compile/run under
a parent that owns and reaps process groups, sets `RLIMIT_CORE=0`, and removes its
TemporaryDirectory in a `finally` block. NvFBC's internal timeout did not reliably
bound its driver wait. Retain text/JSON logs under `code-review/engine-bench/`;
remove temporary executables, SDK headers and encoded fixtures after inspection.
`test-budget.json` preserves the historical user-present allowance; it does not
cap overnight work. Retain per-experiment durations and hard process deadlines.

## Actual localhost capture/encode measurement

`run_loopback_bench.py` builds `stream_latency_client.c` against the existing
`../moonlight-tv/core/moonlight-common-c` checkout with installed Clang, CMake,
Ninja and OpenSSL. No dependencies are downloaded. It starts a separate instance
of the supplied Sunshine executable on a free loopback port block, with absolute
temporary config/log/state/key/app paths and an owned preprovisioned test-client
certificate. The normal service, its pairing/config and its API are not used.
The default host binary already has KMS capabilities; another binary needs its
own capabilities for KMS. This runner does not grant them or modify service files.

```sh
python host/tools/run_loopback_bench.py --twopass disabled --resolution 3840x2160
python host/tools/run_loopback_bench.py --twopass disabled --resolution 2560x1440
python host/tools/run_loopback_bench.py --codec av1 --twopass disabled
python host/tools/run_loopback_bench.py --twopass disabled --contention
python host/tools/run_loopback_bench.py --twopass disabled --graphics-contention
```

The source is the currently selected physical4K DP-2 desktop; it does not change
display modes or launch/focus a game. The receiver requests120FPS and23Mb/s,
collects the unchanged wire host-processing counter after20 warm-up frames,
checks frame-number gaps, writes the complete compressed stream temporarily, and
requires software decoding of every received frame. It drops audio and sends no
input; no client presentation or TV decoder timing is measured. Reported elapsed
seconds include connection teardown, so do not derive a steady-state FPS from it.
Different stream resolutions are different quality/bitrate tradeoffs.

`--contention` uses the bounded separate-context CUDA kernel. `--graphics-contention`
uses `gpu_graphics_load.cpp`, an offscreen fragment shader calibrated toward4ms
per draw with real GPU elapsed queries. The manifest records actual calibration;
do not treat requested duration as measured work. The retained matched graphics
cases calibrated near4ms; a standalone check also validates the calibration.
Neither workload simulates a game.

`nvml_encoder_sample.py` reads only the explicitly selected test server PID's
encoder sessions plus GPU clocks. Its latencies/FPS are driver moving averages,
not per-frame GPU timestamps. On the tested4K sessions, the driver returned zeros
despite actual decoded frames: that telemetry is unavailable, not zero latency.
All subprocess groups are reaped before the TemporaryDirectory is removed. Core
dumps and GPU disk caches are disabled. Each entire run has a120-second deadline;
retained text/JSON evidence is under `code-review/engine-bench/loopback-*`.

## Experimental nibble packing and coordinate folding

```sh
python host/tools/run_surface_packing_bench.py
```

This standalone experiment downloads nothing and changes no service or TV app.
It requires the installed CUDA compiler, GCC14, Compute Sanitizer, Clang, and
FFmpeg development/runtime libraries. Builds, raw frames and HEVC streams live in
an automatically removed `/tmp` directory. The runner owns/reaps process groups,
disables core dumps and GPU caches, and enforces a 120-second deadline. Receipts
and source hashes remain under ignored `code-review/engine-bench/surface-packing*`.

`surface_packing.cu` starts with ready 8-bit limited-range NV12. The seven modes are:

| Mode | Representation | Exact before HEVC? |
| --- | --- | --- |
| `copy` | Unchanged NV12 control | Yes |
| `nibble_lossless` | Separate high/low nibble streams, both retained | Yes; no size reduction |
| `nibble4` | 16 luma/chroma levels, two quantized samples per byte | No; half-size intermediate |
| `chroma4` | Original luma, 16 chroma levels, neutral 128 preserved | Luma only; 16.7% smaller intermediate |
| `row_fold` | Mirror alternate 16-luma-row bands, preserve UV pair order | Yes; same byte count |
| `uniform_half` | Area-prefiltered 4K to 1080p, bilinear restoration | No |
| `hyperbolic_half` | Center-biased 4K to 1080p, matched inverse restoration | No |

Here “non-Euclidean folding” is an experimental interpretation: separable
hyperbolic coordinate companding, not a claim of intrinsic geometric curvature.
For normalized folded coordinate `q`, sample original coordinate
`sinh(2*q)/sinh(2)`; restore using `asinh(u*sinh(2))/2`. These continuous mappings
are inverses; raster reduction loses information. Precomputed maps remove these
functions from per-frame kernels. Footprint-aware filtering uses up to 5x5
bilinear samples at this reduction/strength. Fixed center; no gaze tracking.
Uniform scaling uses the same filtering/restoration machinery as the control.

The GPU microbenchmark runs two reversed orders, 10 warm-up + 80 samples per mode.
GPU forward/restoration and completed CPU wall spans are separate. Nibble
expansion happens before NVENC: a custom 4-bit intermediate is not a supported
NV12 input. Folding instead encodes the 1080p warped surface; its inverse belongs
after decoding. RTX 4090 restoration timing does not predict LG TV cost.

The optional `nvenc_latency --fixture-probe FOUR_FRAME_NV12 WIDTH HEIGHT OUTPUT_DIR`
entrypoint encodes four cycling synthetic frames with P1/ULL, single pass, forced
split, CBR 23 Mb/s at 120 FPS, 10 warm-up + 40 timed packets. Upload/readiness is outside
encode-only service timing; submissions run at maximum rate. Each packet must
match its input PTS. The runner reads each distinct stream to decoder EOF and
checks first/last geometry and reconstructed quality. Byte-identical payloads
with the same inverse share decoder validation. No network/presentation latency
or throughput claim follows from these separate stages.

Correctness includes all 65,536 byte pairs in each plane, independent scalar
quantizer/coordinate oracles, pitched/tail dimensions, padding and overrun guards,
and Compute Sanitizer. Source readiness and scratch initialization are ordered
before timed work. The fixture generator combines gradients, small grids, edges
and pseudorandom detail in four moving frames; it is not game footage. Quality
metrics use 8-bit code values and PSNR peak 255. The center region is 25% of width
and height, 6.25% of pixels. HDR and perceptual/game quality are untested.

The retained idle run found no nibble encoder-time win. Full 4-bit packing took
about 35–37 us GPU for pack plus expansion versus 15–16 us for the copy control,
and introduced up to 7 luma codes of error before HEVC. Hyperbolic folding took
about 67 us forward and 91 us inverse; encode-only medians were 0.590–0.593 ms versus
1.71–1.78 ms at 4K. Uniform 1080p was faster at 0.561–0.571 ms. Hyperbolic folding
reduced final-frame center luma MAE about 28% versus uniform scaling, while whole
frame error increased. These are different spatial-quality allocations, not a
lossless 4K or 80% full-host improvement. `row_fold` nearly doubled measured packet
bytes and worsened reconstruction after lossy encoding; it is only a control.

The current NDL client feeds compressed data to a native video plane; this
experiment adds no decoded-texture/inverse-warp path. Warped streaming therefore
remains off. Source/contracts informing the experiment:
[NVENC supported input formats](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html#getting-supported-list-of-input-formats)
and Li et al., 2021,
[A Log-Rectilinear Transformation for Foveated 360-degree Video Streaming](https://research.google/pubs/a-log-rectilinear-transformation-for-foveated-360-degree-video-streaming/).
The paper motivates filtered nonuniform allocation; this prototype does not
reproduce its summed-area-table algorithm or transfer its VR findings to this TV.


### Captured-pixel motion and GPU scheduling controls

`run_native_surface_bench.py --kind motion --motion-size 4k` compares normal HEVC
search, correct integer motion hints, wrong hints, and NVOFA hints from the same
four translated GPU fixtures. Optical-flow input copies, estimation, readback,
hint construction, encode completion and packet retirement are timed. The pinned
public NVOFA 2.0 API is used, not a claim about SDK5 features. Every120-frame stream
is fully decoded; first/last colored source errors and flow accuracy are retained.

The flow was accurate on these synthetic translations, but its full4K stage cost
about2.28ms; complete flow-plus-encode was about3.98ms versus1.71–1.81ms normally.
Correct hints alone did not produce a repeatable large win. Wrong hints were
similarly fast. This does not validate game motion or estimated future frames.

Optional `--green-sm 8` uses an app-owned CUDA green context, with exact granted-SM
checks and correct green-context destruction. `--context-wait spin` changes only
this probe's CUDA CPU-wait policy. Neither delivered a major improvement. Native
`--codec h264` checks H.264 with automatic split disabled; it took about3.5–3.7ms
for ready4K here, versus roughly1.7ms for forced-split HEVC. Decoder validation uses
passthrough timestamps so raw H.264 demux defaults cannot drop output frames.

### Intra-only wavelet codec and direct GPU timing

```sh
python host/tools/run_pyrowave_bench.py
python host/tools/run_pyrowave_bench.py --graphics-contention
python host/tools/run_pyrowave_bench.py --occupancy 1 --gpu-timestamps --only-4k --rate 200 --graphics-contention
```

This builds pinned [PyroWave](https://github.com/Themaister/pyrowave) and its minimal
Granite/Volk/Vulkan-header sources in `/tmp` (about8.5MiB compressed total).
Archives are bounded, paths checked, source revisions/hashes recorded, and unused
assets/evaluation data are excluded. No upstream install/setup script is executed.
All downloads, builds, raw media and owned processes are removed on exit; results
are ignored under `code-review/engine-bench/`. The overall default deadline is240s.
Requires the existing compiler, CMake/Ninja, Vulkan development files and NVIDIA
GPU. `--draw-ms .5` changes only the owned synthetic fragment draw duration.

The probe uploads four known moving planar-YUV sources before timing. It includes
GPU completion/readback, CPU packet construction, independent packet ingestion and
completed GPU decode. Each case warms for at least200ms, then measures50 frames;
all warmup/timed frames decode, and the last four have independent pixel checks.
It compares graphics/compute queues and23/100/200/400Mb/s at1080p/4K in reversed
order. These are shader codecs on the4090, not LG measurements or full-host spans.

4K CPU packets were ready in roughly0.35–0.42ms at idle. This changes the codec and
quality/bandwidth tradeoff: synthetic4K luma PSNR was21.60dB at23Mb/s,27.21dB at200,
and27.85dB at400. No game-quality equivalence is claimed. The current NDL backend
cannot decode this custom bitstream; TV Vulkan/GLES capability, reconstruction,
network throughput and presentation remain unverified integration requirements.

`--occupancy 0.5` and `--occupancy 1` create equivalent owned Vulkan devices and
explicit command buffers with the supported NV compute-occupancy feature. They
request no privileged global priority and alter no other process. High occupancy
did not consistently remove the graphics stalls. Optional timestamp diagnostics
use completed GPU queries and calibrated monotonic clocks: under continuous4ms
fragment draws, encoding remained near0.28ms, while queue-start delay was often
about4.1ms; CPU notification was roughly10–20us. Instrumented GPU intervals include
possible GPU stalls and are not pure busy-cycle measurements.

`--half-math` tests upstream FP16 arithmetic; it failed the upstream high-bit-rate
reconstruction tolerance, so no accepted performance result is recorded for that
variant. `--full-storage` selects the upstream FP32-storage control. The earlier
`pyrowave-graphics.json` receipt mixed active and expired load and is explicitly
excluded; newer draw-tagged receipts require load alive at probe completion.

### Lossless predicted residual nibbles

```sh
python host/tools/run_surface_packing_bench.py --residual
python host/tools/run_surface_packing_bench.py --residual --rle
```

The opt-in CUDA/CUB prototype uses an earlier NV12 reference and an even integer
translation. Tile modes are zero residual, constant signed residual, packed signed
4-bit residuals (-8..7), or full literal bytes. Out-of-bounds prediction requires
literal fallback; newly revealed edges are not assumed known. The32-byte header
carries dimensions, frame/reference IDs, translation, tile size and payload sizes.
The optional mode-map RLE uses bounded16-bit counts and a literal fallback.

Encoding includes classification, prefix scan, packing, actual CPU readback and
packet construction. Decoding parses the received metadata, rebuilds its own
lengths/prefix scan, uploads the received packet and completes reconstruction.
It does not reuse hidden encoder offsets. These spans include CPU packet copies;
there is no actual network or TV decoder. Source pairs are repeated synthetic
fixtures; the known-motion case is an oracle bound, not measured motion estimation.

Compute Sanitizer plus an independent scalar packet decoder check pitched/tail
images, all65,536 byte pairs, all16 signed nibbles, constant endpoints, guards,
wrong-reference/truncation rejection and RLE metadata. Every timed case reconstructs
all bytes exactly. No codec/service/client setting is enabled.

At4K, raw NV12 is12,441,600 bytes/frame. Even fully packed4-bit residuals would
need about5.97Gb/s at120FPS before headers. Tile metadata alone also matters.
The initial128-byte-tile exact-pan case produced450,188 bytes/frame (432Mb/s);
without correct motion or on scene cuts it required roughly9–12Gb/s. The RLE mode
reduces a static frame to35 bytes, but does not solve changing-image payload cost.
This is a measured bandwidth limitation, despite completed encode spans of about
65–230us for static/oracle cases. It motivates sparse corrections and cheaper
captured-pixel motion estimation, rather than treating nibble packing as free.

### Private compositor and full-host comparisons

```sh
python host/tools/run_compositor_bench.py --resolution 3840x2160 --duration 8 \
  --host-binary host/runtime/sunshine-latency-prototype --compare cache
python host/tools/run_compositor_bench.py --resolution 3840x2160 --duration 60 \
  --host-binary host/runtime/sunshine-latency-prototype --compare cache --reverse \
  --graphics-contention --tag-suffix sustained
```

This starts a private D-Bus session, virtual KWin, PipeWire and WirePlumber's
policy-only profile. All display, permission, cache, state, certificates and port
configuration stays in a temporary directory. It preserves the real desktop,
Sunshine service, pairing and TV. Required installed tools are KWin, PipeWire,
WirePlumber, D-Bus, `gdbus`, `kscreen-doctor`, `wayland-info`, `wayland-scanner`,
Wayland/EGL/GLES development libraries, Clang, OpenSSL, FFmpeg and Python NumPy.
The receiver uses this repository's `core/moonlight-common-c` checkout.

An ordinary Wayland/GLES client draws moving colored content and six 32-bit
frame-ID markers across the image. Every received frame is fully decoded on the
PC, all markers must agree, IDs must match the source log and move forward, and
frame counts must match. Source draw-start to complete compressed-frame arrival
uses the same monotonic clock. That span includes rendering/composition/capture;
the Sunshine wire host span starts later. Neither measures TV decode/presentation
or input. CSV witnesses retain the individual source IDs and timestamps; raw
video and builds are deleted. The cohorts are bounded to120 seconds and collect
up to16384 frames; shorter runs can be biased by GPU or refresh-rate phase.

`--compare cache` runs both cache settings in one persistent compositor, while
`--reverse` reverses their order. `--compare source --cache-imports` compares
output/window sources using patch `0007`; that experimental build must be supplied
explicitly. Its UUID query reads only the owned compositor. It changes no window
rules and makes no calls inside a game. A failed explicit window request does not
fall back to another desktop source. Each run reaps its own process groups.

`--compare engine --graphics-contention --duration 120` compares the old-equivalent
quarter-multipass/auto-split policy with the combined single-pass/forced-split,
cache/early-stream/lease/GFNI policy. Add `--reverse` for reversed order. Use the
expanded staged prototype above; this benchmark preserves the live service.
`--compare stream|leases|fec|codec` isolates the selected switch with the other
explicit flags held fixed. `--codec hevc|av1` selects the stream codec.
`--quality` reads exact owned GPU backbuffers and matches decoded frame IDs for
PSNR/MAE; these instrumented cohorts are excluded from timing evidence.
`run_loopback_bench.py --validate-fec` enables native client debug reconstruction
checks and recovery counters; it is correctness instrumentation, not a clean
performance measurement. Default comparisons have these instruments off.

The external-semaphore and bitstream-polling additions in `nvenc_gl_probe.cpp`,
`vulkan_shared_surface.h` and `run_native_surface_bench.py` are retained **uncompiled,
unrun experiments** at the stopping checkpoint. They are excluded from the staged
Sunshine binary and accepted results. Do not use their flags as validated paths.

### Sparse pixel corrections

```sh
python host/tools/run_surface_packing_bench.py --sparse
python host/tools/run_surface_packing_bench.py --sparse --estimate-motion
python host/tools/run_surface_packing_bench.py --sparse --estimate-motion --mean-bias
```

The additional mode transmits ordered 16-bit positions and literal byte values
inside a tile. It is selected only when smaller than the existing zero, constant,
nibble or raw modes. A 40-byte header and optionally RLE-compressed 16-bit tile
directory describe the received packet; the receiver validates bounds/counts,
rebuilds its own prefix sums and reconstructs exactly. Revealed borders require
real corrections. Pixel-derived prediction searches 81 even translations over
1024 sampled luma values, then estimates brightness from the matched pixels.
The classification/packing kernels read that result directly on the GPU, without
an intervening host round trip. Estimation and packet readback are timed.

The default brightness estimate uses a majority mode so a moving patch does not
shift the background prediction. `--mean-bias` reproduces the less robust mean
control. These are causal previous-frame predictors, with complete correction;
they do not predict unseen game input or use engine buffers. GPU memcheck, scalar
decoding, every byte pair/nibble, malformed packets, pitched tails and a 16-frame
chain with a receiver-owned reference test correctness. Repeated source pairs are
used for timing, rather than a real game or network. Changing-image bandwidth can
still exceed a TV link by orders of magnitude, and NDL cannot decode this format.
