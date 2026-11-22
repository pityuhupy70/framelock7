# Build fix applied after Actions run 44997458665

The source-level fixes below are based on the three MSVC build logs included by the user and the uploaded project headers.

1. `ClockState` was replaced with `flclock::StepState`, which is the actual type declared in `FrameLock/src/hook/clock_math.h`.
2. `d3d11_4.h` was included. The Windows SDK declares `ID3D11Multithread` in this header.
3. The invalid atomic-to-atomic assignment in the take-finalization exception path was replaced with an explicit `load()` and `store()`.

`FrameLock/src/shared.h` was inspected and not modified: `wantRecord` is correctly declared as `volatile LONG` and accessed with Win32 `Interlocked*` operations.

The portable clock tests were rerun after this patch. This environment does not contain the Windows SDK/MSVC compiler, so a Windows DLL compile, link, GitHub Actions pass, and GPU/runtime validation are **not claimed**. The next verification step is to replace `FrameLock/src/hook/dllmain.cpp` in the repository and rerun the Windows Actions matrix.
