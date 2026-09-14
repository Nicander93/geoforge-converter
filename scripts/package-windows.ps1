# Stage Windows x64 Release runtime next to _3dtile.exe.
# Layout must match src/main.rs setup_osg_environment().
param(
  [string]$OutDir = "dist/converter",
  [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Exe = Join-Path $Root "target\release\_3dtile.exe"
if (-not (Test-Path $Exe)) {
  Write-Error "_3dtile.exe missing. Run: cargo build --release"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item -Force $Exe (Join-Path $OutDir "_3dtile.exe")

$ExeDir = Split-Path $Exe -Parent
$VcpkgRoot = Join-Path $Root "vcpkg_installed\x64-windows"
foreach ($dllDir in @($ExeDir, (Join-Path $VcpkgRoot "bin"))) {
  if (-not (Test-Path $dllDir)) { continue }
  Get-ChildItem $dllDir -Filter *.dll -ErrorAction SilentlyContinue | ForEach-Object {
    Copy-Item -Force $_.FullName $OutDir
  }
}

function Copy-RuntimeDir($Src, $Dst) {
  if (-not (Test-Path $Src)) { return $false }
  New-Item -ItemType Directory -Force -Path $Dst | Out-Null
  Copy-Item -Recurse -Force (Join-Path $Src "*") $Dst
  Write-Host "Copied $Src -> $Dst"
  return $true
}

$pluginSrc = @(
  (Join-Path $VcpkgRoot "plugins\osgPlugins-3.6.5"),
  (Join-Path $ExeDir "osgPlugins-3.6.5")
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($pluginSrc) {
  Copy-RuntimeDir $pluginSrc (Join-Path $OutDir "osgPlugins-3.6.5") | Out-Null
} else {
  Write-Error "Missing osgPlugins-3.6.5"
}

$gdalSrc = @(
  (Join-Path $VcpkgRoot "share\gdal"),
  (Join-Path $ExeDir "gdal")
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($gdalSrc) {
  Copy-RuntimeDir $gdalSrc (Join-Path $OutDir "gdal") | Out-Null
} else {
  Write-Error "Missing gdal data"
}

$projSrc = @(
  (Join-Path $VcpkgRoot "share\proj"),
  (Join-Path $ExeDir "proj")
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($projSrc) {
  Copy-RuntimeDir $projSrc (Join-Path $OutDir "proj") | Out-Null
} else {
  Write-Error "Missing proj data"
}

$geoids = Join-Path $Root "geoids"
if (Test-Path $geoids) {
  Copy-RuntimeDir $geoids (Join-Path $OutDir "geoids") | Out-Null
}

$manifest = @{
  name = "geoforge-converter"
  version = $Version
  upstream = "fanvanzh/3dtiles"
  upstreamCommit = "see docs/UPSTREAM.md (GeoForge engines snapshot bdb8b7fd31c1eaa7ef65ad9f3419c6ab8c56c149)"
  platform = "windows-x64"
  createdAt = (Get-Date).ToString("o")
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -Encoding utf8 (Join-Path $OutDir "manifest.json")
Write-Host "Staged $OutDir"
