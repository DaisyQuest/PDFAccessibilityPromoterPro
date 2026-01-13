$ErrorActionPreference = "Stop"

$httpBinary = if (Test-Path "./job_queue_http.exe") { "./job_queue_http.exe" } elseif (Test-Path "./job_queue_http") { "./job_queue_http" } else { $null }
$cliBinary = if (Test-Path "./job_queue_cli.exe") { "./job_queue_cli.exe" } elseif (Test-Path "./job_queue_cli") { "./job_queue_cli" } else { $null }

if (-not $httpBinary -or -not $cliBinary) {
    Write-Host "Build the server binaries first."
    exit 1
}

$rootDir = Join-Path $env:TEMP ("pap_panel_{0}" -f (Get-Random))
$port = 8080
$bindAddr = "127.0.0.1"
$token = $env:JOB_QUEUE_TOKEN

New-Item -ItemType Directory -Path $rootDir -Force | Out-Null
& $cliBinary init $rootDir | Out-Null

$serverArgs = @($rootDir, $port, "--bind", $bindAddr)
if ($token) {
    $serverArgs += "--token"
    $serverArgs += $token
}

$server = Start-Process -FilePath $httpBinary -ArgumentList $serverArgs -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1

$panelUrl = "http://$bindAddr:$port/panel"
if ($token) {
    $panelUrl = "$panelUrl?token=$token"
}

Write-Host "Job queue root: $rootDir"
Write-Host "Monitoring panel: $panelUrl"
Start-Process $panelUrl
Write-Host "Press Enter to stop the server."
[Console]::ReadLine() | Out-Null

if ($server) {
    Stop-Process -Id $server.Id -Force
}
Remove-Item -Path $rootDir -Recurse -Force
