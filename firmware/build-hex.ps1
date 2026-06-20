# build-hex.ps1 — compile Diduino_Nichrome.ino into the per-MCU .hex files in docs/
#
# Maintainer-only step. End users never run this: the web app fetches the committed
# docs/Diduino_Nichrome_v<ver>_<mcu>.hex that matches the chip signature it reads over
# Web Serial. The version comes straight from FW_VERSION in the .ino, and the MCU is in
# the file name, so the artifacts always track the firmware banner and the target chip.
# Re-run after changing the firmware, then commit the new .hex files with the app version.
#
# Builds two targets (Arduino Nano core):
#   atmega328p  — ATmega328P (standard Nano)            32 KB flash
#   atmega168   — ATmega168 (old Nano/Diecimila)        16 KB flash (firmware must still fit)
#
# Usage:  powershell -ExecutionPolicy Bypass -File firmware/build-hex.ps1

$ErrorActionPreference = 'Stop'

$root   = Split-Path -Parent $PSScriptRoot   # repo root (firmware/..)
$sketch = Join-Path $PSScriptRoot 'Diduino_Nichrome'
$ino    = Join-Path $sketch 'Diduino_Nichrome.ino'

# mcu name (goes into the file name) -> Nano FQBN cpu option
$targets = [ordered]@{
  'atmega328p' = 'arduino:avr:nano:cpu=atmega328'
  'atmega168'  = 'arduino:avr:nano:cpu=atmega168'
}

# Pull the version from FW_VERSION so the artifact names track the firmware banner.
$ver = (Select-String -Path $ino -Pattern '#define\s+FW_VERSION\s+"([^"]+)"').Matches[0].Groups[1].Value
if (-not $ver) { throw "FW_VERSION not found in $ino" }
Write-Host "firmware version: $ver"

# Locate arduino-cli: PATH first, then the copy bundled inside Arduino IDE 2.x.
$cli = (Get-Command arduino-cli -ErrorAction SilentlyContinue).Source
if (-not $cli) {
  $bundled = @(
    "$env:ProgramFiles\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe",
    "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
  ) | Where-Object { Test-Path $_ } | Select-Object -First 1
  if ($bundled) { $cli = $bundled }
}
if (-not $cli) { throw "arduino-cli not found (install it, or Arduino IDE 2.x)." }
Write-Host "arduino-cli: $cli"

$htmlPath = Join-Path $root 'docs\diduino_nichrome.html'
$utf8 = New-Object System.Text.UTF8Encoding($false)   # UTF-8 without BOM

# CRC-32 (poly 0xEDB88320) — must match crc32() in the web app, so the page can verify its own embed.
# Uses int64 with explicit 32-bit masking; note 0xFFFFFFFF is a NEGATIVE int32 in PS, hence the L literals.
function Get-Crc32([byte[]]$data){
  [long]$crc = 0xFFFFFFFFL
  foreach($b in $data){
    $crc = $crc -bxor [long]$b
    for($k=0; $k -lt 8; $k++){
      if(($crc -band 1) -ne 0){ $crc = (($crc -shr 1) -bxor 0xEDB88320L) -band 0xFFFFFFFFL }
      else { $crc = ($crc -shr 1) -band 0xFFFFFFFFL }
    }
  }
  return ('0x{0:X8}' -f (($crc -bxor 0xFFFFFFFFL) -band 0xFFFFFFFFL))
}

foreach ($mcu in $targets.Keys) {
  $fqbn   = $targets[$mcu]
  $outDir = Join-Path $env:TEMP "diduino_build_$mcu"
  $dest   = Join-Path $root "docs\Diduino_Nichrome_v${ver}_${mcu}.hex"
  Write-Host "`n=== $mcu ($fqbn) ==="
  & $cli compile --fqbn $fqbn --output-dir $outDir $sketch
  if ($LASTEXITCODE -ne 0) { throw "compile failed for $mcu ($LASTEXITCODE) - does it still fit?" }
  Copy-Item (Join-Path $outDir 'Diduino_Nichrome.ino.hex') $dest -Force
  Write-Host "OK -> $dest"

  # Embed the freshly built hex into the page so a single saved .html flashes with no external file.
  # Replaces the whole <script type="text/plain" id="fw-$mcu" ...>...</script>, stamping a CRC-32 (data-crc)
  # that the app re-checks before flashing. Keeps the .ino/.hex/HTML in sync and guards against a corrupt embed.
  if (Test-Path $htmlPath) {
    $raw  = (([IO.File]::ReadAllText($dest)) -replace "`r","").TrimEnd("`n")
    $crc  = Get-Crc32 ([Text.Encoding]::ASCII.GetBytes($raw))   # '0xXXXXXXXX', must match the app's crc32()
    $hex  = $raw -replace '\$','$$$$'   # escape $ for the regex replacement string (Intel HEX has none, but be safe)
    $html = [IO.File]::ReadAllText($htmlPath)
    $pat  = '(?s)<script type="text/plain" id="fw-' + $mcu + '"[^>]*>.*?</script>'
    $repl = '<script type="text/plain" id="fw-' + $mcu + '" data-crc="' + $crc + '">' + "`n" + $hex + "`n" + '</script>'
    if (-not [regex]::IsMatch($html, $pat)) { throw "embed marker <script id=fw-$mcu> not found in $htmlPath" }
    $new  = [regex]::Replace($html, $pat, $repl)   # may be a no-op if the firmware is unchanged — that's fine
    [IO.File]::WriteAllText($htmlPath, $new, $utf8)
    Write-Host "embedded -> #fw-$mcu (CRC-32 $crc) in diduino_nichrome.html"
  } else {
    Write-Warning "html not found, skipped embed: $htmlPath"
  }
}
