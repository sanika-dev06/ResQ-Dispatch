@echo off
rem One-time setup: installs vcpkg (the C++ package manager) and sets the VCPKG_ROOT variable.
rem Needs Git: https://git-scm.com/download/win
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\vcpkg.exe" goto have
if exist "C:\vcpkg\vcpkg.exe" (
  set "VCPKG_ROOT=C:\vcpkg"
  goto have
)
where git >nul 2>nul
if errorlevel 1 (
  echo Git is not installed. Install it from https://git-scm.com/download/win and run setup.bat again.
  exit /b 1
)
git clone https://github.com/microsoft/vcpkg "C:\vcpkg"
if errorlevel 1 exit /b 1
call "C:\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
if errorlevel 1 exit /b 1
set "VCPKG_ROOT=C:\vcpkg"
:have
setx VCPKG_ROOT "%VCPKG_ROOT%" >nul
echo vcpkg is ready at %VCPKG_ROOT%
echo If VS Code is open, close and reopen it once so it sees the new VCPKG_ROOT variable.
