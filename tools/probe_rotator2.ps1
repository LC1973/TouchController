$ROT = "http://192.168.2.100"

# Test enable/disable/stop endpoints
foreach ($ep in @("/api/rotator/enable", "/api/rotator/disable", "/api/rotator/stop")) {
    try {
        $r = Invoke-WebRequest "$ROT$ep" -Method POST -Body "{}" -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
        Write-Host "POST $ep => $($r.StatusCode): $($r.Content.Substring(0,[Math]::Min(120,$r.Content.Length)))"
    } catch [System.Net.WebException] {
        $code = [int]$_.Exception.Response.StatusCode
        $resp = ""
        try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
        Write-Host "POST $ep => $code : $resp"
    }
}

Write-Host ""

# Test goto with 'heading' field
try {
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body '{"heading":68}' -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "POST /api/rotator/goto heading:68 => $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "POST /api/rotator/goto heading:68 => $code : $resp"
}

# Test goto with 'position' field
try {
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body '{"position":68}' -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "POST /api/rotator/goto position:68 => $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "POST /api/rotator/goto position:68 => $code : $resp"
}

# Full /api/rotator GET status
Write-Host ""
Write-Host "=== Full /api/rotator status ==="
try {
    (Invoke-WebRequest "$ROT/api/rotator" -TimeoutSec 4 -UseBasicParsing).Content
} catch { "FAIL: $_" }

# Try /api/rotator/goto GET to see if it gives us a hint
Write-Host ""
Write-Host "=== GET /api/rotator/goto ==="
try {
    (Invoke-WebRequest "$ROT/api/rotator/goto" -TimeoutSec 4 -UseBasicParsing).Content
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "$code : $resp"
}
