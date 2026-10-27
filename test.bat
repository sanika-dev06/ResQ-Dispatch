@echo off
rem Runs the automated tests (needs build.bat to have run first).
cd /d "%~dp0"
"build\Release\test_core.exe"
if errorlevel 1 exit /b 1
"build\Release\test_service.exe"
