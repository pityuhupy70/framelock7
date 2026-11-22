# FrameLock Review6 — LOCKED-mode timing and synchronization repair

## Scope

This repair was applied to the uploaded source tree without replacing the project architecture. The changes focus on the LOCKED-mode Present/clock transaction, the final-frame boundary, and reliable interpretation of the QPC hook state.

## Confirmed source findings

1. **The Present transaction was not atomic across render threads.** `FrameStepPre()` held `g_stepMx` only while it was executing. The lock was released before the original graphics `Present` call, and `FrameStepPost()` ran without that lock. A second render thread could therefore enter another `FrameStepPre()` before the first thread advanced the virtual clock. This left a window in which frame indexing, capture, and clock-step progression could overlap.
2. **The final committed frame advanced one additional clock boundary.** `FrameStepPost()` advanced after every captured frame, including the last one. The supplied `FrameLock_19552.log` reports 123 committed frames and 124 completed clock steps. The one warm-up boundary is intentional; the additional post-final-frame advance was not.
3. **MinHook status 5 was misreported as a hook failure.** In MinHook 1.3.3, enum value 5 is `MH_ERROR_ENABLED` (“already enabled”), not `MH_ERROR_UNSUPPORTED_FUNCTION`. The old helper logged every non-`MH_OK` enable result as failure. This status alone does not prove that the QPC entrypoint was unhookable or that wall-clock QPC leaked through.
4. **QPC forwarding needed to be evaluated before inline hooks changed the entry bytes.** Resolving a forwarding stub after MinHook patches the destination can follow the detour rather than the original export chain, producing a false “not covered” result.

## Changes made

- Added `FrameLock/src/hook/present_gate.h`, a process-wide sequence gate.
- Kept the gate held across `FrameStepPre()` → the original graphics Present call → `FrameStepPost()` for DXGI/D3D9/OpenGL and the Vulkan layer. The ControlThread stop/release check uses the same gate with a non-blocking try-lock.
- Changed LOCKED-mode clock advancement so a successfully committed final frame holds its boundary when the stop request is already visible in `FrameStepPost`. The startup warm-up step is preserved. If F9 arrives just after that post-step advance, the code permits at most one pending uncommitted boundary, logs that it is discarded on stop, and excludes it from the saved frame sequence; larger mismatches fail the take.
- Corrected MinHook status handling so `MH_ERROR_ENABLED` is recognized as “already enabled.”
- Cached kernelbase/kernel32 QPC forwarding targets before installing hooks, and treat an export as covered when its original tail-jump chain resolves to the hooked `RtlQueryPerformanceCounter`.
- Hardened the QPC IAT scan with PE directory bounds checks and only redirects slots that still point directly to the resolved system QPC exports (including modules without `OriginalFirstThunk`). It does not overwrite an unrelated third-party IAT detour.
- Changed generated `make_video.bat` to set the exact rational FPS on the image-sequence input and removed the redundant output `-r` resampling option; the output now preserves the source frame timestamps rather than invoking an extra rate-conversion stage.
- Added portable tests for cross-thread Present serialization, the stop-monitor try-lock behavior, the 123-frame final-boundary case, and the one-boundary late-stop race.
- Added the new gate header to the CMake target source list.

## Files changed

- `FrameLock/src/hook/dllmain.cpp`
- `FrameLock/src/hook/present_gate.h` (new)
- `FrameLock/tests/clock_test.cpp`
- `FrameLock/CMakeLists.txt`
- `FIX_REPORT_REVIEW6.md` (this report)

## Tests run in this environment

- GCC C++17 portable test build with `-Wall -Wextra -Wpedantic -Werror`: **PASS**.
- Clang C++17 portable test build with AddressSanitizer + UndefinedBehaviorSanitizer and `-Wall -Wextra -Wpedantic -Werror`: **PASS**.
- The tests cover the portable clock arithmetic, rational FPS schedules, immutable clock reads, save/commit gating, slow-save behavior, failed-commit behavior, multi-thread Present-gate serialization, and the no-extra-final-step case.

## Not verified here / remaining limitations

- This environment has no Windows SDK, MSVC toolchain, or game GPU runtime. The Windows hook DLL was **not compiled or run here**. A Windows x64/Win32 build and the F10 self-test plus a real LOCKED take are still required before calling the repair fully runtime-verified.
- A generic user-mode graphics hook cannot prove that every game engine performs exactly one internal simulation update per Present. Games with independent simulation threads, custom engine timers, direct system calls, `RDTSC`/`RDTSCP`, `KUSER_SHARED_DATA`, audio/GPU clocks, or other unhooked time sources may still advance independently. Such a game needs engine-specific integration or a lower-level clock virtualization mechanism.
- The log's heuristic scan found 15 `RDTSC` byte patterns in about 17 MB of executable code. That count by itself does not establish that the game uses TSC for simulation timing.
- The QPC IAT scan covers ordinary imports in loaded modules; delay-load tables and arbitrary cached function pointers are not comprehensively rewritten. Direct calls through the hooked system exports are covered by the inline hooks, but alternate timing paths remain a limitation.
- The supplied log's 123 committed images at 60 FPS represent a nominal video duration of 2.05 seconds. Taking 15 seconds of real time to produce those frames is expected in offline/LOCKED mode; it is not by itself evidence that the output is sped up. The 123-versus-124 count is consistent with one pending boundary after a late user stop as well as with the old unconditional final advance; the repair now suppresses that advance when stop is already visible and reports/discards at most one already-pending boundary without treating it as an exported frame.

## Required Windows validation

1. Build x64 and Win32 with Vulkan ON and OFF where supported.
2. Launch the game through FrameLock before swapchain creation where required (especially D3D12 queue association and Vulkan layer activation).
3. Run the F10 120-frame self-test and inspect `test_report.txt` and the runtime log.
4. Run a longer LOCKED take at a known FPS; confirm committed-frame count matches the final clock boundary, allowing only the single explicitly logged pending boundary if F9 arrives after that boundary has already advanced. Confirm `take_info.txt` / `make_video.bat` preserve the exact rational FPS.
5. Confirm that the actual game simulation states progress one step per committed frame. This last point is game-engine-dependent and cannot be established by frame counts alone.
