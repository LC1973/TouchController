# Deploy TouchController firmware via Pi jump host (192.168.16.11)
# The Pi routes to the ESP32 via 192.168.16.1, and the ESP32 can reach 192.168.16.11
# (confirmed - it sends syslog UDP there).
# Usage: .\deploy_via_pi.ps1 [-SkipBuild]
param([switch]$SkipBuild)

$ErrorActionPreference = "Stop"

$PYTHON    = "C:\Python\313\python.exe"
$ESPOTA    = "C:\BuildCache\platformio\packages\framework-arduinoespressif32\tools\espota.py"
$FIRMWARE  = "C:\BuildCache\pio\TouchController\build\touchpanel-ota\firmware.bin"
$PI_HOST   = "192.168.16.11"
$PI_IFACE  = "192.168.16.11"   # Pi's eth0 — reachable from ESP32 via syslog routing
$ESP_IP    = "192.168.1.200"
$OTA_PASS  = "otapass"

if (-not $SkipBuild) {
    Write-Host "=== Building TouchController (touchpanel-ota) ===" -ForegroundColor Cyan
    & "C:\BuildCache\platformio\penv\Scripts\pio.exe" run -e touchpanel-ota
    if ($LASTEXITCODE -ne 0) { Write-Error "Build failed"; exit 1 }
}

if (-not (Test-Path $FIRMWARE)) { Write-Error "Firmware not found: $FIRMWARE"; exit 1 }

Write-Host "=== Copying files to Pi ($PI_HOST) ===" -ForegroundColor Cyan
scp -o StrictHostKeyChecking=no $ESPOTA "${PI_HOST}:~/espota.py"
scp -o StrictHostKeyChecking=no $FIRMWARE "${PI_HOST}:~/TouchController.bin"

Write-Host "=== Running OTA from Pi (source: $PI_IFACE -> $ESP_IP) ===" -ForegroundColor Cyan
ssh -o StrictHostKeyChecking=no $PI_HOST "python3 ~/espota.py -i $ESP_IP -I $PI_IFACE -a $OTA_PASS --progress -f ~/TouchController.bin"

if ($LASTEXITCODE -eq 0) {
    Write-Host "=== OTA Upload Successful ===" -ForegroundColor Green
} else {
    Write-Error "OTA Upload Failed (exit $LASTEXITCODE)"
    exit 1
}
