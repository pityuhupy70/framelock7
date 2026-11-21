# Build and current status

The current source audit and repairs are documented in [`FINAL_AUDIT_REPORT.md`](FINAL_AUDIT_REPORT.md). User setup and build instructions are in [`FrameLock/README.md`](FrameLock/README.md).

## Current disposition

- The Review5 source has received targeted timing, capture-commit, encoder finalization, DXGI, Vulkan, FPS parsing, test, CI, and documentation repairs.
- Portable g++ clock/parser/model tests passed in the Linux sandbox.
- **Windows x64/x86 builds have not been run or verified.**
- **GitHub Actions has not been triggered.**
- **No Direct3D, Vulkan, or OpenGL game/GPU runtime test has been performed.**
- Weak-hardware, real slow-storage, high-resolution and long-recording stress tests remain outstanding.

This is not a certified finished release. The report separates portable tests from compile/runtime verification and lists the release gates that remain.
