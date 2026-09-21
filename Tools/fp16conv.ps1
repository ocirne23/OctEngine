<#
.SYNOPSIS
  Lists every fp16 <-> fp32 conversion (OpFConvert) in a shader's SPIR-V, by source file and line.

.DESCRIPTION
  glslang is silent about float16_t -> float widening (GL_EXT_shader_explicit_arithmetic_types allows it
  implicitly), so a half block that quietly falls back to 32-bit math shows only in the SPIR-V. This tool
  disassembles Assets/Local/Shaders/<Shader>.spv (spirv-dis from the Vulkan SDK) and attributes each
  OpFConvert to the DebugLine before it (the NonSemantic debug info Shader.cpp always emits; inlined calls
  report the callee's line). Direction: WIDEN = result 32-bit (f16 -> f32), NARROW = result 16-bit.

  The dump is the LAST variant compiled from that file (the defines are not in the name), and the driver may
  still fold an exact f16 -> f32 -> f16 round trip - so the counts are an upper bound on real conversions.

.EXAMPLE
  Tools/fp16conv.ps1 instanced_indirect_terrain.fs.glsl
  Tools/fp16conv.ps1 ocean.fs.glsl -Top 60 -Widen
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Shader,
    [int]$Top = 40,
    [switch]$Widen,   # only f16 -> f32
    [switch]$Narrow   # only f32 -> f16
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$spv = Join-Path $root "Assets/Local/Shaders/$Shader.spv"
if (-not (Test-Path $spv)) { throw "No SPIR-V dump at $spv (run App once so the shader compiles)." }

$dis = if ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK 'Bin/spirv-dis.exe' } else { 'spirv-dis' }
$lines = & $dis $spv

$strings = @{}   # OpString id -> text (single-line strings: the file names)
$sources = @{}   # DebugSource id -> file name
$widths  = @{}   # type id -> float bit width (scalars and vectors)
$counts  = @{}   # "file|line|dir" -> count
$curFile = '?'; $curLine = 0

foreach ($l in $lines) {
    if ($l -match '^\s*(%\S+) = OpString "([^"]*)"\s*$') { $strings[$Matches[1]] = $Matches[2]; continue }
    if ($l -match '^\s*(%\S+) = OpExtInst %\S+ %\S+ DebugSource (%\S+)') {
        $sources[$Matches[1]] = $strings[$Matches[2]]; continue
    }
    if ($l -match '^\s*(%\S+) = OpTypeFloat (\d+)') { $widths[$Matches[1]] = [int]$Matches[2]; continue }
    if ($l -match '^\s*(%\S+) = OpTypeVector (%\S+) \d+') {
        if ($widths.ContainsKey($Matches[2])) { $widths[$Matches[1]] = $widths[$Matches[2]] }
        continue
    }
    if ($l -match 'DebugLine (%\S+) %uint_(\d+)') {
        $curFile = $sources[$Matches[1]]; $curLine = [int]$Matches[2]; continue
    }
    if ($l -match '= OpFConvert (%\S+) ') {
        $dir = if ($widths[$Matches[1]] -eq 16) { 'NARROW' } else { 'WIDEN' }
        if (($Widen -and $dir -ne 'WIDEN') -or ($Narrow -and $dir -ne 'NARROW')) { continue }
        $key = "$curFile|$curLine|$dir"
        $counts[$key] = 1 + [int]$counts[$key]
    }
}

$srcCache = @{}
function Get-SourceLine([string]$file, [int]$line) {
    if (-not $file -or $file -eq '?') { return '' }
    if (-not $srcCache.ContainsKey($file)) {
        $path = @($file, (Join-Path $root "Assets/$file"), (Join-Path $root "Assets/Shaders/$file")) |
            Where-Object { Test-Path $_ } | Select-Object -First 1
        $srcCache[$file] = if ($path) { Get-Content $path } else { $null }
    }
    $text = $srcCache[$file]
    if ($text -and $line -ge 1 -and $line -le $text.Count) { return $text[$line - 1].Trim() }
    return ''
}

$rows = $counts.GetEnumerator() | ForEach-Object {
    $f, $n, $d = $_.Key -split '\|'
    [pscustomobject]@{ Count = $_.Value; Dir = $d; Where = "$(Split-Path -Leaf $f):$n"; File = $f; Line = [int]$n }
} | Sort-Object Count -Descending

$w = ($rows | Where-Object Dir -eq 'WIDEN' | Measure-Object Count -Sum).Sum
$n = ($rows | Where-Object Dir -eq 'NARROW' | Measure-Object Count -Sum).Sum
"$Shader : $([int]$w) widen (f16->f32), $([int]$n) narrow (f32->f16) OpFConvert"
""
$rows | Select-Object -First $Top | ForEach-Object {
    "{0,4}  {1,-6}  {2,-40}  {3}" -f $_.Count, $_.Dir, $_.Where, (Get-SourceLine $_.File $_.Line)
}
