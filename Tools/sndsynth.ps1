# SYNTHESIZE GAME SOUNDS WITHOUT THE GAME: builds Code/Game/Private/SoundSynth.cpp alone as a console tool (no engine,
# no window: a few seconds), synthesizes the given sounds into a file in the sound cache's format, and - with -View -
# draws them with Tools/sndview.ps1. This is Claude's change -> look -> change loop for the sound synthesis.
#
#   Tools/sndsynth.ps1 -Items "16:0","19:0" -View
#   Tools/sndsynth.ps1 -Items "16:0" -View -Zoom "16:0:0:1.5"       (also a zoom on the first 1.5 s)
#
# An item is sound:variant - the index in EGameSound (Code/Game/Private/Sound.ixx) and the variant's index.
# The tool compiles the synthesizer's source as it is NOW, with a few lines in place of the engine (oc::, glm::, Log),
# so SoundSynth.cpp must keep to what it uses today: oc::vector / oc::span / oc::size, the glm functions below, std math.
param(
    [Parameter(Mandatory = $true)][string[]]$Items,
    [string]$OutFile = (Join-Path $env:TEMP "OctEngineSound\synth.snd"),
    [switch]$View,
    [string[]]$Zoom = @()
)

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
$work = Join-Path $env:TEMP "OctEngineSound\build"
New-Item -ItemType Directory -Force $work | Out-Null

# The synthesizer's source without its module lines.
$body = Get-Content (Join-Path $repo "Code/Game/Private/SoundSynth.cpp") | Where-Object { $_ -notmatch '^(module|import)\b' }
Set-Content -Path (Join-Path $work "synth_body.inc") -Value $body -Encoding utf8

# The sound enum, from the game's own header.
$header = Get-Content -Raw (Join-Path $repo "Code/Game/Private/Sound.ixx")
if ($header -notmatch '(?s)export (enum class EGameSound : uint8\s*\{.*?\};)') { throw "EGameSound not found in Sound.ixx" }
$enum = $Matches[1]

$main = @"
#include <vector>
#include <span>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
using uint8 = uint8_t; using int8 = int8_t; using uint16 = uint16_t; using int16 = int16_t;
using uint32 = uint32_t; using int32 = int32_t; using uint64 = uint64_t; using int64 = int64_t;
namespace oc { using std::vector; using std::span; template <class T, size_t N> constexpr size_t size(const T (&)[N]) { return N; } }
namespace glm
{
    template <class T> constexpr T min(T a, T b) { return b < a ? b : a; }
    template <class T> constexpr T max(T a, T b) { return a < b ? b : a; }
    template <class T> constexpr T clamp(T v, T lo, T hi) { return v < lo ? lo : hi < v ? hi : v; }
    template <class T> constexpr T mix(T a, T b, T t) { return a + (b - a) * t; }
    template <class T> T abs(T v) { return v < 0 ? -v : v; }
    inline float smoothstep(float e0, float e1, float x) { float t = clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
    template <class T> constexpr T two_pi() { return T(6.283185307179586); }
}
namespace Log { inline void warning(const char* text) { puts(text); } }
$enum
constexpr int c_gameSoundVariants = 6;
void synthGameSound(EGameSound sound, int variant, oc::vector<float>& out, bool& stereo, uint32& rate);
uint32 synthGameSoundStamp();
#include "synth_body.inc"

int main(int argc, char** argv)
{
    FILE* file = fopen(argv[1], "wb");
    if (!file) return 1;
    const uint32 header[3] = { 0x444e5347u, 0u, (uint32)(argc - 2) };
    fwrite(header, 4, 3, file);
    oc::vector<float> samples;
    oc::vector<int16> packed;
    for (int i = 2; i < argc; ++i)
    {
        int sound = 0, variant = 0;
        sscanf(argv[i], "%d:%d", &sound, &variant);
        bool stereo = false; uint32 rate = 48000;
        synthGameSound((EGameSound)sound, variant, samples, stereo, rate);
        packed.resize(samples.size());
        for (size_t k = 0; k < samples.size(); ++k)
            packed[k] = (int16)(glm::clamp(samples[k], -1.0f, 1.0f) * 32767.0f);
        const uint8 record[4] = { (uint8)sound, (uint8)variant, (uint8)(stereo ? 1 : 0), 0 };
        const uint32 sizes[2] = { rate, (uint32)packed.size() };
        fwrite(record, 1, 4, file); fwrite(sizes, 4, 2, file); fwrite(packed.data(), 2, packed.size(), file);
    }
    fclose(file);
    return 0;
}
"@
Set-Content -Path (Join-Path $work "main.cpp") -Value $main -Encoding utf8

# The compiler: the newest Visual Studio (vswhere), through its vcvars64.
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -prerelease -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found under $vs" }
Remove-Item (Join-Path $work "sndsynth.exe") -ErrorAction SilentlyContinue
$build = "`"$vcvars`" >nul 2>&1 && cd /d `"$work`" && cl /nologo /std:c++latest /O2 /EHsc /fp:fast /W3 /D_CRT_SECURE_NO_WARNINGS main.cpp /Fe:sndsynth.exe"
cmd /c $build | Where-Object { $_ -match 'error|warning C' } | Select-Object -First 30
if (-not (Test-Path (Join-Path $work "sndsynth.exe"))) { throw "the synthesizer did not build" }

New-Item -ItemType Directory -Force (Split-Path -Parent $OutFile) | Out-Null
$sounds = @($Items | ForEach-Object { ($_.Split(':')[0..1]) -join ':' } | Select-Object -Unique) # an array also for ONE item: a string would splat as its characters
& (Join-Path $work "sndsynth.exe") $OutFile @sounds
"synthesized $(@($sounds).Count) sounds into $OutFile"
if ($View)
{
    & (Join-Path $PSScriptRoot "sndview.ps1") -Cache $OutFile -Items (@($Items) + @($Zoom))
}
