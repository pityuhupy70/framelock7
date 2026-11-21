> **Historical Review4 handoff — superseded.** Use [`FINAL_AUDIT_REPORT.md`](FINAL_AUDIT_REPORT.md) and [`FrameLock/README.md`](FrameLock/README.md) for the current source audit and build instructions.

# FrameLock - phase review4 handoff

Starting point: FrameLock_phase_review3.zip. Result: FrameLock_phase_review4.zip (same layout).

## Honest status
- NOTHING in review4 was compiled, linked or run. The sandbox had no MSVC, no Windows SDK, no Vulkan headers, no
  MinGW/clang, no root and no internet (GitHub Actions could not be triggered from here either).
- Only verification: a static check that brackets, braces and parentheses balance in launcher/main.cpp, hook/dllmain.cpp
  and hook/vk_layer.cpp (comments, string and char literals stripped). All three are balanced.
- The AI that did review4 could not see its own credit or token balance, so it packaged early.

## Changes in review4

### 1. Vulkan child-process initialization (priority 2) - real fix, untested
Problem found: review3 did NOT fix this. FRAMELOCK_VK_CAPTURE=1 is inherited by every child of the launched game.
In a child, the Vulkan loader loads FrameLockHook*.dll, and DllMain -> InitThread then created its own shared memory,
installed the QPC/GetTickCount/timeGetTime hooks, started ControlThread (F9/F10 hotkeys) and hooked DXGI/D3D9/GL.
So a crash reporter, shader compiler or the real game behind a launcher stub could start its own take or step its own clock.

Fix:
- `dllmain.cpp` `SharedExists()` + `InitThread`: the core initialises only if `Local\FrameLock_<pid>` already exists.
  Otherwise the DLL is dormant: no hooks, no threads, and the Vulkan layer passes through (`FrameStepPre` returns 0
  when `g_sh` is null). It polls every 500 ms and initialises if FrameLock attaches to that PID later. Attach creates
  the mapping, and LoadLibrary on an already loaded DLL only bumps the refcount.
- `launcher/main.cpp` `LaunchGame()`: the game is created with `CREATE_SUSPENDED`. `OpenShared(pid)` runs, then `ResumeThread`.
  This guarantees the mapping exists before the layer can load in the launched process.
- Side benefit: Vulkan games behind a launcher stub become capturable. Start the stub with "Launch game", then Attach
  to the real game process. The child already has the layer because it inherited the env var.
- Attach to an already running game is unchanged: `Attach()` already calls `OpenShared` before `Inject`.

### 2. Both layer architectures registered (priority 3) - real change, untested
- `RegisterVulkanLayer()` now writes and registers `FrameLock_VkLayer64.json` and `FrameLock_VkLayer32.json` whenever
  the matching DLL is next to the launcher. The launcher's own bitness is required; the other bitness is best effort.
- Manifest `file_format_version` 1.2.1 with `"library_arch": "64"` or `"32"`. Current loaders skip the
  wrong-bitness manifest. Older loaders try LoadLibrary, fail with ERROR_BAD_EXE_FORMAT and skip the implicit layer.
  (Behaviour on old loaders that see an unknown newer minor format version is believed to be warn-and-continue.
  VERIFY with VK_LOADER_DEBUG=all.)
- Stale-registry cleanup now covers both manifest names and never deletes the two current paths.
- `LaunchGame()` checks the exe with `GetBinaryTypeW`. On a bitness mismatch it explains that the other launcher
  must be used for D3D/GL injection, and offers "start anyway" for launcher stubs.
- Limitation (unchanged and by design): a 64-bit launcher cannot inject into a 32-bit game. Clock hooks need
  injection or the Vulkan layer, and controlling the take needs the same-bitness launcher or the shared memory.
  The FLShared layout is identical on both bitnesses, but this was not verified at runtime.

### 3. CI / packaging (priority 1) - workflow edited, never run
`.github/workflows/build.yml`:
- Extra diagnostic leg: x64 with `-DFL_ENABLE_VULKAN=OFF`. It separates Vulkan compile errors from the rest of the project.
- Collects only FrameLock*.exe and FrameLockHook*.dll from `Release` folders. Fails if either is missing.
- `ilammy/msvc-dev-cmd` + `dumpbin`. Fails if the hook DLL lacks the undecorated `vkNegotiateLoaderLayerInterfaceVersion`
  export, if the export is decorated (Win32 __stdcall), or if the machine type is wrong.
- New `package` job. It merges the x64 and Win32 Vulkan-ON artifacts into one `FrameLock-dist` artifact with both exes,
  both DLLs, the README and the status doc. Both launchers need both DLLs next to them for dual-layer registration.

## Not done (in order of what to do next)
1. Push to GitHub and run Actions. Fix the compile, link and packaging errors it reports. Start with the vkOFF leg log.
   Likely risk areas, by inspection only:
   - the `CreateSemaphore` #undef in vk_layer.cpp;
   - the Vulkan-Headers FetchContent include path;
   - the static-CRT /MT setting matching MinHook;
   - the `GetBinaryTypeW` declaration (winbase.h, should be fine with WIN32_LEAN_AND_MEAN).
2. On Windows, test the child-process fix:
   - launch a Vulkan app that spawns a child;
   - check `%TEMP%\FrameLock_<childpid>.log`, which should say "dormant";
   - check that F9 in the child does nothing.
3. On Windows, test dual registration:
   - `reg query HKCU\Software\Khronos\Vulkan\ImplicitLayers` should show both json files;
   - run `VK_LOADER_DEBUG=layer` on a 32-bit vkcube launched from FrameLock64 (start anyway), then Attach from FrameLock32.
4. NVIDIA tests (priority 4): Vulkan capture, the sticky DXGI stand-down, 4K, and a slow disk. Backpressure in
   FL_QueueFrame must block, never drop.
5. D3D12 (priority 5): review `RememberSwapchainQueue` / `LookupSwapchainQueue` / `SubmitD3D12` in dllmain.cpp
   (around lines 580-735). Not reviewed in review4. D3D12 MSAA (priority 6): flip-model swapchains require
   SampleDesc.Count = 1, so the back buffer is never MSAA. Nothing to resolve. Do not spend time on it.
6. Module split (priority 7): move the D3D9/10/11/12/GL capture code out of dllmain.cpp behind flcore.h, as
   vk_layer.cpp already is. Only do this after CI is green, so regressions are visible.
7. Runtime tests per API (priority 8) need a real Windows machine with a GPU.

## Do not redo
D3D10 path, opus55 null guards, review2 capOk, review3 sticky stand-down (never go back to a time window), review3
stale cleanup (extended here), D3D12 MSAA resolve (impossible), VEH rdtsc interception (impossible).
