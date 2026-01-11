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

set "ROOT_DIR=%TEMP%\pap_demo_%RANDOM%"
set "PORT=8090"
set "UUID=123e4567-e89b-12d3-a456-426614174000"
set "BASE_URL=http://127.0.0.1:%PORT%"

mkdir "%ROOT_DIR%" >nul
if exist job_queue_cli.exe (
  job_queue_cli.exe init "%ROOT_DIR%" >nul
) else (
  job_queue_cli init "%ROOT_DIR%" >nul
)

mkdir "%ROOT_DIR%\inbox" >nul
echo %%PDF-1.4 demo file>"%ROOT_DIR%\inbox\demo.pdf"
echo {"title":"Demo PDF","source":"demo.bat"}>"%ROOT_DIR%\inbox\demo.metadata.json"

for /f "delims=" %%P in ('powershell -NoProfile -Command "^$p=Start-Process -FilePath (Get-Command .\\job_queue_http.exe -ErrorAction SilentlyContinue ^| Select-Object -ExpandProperty Source) -ArgumentList '\"%ROOT_DIR%\"','%PORT%' -PassThru -WindowStyle Hidden; if (-not ^$p) { ^$p=Start-Process -FilePath .\\job_queue_http -ArgumentList '\"%ROOT_DIR%\"','%PORT%' -PassThru -WindowStyle Hidden }; ^$p.Id"') do set "SERVER_PID=%%P"

timeout /t 1 /nobreak >nul

echo Health check:
curl.exe -sS "%BASE_URL%/health"

echo Submit job:
curl.exe -sS "%BASE_URL%/submit?uuid=%UUID%^&pdf=inbox/demo.pdf^&metadata=inbox/demo.metadata.json"

echo Status (queued):
curl.exe -sS "%BASE_URL%/status?uuid=%UUID%"

echo Claim job:
curl.exe -sS "%BASE_URL%/claim?prefer_priority=0"

echo Finalize job:
curl.exe -sS "%BASE_URL%/finalize?uuid=%UUID%^&from=jobs^&to=complete"

echo Status (complete):
curl.exe -sS "%BASE_URL%/status?uuid=%UUID%"

echo Retrieve metadata:
curl.exe -sS "%BASE_URL%/retrieve?uuid=%UUID%^&state=complete^&kind=metadata"

if defined SERVER_PID (
  powershell -NoProfile -Command "Stop-Process -Id %SERVER_PID% -Force" >nul 2>&1
)

rmdir /s /q "%ROOT_DIR%" >nul 2>&1

echo Demo complete.
endlocal
