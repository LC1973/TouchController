# Fetch and display rotator log
$raw = (Invoke-WebRequest "http://192.168.2.100/log" -TimeoutSec 8 -UseBasicParsing).Content
$clean = $raw -replace '<br\s*/?>', "`n" -replace '<[^>]+>','' -replace '&gt;','>' -replace '&lt;','<' -replace '&amp;','&'
$lines = $clean -split "`n" | Where-Object { $_.Trim() -ne '' }
Write-Host "=== ROTATOR LOG (last 60 lines) ==="
$lines | Select-Object -Last 60

# Also try the rotator JSON status
Write-Host ""
Write-Host "=== ROTATOR /api/rotator/status ==="
try {
    (Invoke-WebRequest "http://192.168.2.100/api/rotator/status" -TimeoutSec 4 -UseBasicParsing).Content
} catch {
    Write-Host "FAIL: $_"
}
