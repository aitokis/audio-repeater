# Audio Repeater WASAPI Shared

An independent Windows 10/11 x64 application that transfers one capture endpoint
to one render endpoint using WASAPI Shared. It has a native single-window interface,
a bounded audio transport, and finite command-line diagnostics.

Real-device audio operation, end-to-end latency, and any latency difference versus
the MME version of Audio Repeater are unverified. A passing synthetic test or hosted
build does not establish audible routing or a latency benefit.

## Download and use

Open a successful [Windows application Actions run](https://github.com/aitokis/audio-repeater/actions)
and download its `audio-repeater-windows-x64-<source SHA>` artifact (GitHub login
may be required). Extract the outer Actions download, then the application ZIP.
The ZIP contains `audio-repeater.exe`, this README, `LICENSE`,
`THIRD_PARTY_NOTICES.md`, `SOURCE_SHA.txt`, and `SHA256SUMS`. Check the source SHA
against the run and use `Get-FileHash -Algorithm SHA256` to compare files with
`SHA256SUMS`. The run also prints the ZIP checksum and inspected contents.
Artifacts expire after 30 days; these builds are unsigned and are not GitHub Releases.
No installer, external audio DLL, or Visual C++ runtime installation is required.

Double-click `audio-repeater.exe` to open the GUI:

1. Select an input (capture) and output (render) endpoint. The details box shows
   their endpoint IDs; selections survive Refresh by ID, including duplicate names.
2. Choose **Low latency** (minimum legal period) or **Stable** (default period and
   larger queue), then **Start**. These are design settings, not latency guarantees.
3. Use **Stop** before changing settings. Closing the window stops and joins the
   worker before destroying it. The settings stay disabled during startup/routing.
4. After a device error, use **Refresh**, reselect endpoints, then **Start**. A missing
   selection is not silently switched to another device, even after repeated Refresh
   or an empty list. Its ID is restored if it returns; selecting another endpoint
   explicitly replaces it. Empty device lists explain the problem, disable Start,
   and leave Refresh available.

Status and details update four times per second. They show actual native formats,
requested/supported/current periods, buffer frames, fallback HRESULT, MMCSS,
FIFO/SRC queue time, drift correction, underflow/overflow/discontinuity/timestamp
errors, resyncs, discarded frames and startup silence. Queue time includes SRC
backlog. It and API stream latency are diagnostics, not end-to-end measurements.
Use the scrollable details box to inspect values. Tab and access keys navigate controls.

The interface uses English labels. Interactive desktop operation, visual layout,
audibility, real-device routing/removal and long-running real-device behavior
remain unverified. Hosted native-window tests are automated message/control tests,
not evidence of a person operating a Windows desktop. Physical end-to-end latency
and the difference from MME remain unverified. This project does not implement MME,
exclusive mode, ASIO, kernel streaming, render-loopback capture, a driver, a tray
application, or compatibility with the original application's command line.

## Build

Requires CMake 3.26+, a C++20 compiler and a C99 compiler. The dependency is vendored;
configuration does not download code. Windows x64 uses Visual Studio 2022 and the
Windows SDK. The Windows workflow logs the selected compiler, SDK and hosted image.

Linux:

```sh
cmake -S . -B build-core -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-core
ctest --test-dir build-core --output-on-failure --no-tests=error
```

Windows:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure --no-tests=error
```

Use `BUILD_TESTING=OFF` for a production build without allocator hooks. Actions
builds that configuration separately in `build-dist`, packages only explicit
files, extracts the ZIP into a separate directory, verifies hashes/source identity,
checks PE imports and path/debug-data exclusions, and executes that extracted
application's self-test and native-window test. The downloadable ZIP is this
production build, not a core-test executable.
Own code builds with warnings as errors; vendor warnings are isolated. Windows
uses the static C/C++ runtime. No external audio DLL or libsndfile is needed.

## Command-line application (Windows)

```powershell
build/Release/audio-repeater.exe --self-test
build/Release/audio-repeater.exe --list-devices
build/Release/audio-repeater.exe --probe --input-id "CAPTURE_ENDPOINT_ID" --output-id "RENDER_ENDPOINT_ID" --duration-ms 1000 --preset low-latency
```

Use IDs from `--list-devices`, which reports active capture and render endpoints,
native formats, unsupported formats, and explicit empty lists. `--self-test` uses
fake sessions and requires no audio device; it does not demonstrate audible routing.
The probe runs for 1–5000 ms (default 1000) including startup, then requests stop and
joins the worker. Invalid arguments return 2, API errors return 3, and an incomplete
startup returns 4. It never changes endpoints automatically or retries indefinitely.
After invalidation, enumerate again and choose endpoints before restarting.

The low-latency preset requests the smallest legal IAudioClient3 period; `stable`
requests its reported default. Requests must be fundamental-period multiples in
range. If negotiation or initialization fails, a fresh client attempts ordinary
shared/event initialization once. Diagnostics distinguish the requested period,
actual buffer frames, queried current engine period (or `unavailable`), default
device period, API stream latency, and fallback HRESULT. These values are not
measured end-to-end latency. Native mix formats are used without automatic system
format conversion. Only the formats and channel layouts described below are accepted.

One worker owns COM and both streams. It drains at most eight capture packets per
wake with cancellation checks, pre-fills before starting render, and only writes
within `buffer_frames - padding`. Silent capture packets are never dereferenced.
Capture discontinuities reset the transport; timestamp errors have separate counters.
The initial target uses the larger endpoint period (twice that for `stable`), with a
safety floor of one maximum render request plus SRC lookahead. The fixed capacity
includes at least four targets and capture-packet headroom. Buffers over half a
second and transport capacities over two input seconds are rejected at startup.
This conservative floor can exceed the requested period. MMCSS registration status,
queue including SRC, recovery counters, and startup silence are reported separately.

CTest smoke commands each have a ten-second limit and capture redirected output
from the GUI-subsystem executable. Windows Actions also invokes the application's
self-test directly. `audio-repeater.exe --gui-self-test` creates the actual native
window and drives its message loop. It reports native endpoint counts, then uses
synthetic empty lists/IDs to check empty-state controls, ID-based selection after
reordering/removal, settings locks, a real invalid-endpoint startup failure,
restart, repeated Stop, and close during startup with join. It requires window
creation but no human interaction or audio hardware. It does not claim routing.
Fake sessions exercise failed startup, restart after failure,
repeated stop, close while running, cancellation during initialization, invalidation,
and absent events. Hardware routing, device removal, audible output, and long-running
real-device behavior remain unverified.

## Core contract

`src/audio_core.h` contains the single-thread owned processing API. Configuration
and construction allocate before streaming. Processing does not allocate, lock,
log, perform I/O, or access devices.

- Formats: little endian interleaved float32, PCM16, packed PCM24, and PCM32 with
  24 or 32 valid bits. Valid24 in PCM32 is left aligned; unused low bits are ignored.
  Rates are 8–192 kHz. Mono/stereo use unspecified masks or front-center / front-left
  and front-right masks. Other masks, channels, alignments and valid bits are rejected.
  The Windows adapter also validates the native mix format structure and byte rate.
- Mono to stereo duplicates; stereo to mono averages left and right. Nonfinite
  samples become zero. Integer encoding rounds to nearest (ties away from zero)
  and saturates. Float values retain finite headroom; nonfinite SRC output becomes
  zero. Integer conversion has no dither. Conversion spans must not overlap.
- The fixed FIFO returns actual partial frame counts. `Transport` treats exceeding
  its combined FIFO/SRC capacity as an overflow, counts dropped frames, resets the
  SRC/controller, and starts prefill again. Discontinuity performs the same reset
  and has its own counter. Steady underflow zero fills and explicitly resynchronizes.
  Startup silence is counted separately. Invalid processing extents return failure.
- SRC uses persistent `src_process`, actual input/output counts and live
  `end_of_input=0`. It feeds only output demand plus fixed lookahead. Queue size is
  visible FIFO plus SRC input accepted minus generated output's input phase advance.
  This includes frames hidden in SRC, rather than reporting only the external FIFO.
- Drift feedback compares the combined queue with its configured target. Increasing
  queue increases input-clock correction and reduces `outRate/inRate` accordingly.
  Updates occur at most 10 Hz, with a 1-second exponential error filter, PI feedback,
  conditional antiwindup, a 200 ppm/second slew limit, and ±2000 ppm input-clock
  correction bounds. Ratio is `nominal_ratio / (1 + correction_ppm / 1e6)`.
  Reset clears controller history. Recovery outside that range can cause silence
  or discarded frames and is counted explicitly.

`Transport` requires a target larger than one maximum output request converted to
input frames plus SRC lookahead, and capacity at least four times the target.
The caller supplies a maximum render request and processes all operations on one
thread. These are implementation bounds, not measured latency settings.

## Automated verification

CTest runs without audio hardware and fails on an empty suite. Checks use explicit
failure exits in Release builds. Seed `0x5A17C0DE` is fixed and printed with counts.
Use `ctest --test-dir build-core --verbose` to retain full numeric results in CTest's
`Testing/Temporary/LastTest.log`.

Tests cover queue wraparound and randomized chunks, format boundaries, mapping,
nonfinite samples, silence, invalid formats, equal and 44.1↔48 kHz conversion,
impulse position and startup filter lookahead, chunk independence, and 8↔192 kHz
including ±2000 ppm. Fixed 0.25-amplitude tones use a rectangular coherent window
from 0.5 to 1.5 seconds: gain at 1/5/10 kHz must be within ±0.2 dB for 44.1↔48 kHz;
a 96→48 kHz conversion of 30 kHz must yield an 18 kHz alias at most −60 dB.
These are specific signal checks, not proof of full-band transparency.

Six actual SRC transport simulations run ±100/500/2000 ppm for 30 minutes of virtual
time at 8 kHz, using irregular capture chunks and 10 ms render requests. After a
30-second warm-up they require zero underflow, overflow and resync, bounded queue,
and no sustained queue growth. Out-of-range clocks and discontinuities must trigger
explicit recovery. C and C++ allocation hooks cover core and vendor processing,
ratio changes and resets after warm-up. Actual compiled reverse-feedback and
capacity-guard mutations must fail tests that also pass on the unmodified core.

## Dependency and license

Project code uses the [MIT license](LICENSE). Vendored libsamplerate uses
BSD-2-Clause. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and its
[provenance manifest](third_party/libsamplerate/PROVENANCE.md) for the immutable
revision, checked source hashes and build settings. `SRC_SINC_FASTEST` has a
bandwidth tradeoff (upstream documents 80% relative bandwidth); conversion is not
bit-perfect and is not presented as full-band transparent.
