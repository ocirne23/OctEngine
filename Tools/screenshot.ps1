# Runs App.exe (sandbox, fullscreen) and saves the frame presented after -After seconds as a PNG: App's own
# --screenshot-after (read back from the swapchain, so the window may be covered or unfocused), scaled by -Scale.
# Claude's visual repro loop: -AppArgs @("--camera", x, y, z, dx, dy, dz, "--tweak", "`"Cat/Name=v`"").
# Build first: cmake --build Build --config RelWithDebInfo --target App. stdout / stderr go next to the PNG.
param(
    [string]$Out = "$PSScriptRoot\..\Build\screenshot.png",
    [double]$After = 10,
    [double]$Scale = 0.5,
    [string]$Config = "RelWithDebInfo",
    [string[]]$AppArgs = @()
)
Add-Type -AssemblyName System.Drawing
$root = Resolve-Path "$PSScriptRoot\.."
$exe = "$root\Build\Code\App\$Config\App.exe"
$outDir = Split-Path -Parent $Out
$shot = "$root\Assets\Local\screenshot.png"
Remove-Item $shot -ErrorAction SilentlyContinue
$quit = [int]($After + 3)
$allArgs = @("--sandbox", "--fullscreen", "--screenshot-after", "$After", "--quit-after", "$quit") + $AppArgs
$p = Start-Process -FilePath $exe -ArgumentList $allArgs -WorkingDirectory "$root\Assets" -PassThru `
    -RedirectStandardOutput "$outDir\screenshot.stdout.txt" -RedirectStandardError "$outDir\screenshot.stderr.txt"
$p.WaitForExit(([int]$quit + 60) * 1000) | Out-Null
if (!$p.HasExited) { $p.Kill() }
if (!(Test-Path $shot)) { "no screenshot written - see $outDir\screenshot.stdout.txt"; exit 1 }
$src = [System.Drawing.Image]::FromFile($shot)
$dst = New-Object System.Drawing.Bitmap $src, ([int]($src.Width * $Scale)), ([int]($src.Height * $Scale))
$src.Dispose()
$dst.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$dst.Dispose()
"saved $Out"
