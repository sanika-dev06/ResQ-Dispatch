@echo off
rem Starts the ResQ Dispatch server, then open http://localhost:8080
rem Optional settings (set before running):  RESQ_PORT=8080   RESQ_ADMIN_PASSWORD=...   RESQ_DB=resq.db
cd /d "%~dp0"
if not exist "build\Release\resq_server.exe" (
  echo The server is not built yet. Run build.bat first.
  exit /b 1
)
"build\Release\resq_server.exe"
