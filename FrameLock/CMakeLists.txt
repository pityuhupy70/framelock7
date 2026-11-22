cmake_minimum_required(VERSION 3.20)
project(FrameLock LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")   # static CRT: no VC++ redist needed

# ---- MinHook (downloaded automatically at configure time)
include(FetchContent)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(minhook URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.3.zip)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
  FetchContent_Populate(minhook)
endif()
add_library(minhook_static STATIC
  ${minhook_SOURCE_DIR}/src/buffer.c
  ${minhook_SOURCE_DIR}/src/hook.c
  ${minhook_SOURCE_DIR}/src/trampoline.c
  ${minhook_SOURCE_DIR}/src/hde/hde32.c
  ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include)

# ---- Vulkan-Headers (header-only; for the Vulkan capture layer in vk_layer.cpp). OFF -> stub layer, no download.
option(FL_ENABLE_VULKAN "Build the Vulkan capture layer" ON)
if(FL_ENABLE_VULKAN)
  FetchContent_Declare(vulkan_headers URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.3.290.zip)
  FetchContent_GetProperties(vulkan_headers)
  if(NOT vulkan_headers_POPULATED)
    FetchContent_Populate(vulkan_headers)
  endif()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(FL_SUFFIX 64)
else()
  set(FL_SUFFIX 32)
endif()

# ---- hook DLL (injected into the game)
add_library(FrameLockHook SHARED src/hook/dllmain.cpp src/hook/vk_layer.cpp src/hook/flcore.h src/hook/present_gate.h src/hook/FrameLockHook.def)
if(FL_ENABLE_VULKAN)
  target_include_directories(FrameLockHook PRIVATE ${vulkan_headers_SOURCE_DIR}/include)
else()
  target_compile_definitions(FrameLockHook PRIVATE FL_NO_VULKAN)
endif()
set_target_properties(FrameLockHook PROPERTIES OUTPUT_NAME FrameLockHook${FL_SUFFIX})
target_compile_definitions(FrameLockHook PRIVATE UNICODE _UNICODE)
target_compile_options(FrameLockHook PRIVATE /O2 /utf-8 /W3)
target_link_libraries(FrameLockHook PRIVATE minhook_static ole32 oleaut32 windowscodecs uuid dxguid user32 gdi32 shell32)

# ---- launcher GUI
add_executable(FrameLock WIN32 src/launcher/main.cpp)
set_target_properties(FrameLock PROPERTIES OUTPUT_NAME FrameLock${FL_SUFFIX})
target_compile_definitions(FrameLock PRIVATE UNICODE _UNICODE)
target_compile_options(FrameLock PRIVATE /O2 /utf-8 /W3)
target_link_libraries(FrameLock PRIVATE user32 gdi32 shell32 comdlg32 ole32 advapi32)

# Portable deterministic tests: these validate clock arithmetic and the save-before-advance
# state-machine contract. They do not substitute for Windows, GPU, or game-runtime testing.
if(BUILD_TESTING)
  find_package(Threads REQUIRED)
  add_executable(FrameLockClockTests tests/clock_test.cpp)
  target_compile_features(FrameLockClockTests PRIVATE cxx_std_17)
  target_link_libraries(FrameLockClockTests PRIVATE Threads::Threads)
  add_test(NAME FrameLockClockTests COMMAND FrameLockClockTests)
endif()
