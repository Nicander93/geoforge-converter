param(
  [Parameter(Mandatory = $true)]
  [string]$ZipPath,
  [string]$ScratchDir = $(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP })
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")
$Zip = (Resolve-Path -LiteralPath $ZipPath).Path
$SmokeRoot = Join-Path $ScratchDir ("geoforge-release-package-smoke-" + [guid]::NewGuid().ToString("N"))
$Runtime = Join-Path $SmokeRoot "runtime"
New-Item -ItemType Directory -Force -Path $Runtime | Out-Null
Expand-Archive -LiteralPath $Zip -DestinationPath $Runtime
$Converter = Join-Path $Runtime "_3dtile.exe"
if (-not (Test-Path -LiteralPath $Converter -PathType Leaf)) {
  throw "Release archive has no _3dtile.exe: $Zip"
}

$Cases = @(
  @{
    Name = "fbx"
    Input = Join-Path $Root "thirdparty\ufbx\data\blender_279_ball_7400_binary.fbx"
    Config = $null
  },
  @{
    Name = "obj"
    Input = Join-Path $Root "tests\fixtures\texture-root.obj"
    Config = Join-Path $Root "tests\fixtures\texture-root.config.json"
  }
)

foreach ($case in $Cases) {
  $Name = $case.Name
  $InputPath = (Resolve-Path -LiteralPath $case.Input).Path
  $OutputDir = Join-Path $SmokeRoot "$Name-output"
  New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
  $Arguments = @("-f", $Name, "-i", ('"' + $InputPath + '"'), "-o", ('"' + $OutputDir + '"'))
  if ($case.Config) {
    $ConfigPath = (Resolve-Path -LiteralPath $case.Config).Path
    $Arguments += @("--model-config", ('"' + $ConfigPath + '"'))
  }
  $StartInfo = New-Object System.Diagnostics.ProcessStartInfo
  $StartInfo.FileName = $Converter
  $StartInfo.Arguments = $Arguments -join " "
  $StartInfo.WorkingDirectory = $Root.Path
  $StartInfo.UseShellExecute = $false
  $StartInfo.CreateNoWindow = $true
  $Process = [System.Diagnostics.Process]::Start($StartInfo)
  if (-not $Process.WaitForExit(45000)) {
    $Process.Kill()
    throw "$Name conversion timed out after 45 seconds. Input: $InputPath"
  }
  $Process.Refresh()
  if ($Process.ExitCode -ne 0) {
    throw "$Name conversion failed with exit=$($Process.ExitCode). Input: $InputPath"
  }
  $Tileset = Join-Path $OutputDir "tileset.json"
  $Tiles = @(Get-ChildItem -LiteralPath $OutputDir -Filter "*.b3dm" -File -Recurse | Where-Object { $_.Length -gt 0 })
  if (-not (Test-Path -LiteralPath $Tileset -PathType Leaf) -or $Tiles.Count -eq 0) {
    throw "$Name conversion produced no tileset.json or non-empty B3DM in $OutputDir"
  }
  Write-Host "Release smoke passed: $Name -> $Tileset ($($Tiles.Count) B3DM)"
}
