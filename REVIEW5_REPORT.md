> **Historical Review5 report — superseded.** This report contains assumptions later found unsafe (including adaptive clock creep and capture retries). The current source state, repairs, test evidence, and limitations are in [`FINAL_AUDIT_REPORT.md`](FINAL_AUDIT_REPORT.md). Do not use the old timing claims below as the current contract.

# FrameLock – Review 5 report

## Read this first: limits of this pass

- **I could not compile any of the Windows code, and nothing ran on DX9, DX10, DX11, DX12, Vulkan or OpenGL.** The sandbox runs Linux with no MinGW, MSVC, clang, CMake, Windows SDK, GPU or internet. I tried installing a cross-compiler (`dnf mingw64-gcc-c++`, `pip ziglang`) and both installs failed.
- No earlier phase compiled the project either, according to the handoff notes. **The first real build has to happen on Windows**, either with `build.bat` or with the GitHub Actions workflow in `.github/workflows/build.yml`.
- So this pass was a static code review, targeted fixes and a host-side test of the clock math. I can't claim a "fully functional Windows application" or "tested on all APIs" until the steps in the Windows checklist below have been run.

## Bugs found and fixed (all in `FrameLock/src/hook/dllmain.cpp`)

### 1. Uneven frame timing on weak hardware (main sync bug)
The v3 locked clock froze game time at the exact step boundary B(n) for only **20 ms** after each Present. After that, time crept forward toward B(n+1).

On a slow CPU, the game's frame-start time read can come more than 20 ms after Present. That read then saw a crept value, so the per-frame step varied between 0 and 2/FPS. The total stayed exact, but motion stuttered and frames could look like near-duplicates or skips.

**Fix: adaptive hold.**
- The render thread's first clock read in each step is timed.
- The hold is set to 4× a slowly decaying maximum of that delay, limited to 20 ms–1 s.
- At take start the hold begins at a safe 200 ms and then adapts.

Frame limiters still finish because the creep after the hold is unchanged. They just wait a little longer, so exports run slower but frames are exact.

Simulated results (60 fps, 20,000 frames, random frame-start delay plus spikes, up to 500 ms render/readback):

| Frame-start delay | v4 (fixed 20 ms) exact frames | Review 5 exact frames |
|---|---|---|
| ≤ 5 ms | 100% | 100% |
| ≤ 15 ms | 99.34% (264 timing errors) | 99.995% (2) |
| ≤ 40 ms | 49.6% (15,008 errors) | 99.995% (2) |
| ≤ 120 ms | 16.5% (16,285 errors) | 99.995% (2) |

The few remaining errors come from sudden spikes above 4× the recent maximum, in the frame before the hold adapts. Total game time is still exact.

### 2. Game time could run backwards when another window presents
`FrameStepPost` always cleared the "target busy" flag, even when a Present came from a different swapchain or window (overlays, launchers, tool windows) on another thread.

If you pressed stop at that moment, the stall monitor could release the clock in the middle of a step. The next `ClockAdvance` then moved time backwards. **Fix:** only the thread that set the flag can clear it (`t_ownsBusy`).

### 3. Race between pressing stop and capture
The control thread checked "busy" and then released the clock without any locking. A capture could start in between, with the same result as bug 2: time going backwards.

**Fix:** the release now happens only while holding the frame-step mutex (non-blocking try-lock), and "busy" and "stop requested" are checked again under that lock.

### 4. No end-of-take integrity check
A take was reported as "Finished" without checking that every captured frame was saved.

**Fix:** the encoder now tracks the saved-frame count per take and checks that indices arrive strictly as 0,1,2…
- If an index gaps or repeats, the take stops with an error. Frames saved up to that point remain a valid, contiguous sequence.
- At the end, if saved ≠ captured, the status shows **ERROR: take integrity check failed** and no take info is written. A bad take is never reported as good.

## Reviewed with no change needed
- **Exact FPS / playback speed:** step positions are integer-exact. B(n) = n·freq·den/num is computed through `MulDiv64` with no drift. It was tested over 10 hours of frames at 24, 25, 30, 50, 60, 120, 144, 240, 23.976, 29.97 and 59.94 fps with several timer frequencies, with no overflow. `make_video.bat` encodes with `-framerate N/D … -r N/D`, so there are no added or dropped frames.
- **No drops:** the encode queue blocks instead of dropping. A failed capture retries the same step up to 3 times without advancing the frame index.
- **Saving:** JPEGs are written to a temp file and then renamed. The F10 self-test checks for missing, empty, undecodable and leftover temp files, plus repeated images.
- **Capture paths:** D3D11 and D3D10 (copy, MSAA resolve, blocking Map), D3D12 (swapchain queue, PRESENT→COPY_SOURCE→PRESENT barriers, waits on a fence before the game may continue), D3D9 (GetRenderTargetData, MSAA via StretchRect), OpenGL (glReadPixels from the back buffer with pack/FBO/PBO state saved and restored) and Vulkan (layer present hook using the same step functions, with the DXGI hooks standing down). They follow the same rule: **the frame is in CPU memory before game time advances.**

## Known remaining limits (not fixable from this sandbox)
1. Nothing has been compiled. Expect a first round of compile errors on Windows. The handoff already flags these risk areas: `#undef CreateSemaphore`, the Vulkan-Headers include path, `/MT` with MinHook, `GetBinaryTypeW`, the `Hk_CSC*` typedefs, and `FL_VkLayerPresenting` linkage.
2. Games that time themselves with rdtsc, audio clocks or GPU timestamps can't be slowed from user mode. The log prints a heuristic warning for this.
3. If a capture fails and is retried, that one frame may be up to one step late. It's very rare and is logged.
4. The F10 test marks repeated images as FAIL for static or paused scenes. This is expected; the message explains it.
5. The adaptive hold only measures the presenting thread. Engines that read time on a separate simulation thread keep the old 20 ms minimum.

## Files changed
- `FrameLock/src/hook/dllmain.cpp`: fixes 1–4, marked `review5`.
- `FrameLock/tests/clock_test.cpp`: **new** host-side test of the clock math and simulation.
- `REVIEW5_REPORT.md`: this report. `review5.diff` is the full code diff.
- `BUILD_AND_STATUS.md`: Review 5 section added.

## Test results (Linux, g++ 11.5)
`g++ -O2 -std=c++17 FrameLock/tests/clock_test.cpp && ./a.out` → **ALL CHECKS PASSED**. This covers exact step positions over 10 hours at 14 FPS/timer combinations, no drift, step jitter ≤ 1 tick, monotonic time, and clamping at B(n+1).

## Windows checklist (to finish the job)
1. Push to GitHub, or run `FrameLock\build.bat` from a VS 2022 x64 Developer Prompt. Build both x64 and Win32 (32-bit games need the 32-bit DLL). Fix any compile errors (see limit 1).
2. For each API, use one small sample app or game. Suggestions: DX9/10/11/12 SDK samples, a Vulkan cube sample (start it with **Launch game**) and an OpenGL GLFW sample.
   1. Set FPS to 60, attach, and press **F10** (120-frame self-test).
   2. You should see **TEST PASS** and 120 files, frame_000000 to frame_000119.
   3. Also check that the log shows step 0..120 and `read back in … ms`.
3. Speed check: record 10 s of an in-game clock or animation at 30 fps. Run `make_video.bat`. The video should be exactly 10.000 s long, and its timing should match the in-game clock.
4. Weak-hardware check: limit the CPU (for example with BES or by setting process affinity to 1 core), repeat step 3 and look for stutter.
5. Stop test: press F9 while an overlay window such as Discord or Steam is presenting. The log must not show time going backwards, and the take ends cleanly.
