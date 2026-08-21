# Build (optional) + run App.exe unattended + print the profiler text report.
#
#   Tools\profile.ps1                         # RelWithDebInfo, dump at frame 600 over the last 256 frames, quit
#   Tools\profile.ps1 -Config Debug -NoBuild  # reuse the last build
#   Tools\profile.ps1 -Game -After 1200 -Frames 300 -Show 120
#   Tools\profile.ps1 -Scenario default -Server -After 900   # --game --server: load Local/gamesave.txt at frame 30, every own unit marches on the enemy Base (THE standard perf run)
#   Tools\profile.ps1 -Tweak "Time/Max FPS=0","Spatial/Culling/Mode=0"   # tweak overrides (never saved)
#   Tools\profile.ps1 -AppArgs "--server"     # extra App.exe arguments
#
# The report lands in Assets/Local/profile.txt (or -Out); the first -Show lines print to the console.
# The run counts as focused (no inactive-fps cap), passes --no-vsync unless -VSync is given, and
# always overrides "Time/Max FPS=0" so the saved frame cap is not what gets measured.
param(
    [string]$Config = "RelWithDebInfo",
    [int]$After = 600,      # dump when this many frames completed
    [int]$Frames = 256,     # window: the last N frames before the dump (max 511)
    [int]$QuitAfter = 0,    # default: After + 2
    [string]$Out = "Local/profile.txt",
    [switch]$Game,
    [string]$Scenario = "",   # --game scenario: a save path (Assets/-relative) or "default" (= F10's Local/gamesave.txt);
                              # loads it, selects every own unit and orders them to the other Base. Implies -Game
    [int]$ScenarioFrame = 30,
    [switch]$Server,          # windowed listen server (--server --port $Port): the networking code is live
    [int]$Port = 27999,       # not the default 27888, so a manually running instance keeps its port
    [switch]$VSync,
    [switch]$Workers,       # one tree per worker instead of the merged one
    [switch]$NoBuild,
    [int]$TimeoutSec = 300,
    [int]$Show = 80,
    [string[]]$Tweak = @(),  # "Category/Name=value" overrides, applied on top of Time/Max FPS=0
    [string]$AppArgs = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ($QuitAfter -le 0) { $QuitAfter = $After + 2 }

if (-not $NoBuild)
{
    Write-Host "== building App ($Config) =="
    & cmake --build (Join-Path $repo "Build") --config $Config --target App
    if ($LASTEXITCODE -ne 0) { Write-Host "build failed ($LASTEXITCODE)"; exit $LASTEXITCODE }
}

$exe = Join-Path $repo "Build\Code\App\$Config\App.exe"
if (-not (Test-Path $exe)) { Write-Host "missing $exe"; exit 1 }

$reportPath = Join-Path (Join-Path $repo "Assets") $Out
if (Test-Path $reportPath) { Remove-Item $reportPath -Force }

$argList = @("--profile-after", $After, "--profile-frames", $Frames, "--profile-out", $Out, "--quit-after", $QuitAfter)
if (-not $VSync) { $argList += "--no-vsync" }
if ($Workers) { $argList += "--profile-workers" }
if ($Scenario -ne "") { $Game = $true; $argList += @("--scenario", $Scenario, "--scenario-frame", $ScenarioFrame) }
if ($Game) { $argList += "--game" }
if ($Server) { $argList += @("--server", "--port", $Port) }
foreach ($t in (@("Time/Max FPS=0") + $Tweak)) { $argList += "--tweak"; $argList += "`"$t`"" } # quoted: tweak keys contain spaces
if ($AppArgs -ne "") { $argList += $AppArgs.Split(" ", [System.StringSplitOptions]::RemoveEmptyEntries) }

Write-Host "== running App.exe $($argList -join ' ') =="
$proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $repo -PassThru
if (-not $proc.WaitForExit($TimeoutSec * 1000))
{
    Write-Host "timeout after $TimeoutSec s - killing App.exe"
    $proc.Kill()
}
Write-Host "== App.exe exit code $($proc.ExitCode) =="

if (-not (Test-Path $reportPath)) { Write-Host "no report at $reportPath"; exit 1 }
Write-Host "== $reportPath (first $Show lines; open the file for the per-track trees) =="
Get-Content $reportPath -TotalCount $Show
