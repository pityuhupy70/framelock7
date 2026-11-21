# FrameLock

FrameLock is a Windows frame-sequence recorder. It injects a hook DLL into a game, captures the presented back buffer, writes sequential JPEG files, and can generate an FFmpeg batch file for locked-rate export.

> **Verification status:** This source package has received a static audit and portable clock tests. It has **not** been compiled or run on Windows in the environment where this package was prepared. No Direct3D, Vulkan, OpenGL, driver, GPU, game, or physical-storage stress test has been run. Treat it as an audited candidate, not a certified release. See [`FINAL_AUDIT_REPORT.md`](../FINAL_AUDIT_REPORT.md) before use.

## Recording modes

### Locked clock (default)

FrameLock virtualizes several Windows time APIs and holds the virtual QPC timestamp at one immutable frame boundary. It captures the target Present's image, reads it back, encodes it, flushes the temporary JPEG, and renames it to `frame_NNNNNN.jpg`. The frame index and virtual clock advance only after that commit succeeds. The encoder queue is bounded to one frame; if rendering, GPU readback, encoding, or storage is slow, the game thread blocks rather than dropping queued frames. A capture or write error stops the take.

The target rate is represented as an exact rational number. Decimal input is kept as the exact base-10 value (for example, `59.94` is `2997/50`); a broadcast rational must be entered explicitly (for example, `60000/1001`). Frame boundaries are quantized to integer QPC ticks, so individual intervals can differ by at most one QPC tick while the rational schedule has no cumulative arithmetic drift.

**Important:** FrameLock cannot enforce the number of private simulation updates performed by an external game engine. It serializes the hooked time reads and captured Present steps, but fixed-step engines, separate simulation threads, multiple updates per render, repeated Presents, `RDTSC/RDTSCP`, direct shared-kernel-time reads, audio/GPU clocks, or other unhooked timers can bypass it. A literal guarantee that every internal game simulation step maps one-to-one to one saved image requires game-specific engine integration. See the audit report.

A game limiter that requires more virtual elapsed time per frame than the selected locked step can stall at the frame boundary. Disable that limiter or select a locked FPS no higher than the game's own frame cap. The stall monitor warns after three seconds. Press F9 to stop. If a frame capture/write is already in progress, FrameLock lets that frame finish and commit before stopping; it does not cancel or drop a partially written frame. If the game is waiting for the next virtual-time boundary before starting a capture, F9 releases that unrecorded step so the game can unwind.

### Real-time capture

Enable **Real-time capture** to leave the game clock untouched. FrameLock saves the frames the game actually presents. It does not guarantee a fixed FPS or a fixed number of images per simulation update; use the measured FPS in `take_info.txt` when encoding.

## Windows builds

### Recommended: GitHub Actions

No local C++ toolchain is needed when building in GitHub Actions:

1. Put this project in a GitHub repository and push the source.
2. Open **Actions** and run the **Build FrameLock** workflow (or push a commit / open a pull request).
3. The workflow configures and builds Windows x64 and Win32 with Vulkan enabled, runs CTest, and verifies the DLL architecture and the undecorated Vulkan layer entry-point export. It also has an x64 Vulkan-disabled diagnostic build.
4. Download the `FrameLock-dist` artifact from the package job. It is intended to contain the two launchers and two hook DLLs side by side, plus these instructions and the audit report.

The workflow definition has been updated in this source package, but it has **not** been triggered from this environment. An Actions artifact does not exist until the project is pushed and the workflow succeeds.

### Local Windows build (optional)

`build.bat` builds x64 and Win32 and runs the portable tests. It requires Visual Studio 2022 with **Desktop development with C++**, a Windows SDK, CMake, and internet access for the CMake FetchContent dependencies. Run it from a Windows machine. This route is not required when using GitHub Actions.

## Using FrameLock

1. Extract the combined distribution so `FrameLock64.exe`, `FrameLock32.exe`, `FrameLockHook64.dll`, and `FrameLockHook32.dll` are in the **same directory**.
2. Run the launcher matching the game's architecture: `FrameLock64.exe` for a 64-bit game or `FrameLock32.exe` for a 32-bit game. Launch FrameLock at the same privilege level as the game.
3. For Vulkan, use **Launch game .exe and attach...**. The Vulkan implicit layer must be loaded when the game creates its Vulkan instance; attaching to an already-running Vulkan process cannot insert the layer retrospectively. For other APIs, select the game in the process list and press **Attach**, or use Launch.
4. Enter the desired FPS. Examples: `24`, `30`, `60`, `59.94`, or `60000/1001`. Choose an output folder.
5. Leave **Real-time capture** unchecked for locked-clock export. Press **RECORD (F9)** or F9 in the game. Press F9 again to stop. F10 runs a 120-frame capture/self-test.
6. Each take is written into its own `Take_...` subfolder. Locked takes contain sequential JPEGs, `take_info.txt`, and `make_video.bat` (when at least one frame was saved). Install FFmpeg and make sure `ffmpeg.exe` is on `PATH`, then run `make_video.bat` to encode the image sequence at the recorded rational FPS.

The frame JPEG path requests quality 100% and 4:4:4 chroma. If the WIC encoder cannot apply either option, the capture fails closed. JPEG is still a lossy format; use the image sequence as the recording master. FrameLock does not record audio.

## Capture paths implemented in source

The hook contains capture paths for Direct3D 9, Direct3D 10, Direct3D 11, Direct3D 12, Vulkan (implicit layer), and OpenGL/WGL. This is a statement about code paths, **not** a claim that each API has been compiled or verified on real Windows hardware. The F10 test checks image-file existence, sequence names, file sizes, and WIC decodability; it does not prove that a game advanced exactly one internal simulation tick per image or that every driver path is correct.

## Known constraints and fail-closed cases

- HDR and wide-gamut color spaces are not tone-mapped to SDR JPEG. Known non-SDR DXGI/Vulkan color spaces are rejected; unknown high-bit-depth DXGI states are rejected.
- Vulkan must be enabled before process instance creation (use Launch). Multi-swapchain presents are not captured. Vulkan per-image present semaphores are retained until device teardown after swapchain destruction because the presentation engine's consumption cannot otherwise be proven complete with the currently implemented synchronization path. Extreme swapchain churn can therefore retain semaphore handles until device destruction.
- D3D12 capture requires identification of the actual swapchain present queue. If FrameLock cannot establish that association, it stops rather than borrowing another queue.
- Some OpenGL contexts, non-window/pbuffer paths, uncommon swapchain formats, or driver behaviors may be unsupported and can stop a take.
- Direct TSC reads, `KUSER_SHARED_DATA`, `NtQuerySystemTime`, audio/GPU clocks, custom timers, and game-specific simulation clocks are not virtualized.
- Two target Present calls occur before the first saved locked frame: the first starts/rebases the clock after it returns, and the next is an uncaptured warm-up step. `frame_000000` is captured at the following Present, after one full 1/FPS virtual interval. The game state before the take starts is not part of the image sequence.
- A stop request during an already-running capture allows that capture to finish and commit; a stop request while the game is waiting at a virtual-time boundary releases the unrecorded step. A take that encounters any capture/save failure is incomplete and must not be treated as a valid finished sequence.
- Storage flush requests and write-through rename are issued, but no software can guarantee persistence through a failing controller, dishonest device cache, sudden power loss, or filesystem/hardware faults.

## Logs and troubleshooting

The hook writes `%TEMP%\FrameLock_<pid>.log`. Check it for the active API, capture stage, HRESULT/Vulkan result, back-buffer index, GPU wait, queue identification, and disk commit errors. `take_info.txt` contains the selected rational rate. `test_report.txt` is written by F10.

If a take stalls, check whether the game's own FPS limiter is lower than the requested locked rate. If capture stops, do not rename missing files or continue by hand; use only the contiguous images written before the reported failure.
