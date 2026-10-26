@echo off
rem Builds the server (Release). The first build downloads and compiles Crow + SQLite: allow 5-10 minutes.
cd /d "%~dp0"

rem ---- 1. CMake present?
where cmake >nul 2>nul
if errorlevel 1 (
  echo CMake was not found. Install it from https://cmake.org/download/ and tick "Add CMake to the PATH".
  echo Then close and reopen VS Code and run the build again.
  exit /b 1
)

rem ---- 2. Visual Studio with the C++ tools present?
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novs
set "VSPATH="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if defined VSPATH goto vsok
:novs
echo Visual Studio C++ build tools were not found.
echo Install "Visual Studio 2022 Build Tools" and tick the "Desktop development with C++" workload:
echo   https://visualstudio.microsoft.com/downloads/
exit /b 1
:vsok

rem ---- 3. vcpkg present? (installed automatically if missing; needs Git)
if defined VCPKG_ROOT goto havevcpkg
if exist "C:\vcpkg\vcpkg.exe" (
  set "VCPKG_ROOT=C:\vcpkg"
  goto havevcpkg
)
call setup.bat
if errorlevel 1 exit /b 1
set "VCPKG_ROOT=C:\vcpkg"
:havevcpkg

rem ---- 4. configure + build
cmake --preset windows
if errorlevel 1 exit /b 1
cmake --build --preset release
if errorlevel 1 exit /b 1
echo.
echo Build finished. Run run.bat (or the task "ResQ: Run server") and open http://localhost:8080
