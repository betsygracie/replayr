@echo off
setlocal
cd /d "%~dp0"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [Replayr] CMake was not found.
    echo Install Visual Studio 2022 or newer with the "Desktop development with C++" workload,
    echo then run this file from the "x64 Native Tools Command Prompt for VS".
    exit /b 1
)

echo [Replayr] Configuring...
cmake -S . -B build
if errorlevel 1 goto :fail

echo [Replayr] Building Release...
cmake --build build --config Release --parallel
if errorlevel 1 goto :fail

echo.
if exist "build\Release\Replayr.exe" (
    echo [Replayr] Done: build\Release\Replayr.exe
) else (
    echo [Replayr] Done. Look for Replayr.exe under the build folder.
)
endlocal
exit /b 0

:fail
echo.
echo [Replayr] Build failed. Scroll up for the first error.
endlocal
exit /b 1
