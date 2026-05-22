$ROT = "http://192.168.2.100"

# Enable first
$e = Invoke-WebRequest "$ROT/api/rotator/enable" -Method POST -Body "{}" -ContentType "application/json" -UseBasicParsing
Write-Host "Enable: $($e.StatusCode) $($e.Content)"

# Test goto with correct field: position (form-encoded)
try {
    $r = Invoke-WebRequest "$ROT/api/rotator/goto" -Method POST -Body "position=68" -ContentType "application/x-www-form-urlencoded" -UseBasicParsing -ErrorAction Stop
    Write-Host "goto position=68: $($r.StatusCode) $($r.Content)"
} catch [System.Net.WebException] {
    $code = [int]$_.Exception.Response.StatusCode
    $resp = ""
    try { $sr = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream()); $resp = $sr.ReadToEnd() } catch {}
    Write-Host "goto position=68: $code : $resp"
}

# Confirm it moved (check status)
Start-Sleep -Milliseconds 500
$s = (Invoke-WebRequest "$ROT/api/status" -UseBasicParsing).Content | ConvertFrom-Json
Write-Host "Status after goto: enabled=$($s.rotator.enabled) target=$($s.rotator.targetPosition) moving=$($s.rotator.motorRunning)"
