# Cleanup script: free C: drive space by removing old PlatformIO packages
# Safe to run - PLATFORMIO_HOME_DIR=D:\.platformio is set permanently,
# so all active builds use D:. The C: install is obsolete.
#
# Estimated savings: ~425 MB (safe deletions only)
# Do NOT run this if you still build RotatorController / LoraClient on this machine
# without the D: pioarduino platform — the toolchains in C: are still needed for those.

$ErrorActionPreference = "SilentlyContinue"

function Remove-WithSize {
    param([string]$Path, [string]$Reason)
    if (Test-Path $Path) {
        $sizeMB = [math]::Round((Get-ChildItem $Path -Recurse -Force -ErrorAction SilentlyContinue |
            Measure-Object Length -Sum).Sum / 1MB, 0)
        Write-Host "Deleting $Path ($($sizeMB) MB) — $Reason" -ForegroundColor Yellow
        Remove-Item -Recurse -Force $Path
        Write-Host "  Done." -ForegroundColor Green
    } else {
        Write-Host "Not found (already gone): $Path" -ForegroundColor DarkGray
    }
}

Write-Host "`n=== PlatformIO C: Drive Cleanup ===" -ForegroundColor Cyan
Write-Host "Only removes packages that are duplicated/superseded by D:\.platformio`n"

# Old ESP-IDF 4.x prebuilt libs (321 MB) — replaced by pioarduino on D:
Remove-WithSize `
    "C:\Users\$env:USERNAME\.platformio\packages\framework-arduinoespressif32-libs" `
    "ESP-IDF 4.x prebuilt libs — superseded by pioarduino 55.x on D:"

# Old ESP-IDF 4.x framework core (49 MB)
Remove-WithSize `
    "C:\Users\$env:USERNAME\.platformio\packages\framework-arduinoespressif32" `
    "ESP-IDF 4.x framework core — superseded by pioarduino 55.x on D:"

# ESP8266 framework (1 MB) — not used in any project
Remove-WithSize `
    "C:\Users\$env:USERNAME\.platformio\packages\framework-arduinoespressif8266" `
    "ESP8266 framework — no ESP8266 boards in use"

# PIO Remote contribution (54 MB) — not needed
Remove-WithSize `
    "C:\Users\$env:USERNAME\.platformio\packages\contrib-pioremote" `
    "PlatformIO Remote — not in use"

# TouchController build artifacts (fully regenerable)
Remove-WithSize `
    "C:\Dev\PIO\TouchController\.pio" `
    "Build artifacts — fully regenerable"

# D: build cache (regenerable)
Remove-WithSize `
    "D:\pio-builds\touchcontroller" `
    "Build cache on D: — regenerable"

Write-Host "`n=== Skipped (still needed for other projects) ===" -ForegroundColor Cyan
Write-Host "  C:\Users\$env:USERNAME\.platformio\packages\toolchain-xtensa-esp32       (397 MB) — RotatorController / LoraClient"
Write-Host "  C:\Users\$env:USERNAME\.platformio\packages\toolchain-xtensa-esp32s3     (262 MB) — RotatorController / LoraClient"
Write-Host "  C:\Users\$env:USERNAME\.platformio\packages\toolchain-xtensa             (216 MB) — ESP generic"
Write-Host "  C:\Users\$env:USERNAME\.platformio\tools\                               (1434 MB) — ESP-IDF native tools, required"

Write-Host "`nDone. Run 'Get-PSDrive C' to see new free space." -ForegroundColor Cyan
