# FrameLock — Final source audit and repair report

**Baseline:** `FrameLock_phase_review5.zip` (Review5)  
**Audit package date:** 2026-11-21  
**Disposition:** **Source repairs completed; Windows build and real-runtime validation remain outstanding. This is not certified as finished.**

## 1. Executive summary

I read the complete uploaded Review5 ZIP, including `REVIEW5_REPORT.md`, `BUILD_AND_STATUS.md`, `HANDOFF_REVIEW4.md`, `review5.diff`, the CMake project, launcher, shared header, hook core, Vulkan layer, and the portable test source.

Review5 still had correctness problems that contradicted the frame-lock guarantee. The most important were:

- A frame was considered complete when queued to the encoder, not when the final image file had been flushed and committed.
- The adaptive clock hold allowed the virtual time to creep after its hold interval, so late clock readers could observe a different frame timestamp.
- Capture/write retries could let the external game advance on a step whose first capture failed.
- The self-test could report a file-integrity failure after the worker had already emitted a successful `Finished` status.
- The D3D10/D3D11 capture path selected back buffer 0 even when a flip-model swapchain was presenting another index.
- Vulkan reused a fixed ring of present-wait semaphores without proving the presentation engine had finished consuming them.
- Review5 called a nonexistent `IDXGISwapChain3::GetColorSpace1` method.
- The JPEG quality/options block had a `VARIANT` lifetime/scope error that would prevent the Windows hook from compiling.
- The GitHub Actions x64 Vulkan-enabled and x64 Vulkan-disabled matrix jobs used the same build directory and could race when scheduled concurrently.
- Decimal FPS input could be rewritten to a nearby common broadcast fraction.
- The old report made claims that were no longer safe under strict timing (adaptive creep, retry behavior, and repeated-image failures).

The source has been modified to address these and related issues. The central implementation choice is deliberately conservative: **do not advance the hooked virtual clock or frame index until the capture callback confirms that its frame has been read back, encoded, flushed, and renamed into the final sequential filename. Any failure stops the take.**

This still cannot guarantee one private simulation tick per saved image for every third-party game. FrameLock does not own the engine's scheduler, render loop, worker threads, or all of its time sources. The precise guarantee and unavoidable limits are described below.

## 2. Important changes made

### 2.1 Locked clock and exact FPS

Files: `FrameLock/src/hook/dllmain.cpp`, `FrameLock/src/hook/clock_math.h`, `FrameLock/src/hook/fps_parse.h`, `FrameLock/src/launcher/main.cpp`.

- Removed Review5's adaptive 20 ms–1 s hold and all wall-clock creep. In locked mode, QPC reads return the same immutable virtual timestamp for the current step.
- QPC reads hold the shared clock lock through the real-QPC sample and virtual-time evaluation. Clock-step changes use the exclusive lock, preventing a delayed reader from evaluating an old state after another reader has observed the new boundary.
- Clock boundaries are calculated as `floor(n * QPC_frequency * FPS_denominator / FPS_numerator)` using checked integer arithmetic. The selected rational rate is retained. With an integer QPC counter, individual intervals can quantize to adjacent tick counts; schedule drift is not accumulated.
- Decimal FPS is parsed as an exact base-10 rational with up to six decimal places. For example, `59.94` becomes `2997/50`; `60000/1001` remains that exact rational when entered explicitly. Input outside 1–1000 FPS, malformed values, and overflow are rejected rather than silently defaulted.
- Added guards against frame-counter overflow before incrementing the step index.
- Explicit stop releases the current unrecorded step when the game is blocked waiting for a virtual-time boundary. If stop is requested while a frame capture/write is already running, FrameLock allows that frame to finish and commit before stopping; it does not cancel a partial write or silently drop an in-flight capture. This preserves the save-before-advance boundary.

**Clock APIs hooked:** the source hooks QueryPerformanceCounter/RtlQueryPerformanceCounter and several common Windows time APIs, including GetTickCount/GetTickCount64, timeGetTime, system FILETIME APIs, and available interrupt/unbiased-time APIs. This is not a complete virtualization of Windows time.

### 2.2 Capture/save commit boundary

Files: `FrameLock/src/hook/dllmain.cpp`, `FrameLock/src/hook/flcore.h`, `FrameLock/src/shared.h`, `FrameLock/src/launcher/main.cpp`.

- Capture callbacks now return success only after synchronous readback and worker completion of JPEG encode, `FlushFileBuffers`, and a write-through rename from the temporary file to `frame_NNNNNN.jpg`.
- The queue is bounded to one in-flight worker frame. The render/present thread blocks until the worker finishes; slow storage reduces recording throughput but does not move the virtual timestamp forward.
- A failed capture, GPU readback, queue handoff, conversion, JPEG option setup, file open/write, flush, or rename stops the take. There is no retry of the same game step; an external simulation thread may already have advanced independently of the hooked clock.
- The rename no longer uses `MOVEFILE_REPLACE_EXISTING`, so an already-existing sequential image cannot be silently overwritten.
- The shared/UI counter was renamed from `framesQueued` to `framesCommitted` because it is updated only after the synchronous durable-commit callback succeeds.
- JPEG quality 100% and 4:4:4 chroma options are now required. If the Windows Imaging Component encoder cannot apply either setting, the frame is rejected rather than silently emitted using a different quality/subsampling configuration. JPEG itself remains lossy.
- Output paths that cannot fit ordinary Win32 frame filenames are rejected before the take starts.
- Take folders are unique; the end-of-take worker checks that frame indices are sequential and that the saved count matches the captured count.
- F10 verification now participates in the finalization decision. Missing, zero-sized, undecodable, extra, or leftover temporary files make the self-test fail and prevent a successful `Finished` status. Repeated pixel hashes remain a warning, not a failure, because a static or paused game can legitimately produce identical images.
- `take_info.txt` and `make_video.bat` write failures are surfaced. The report distinguishes a complete committed image sequence from a missing sidecar/batch file.

### 2.3 DXGI / Direct3D capture correctness

File: `FrameLock/src/hook/dllmain.cpp`.

- D3D10/D3D11 flip-model paths use `IDXGISwapChain3::GetCurrentBackBufferIndex` before reading the buffer. If the required interface or a valid index is unavailable, capture fails closed rather than saving a stale `GetBuffer(0)` image.
- D3D10/D3D11 immediate-context access uses the API's multithread-protection interface and scoped locking. The lock is released before waiting on JPEG/disk completion.
- D3D12 requires an identified command queue for the actual swapchain; it no longer guesses from a process-wide “last queue.” Queue associations are bounded, and an unresolved queue is a fatal capture error.
- D3D12 readback validates resource dimensions/footprint and waits on the capture fence before accessing the readback buffer. Unsafe resources are retired rather than reused after a fence/device error.
- D3D9 pitch/dimension checks and OpenGL readback state-preservation paths were reviewed and retained. OpenGL restores framebuffer/read-buffer/pixel-pack state and rejects a context/DC mismatch; unusual non-window paths can be unsupported.
- DXGI HDR/wide-color state tracking no longer calls a nonexistent getter. The hook tracks successful `SetColorSpace1` calls and rejects known HDR/wide-gamut states. Unknown high-bit-depth state is rejected. HDR tone mapping is not implemented.

Microsoft's `IDXGISwapChain3` method list documents `CheckColorSpaceSupport`, `GetCurrentBackBufferIndex`, `ResizeBuffers1`, and `SetColorSpace1`; it does not define `GetColorSpace1`. See [IDXGISwapChain3](https://learn.microsoft.com/windows/win32/api/dxgi1_4/nn-dxgi1_4-idxgiswapchain3), [SetColorSpace1](https://learn.microsoft.com/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-setcolorspace1), and [GetCurrentBackBufferIndex](https://learn.microsoft.com/windows/win32/api/dxgi1_4/nf-dxgi1_4-idxgiswapchain3-getcurrentbackbufferindex).

### 2.4 Vulkan layer hardening

File: `FrameLock/src/hook/vk_layer.cpp`.

- Replaced the fixed 16-semaphore reuse ring with one present-wait semaphore per swapchain image. A semaphore is reused only after the corresponding image is reacquired.
- On swapchain destruction, per-image semaphores are retained until device teardown. Without an enabled present-completion/fence mechanism, FrameLock does not guess when the presentation engine has consumed a wait semaphore.
- Present queue/device/swapchain association remains exact; missing queue-family or swapchain tracking causes capture failure rather than a guessed queue.
- Added null and input guards around instance/device loader chain handling, downstream GIPA/GDPA, create outputs, and tracking insertion. Duplicate tracking keys are rejected rather than silently keeping an unrelated dispatch object.
- Command pool/buffer/fence setup cleans partially created command state. CPU-side wait-stage allocation occurs before command-buffer recording.
- Allocation failures in capture/setup stop capture or leave the game on a pass-through path where the layer has enough dispatch information. The layer never claims a frame was captured when its tracking is incomplete.
- Multi-swapchain presents and HDR/wide-gamut paths are rejected. The original present is still forwarded whenever the layer has the required downstream dispatch pointer.

### 2.5 Launcher, packaging, and build/test workflow

Files: `FrameLock/src/launcher/main.cpp`, `FrameLock/src/shared.h`, `FrameLock/CMakeLists.txt`, `FrameLock/build.bat`, `.github/workflows/build.yml`.

- The launcher uses the exact FPS parser, checks target process bitness, and waits for the remote `LoadLibrary` thread before releasing its DLL path buffer.
- The launcher starts games suspended and creates the per-process shared mapping before resuming the game. This is needed for early Vulkan child/process initialization behavior.
- Vulkan implicit-layer registration covers both architectures when the corresponding DLL/manifests are present; Vulkan still must be loaded at process instance creation, so Launch is the supported path.
- The workflow configures x64 and Win32 Windows builds with Vulkan enabled, includes an x64 Vulkan-disabled diagnostic build, runs CTest, checks the x86/x64 machine type and the undecorated Vulkan layer negotiation export, and packages the exact launcher and hook DLL names. Each matrix combination now has a distinct build directory so the x64 Vulkan-ON and Vulkan-OFF legs cannot overwrite one another when they run concurrently. The artifact collection filter no longer includes `FrameLockClockTests.exe` as an application binary.
- Fixed the JPEG encoder's `VARIANT` declaration scope so `VariantClear` references a live initialized value in the Windows build.
- `build.bat` now runs CTest after both local architecture builds.
- Replaced the outdated README and added this audit report. The Review5 report and Review4 handoff are retained as historical artifacts; their old “adaptive hold,” retry, and status claims are superseded by this report.

## 3. Portable tests run in this environment

A reproducible command transcript and exact stdout are included in [`PORTABLE_TEST_RESULTS.txt`](PORTABLE_TEST_RESULTS.txt).

Command executed from `FrameLock/`:

```bash
g++ -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror \
  tests/clock_test.cpp -o /tmp/framelock-clock-tests
/tmp/framelock-clock-tests
```

**Result:** passed on the Linux host. The test source covers:

- Exhaustive small-domain checks of the overflow-safe multiply/divide helper.
- Ten-hour schedules across 14 rational FPS/QPC-frequency combinations, independently checked by a quotient/remainder recurrence. The observed interval spread is at most one integer QPC tick.
- 48 concurrent reader threads evaluating one immutable clock state.
- Decimal-vs-rational FPS parsing, whitespace, invalid values, and bounds.
- Simulated capture, encode, flush/rename failure points; the model checks that failed frames do not increment the index or clock and permanently stop the take.
- A condition-variable commit gate modelling slow storage; the model verifies that frame index and virtual step remain unchanged until the commit is released.

These are portable arithmetic and model/state-contract tests. The `SimulatedRecorder` is a test model, not the actual Windows worker thread or Present hook. These tests do **not** test Win32 file semantics, COM/WIC, DLL injection, GPU synchronization, a game engine's simulation thread, or actual storage latency/failure behavior. The CMake/CTest workflow itself was not run here.

## 4. Build and runtime verification status

| Area | Status in this audit | Evidence / limitation |
|---|---|---|
| Portable clock arithmetic tests | **Passed** | GCC 14.2, `-Werror`; output recorded in `PORTABLE_TEST_RESULTS.txt` |
| FPS parser and modeled commit/failure tests | **Passed** | Included in the same portable test executable |
| Portable AddressSanitizer / UndefinedBehaviorSanitizer run | **Passed** | Exit 0; leak detection enabled; no sanitizer diagnostics |
| CMake configuration / FetchContent | **Not run** | Requires dependency retrieval; no Windows build environment here |
| Windows x64 application build | **Not verified** | No Windows SDK/MSVC linker in this sandbox |
| Windows x86 application build | **Not verified** | No Windows SDK/MSVC linker in this sandbox |
| GitHub Actions workflow | **Not run** | No connected repository/remote or workflow execution in this session |
| DX9 / DX10 / DX11 / DX12 runtime capture | **Not tested** | No Windows machine or GPU |
| Vulkan layer registration / capture | **Not tested** | No Windows Vulkan loader/device; layer code only statically audited |
| OpenGL capture | **Not tested** | No Windows WGL runtime |
| Weak CPU/GPU hardware | **Not tested** | The slow-commit test is simulated, not a real hardware stress test |
| Slow/failing storage | **Not tested** | Failure injection is modeled; no physical slow/failing device was available |
| 1440p/4K, long-run image capture, swapchain churn | **Not tested** | No game/GPU runtime |

No Windows binaries are included in this ZIP. The GitHub workflow is configured but has not been run. A successful compile is not itself proof of GPU correctness; the per-API game tests listed in the README still need to run on real target systems.

## 5. Fundamental limits and remaining risks

### 5.1 What FrameLock can enforce

For a process where the corresponding hooks are installed and the game actually uses the hooked clocks, FrameLock can:

1. Hold hooked QPC/time API reads at an immutable virtual step boundary.
2. Serialize each intercepted target Present through one capture callback.
3. Block the caller until the captured image has been read back and committed as a sequential JPEG.
4. Advance the virtual step and frame index only after that commit succeeds.
5. Stop on errors instead of retrying or silently skipping a saved frame.

### 5.2 What FrameLock cannot guarantee for an arbitrary game

The application is external. It owns its simulation scheduler and can run more than one simulation update per render, run simulation on another thread, render/present the same state multiple times, or update state without presenting. User-mode hooks cannot see every time source. Direct `RDTSC/RDTSCP`, direct `KUSER_SHARED_DATA` access, unhooked native time APIs, audio clocks, GPU timestamps, custom clocks, and engine-internal timers can bypass the virtual clock. A code-byte scan for TSC instructions is only a heuristic, not proof of use or non-use.

Therefore **FrameLock cannot universally guarantee “one internal game simulation step equals one saved frame” for arbitrary commercial games.** It guarantees one successful committed image per intercepted recording step only to the extent the game's update/present loop and selected timebase follow that model. A stronger universal guarantee requires engine-specific instrumentation/plugin support or modification of the game itself.

### 5.3 API-specific limits

- **Vulkan:** only one swapchain per present is captured. The layer must be loaded before the instance is created (start through FrameLock). Retired present semaphores remain allocated until device teardown; repeated swapchain creation/destruction can increase retained handles during a long process. Present-completion fences/maintenance extensions are not implemented. A GPU/fence failure may cause resources to be intentionally retired/leaked rather than unsafely destroyed. If the device is lost after FrameLock has submitted a wait/signal replacement, recovery behavior is ultimately driver/device dependent; capture is marked failed, but no user-mode layer can restore a lost Vulkan device to a healthy state.
- **D3D12:** exact swapchain queue association is mandatory. If the swapchain was created before FrameLock could track its queue and `GetDevice` cannot provide the queue, the take stops.
- **DXGI HDR:** SDR JPEG output only; no HDR/wide-gamut tone mapping. Unknown high-bit-depth color-space state is rejected.
- **OpenGL:** readback is tied to the current WGL context/DC and common back-buffer path. Pbuffers, unusual context ownership, uncommon pixel formats, or driver-specific behavior may be unsupported.
- **Device loss / hung GPU / storage stall:** no arbitrary timeout is used to advance the clock. A non-completing GPU/storage operation can stall recording indefinitely; stopping can release the time lock only when the control thread can regain control. The implementation chooses correctness over pretending a missing frame completed.
- **Durability:** a flush and write-through rename are requested, but software cannot guarantee survival of power loss or a storage device/controller that lies about flush completion.

## 6. Required release gate before calling this complete

1. Push this source to GitHub and run the Actions workflow. Fix every real MSVC/Windows-SDK error in both x64 and Win32 builds. Confirm `FrameLock-dist` contains the two launchers and two hook DLLs.
2. On Windows hardware, run F10 for D3D9, D3D10, D3D11, D3D12, and OpenGL. Start at least one Vulkan application through Launch and verify layer activation, no duplicate DXGI/Vulkan stepping, and a valid 120-frame self-test.
3. Validate actual image content and back-buffer rotation (including flip model), not just file count. Compare known per-frame markers in the game or a purpose-built test app against sequential JPEGs.
4. Stress 1080p/1440p/4K, long takes, swapchain resize/recreation, multiple GPUs/adapters, CPU affinity/low CPU, GPU saturation, and a deliberately slow/failing output volume. Confirm no deadlocks, use-after-free, semaphore reuse hazards, or false `Finished` status.
5. Test games with different frame limiters and timebases. For titles that do not advance deterministically from hooked QPC, add engine-specific support or document them as unsupported rather than claiming exact simulation stepping.

Until these gates pass, do not describe FrameLock as fully verified, production-stable, or guaranteed to map every game simulation step to exactly one image.
