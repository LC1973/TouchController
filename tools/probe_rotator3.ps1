$ROT = "http://192.168.2.100"

# Try form-encoded body
Write-Host "=== POST /api/rotator/goto form-encoded bearing=68 ==="
try {
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body "bearing=68" -ContentType "application/x-www-form-urlencoded" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Try form-encoded with heading
Write-Host ""
Write-Host "=== POST /api/rotator/goto form-encoded heading=68 ==="
try {
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body "heading=68" -ContentType "application/x-www-form-urlencoded" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Try form-encoded with target
Write-Host ""
Write-Host "=== POST /api/rotator/goto form-encoded target=68 ==="
try {
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body "target=68" -ContentType "application/x-www-form-urlencoded" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Try PUT to /api/rotator with bearing
Write-Host ""
Write-Host "=== PUT /api/rotator {bearing:68} ==="
try {
    $r = Invoke-WebRequest "$ROT/api/rotator" -Method PUT -Body '{"bearing":68}' -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Try POST to /api/rotator with bearing
Write-Host ""
Write-Host "=== POST /api/rotator {bearing:68} ==="
try {
    $r = Invoke-WebRequest "$ROT/api/rotator" -Method POST -Body '{"bearing":68}' -ContentType "application/json" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Try query-string approach
Write-Host ""
Write-Host "=== POST /api/rotator/goto?bearing=68 ==="
try {
    $r = Invoke-WebRequest "$ROT/api/rotator/goto?bearing=68" -Method POST -Body "" -TimeoutSec 3 -UseBasicParsing -ErrorAction Stop
    Write-Host "=> $($r.StatusCode): $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "=> $code : $resp"
}

# Try the rotator's root page for any clues
Write-Host ""
Write-Host "=== GET / (root title/heading) ==="
try {
    $raw = (Invoke-WebRequest "$ROT/" -TimeoutSec 4 -UseBasicParsing).Content
    $raw -replace '<[^>]+>','' -split "`n" | Where-Object { $_.Trim() -ne "" } | Select-Object -First 10
} catch { "FAIL" }
