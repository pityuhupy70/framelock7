@echo off
REM Builds FrameLock (64-bit and 32-bit) with Visual Studio 2022 + CMake.
REM Requirements: Visual Studio 2022 with "Desktop development with C++" (includes CMake + Windows SDK), internet once (MinHook download).
setlocal
cd /d "%~dp0"
where cmake >nul 2>nul
if errorlevel 1 (
  for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VSPATH=%%i
  call "%VSPATH%\Common7\Tools\VsDevCmd.bat" -arch=amd64 >nul
)
cmake -S . -B build64 -G "Visual Studio 17 2022" -A x64 -DFL_ENABLE_VULKAN=ON || goto :err
cmake --build build64 --config Release || goto :err
ctest --test-dir build64 -C Release --output-on-failure || goto :err
cmake -S . -B build32 -G "Visual Studio 17 2022" -A Win32 -DFL_ENABLE_VULKAN=ON || goto :err
cmake --build build32 --config Release || goto :err
ctest --test-dir build32 -C Release --output-on-failure || goto :err
if not exist dist mkdir dist
copy /y build64\Release\FrameLock64.exe dist\ >nul
copy /y build64\Release\FrameLockHook64.dll dist\ >nul
copy /y build32\Release\FrameLock32.exe dist\ >nul
copy /y build32\Release\FrameLockHook32.dll dist\ >nul
echo.
echo ==== Done. Program is in the "dist" folder. Run FrameLock64.exe for 64-bit games. ====
pause
exit /b 0
:err
echo BUILD FAILED
pause
exit /b 1
