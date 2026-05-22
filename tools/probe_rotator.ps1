$ROT = "http://192.168.2.100"

# Probe common API paths
$paths = @("/api/status","/api/rotator/status","/api/rotator","/api/goto",
           "/api/rotator/goto","/api/memory","/api/rotator/state","/api/state","/api/rotator/info")
Write-Host "=== Endpoint probe ==="
foreach ($path in $paths) {
    try {
        $r = Invoke-WebRequest "$ROT$path" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
        Write-Host "GET $path => $($r.StatusCode): $($r.Content.Substring(0,[Math]::Min(200,$r.Content.Length)))"
    } catch [System.Net.WebException] {
        $code = [int]$_.Exception.Response.StatusCode
        $body = ""
        try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $body = $sr.ReadToEnd() } catch {}
        Write-Host "GET $path => $code $body"
    } catch { Write-Host "GET $path => ERR: $_" }
}

# Test POST to goto with 'bearing' field
Write-Host ""
Write-Host "=== POST /api/rotator/goto {bearing:68} ==="
try {
    $body = '{"bearing":68}'
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body $body -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Test POST to goto with 'target' field (possible alternative)
Write-Host ""
Write-Host "=== POST /api/rotator/goto {target:68} ==="
try {
    $body = '{"target":68}'
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body $body -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Also fetch the rotator log tail
Write-Host ""
Write-Host "=== ROTATOR RECENT LOG ==="
try {
    $raw = (Invoke-WebRequest "$ROT/log" -TimeoutSec 6 -UseBasicParsing).Content
    $clean = $raw -replace '<br\s*/?>', "`n" -replace '<[^>]+>','' -replace '&gt;','>' -replace '&lt;','<' -replace '&amp;','&'
    $lines = $clean -split "`n" | Where-Object { $_.Trim() -ne "" }
    $lines | Select-Object -Last 30
} catch { Write-Host "Log fetch failed: $_" }
