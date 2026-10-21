# Collect license texts and notices for a staged Windows x64 converter runtime.
# Called by scripts/package-windows.ps1 after the runtime has been staged.
#
# Output inside -OutDir:
#   LICENSE, NOTICE                 geoforge-converter itself (Apache-2.0)
#   THIRD-PARTY-NOTICES.txt         every bundled component: version, license,
#                                   homepage, source, shipped files
#   LGPL-SOURCE-OFFER.txt           how to obtain the source of LGPL/MPL parts
#   licenses/vcpkg/<port>/...       share/<port>/copyright from vcpkg_installed
#   licenses/source/<name>/LICENSE  vendored sources compiled into _3dtile.exe
#   licenses/rust/<crate>-<ver>/... license files shipped by each Rust crate
#   licenses/inventory.json         the same data, machine readable
#
# Every DLL in the staged runtime must map to a vcpkg port or to the MSVC
# runtime; an unmapped DLL stops the build.
param(
  [Parameter(Mandatory = $true)][string]$OutDir,
  [Parameter(Mandatory = $true)][string]$Root,
  [Parameter(Mandatory = $true)][string]$VcpkgInstalledRoot,
  [string]$Triplet = "x64-windows",
  [string]$Version = "",
  [switch]$SkipRust
)

$ErrorActionPreference = "Stop"
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Write-Utf8([string]$Path, [string[]]$Lines) {
  [System.IO.File]::WriteAllLines($Path, $Lines, $utf8)
}

$OutDir = (Resolve-Path $OutDir).Path
$Root = (Resolve-Path $Root).Path
$tripletDir = Join-Path $VcpkgInstalledRoot $Triplet
$vcpkgMeta = Join-Path $VcpkgInstalledRoot "vcpkg"
$statusFile = Join-Path $vcpkgMeta "status"
$infoDir = Join-Path $vcpkgMeta "info"
foreach ($p in @($tripletDir, $statusFile, $infoDir)) {
  if (-not (Test-Path $p)) { throw "vcpkg metadata missing: $p" }
}

$licRoot = Join-Path $OutDir "licenses"
if (Test-Path $licRoot) { Remove-Item -Recurse -Force $licRoot }
New-Item -ItemType Directory -Force -Path $licRoot | Out-Null

# --- geoforge-converter itself -------------------------------------------
foreach ($name in @("LICENSE", "NOTICE")) {
  $src = Join-Path $Root $name
  if (-not (Test-Path $src)) { throw "Missing $src" }
  Copy-Item -Force $src (Join-Path $OutDir $name)
}

# --- vcpkg ports ---------------------------------------------------------
$ports = @{}
$statusText = [System.IO.File]::ReadAllText($statusFile)
foreach ($block in ($statusText -split "(\r?\n){2,}")) {
  if ($block -notmatch "(?m)^Package: (.+)$") { continue }
  $name = $Matches[1].Trim()
  if ($block -match "(?m)^Feature: ") { continue }
  if ($block -notmatch "(?m)^Status: install ok installed") { continue }
  if ($block -match "(?m)^Architecture: (.+)$" -and $Matches[1].Trim() -ne $Triplet) { continue }
  if ($name -like "vcpkg-*") { continue }   # vcpkg build helpers, nothing is shipped
  $ver = ""
  if ($block -match "(?m)^Version: (.+)$") { $ver = $Matches[1].Trim() }
  if ($block -match "(?m)^Port-Version: (.+)$") { $ver = "$ver#$($Matches[1].Trim())" }
  $ports[$name] = [ordered]@{ name = $name; version = $ver; files = New-Object System.Collections.ArrayList }
}

# Which port installed which runtime file.
$fileToPort = @{}
foreach ($list in Get-ChildItem $infoDir -Filter "*_$Triplet.list") {
  if ($list.Name -notmatch "^(.+?)_[^_]+_$([regex]::Escape($Triplet))\.list$") { continue }
  $port = $Matches[1]
  foreach ($line in [System.IO.File]::ReadAllLines($list.FullName)) {
    if ($line -match "^$([regex]::Escape($Triplet))/(bin|plugins/[^/]+)/([^/]+\.(dll|exe))$") {
      $fileToPort[$Matches[2].ToLowerInvariant()] = $port
    }
  }
}

$crtPattern = "^(msvcp140.*|vcruntime140.*|concrt140|vccorlib140)\.dll$"
$crtFiles = New-Object System.Collections.ArrayList
$unmapped = New-Object System.Collections.ArrayList
$staged = Get-ChildItem $OutDir -Recurse -File -Include *.dll, *.exe |
  Where-Object { $_.FullName -notlike "$licRoot*" -and $_.Name -ne "_3dtile.exe" }
foreach ($f in $staged) {
  $rel = $f.FullName.Substring($OutDir.Length + 1)
  $key = $f.Name.ToLowerInvariant()
  if ($key -match $crtPattern) { [void]$crtFiles.Add($f); continue }
  if ($fileToPort.ContainsKey($key) -and $ports.ContainsKey($fileToPort[$key])) {
    [void]$ports[$fileToPort[$key]].files.Add($rel)
  } else {
    [void]$unmapped.Add($rel)
  }
}
if ($unmapped.Count -gt 0) {
  throw "These staged files do not belong to any vcpkg port: $($unmapped -join ', ')"
}

# Licenses that vcpkg port metadata leaves empty, taken from the upstream
# license files (checked against share/<port>/copyright).
$licenseFallback = @{
  "osg"        = "OSGPL-1.0 (LGPL-2.1 with wxWindows Library Licence exception)"
  "gdal"       = "MIT, plus the component licenses in licenses/vcpkg/gdal/"
  "libspatialite" = "(MPL-1.1 OR GPL-2.0-or-later OR LGPL-2.1-or-later)"
  "libiconv"   = "LGPL-2.1-or-later"
  "libgta"     = "LGPL-2.1-or-later"
  "basisu"     = "Apache-2.0"
  "jasper"     = "JasPer-2.0"
  "liblzma"    = "0BSD"
  "qhull"      = "Qhull"
  "pkgconf"    = "ISC"
}

# Host build tools pulled in by vcpkg; nothing from them is linked or shipped.
$buildOnly = @("gperf", "boost-uninstall")

$copyPattern = "^(copyright|copying.*|license.*|licence.*|notice.*|patents.*|authors.*)$"
$portRows = New-Object System.Collections.ArrayList
$offerRows = New-Object System.Collections.ArrayList
$warnings = New-Object System.Collections.ArrayList
foreach ($name in ($ports.Keys | Sort-Object)) {
  $p = $ports[$name]
  if ($buildOnly -contains $name) {
    if ($p.files.Count -gt 0) { throw "Build-only port $name unexpectedly ships $($p.files -join ', ')" }
    [void]$warnings.Add("vcpkg port $name $($p.version) is a build tool used while compiling other ports; it is not linked or shipped and is not listed above")
    continue
  }
  $share = Join-Path $tripletDir "share\$name"
  $license = ""; $homepage = ""; $sources = @(); $portRef = ""
  $spdxPath = Join-Path $share "vcpkg.spdx.json"
  if (Test-Path $spdxPath) {
    $spdx = Get-Content -Raw -Encoding utf8 $spdxPath | ConvertFrom-Json
    $pkgs = @($spdx.packages)
    if ($pkgs.Count -gt 0) {
      $license = "$($pkgs[0].licenseConcluded)"
      if (-not $license -or $license -eq "NOASSERTION" -or $license -eq "LicenseRef-vcpkg-null") { $license = "$($pkgs[0].licenseDeclared)" }
      $homepage = "$($pkgs[0].homepage)"
      $portRef = "$($pkgs[0].downloadLocation)"
    }
    $sources = @($pkgs | Select-Object -Skip 2 | ForEach-Object { "$($_.downloadLocation)" } |
      Where-Object { $_ -and $_ -ne "NONE" -and $_ -ne "NOASSERTION" } |
      ForEach-Object { $_ -replace '\$\{[A-Za-z_]*VER[A-Za-z_]*\}', ($p.version -replace '#.*$', '') })
  }
  $unknown = @("", "NOASSERTION", "LicenseRef-vcpkg-null")
  if ($unknown -contains $license) {
    if ($licenseFallback.ContainsKey($name)) { $license = $licenseFallback[$name] + " [from upstream license file]" }
    else { $license = "see licenses/vcpkg/$name/copyright" }
  }

  $dst = Join-Path $licRoot "vcpkg\$name"
  $copied = @()
  $text = ""
  if (Test-Path $share) {
    foreach ($lf in Get-ChildItem $share -File | Where-Object { $_.Name -match $copyPattern }) {
      New-Item -ItemType Directory -Force -Path $dst | Out-Null
      Copy-Item -Force $lf.FullName (Join-Path $dst $lf.Name)
      $copied += "licenses/vcpkg/$name/$($lf.Name)"
      $text += [System.IO.File]::ReadAllText($lf.FullName)
    }
  }
  if ($copied.Count -eq 0) {
    if ($p.files.Count -gt 0) { throw "No license file for shipped port $name (expected $share\copyright)" }
    [void]$warnings.Add("vcpkg port $name has no license file under share/$name (header-only or build-only, nothing shipped from it)")
  }

  $lgplText = $text -match "GNU LESSER GENERAL PUBLIC|GNU LIBRARY GENERAL PUBLIC|OpenSceneGraph Public License|Mozilla Public License"
  $lgplSpdx = $license -match "LGPL|MPL|OSGPL"
  $licenseKnown = $license -notlike "see licenses/*"
  if ($text -match "GNU GENERAL PUBLIC LICENSE" -and -not $lgplText -and $license -notmatch " OR ") {
    [void]$warnings.Add("vcpkg port $name license text mentions the GPL without an LGPL/MPL/permissive alternative; review it")
  }
  $election = ""
  if ($license -match " OR " -and $license -match "GPL") {
    $election = "Used under the non-GPL alternative(s) of: $license"
  }

  $row = [ordered]@{
    name = $name; version = $p.version; license = $license; election = $election
    homepage = $homepage; source = ($sources -join " "); vcpkgPort = $portRef
    shipped = @($p.files); licenseFiles = $copied
  }
  [void]$portRows.Add($row)
  if ($lgplSpdx -or ($lgplText -and -not $licenseKnown)) { [void]$offerRows.Add($row) }
}

# --- sources vendored into _3dtile.exe --------------------------------------
$vendored = @(
  @{ name = "ufbx"; dir = "thirdparty\ufbx"; license = "MIT OR Unlicense"; homepage = "https://github.com/ufbx/ufbx"; how = "compiled into _3dtile.exe (static library)" },
  @{ name = "base64"; dir = "thirdparty\base64"; license = "MIT"; homepage = "https://github.com/tobiaslocker/base64"; how = "header, compiled into _3dtile.exe" },
  @{ name = "tinygltf"; dir = "thirdparty\tinygltf"; license = "MIT"; homepage = "https://github.com/syoyo/tinygltf"; how = "header, compiled into _3dtile.exe" }
)
$vendorRows = New-Object System.Collections.ArrayList
foreach ($v in $vendored) {
  $dir = Join-Path $Root $v.dir
  $lf = Get-ChildItem $dir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match "^licen[cs]e" } | Select-Object -First 1
  if (-not $lf) { throw "Missing license file in $dir (run git submodule update --init)" }
  $ver = ""
  $gitDir = Join-Path $dir ".git"
  if (Test-Path $gitDir) { $ver = (& git -C $dir rev-parse HEAD 2>$null) }
  if (-not $ver -and $v.name -eq "tinygltf") { $ver = "v2.9.7 (vendored copy, see docs/UPSTREAM.md)" }
  $dst = Join-Path $licRoot "source\$($v.name)"
  New-Item -ItemType Directory -Force -Path $dst | Out-Null
  Copy-Item -Force $lf.FullName (Join-Path $dst $lf.Name)
  [void]$vendorRows.Add([ordered]@{ name = $v.name; version = "$ver"; license = $v.license; homepage = $v.homepage; how = $v.how; licenseFiles = @("licenses/source/$($v.name)/$($lf.Name)") })
}

# --- Rust crates linked into _3dtile.exe ------------------------------------
$rustRows = New-Object System.Collections.ArrayList
if (-not $SkipRust) {
  Push-Location $Root
  try {
    $metaJson = & cargo metadata --format-version 1 --locked --filter-platform x86_64-pc-windows-msvc
    if ($LASTEXITCODE -ne 0) { throw "cargo metadata failed with exit=$LASTEXITCODE" }
  } finally { Pop-Location }
  $meta = ($metaJson -join "`n") | ConvertFrom-Json
  $byId = @{}
  foreach ($pkg in $meta.packages) { $byId[$pkg.id] = $pkg }
  $nodes = @{}
  foreach ($n in $meta.resolve.nodes) { $nodes[$n.id] = $n }
  # Walk normal (non-dev, non-build) dependencies from the root crate.
  $seen = @{}
  $queue = New-Object System.Collections.Queue
  $queue.Enqueue($meta.resolve.root)
  while ($queue.Count -gt 0) {
    $id = $queue.Dequeue()
    if ($seen.ContainsKey($id)) { continue }
    $seen[$id] = $true
    foreach ($d in $nodes[$id].deps) {
      $normal = @($d.dep_kinds | Where-Object { $null -eq $_.kind }).Count -gt 0
      if ($normal) { $queue.Enqueue($d.pkg) }
    }
  }
  foreach ($id in $seen.Keys) {
    $pkg = $byId[$id]
    if ($id -eq $meta.resolve.root) { continue }
    $dir = Split-Path $pkg.manifest_path -Parent
    $files = @(Get-ChildItem $dir -File | Where-Object { $_.Name -match "^(licen[cs]e|copying|notice|unlicense|copyright)" })
    $rel = @()
    if ($files.Count -gt 0) {
      $dst = Join-Path $licRoot "rust\$($pkg.name)-$($pkg.version)"
      New-Item -ItemType Directory -Force -Path $dst | Out-Null
      foreach ($f in $files) {
        Copy-Item -Force $f.FullName (Join-Path $dst $f.Name)
        $rel += "licenses/rust/$($pkg.name)-$($pkg.version)/$($f.Name)"
      }
    } else {
      [void]$warnings.Add("Rust crate $($pkg.name) $($pkg.version) ($($pkg.license)) ships no license file; its SPDX license is listed in THIRD-PARTY-NOTICES.txt")
    }
    $repo = "$($pkg.repository)"
    if (-not $repo) { $repo = "https://crates.io/crates/$($pkg.name)" }
    [void]$rustRows.Add([ordered]@{ name = $pkg.name; version = $pkg.version; license = "$($pkg.license)"; homepage = $repo; licenseFiles = $rel })
  }
  $rustRows = @($rustRows | Sort-Object { $_.name })
}

# --- THIRD-PARTY-NOTICES.txt ----------------------------------------------
$baseline = ""
$cfg = Join-Path $Root "vcpkg.json"
if (Test-Path $cfg) { $baseline = (Get-Content -Raw $cfg | ConvertFrom-Json)."builtin-baseline" }

# The FreeType License asks for "Portions of this software are copyright (C) <year>
# The FreeType Project"; use the year this archive is built.
$ftYears = "$((Get-Date).Year) "

$out = New-Object System.Collections.ArrayList
function Add([string]$s) { [void]$out.Add($s) }
Add "geoforge-converter $Version for Windows x64 - third-party notices"
Add "================================================================"
Add ""
Add "geoforge-converter is licensed under the Apache License, Version 2.0 (see"
Add "LICENSE). It is a modified version of fanvanzh/3dtiles; see NOTICE for the"
Add "upstream base and the list of modified files."
Add ""
Add "This archive also contains the third-party software listed below. Each entry"
Add "gives the version, the license (SPDX expression where known), where the"
Add "upstream project and its source code can be found, which files of this"
Add "archive come from it, and where its license text is stored. Native libraries"
Add "were built with vcpkg, baseline $baseline, triplet $Triplet."
Add "Source code for the LGPL- and MPL-licensed libraries: see LGPL-SOURCE-OFFER.txt."
Add ""
Add "Required attributions"
Add "---------------------"
Add "* This software is based in part on the work of the Independent JPEG Group (libjpeg-turbo)."
Add "* Portions of this software are copyright (C) $($ftYears)The FreeType Project (www.freetype.org). All rights reserved."
Add "* proj/proj.db contains data derived from the EPSG Geodetic Parameter Dataset, maintained by IOGP. The data is redistributed unmodified under the EPSG terms of use (https://epsg.org/terms-of-use.html); it is not an official EPSG publication."
Add "* geoids/egm96-5.pgm is the EGM96 geoid model (NGA / NASA, public domain) in the PGM format published by GeographicLib (https://geographiclib.sourceforge.io/)."
Add ""
Add "1. Libraries built from vcpkg ports"
Add "-----------------------------------"
foreach ($r in $portRows) {
  Add ""
  Add "$($r.name) $($r.version)"
  Add "  License:  $($r.license)"
  if ($r.election) { Add "  Election: $($r.election)" }
  if ($r.homepage) { Add "  Homepage: $($r.homepage)" }
  if ($r.source) { Add "  Source:   $($r.source)" }
  if ($r.vcpkgPort) { Add "  Port:     $($r.vcpkgPort)" }
  if ($r.shipped.Count -gt 0) { Add "  Files:    $($r.shipped -join ', ')" }
  else { Add "  Files:    no DLL of its own; linked statically, header-only, or needed only to build another port" }
  if ($r.licenseFiles.Count -gt 0) { Add "  Text:     $($r.licenseFiles -join ', ')" }
}
Add ""
Add "2. Source code compiled into _3dtile.exe"
Add "----------------------------------------"
foreach ($r in $vendorRows) {
  Add ""
  Add "$($r.name) $($r.version)"
  Add "  License:  $($r.license)"
  Add "  Homepage: $($r.homepage)"
  Add "  Use:      $($r.how)"
  Add "  Text:     $($r.licenseFiles -join ', ')"
}
Add ""
Add "3. Rust crates compiled into _3dtile.exe"
Add "----------------------------------------"
if ($SkipRust) { Add "(not collected in this build: -SkipRust)" }
foreach ($r in $rustRows) {
  $t = if ($r.licenseFiles.Count -gt 0) { $r.licenseFiles -join ', ' } else { "no license file shipped by the crate" }
  Add "$($r.name) $($r.version) | $($r.license) | $($r.homepage) | $t"
}
Add ""
Add "4. Microsoft Visual C++ runtime"
Add "-------------------------------"
Add "The following files are Microsoft Visual C++ Redistributable files, distributed"
Add "as Distributable Code under the Microsoft Visual Studio license terms:"
foreach ($f in ($crtFiles | Sort-Object FullName)) {
  Add "  $($f.FullName.Substring($OutDir.Length + 1)) $($f.VersionInfo.FileVersion)"
}
if ($warnings.Count -gt 0) {
  Add ""
  Add "5. Notes from the license collection"
  Add "------------------------------------"
  foreach ($w in $warnings) { Add "* $w" }
}
Write-Utf8 (Join-Path $OutDir "THIRD-PARTY-NOTICES.txt") $out

# --- LGPL-SOURCE-OFFER.txt -------------------------------------------------
$offer = New-Object System.Collections.ArrayList
function AddO([string]$s) { [void]$offer.Add($s) }
AddO "geoforge-converter $Version - source code for LGPL- and MPL-licensed components"
AddO "=============================================================================="
AddO ""
AddO "The libraries below are licensed under the GNU Lesser General Public License,"
AddO "the OpenSceneGraph Public License (LGPL-2.1 with the wxWindows exception) or"
AddO "the Mozilla Public License. GeoForge does not modify them beyond the patches"
AddO "that the vcpkg port applies. Libraries that list Files below are shipped as"
AddO "separate DLLs and loaded dynamically by _3dtile.exe, so you may replace any of"
AddO "them with a compatible build of your own. Reverse engineering for the purpose"
AddO "of debugging such modifications is permitted. Entries without Files (for"
AddO "example the header-only Eigen library, MPL-2.0) are compiled unmodified into"
AddO "the binaries that use them; their source is available at the listed location."
AddO ""
AddO "Each library was built by vcpkg (https://github.com/microsoft/vcpkg) at"
AddO "baseline $baseline for the $Triplet triplet. The exact"
AddO "upstream source archive, the vcpkg port (build scripts and patches) and the"
AddO "license text are listed for each library. The vcpkg.json and"
AddO "vcpkg-configuration.json files in https://github.com/Nicander93/geoforge-converter"
AddO "at the tag of this release give the complete build configuration."
AddO ""
AddO "Written offer: for at least three years after we distribute this release, we"
AddO "will provide the complete corresponding source code of these libraries, as"
AddO "used for this release, to anyone who asks, at no charge beyond the cost of"
AddO "physically performing the distribution. To request it, open an issue at"
AddO "https://github.com/Nicander93/geoforge-converter/issues and name this release."
AddO ""
foreach ($r in $offerRows) {
  AddO "$($r.name) $($r.version)"
  AddO "  License:  $($r.license)"
  if ($r.election) { AddO "  Election: $($r.election)" }
  if ($r.source) { AddO "  Source:   $($r.source)" }
  if ($r.homepage) { AddO "  Homepage: $($r.homepage)" }
  if ($r.vcpkgPort) { AddO "  Port:     $($r.vcpkgPort)" }
  if ($r.shipped.Count -gt 0) { AddO "  Files:    $($r.shipped -join ', ')" }
  if ($r.licenseFiles.Count -gt 0) { AddO "  Text:     $($r.licenseFiles -join ', ')" }
  AddO ""
}
Write-Utf8 (Join-Path $OutDir "LGPL-SOURCE-OFFER.txt") $offer

$inventory = [ordered]@{
  converterVersion = $Version; vcpkgBaseline = $baseline; triplet = $Triplet
  vcpkgPorts = $portRows; vendored = $vendorRows; rustCrates = $rustRows
  msvcRuntime = @($crtFiles | ForEach-Object { $_.FullName.Substring($OutDir.Length + 1) })
  lgplOrMplPorts = @($offerRows | ForEach-Object { $_.name }); warnings = $warnings
}
[System.IO.File]::WriteAllText((Join-Path $licRoot "inventory.json"), ($inventory | ConvertTo-Json -Depth 6), $utf8)

# Some crates.io packages carry 1970 file times, which Compress-Archive cannot
# store in a zip (the format starts at 1980). Give copied license files the
# time of this build.
$now = Get-Date
Get-ChildItem $licRoot -Recurse -File | Where-Object { $_.LastWriteTime.Year -lt 1980 } | ForEach-Object { $_.LastWriteTime = $now }

Write-Host ("Collected licenses: {0} vcpkg ports, {1} vendored sources, {2} Rust crates, {3} LGPL/MPL ports, {4} notes" -f `
  $portRows.Count, $vendorRows.Count, $rustRows.Count, $offerRows.Count, $warnings.Count)
