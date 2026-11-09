@echo off
REM Builds InputLayoutGuard.dll (Release) and packages the release archive.
REM
REM   build.bat                 build + package into dist\package and InputLayoutGuard-<version>.zip
REM   build.bat "<mod folder>"  also copy the DLL (and the ini, if not there yet) into <mod folder>\SKSE\Plugins
REM
REM Needs: Visual Studio 2022 (C++ workload), CMake 3.26+, vcpkg with VCPKG_ROOT set,
REM        and the CommonLibSSE-NG submodule (git submodule update --init --recursive).
setlocal
cd /d "%~dp0"
if not defined VCPKG_ROOT (echo *** VCPKG_ROOT is not set *** & exit /b 1)
if not exist "extern\CommonLibSSE-NG\include\REL\Relocation.h" if not defined CommonLibSSE-NGPath (
    echo *** CommonLibSSE-NG missing: run  git submodule update --init --recursive *** & exit /b 1
)

if not exist build (
    cmake --workflow --preset release-msvc
) else (
    cmake --build build --config Release
)
if errorlevel 1 (echo *** BUILD FAILED *** & exit /b 1)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1"
if errorlevel 1 (echo *** PACKAGING FAILED *** & exit /b 1)

if "%~1"=="" goto :eof
if not exist "%~1\SKSE\Plugins" mkdir "%~1\SKSE\Plugins"
copy /Y "build\Release\InputLayoutGuard.dll" "%~1\SKSE\Plugins\InputLayoutGuard.dll"
if errorlevel 1 (echo *** COPY FAILED - is the game running? *** & exit /b 1)
if not exist "%~1\SKSE\Plugins\InputLayoutGuard.ini" copy /Y "dist\InputLayoutGuard.ini" "%~1\SKSE\Plugins\InputLayoutGuard.ini"
echo === DEPLOYED to %~1 ===
