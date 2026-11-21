// FrameLock core interface shared by per-API capture modules (phase opus55).
//
// This is the first step of the ICapture split: the clock/take/encode core stays in dllmain.cpp, and capture
// back-ends that live in their own translation unit (currently: vk_layer.cpp) talk to it ONLY through the
// functions below. Contract (unchanged v3 timing model):
//   * FL_StepPre() must be called right before the original present, FL_StepPost() right after it.
//   * The capture callback is invoked (under the core's step mutex) only while a take is active. It must read the
//     completed frame back into CPU memory SYNCHRONOUSLY and hand it to FL_QueueFrame() before returning true.
//   * Returning true means the frame was read back, encoded, flushed and committed to its final filename.
//   * Returning false is a fatal take error: the core stops immediately and does not advance its frame index or
//     virtual clock. It does NOT retry, because engines may advance simulation independently of hooked clocks.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstddef>
#include <vector>

// CPU pixel layouts understood by the encoder (Convert() in dllmain.cpp).
enum class PF { BGRA8, RGBA8, RGB10A2, RGBA16F, RGBA16F_SRGB, D9_A2R10G10B10, BGR565, BGR24 };

// bool cap(ctx, frameIndex, verboseLogging, outStage, outHr)
typedef bool (*FLCaptureFn)(void* ctx, int64_t idx, bool verbose, const wchar_t** stage, HRESULT* hr);

// Present bracketing. FL_BeginPresent() returns false for a nested present on the same thread (e.g. a present
// hook reached twice through layered/loader trampolines); in that case call the original present directly and do
// NOT call FL_StepPre/FL_StepPost/FL_EndPresent.
bool FL_BeginPresent();
void FL_EndPresent();
int  FL_StepPre(void* target, FLCaptureFn cap, void* ctx);   // returns the code to pass to FL_StepPost
void FL_StepPost(int r);

// Hand a frame already in CPU memory to the encoder and WAIT until the final JPEG has been
// flushed and atomically renamed. False means no simulation step may advance.
bool FL_QueueFrame(int64_t idx, int w, int h, PF pf, size_t pitch, std::vector<uint8_t>&& raw,
                   const wchar_t** outStage, HRESULT* outHr);
size_t FL_BytesPerPixel(PF pf);

void    FL_Log(const wchar_t* fmt, ...);
void    FL_SetApi(const wchar_t* s);
int64_t FL_RealQPC();
double  FL_RealMs(int64_t ticks);

// Implemented in vk_layer.cpp: true once the Vulkan layer has intercepted vkCreateInstance in this process.
bool FL_VkLayerActive();
// Implemented in vk_layer.cpp (review3): true once the layer has seen a vkQueuePresentKHR in this process (sticky,
// never cleared). The DXGI Present hooks then call the original present directly, so a Vulkan game whose driver
// presents through DXGI on another thread cannot be stepped/captured twice, even while one Vulkan present is
// blocked for > 1 s inside the capture. Stub build (FL_NO_VULKAN): always false (the layer never runs).
bool FL_VkLayerPresenting();
