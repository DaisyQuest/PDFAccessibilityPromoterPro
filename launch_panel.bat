@echo off
setlocal enabledelayedexpansion

if not exist job_queue_http.exe if not exist job_queue_http (
  echo Build the server binaries first.
  exit /b 1
)
if not exist job_queue_cli.exe if not exist job_queue_cli (
  echo Build the CLI binary first.
  exit /b 1
)

set "ROOT_DIR=%TEMP%\pap_panel_%RANDOM%"
set "PORT=8080"
set "BIND_ADDR=127.0.0.1"
set "TOKEN=%JOB_QUEUE_TOKEN%"

mkdir "%ROOT_DIR%" >nul

if exist job_queue_cli.exe (
  job_queue_cli.exe init "%ROOT_DIR%" >nul
) else (
  job_queue_cli init "%ROOT_DIR%" >nul
)

set "HTTP_BIN=job_queue_http"
if exist job_queue_http.exe set "HTTP_BIN=job_queue_http.exe"

if not "%TOKEN%"=="" (
  for /f "delims=" %%P in ('powershell -NoProfile -Command "^$p=Start-Process -FilePath .\%HTTP_BIN% -ArgumentList '"%ROOT_DIR%"','%PORT%','--bind','%BIND_ADDR%','--token','%TOKEN%' -PassThru -WindowStyle Hidden; ^$p.Id"') do set "SERVER_PID=%%P"
) else (
  for /f "delims=" %%P in ('powershell -NoProfile -Command "^$p=Start-Process -FilePath .\%HTTP_BIN% -ArgumentList '"%ROOT_DIR%"','%PORT%','--bind','%BIND_ADDR%' -PassThru -WindowStyle Hidden; ^$p.Id"') do set "SERVER_PID=%%P"
)

timeout /t 1 /nobreak >nul

set "PANEL_URL=http://%BIND_ADDR%:%PORT%/panel"
if not "%TOKEN%"=="" set "PANEL_URL=%PANEL_URL%?token=%TOKEN%"

echo Job queue root: %ROOT_DIR%
echo Monitoring panel: %PANEL_URL%
start "" "%PANEL_URL%"
echo Press any key to stop the server.
pause >nul

if defined SERVER_PID (
  powershell -NoProfile -Command "Stop-Process -Id %SERVER_PID% -Force" >nul 2>&1
)

rmdir /s /q "%ROOT_DIR%" >nul 2>&1

endlocal
