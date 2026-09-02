# fetch.ps1 — download, VERIFY, and extract the pinned corvid FFI release
# for Windows, then normalize into deps/current (what config.w32 points
# the extension build at). macOS/Linux: fetch.sh.
#
# Binding rules (docs/PLAN.md):
#   - the engine pin is EXACT and lives in ONE variable: $CorvidVersion;
#   - artifacts come only from the tag's GitHub release and are SHA256-
#     verified against the release's checksums.txt before extraction;
#   - deps/ is gitignored — no vendored binaries, ever;
#   - the vendored golden/ fixtures are byte-compared against the
#     release's copies — a mismatch is a hard failure.

param()
$ErrorActionPreference = "Stop"

# THE pin. Bump here and nowhere else (fetch.sh carries the same value).
$CorvidVersion = "v0.3.4"
$Repo = "corvid-db/corvid"

$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Dl = Join-Path $Root "deps\dl"

$Target = "x86_64-pc-windows-msvc"
$Archive = "corvid-ffi-$CorvidVersion-$Target.zip"
$BaseUrl = "https://github.com/$Repo/releases/download/$CorvidVersion"
$Extracted = Join-Path $Root "deps\corvid-ffi-$CorvidVersion-$Target"

Write-Host "fetch: corvid $CorvidVersion for $Target"

New-Item -ItemType Directory -Force -Path $Dl | Out-Null

# ---- stale-version cleanup: always discard anything not the current pin
Get-ChildItem -Path (Join-Path $Root "deps") -Directory -Filter "corvid-ffi-*" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne "corvid-ffi-$CorvidVersion-$Target" } |
    Remove-Item -Recurse -Force
Get-ChildItem -Path $Dl -File -Filter "corvid-ffi-*.zip" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -ne $Archive } |
    Remove-Item -Force

# ---- download checksums + (if needed) the archive ----------------------
Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/checksums.txt" -OutFile (Join-Path $Dl "checksums.txt")

if (-not (Test-Path $Extracted)) {
    Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/$Archive" -OutFile (Join-Path $Dl $Archive)

    # ---- verify: SHA256 against the release's checksums.txt -----------
    $expected = (Get-Content (Join-Path $Dl "checksums.txt") |
        Where-Object { ($_ -split '\s+')[1] -eq $Archive } |
        ForEach-Object { ($_ -split '\s+')[0] } | Select-Object -First 1)
    if (-not $expected) { Write-Error "fetch: $Archive is not listed in the release checksums.txt" }
    $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $Dl $Archive)).Hash.ToLower()
    if ($actual -ne $expected) {
        Write-Error "fetch: sha256 MISMATCH for ${Archive}: expected $expected, actual $actual"
    }
    Write-Host "fetch: sha256 ok ($actual)"

    # ---- extract -------------------------------------------------------
    Expand-Archive -Path (Join-Path $Dl $Archive) -DestinationPath (Join-Path $Root "deps") -Force
}

# Required: corvid.h + corvid.dll (+ the MSVC import library).
if (-not (Test-Path (Join-Path $Extracted "corvid.h")) -or
    -not (Test-Path (Join-Path $Extracted "corvid.dll"))) {
    Write-Error "fetch: $Extracted is missing corvid.h / corvid.dll — bad archive?"
}

# ---- the vendored golden fixtures must match the release's byte for byte --
Get-ChildItem -Path (Join-Path $Root "golden") -Filter "*.txt" | ForEach-Object {
    $theirs = Join-Path $Extracted "golden\$($_.Name)"
    if (-not (Test-Path $theirs)) {
        Write-Error "fetch: release golden/ lacks $($_.Name)"
    }
    $a = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
    $b = (Get-FileHash -Algorithm SHA256 $theirs).Hash
    if ($a -ne $b) {
        Write-Error "fetch: vendored golden/$($_.Name) differs from the release's copy — artifact finding, not a patch-here"
    }
}

# ---- normalize into deps/current (what config.w32 points at) ------------
$Cur = Join-Path $Root "deps\current"
if (Test-Path $Cur) { Remove-Item -Recurse -Force $Cur }
New-Item -ItemType Directory -Force -Path $Cur | Out-Null
Copy-Item (Join-Path $Extracted "corvid.h") $Cur
Copy-Item (Join-Path $Extracted "corvid.dll") $Cur
Get-ChildItem $Extracted -Filter "*.lib" | Copy-Item -Destination $Cur
Get-ChildItem $Extracted -Filter "*.dll.a" -ErrorAction SilentlyContinue | Copy-Item -Destination $Cur

Set-Content -Path (Join-Path $Root "deps\version.txt") -Value $CorvidVersion
Write-Host "fetch: deps/current ready (corvid.h, corvid.dll, import lib) — pin $CorvidVersion"
