# Audio Repeater WASAPI

An independent Windows 10/11 x64 application that transfers one capture endpoint
to one render endpoint using WASAPI Shared or Exclusive. It has native tray settings, per-user sign-in startup, quiet device reconnection,
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

Double-click `audio-repeater.exe`. The first launch opens settings:

1. Select input (capture) and output (render) endpoints. Selections are stored by
   endpoint ID, including devices with duplicate names.
2. Choose **Shared** (the default, native mix formats) or **Exclusive** (48000 Hz,
   24-bit capture to 16-bit render). Exclusive probes stereo, then mono, independently
   for each endpoint. It can prevent other applications from using those devices.
3. Choose **Low latency** (minimum legal period) or **Stable** (default period and
   larger queue), then **Start**. These are design settings, not latency guarantees.
4. To launch automatically after Windows sign-in, check **Launch at Windows sign-in**.
   Check **Resume audio on launch** to restart the saved route too, then **Save settings**.
   Both options are off initially. Start also saves current settings. Stop before
   changing settings. Configured later launches normally stay in the tray.
5. Closing or minimizing hides settings and keeps audio running. Click the tray icon
   or choose **Settings** in its menu to reopen them. Double-clicking the executable
   again opens the existing settings window; it does not create another audio worker.
   The tray menu also offers **Start / resume**, **Stop**, and **Exit**.
6. Device removal and startup/audio errors do not open dialogs, show notifications,
   or bring settings to the foreground. The application waits for the saved endpoint
   IDs and immediately retries when Windows reports a device change. If a driver or
   audio service is not ready, it retries after 1, 2, 4, 8, 16, then at most 30 seconds.
   Device notifications bypass that delay. A failed notification subscription retries
   quietly every two seconds and rescans when restored. Actual recovery still needs both devices
   and their requested formats to become usable. The application never silently
   substitutes another endpoint or switches Exclusive to Shared. **Stop** cancels
   reconnect retries until **Start**; **Exit** stops and joins the worker and exits.

The tray icon is recreated after Explorer restarts. If Windows cannot create the
icon, settings remain visible and tray registration is retried. Windows may put
an icon in its overflow area; expand that area or pin it in Windows taskbar settings.

### Stored files and removal

No installer, administrator access, service, driver, startup registry entry, or
external runtime is used. Settings occupy only
`%LOCALAPPDATA%\AudioRepeaterWasapi\settings.conf` (plus a temporary file while saving).
Enabling sign-in launch creates only `AudioRepeaterWasapi.lnk` in the current user's
Startup folder (`Win+R`, `shell:startup`). It targets this executable with
`--background`. Moving the extracted application requires enabling sign-in launch
and saving again from the new location. Foreign shortcuts with that filename are
not overwritten or removed; rename or remove the conflicting shortcut yourself.

Choose **Remove startup and saved settings, then exit** from the tray menu, then
remove the extracted application directory. Cleanup removes only this application's
shortcut and settings files; it does not recursively delete other files. If doing
this manually, choose **Exit**, delete the Startup shortcut and the settings file,
then delete the extracted application. Unchecking **Launch at Windows sign-in** and
saving disables automatic launch without deleting settings. Windows itself can also
disable a Startup entry, independently of the shortcut's presence.

Status and details update four times per second. They show the selected mode and negotiated formats,
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
ASIO, kernel streaming, render-loopback capture, a driver, or compatibility with the original application's command line.

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
build/Release/audio-repeater.exe --probe --input-id "CAPTURE_ENDPOINT_ID" --output-id "RENDER_ENDPOINT_ID" --mode exclusive --duration-ms 1000 --preset low-latency
```

Use IDs from `--list-devices`, which reports active capture and render endpoints,
native formats, unsupported formats, and explicit empty lists. `--self-test` uses
fake sessions and requires no audio device; it does not demonstrate audible routing.
The probe runs for 1–5000 ms (default 1000) including startup, then requests stop and
joins the worker. Invalid arguments return 2, API errors return 3, and an incomplete
startup returns 4. It never changes endpoint IDs automatically. Session invalidation
and event timeouts trigger fresh-session retries within the probe's duration; its
deadline still stops and joins the worker.

In shared mode (`--mode shared`, the default), the low-latency preset requests the smallest legal IAudioClient3 period; `stable`
requests its reported default. Requests must be fundamental-period multiples in
range. If negotiation or initialization fails, a fresh client attempts ordinary
shared/event initialization once, except that session invalidation rebuilds the
whole session. Diagnostics distinguish the requested period,
actual buffer frames, queried current engine period (or `unavailable`), default
device period, API stream latency, and fallback HRESULT. These values are not
measured end-to-end latency. Native mix formats are used without automatic system
format conversion. Only the formats and channel layouts described below are accepted.

Exclusive mode always requests 48000 Hz in both directions. Capture probes stereo
packed PCM24 (EXTENSIBLE, then basic PCM), then stereo PCM32 with 24 valid bits;
the same candidates follow in mono. Render probes stereo PCM16 (EXTENSIBLE, then
basic PCM), then mono PCM16. Only an exact `IsFormatSupported` success accepts a
candidate; the shared mix format is not used as evidence of exclusive support.
If none is supported, startup fails with `AUDCLNT_E_UNSUPPORTED_FORMAT`. Device-in-use,
exclusive-mode-disabled and other API failures retain their HRESULT. Session
invalidation rebuilds the whole session with the same exclusive settings.
There is no automatic shared-mode fallback. Stop and explicitly choose Shared if needed.

Exclusive event initialization requests the endpoint's minimum device period for
`low-latency` or its default period for `stable`, with equal buffer duration and
periodicity. An alignment error permits one retry using the returned aligned frame
count on a fresh client. Diagnostics include minimum/requested periods and whether
that retry occurred. Exclusive initialization does not use IAudioClient3 shared periods.

One worker owns COM and both streams. It drains at most eight capture packets per
shared capture wake with cancellation checks. Exclusive capture reads one complete
packet per capture event without `GetNextPacketSize`. Render pre-fills before Start;
subsequent writes occur only on render events. Shared writes stay within
`buffer_frames - padding`; exclusive writes use exactly the full endpoint buffer
without a padding query. Each direction has its own two-second event timeout;
initial prefill has the same deadline. Either watchdog or `AUDCLNT_E_DEVICE_INVALIDATED` /
`AUDCLNT_E_RESOURCES_INVALIDATED` immediately leaves the audio loop, stops both streams
and releases every old client/service before creating a fresh session with the same
IDs, mode and preset. Setup and counters reset for the new session. Retries pause
for 100 ms and Stop/Exit cancels the pause or startup. Other failures retain the
ordinary failed-state handling. Silent capture packets are never dereferenced.
For running exclusive streams, an unavailable endpoint buffer (`AUDCLNT_E_BUFFER_ERROR`)
defers processing until the next event, without consuming transport samples or
releasing an unacquired buffer. Each direction permits three consecutive errors;
the fourth stops with that HRESULT. Successful acquisition clears that direction's
streak. Initial render prefill and shared-mode failures stop immediately.
Capture discontinuities increment a diagnostic counter and retain transport state;
timestamp errors have separate counters.
The initial target is at least 12 ms of input (576 frames at 48 kHz). It also covers
the larger endpoint period (twice that for `stable`) and one maximum render request
plus SRC lookahead, retaining whichever requirement is largest. The fixed capacity
includes at least four targets and capture-packet headroom. Buffers over half a
second and transport capacities over two input seconds are rejected at startup.
This conservative floor can exceed the requested period. MMCSS registration status,
queue including SRC, recovery counters, and startup silence are reported separately.

CTest smoke commands each have a ten-second limit and capture redirected output
from the GUI-subsystem executable. Portable desktop-policy tests check strict settings
round trips, malformed/Unicode IDs, retry bounds and Stop cancellation. Windows Actions also invokes the application's
self-test directly. Fake-client checks exercise exclusive probe ordering, rejection
of nonexact support, period bounds, fresh-client alignment retry and its finite failure
paths, and full-buffer rendering without padding. Direction-specific fake events
verify dispatch and timeout when only one endpoint signals. Windows CLI checks cover
default/shared/exclusive selection, invalid modes and duplicate options.
`audio-repeater.exe --gui-self-test` creates the actual native
window and drives its message loop. It reports native endpoint counts, then uses
synthetic empty lists/IDs to check empty-state controls, ID-based selection after
reordering/removal, settings locks, a real invalid-endpoint startup failure,
restart, repeated Stop, mode selection/locking, tray hide/show and Explorer recreation
messages, saved-ID waiting/cancellation, and explicit Exit during startup with join.
It also checks settings and Startup shortcut creation/removal/relocation/foreign
shortcut preservation in a unique temporary directory, never in the user profile.
Native launch cases load disposable missing, corrupt and valid settings through the
production initialization, restore mode/preset/IDs/resume, check configured visibility
with available/unavailable tray status, and exit during synthetic startup and running.
Two child processes use a GUID-isolated instance/window identity to exercise the same
mutex and foreground/background forwarding branch without creating an audio worker.
A failed notification subscription followed by success checks the timer retry path.
The minimum window size is checked against the actual Exit control bounds.
A synthetic Session and endpoint
lists drive the same worker and notification-message handlers to check successful
reconnect with unchanged IDs/mode/preset, unrelated notifications without restart,
removal cleanup, and Stop preventing later reconnect. It requires window
creation but no human interaction or audio hardware. It does not claim routing.
Fake sessions exercise failed startup, restart after failure,
repeated stop, close while running, invalidation at initialization and both streams'
start/processing calls, destruction before replacement, cancellation during retries,
and watchdog recovery even while the other endpoint keeps signaling. Hardware routing,
device removal, audible output, and long-running real-device behavior remain unverified.

## Core contract

`src/audio_core.h` contains the single-thread owned processing API. Configuration
and construction allocate before streaming. Processing does not allocate, lock,
log, perform I/O, or access devices.

- Formats: little endian interleaved float32, PCM16, packed PCM24, and PCM32 with
  24 or 32 valid bits. Valid24 in PCM32 is left aligned; unused low bits are ignored.
  Rates are 8–192 kHz. Mono/stereo use unspecified masks or front-center / front-left
  and front-right masks. Other masks, channels, alignments and valid bits are rejected.
  The Windows adapter also validates format structures and byte rates. Requested
  PCM24/48000 Hz to PCM16/48000 Hz conversion is checked for packed and padded capture,
  signed halfway rounding and saturation.
- Mono to stereo duplicates; stereo to mono averages left and right. Nonfinite
  samples become zero. Integer encoding rounds to nearest (ties away from zero)
  and saturates. Float values retain finite headroom; nonfinite SRC output becomes
  zero. Integer conversion has no dither. Conversion spans must not overlap.
- The fixed FIFO returns actual partial frame counts. When combined FIFO/SRC
  backlog plus an incoming packet reaches twice the target, `Transport` discards
  oldest FIFO frames and, if needed, the oldest incoming frames in one correction
  down to the target (rounded down for fractional SRC backlog). At 48 kHz with a
  576-frame target, 1152 queued frames become 576. Newest audio, SRC history,
  drift state and running state survive; normal rendering resumes immediately.
  Each correction increments resyncs and counts discarded input frames. Exceeding
  physical capacity also increments overflows and uses the same correction,
  including packets larger than capacity. `push` returns false for invalid
  extents or any correction that discards queued or incoming audio.
  Discontinuity only increments its counter. Steady underflow keeps the generated
  output prefix and zero pads only its missing tail, preserving queued audio, SRC history, priming
  and drift-controller state. SRC errors still reset the transport.
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
  The controller remains active when nominal input/output rates match. Only
  SRC-error recovery clears transport/controller history and restarts prefill.
  Clocks outside the correction range can cause counted silence or backlog corrections.

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
and no sustained queue growth. Two more five-minute runs use equal nominal 48 kHz
rates, 144-frame requests, a 576-frame target and ±500 ppm, checking correction sign
and convergence. Repeated partial/full underruns check zero tails, retained audio
against a persistent SRC, sub-target resumption and continuing controller updates.
Out-of-range slow capture pads shortages; fast capture triggers backlog corrections.
Repeated discontinuity flags preserve actual output, queue, SRC and live drift.
Backlog tests cover initial and running streams, the twice-target threshold,
sub-threshold jitter, fractional SRC backlog, retained newest stereo frames,
oversized packets and immediate output without recurring corrections.
C and C++ allocation hooks cover core and vendor processing,
ratio changes, backlog corrections and resets after warm-up. Actual compiled
reverse-feedback and capacity-guard mutations must fail tests that also pass on
the unmodified core.

## Dependency and license

Project code uses the [MIT license](LICENSE). Vendored libsamplerate uses
BSD-2-Clause. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and its
[provenance manifest](third_party/libsamplerate/PROVENANCE.md) for the immutable
revision, checked source hashes and build settings. `SRC_SINC_FASTEST` has a
bandwidth tradeoff (upstream documents 80% relative bandwidth); conversion is not
bit-perfect and is not presented as full-band transparent.
