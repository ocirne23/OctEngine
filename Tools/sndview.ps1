# LOOK AT A GAME SOUND (nobody can listen for Claude): draws, per sound, a PNG with its waveform, its spectrogram
# (30 Hz .. 20 kHz, log) and its level over time, writes a .wav next to it, and prints the numbers that found the
# real faults so far: the energy per frequency band, and how much the part above 1 kHz flutters.
#
#   Tools/sndview.ps1 -Items "16:0","16:2:0:1.5"
#
# An item is  sound:variant  or  sound:variant:start:length  (seconds: a zoom). `sound` is the index in EGameSound
# (Code/Game/Private/Sound.ixx), `variant` the variant's index. -Cache is a file in the sound cache's format: the
# game's own (the default: what the last run played) or one from Tools/sndsynth.ps1 (the synthesizer as it is NOW).
#
# WHAT TO LOOK FOR (Docs/Context/Game/Sound.md):
#   band dB    energy under 40 Hz must be far down (-25 dB or less); a sound of air or water must not peak under 160 Hz
#   waveform   a row of separate spikes, or a comb in the level curve, is a chop: 30 - 150 a second is heard as a jitter
#   spectrogram  one flat block with no shape is a wall of noise; hard vertical edges are cuts
param(
    [string]$Cache = (Join-Path $PSScriptRoot "..\Assets\Local\Sounds\game.snd"),
    [string]$Out = (Join-Path $env:TEMP "OctEngineSound"),
    [Parameter(Mandatory = $true)][string[]]$Items
)

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.IO;
using System.Drawing;
using System.Drawing.Imaging;

public static class SndView
{
    static void Fft(double[] re, double[] im)
    {
        int n = re.Length;
        for (int i = 1, j = 0; i < n; i++)
        {
            int bit = n >> 1;
            for (; (j & bit) != 0; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
        }
        for (int len = 2; len <= n; len <<= 1)
        {
            double ang = -2 * Math.PI / len;
            double wr = Math.Cos(ang), wi = Math.Sin(ang);
            for (int i = 0; i < n; i += len)
            {
                double cr = 1, ci = 0;
                for (int k = 0; k < len / 2; k++)
                {
                    int a = i + k, b = i + k + len / 2;
                    double xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
                    re[b] = re[a] - xr; im[b] = im[a] - xi;
                    re[a] += xr; im[a] += xi;
                    double nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
                }
            }
        }
    }

    static Color Heat(double v) // 0..1
    {
        v = Math.Max(0, Math.Min(1, v));
        int r = (int)(255 * Math.Min(1, v * 2.2));
        int g = (int)(255 * Math.Max(0, Math.Min(1, (v - 0.35) * 2.0)));
        int b = (int)(255 * Math.Max(0, Math.Min(1, v < 0.3 ? v * 2.5 : (v - 0.75) * 4.0)));
        return Color.FromArgb(r, g, b);
    }

    public static string Run(string cache, string outDir, int sound, int variant, double start, double length)
    {
        // The cache: a 12 byte header (magic, stamp, count), then per sound a 12 byte record (sound, variant, stereo,
        // pad, rate, sample count) and the samples as 16 bit (GameSound::load, Code/Game/Private/Sound.cpp).
        byte[] d = File.ReadAllBytes(cache);
        uint count = BitConverter.ToUInt32(d, 8);
        int at = 12;
        float[] mono = null; int rate = 48000; bool stereo = false;
        for (uint i = 0; i < count && at + 12 <= d.Length; i++)
        {
            int s = d[at], v = d[at + 1]; bool st = d[at + 2] != 0;
            int r = (int)BitConverter.ToUInt32(d, at + 4); int n = (int)BitConverter.ToUInt32(d, at + 8);
            if (s == sound && v == variant)
            {
                rate = r; stereo = st;
                int ch = st ? 2 : 1; int frames = n / ch;
                mono = new float[frames];
                for (int f = 0; f < frames; f++)
                {
                    double sum = 0;
                    for (int c = 0; c < ch; c++) sum += BitConverter.ToInt16(d, at + 12 + (f * ch + c) * 2) / 32768.0;
                    mono[f] = (float)(sum / ch);
                }
                using (var w = new BinaryWriter(File.Create(Path.Combine(outDir, "snd_" + sound + "_" + variant + ".wav"))))
                {
                    int bytes = n * 2;
                    w.Write(new char[] { 'R', 'I', 'F', 'F' }); w.Write(36 + bytes); w.Write(new char[] { 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ' });
                    w.Write(16); w.Write((short)1); w.Write((short)ch); w.Write(r); w.Write(r * ch * 2); w.Write((short)(ch * 2)); w.Write((short)16);
                    w.Write(new char[] { 'd', 'a', 't', 'a' }); w.Write(bytes); w.Write(d, at + 12, bytes);
                }
                break;
            }
            at += 12 + n * 2;
        }
        if (mono == null) return "not found: " + sound + ":" + variant;

        int first = (int)(start * rate);
        int total = length > 0 ? Math.Min((int)(length * rate), mono.Length - first) : mono.Length - first;
        const int W = 1500, waveH = 220, specH = 420, envH = 160, margin = 46;
        string name = Path.Combine(outDir, "snd_" + sound + "_" + variant + (length > 0 ? "_z" + start.ToString("0.##") : "") + ".png");
        using (var bmp = new Bitmap(W + margin, waveH + specH + envH + 40))
        using (var g = Graphics.FromImage(bmp))
        using (var font = new Font("Consolas", 9))
        {
            g.Clear(Color.Black);
            // The waveform: min / max per column.
            for (int x = 0; x < W; x++)
            {
                int a = first + (int)((long)x * total / W), b = first + (int)((long)(x + 1) * total / W);
                float lo = 0, hi = 0;
                for (int i = a; i < b && i < mono.Length; i++) { lo = Math.Min(lo, mono[i]); hi = Math.Max(hi, mono[i]); }
                int y0 = (int)(waveH / 2 - hi * (waveH / 2 - 2)), y1 = (int)(waveH / 2 - lo * (waveH / 2 - 2));
                g.DrawLine(Pens.LightGreen, margin + x, y0, margin + x, Math.Max(y1, y0 + 1));
            }
            // The spectrogram: log frequency 30 Hz .. 20 kHz, -84 .. 0 dB.
            const int N = 2048;
            double[] win = new double[N];
            for (int i = 0; i < N; i++) win[i] = 0.5 - 0.5 * Math.Cos(2 * Math.PI * i / N);
            double[] re = new double[N], im = new double[N];
            double fLo = 30, fHi = 20000;
            for (int x = 0; x < W; x++)
            {
                int centre = first + (int)(((long)x * total + total / 2) / W);
                for (int i = 0; i < N; i++)
                {
                    int idx = centre - N / 2 + i;
                    re[i] = idx >= 0 && idx < mono.Length ? mono[idx] * win[i] : 0; im[i] = 0;
                }
                Fft(re, im);
                for (int y = 0; y < specH; y++)
                {
                    double f0 = fLo * Math.Pow(fHi / fLo, (double)(specH - 1 - y) / specH), f1 = fLo * Math.Pow(fHi / fLo, (double)(specH - y) / specH);
                    int b0 = Math.Max(1, (int)(f0 * N / rate)), b1 = Math.Max(b0 + 1, (int)Math.Ceiling(f1 * N / rate));
                    double p = 0;
                    for (int k = b0; k < b1 && k < N / 2; k++) p = Math.Max(p, re[k] * re[k] + im[k] * im[k]);
                    double db = 10 * Math.Log10(p / (N * N / 16.0) + 1e-12);
                    bmp.SetPixel(margin + x, waveH + 10 + y, Heat((db + 84) / 84));
                }
            }
            // The LEVEL over time (RMS in 5 ms windows, 0 .. -60 dB): a chop is a comb here. (A sound that is mostly
            // under 100 Hz shows its own cycles in this curve: that is no chop.)
            int hop = Math.Max(1, rate / 200);
            int steps = total / hop;
            Point[] pts = new Point[Math.Max(2, steps)];
            for (int s = 0; s < pts.Length; s++)
            {
                double sum = 0; int a = first + s * hop;
                for (int i = a; i < a + hop && i < mono.Length; i++) sum += mono[i] * mono[i];
                double db = 10 * Math.Log10(sum / hop + 1e-9);
                int y = waveH + specH + 20 + (int)(Math.Max(0, Math.Min(1, -db / 60)) * envH);
                pts[s] = new Point(margin + (int)((long)s * W / pts.Length), y);
            }
            g.DrawLines(Pens.Yellow, pts);
            // The axes.
            double secs = (double)total / rate;
            double tick = secs > 6 ? 1 : secs > 2 ? 0.5 : secs > 0.6 ? 0.1 : 0.02;
            for (double t = 0; t <= secs; t += tick)
            {
                int x = margin + (int)(t / secs * W);
                g.DrawLine(Pens.DimGray, x, 0, x, waveH + specH + envH + 20);
                g.DrawString((start + t).ToString("0.##"), font, Brushes.White, x + 2, waveH + specH + envH + 22);
            }
            foreach (double f in new double[] { 50, 100, 200, 500, 1000, 2000, 5000, 10000 })
            {
                int y = waveH + 10 + specH - 1 - (int)(Math.Log(f / fLo) / Math.Log(fHi / fLo) * specH);
                g.DrawLine(Pens.DimGray, margin, y, margin + W, y);
                g.DrawString(f >= 1000 ? (f / 1000) + "k" : f.ToString(), font, Brushes.White, 2, y - 6);
            }
            g.DrawString("0dB", font, Brushes.Yellow, 2, waveH + specH + 20);
            g.DrawString("-60", font, Brushes.Yellow, 2, waveH + specH + 20 + envH - 12);
            bmp.Save(name, ImageFormat.Png);
        }

        // THE NUMBERS. The energy per band (its share of the whole, dB), from one long FFT of the window.
        int M = 1; while (M < total) M <<= 1; M = Math.Min(M, 1 << 20);
        double[] br = new double[M], bi = new double[M];
        for (int i = 0; i < M && i < total; i++) br[i] = mono[first + i];
        Fft(br, bi);
        double[] edges = { 0, 20, 40, 80, 160, 320, 640, 1280, 2560, 5120, 10240, 24000 };
        double[] band = new double[edges.Length - 1]; double all = 1e-20;
        for (int k = 1; k < M / 2; k++)
        {
            double f = (double)k * rate / M, p = br[k] * br[k] + bi[k] * bi[k];
            all += p;
            for (int e = 0; e < band.Length; e++) if (f >= edges[e] && f < edges[e + 1]) band[e] += p;
        }
        string text = name + "  (" + ((double)mono.Length / rate).ToString("0.00") + " s, " + (stereo ? "stereo" : "mono") + ")\n  band dB:";
        for (int e = 0; e < band.Length; e++) text += "  " + edges[e] + "-" + edges[e + 1] + ": " + (10 * Math.Log10(band[e] / all + 1e-12)).ToString("0");
        // The FLUTTER of the part above 1 kHz: its level (rectified, smoothed to 60 Hz) against its own slow trend.
        // Steady noise gives about 0.15 - 0.25; a level that is chopped gives much more. (A short one-shot is all
        // attack and decay: the number says nothing there.)
        double hp1 = 0, hp2 = 0, x1 = 0, x2 = 0, env = 0, slow = 0;
        double aHp = Math.Exp(-2 * Math.PI * 1000.0 / rate), aEnv = 1 - Math.Exp(-2 * Math.PI * 60.0 / rate), aSlow = 1 - Math.Exp(-2 * Math.PI * 1.5 / rate);
        double sumDev = 0, sumW = 0, peak = 0, sq = 0;
        for (int i = 0; i < total; i++)
        {
            double x = mono[first + i];
            peak = Math.Max(peak, Math.Abs(x)); sq += x * x;
            hp1 = aHp * (hp1 + x - x1); x1 = x;
            hp2 = aHp * (hp2 + hp1 - x2); x2 = hp1;
            env += (Math.Abs(hp2) - env) * aEnv;
            slow += (env - slow) * aSlow;
            if (i > rate / 4 && slow > 1e-4) { double dev = env / slow - 1; sumDev += dev * dev * slow; sumW += slow; }
        }
        double crest = peak / Math.Sqrt(sq / total + 1e-20);
        text += "\n  flutter above 1 kHz: " + Math.Sqrt(sumDev / Math.Max(sumW, 1e-9)).ToString("0.00") + "   peak " + peak.ToString("0.00") + "   crest " + crest.ToString("0.0");
        return text;
    }
}
'@

New-Item -ItemType Directory -Force $Out | Out-Null
foreach ($item in $Items)
{
    $p = $item.Split(':')
    $start = 0.0; $len = 0.0
    if ($p.Length -ge 4) { $start = [double]::Parse($p[2], [cultureinfo]::InvariantCulture); $len = [double]::Parse($p[3], [cultureinfo]::InvariantCulture) }
    [SndView]::Run((Resolve-Path $Cache).Path, (Resolve-Path $Out).Path, [int]$p[0], [int]$p[1], $start, $len)
}
