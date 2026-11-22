# FrameLock Build Fix Notes — follow-up to Actions run 44997458665

## Latest uploaded Actions logs

All three jobs (x64/Vulkan ON, x64/Vulkan OFF, and Win32/Vulkan ON) stop in `dllmain.cpp` with the same MSVC error:

`C1189` from `d3d10_1.h`: `d3d10.h` was included before `d3d10_1.h`.

There is exactly one compiler error recorded in each of the three build logs. The build stops at this preprocessing error, so the logs do not establish whether later compilation stages will reveal additional errors.

## Fix applied

Reordered the Direct3D 10 headers in `FrameLock/src/hook/dllmain.cpp` so that:

1. `#include <d3d10_1.h>`
2. `#include <d3d10.h>`

The remaining previously applied source fixes are retained:
- `flclock::StepState` is used instead of the undeclared `ClockState`.
- `d3d11_4.h` is included for `ID3D11Multithread`.
- atomic-to-atomic transfer uses explicit `load()` and `store()`.

`shared.h` was inspected and remains unchanged.

## Verification status

The three latest MSVC logs were inspected and the include-order error was reproduced directly from the checked-in source ordering. The include-order correction is applied in this package. This environment does not provide the Windows SDK/MSVC compiler, so a Windows build or a passing GitHub Actions run is not claimed. Re-run the same Actions matrix after committing this update; later compile errors, if any, will need to be handled based on the new logs.
