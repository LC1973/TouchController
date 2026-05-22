$ports = @(80, 8080, 8000, 3000)
$lastOctet = @(30,32,33,34,35,59,71,76,90,92,96,99,104,105,118,119,126,141,150,158,161,168,171,190,251)
foreach ($oct in $lastOctet) {
    $ip = "192.168.1.$oct"
    foreach ($port in $ports) {
        try {
            $r = Invoke-WebRequest "http://${ip}:${port}/api/rotator/status" -TimeoutSec 1 -UseBasicParsing -ErrorAction Stop
            Write-Host "FOUND rotator at ${ip}:${port} => $($r.Content.Substring(0,300))"
        } catch { }
    }
}
Write-Host "Done."
