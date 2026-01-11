$ErrorActionPreference = "Stop"

$httpBinary = if (Test-Path "./job_queue_http.exe") { "./job_queue_http.exe" } elseif (Test-Path "./job_queue_http") { "./job_queue_http" } else { $null }
$cliBinary = if (Test-Path "./job_queue_cli.exe") { "./job_queue_cli.exe" } elseif (Test-Path "./job_queue_cli") { "./job_queue_cli" } else { $null }

if (-not $httpBinary -or -not $cliBinary) {
    Write-Host "Build the server binaries first."
    exit 1
}

$rootDir = Join-Path $env:TEMP ("pap_demo_{0}" -f (Get-Random))
$port = 8090
$uuid = "123e4567-e89b-12d3-a456-426614174000"
$baseUrl = "http://127.0.0.1:$port"

New-Item -ItemType Directory -Path $rootDir -Force | Out-Null
& $cliBinary init $rootDir | Out-Null

$inbox = Join-Path $rootDir "inbox"
New-Item -ItemType Directory -Path $inbox -Force | Out-Null
"%PDF-1.4 demo file" | Set-Content -Path (Join-Path $inbox "demo.pdf") -Encoding Ascii
"{""title"":""Demo PDF"",""source"":""demo.ps1""}" | Set-Content -Path (Join-Path $inbox "demo.metadata.json") -Encoding Ascii

$server = Start-Process -FilePath $httpBinary -ArgumentList @($rootDir, $port) -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 1

try {
    Write-Host "Health check:"
    & curl.exe -sS "$baseUrl/health"

    Write-Host "Submit job:"
    & curl.exe -sS "$baseUrl/submit?uuid=$uuid&pdf=inbox/demo.pdf&metadata=inbox/demo.metadata.json"

    Write-Host "Status (queued):"
    & curl.exe -sS "$baseUrl/status?uuid=$uuid"

    Write-Host "Claim job:"
    & curl.exe -sS "$baseUrl/claim?prefer_priority=0"

    Write-Host "Finalize job:"
    & curl.exe -sS "$baseUrl/finalize?uuid=$uuid&from=jobs&to=complete"

    Write-Host "Status (complete):"
    & curl.exe -sS "$baseUrl/status?uuid=$uuid"

    Write-Host "Retrieve metadata:"
    & curl.exe -sS "$baseUrl/retrieve?uuid=$uuid&state=complete&kind=metadata"
} finally {
    if ($server) {
        Stop-Process -Id $server.Id -Force
    }
    Remove-Item -Path $rootDir -Recurse -Force
}

Write-Host "Demo complete."
