module Game;

import Core;
import Core.glm;
import Core.Log;
import :Sound;

// THE SYNTHESIZED SOUNDS: the game has its full set of sounds with no sound file. Each sound is a few LAYERS mixed on a
// canvas, from these building blocks:
//   thump    a burst of low noise into a damped low resonance: the weight of a hit, a shot, an explosion
//   noise    white / pink / brown noise through a filter that glides: slaps, air, rumble, crackle
//   modal    a struck body: noise that dies away, through a bank of resonances (wood, stone, metal, a tube)
//   vocal    irregular breathy pulses through three formants that move: a creature's grunt and growl
//   reverb   a room for the whole canvas (comb + allpass network), so nothing sounds as if it came from a wire
// THE RULE: NOISE IS THE SOURCE, A FILTER GIVES IT ITS PITCH. A pure sine, a sine whose pitch slides, and a regular
// tremolo are what makes a synthesized sound read as an old game console, so no layer is one: a thump has no pitch
// slide, a struck body is never a sum of sines, a voice's pulses are never regular. The only tones are the birds, the
// crickets and the owl (whistle: a real whistle is a tone; it carries breath and its pitch is not steady).
// A LOOP mixes its events with a wrap (an event that runs over the end continues at the start) and runs every
// filter and the reverb twice around, keeping the second round: the loop has no seam.
// Run once, at GameSound's first update. Everything is 48 kHz (the engine's rate: no resampling at pitch 1).

namespace
{
    constexpr float c_sr = 48000.0f;
    constexpr float c_tau = 6.2831853f;

    using Buf = oc::vector<float>; // one mono layer

    struct Rng
    {
        uint32 s;
        float bi() // -1..1
        {
            s = s * 1664525u + 1013904223u;
            return float(s >> 8) * (1.0f / 8388608.0f) - 1.0f;
        }
        float uni() { return bi() * 0.5f + 0.5f; }
        float range(float a, float b) { return a + (b - a) * uni(); }
        int pick(int count) { return glm::min((int)(uni() * (float)count), count - 1); }
    };

    // A state-variable filter that stays stable while its frequency moves (the trapezoidal form).
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
        float lastHz = -1.0f, lastQ = -1.0f;
        float lp = 0.0f, bp = 0.0f, hp = 0.0f; // bp has a peak gain of 1

        void set(float hz, float q)
        {
            if (glm::abs(hz - lastHz) < lastHz * 0.002f && q == lastQ)
                return;
            lastHz = hz;
            lastQ = q;
            g = std::tan(0.5f * c_tau * glm::clamp(hz, 10.0f, c_sr * 0.45f) / c_sr);
            k = 1.0f / glm::max(q, 0.05f);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }
        void step(float in)
        {
            const float v3 = in - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            lp = v2;
            bp = k * v1;
            hp = in - k * v1 - v2;
        }
    };

    enum EColor { White, Pink, Brown };
    enum EFilter { Low, Band, High };

    struct Noise
    {
        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, brown = 0.0f;
        float next(Rng& rng, EColor color)
        {
            const float w = rng.bi();
            if (color == White)
                return w;
            if (color == Pink)
            {
                b0 = 0.99765f * b0 + w * 0.0990460f;
                b1 = 0.96300f * b1 + w * 0.2965164f;
                b2 = 0.57000f * b2 + w * 1.0526913f;
                return (b0 + b1 + b2 + w * 0.1848f) * 0.25f;
            }
            brown = (brown + w * 0.02f) * 0.998f;
            return brown * 6.0f;
        }
    };

    float hann(float u) { return 0.5f - 0.5f * std::cos(c_tau * glm::clamp(u, 0.0f, 1.0f)); }
    uint32 framesOf(float seconds) { return (uint32)(glm::max(seconds, 0.0f) * c_sr); }

    // ---- Layers ----

    // THE WEIGHT of a blow: a short burst of low noise into a damped resonance between hz0 and hz1 (it rings for a
    // cycle or two: a thud, never a note), with duller noise around it that dies over `decay`. Peak 1.
    // strike = how long the burst is; drive saturates it.
    Buf thump(float dur, float hz0, float hz1, float strike, float decay, float drive)
    {
        Buf out(framesOf(dur));
        Rng r{ (uint32)(hz0 * 131.0f + hz1 * 17.0f + dur * 10000.0f + decay * 7777.0f) | 1u };
        Noise low, around;
        Svf body, air;
        const float hz = glm::max(std::sqrt(hz0 * hz1), 35.0f);
        body.set(hz, glm::clamp(0.5f * c_tau * hz * decay * 0.5f, 0.8f, 3.5f));
        air.set(hz * 3.5f, 0.7f);
        const float burst = glm::max(strike * 0.5f, 0.004f);
        float top = 1e-6f;
        for (size_t i = 0; i < out.size(); ++i)
        {
            const float t = (float)i / c_sr;
            body.step(low.next(r, Brown) * std::exp(-t / burst));
            air.step(around.next(r, Pink) * std::exp(-t / (decay * 0.6f)));
            out[i] = (body.lp + air.lp * 0.25f) * glm::min(t / 0.0006f, 1.0f) * (1.0f - glm::smoothstep(0.85f, 1.0f, (float)i / (float)out.size()));
            top = glm::max(top, glm::abs(out[i]));
        }
        const float amount = glm::max(drive * 0.6f, 0.1f), norm = 1.0f / std::tanh(amount);
        for (float& v : out)
            v = std::tanh(v / top * amount) * norm;
        return out;
    }

    // Noise through a filter that glides from hz0 to hz1 over the burst.
    Buf noise(Rng& rng, float dur, EColor color, EFilter filter, float hz0, float hz1, float q, float attack, float decay)
    {
        Buf out(framesOf(dur));
        Noise source;
        Svf f;
        const float glide = std::log(hz1 / hz0);
        const float n = (float)glm::max(out.size(), (size_t)1);
        for (size_t i = 0; i < out.size(); ++i)
        {
            const float t = (float)i / c_sr, u = (float)i / n;
            if ((i & 7) == 0)
                f.set(hz0 * std::exp(glide * u), q);
            f.step(source.next(rng, color));
            const float v = filter == Low ? f.lp : filter == Band ? f.bp : f.hp;
            out[i] = v * glm::min(t / attack, 1.0f) * std::exp(-t / decay) * (1.0f - glm::smoothstep(0.85f, 1.0f, u));
        }
        return out;
    }

    struct Mode { float hz, gain, decay; };

    // A STRUCK BODY: noise that dies away (the strike and what it leaves in the body), through one resonance per mode.
    // The noise keeps driving every resonance while it rings, so a mode is a narrow band of noise whose level and
    // phase wander - what a real board or stone does - and never a sine. maxQ bounds how narrow (how near a tone).
    // scale moves all pitches, ring all decays. Peak 1.
    Buf modal(oc::span<const Mode> modes, float dur, float scale = 1.0f, float ring = 1.0f, float maxQ = 22.0f)
    {
        Buf out(framesOf(dur), 0.0f);
        const uint32 seed = (uint32)(modes[0].hz * scale * 13.0f + dur * 9000.0f + ring * 501.0f) | 1u;
        for (const Mode& m : modes)
        {
            const float hz = m.hz * scale;
            if (hz > c_sr * 0.4f)
                continue;
            const float decay = m.decay * ring;
            Svf f;
            const float q = glm::clamp(0.5f * c_tau * hz * decay, 3.0f, maxQ);
            f.set(hz, q);
            Rng r{ seed }; // the same strike for every mode
            const float gain = m.gain * std::sqrt(q);
            for (size_t i = 0; i < out.size(); ++i)
            {
                const float t = (float)i / c_sr;
                f.step(r.bi() * (std::exp(-t / decay) + 3.0f * std::exp(-t / 0.002f)));
                out[i] += f.bp * gain;
            }
        }
        float top = 1e-6f;
        for (const float v : out)
            top = glm::max(top, glm::abs(v));
        const size_t fade = glm::min(out.size(), (size_t)framesOf(0.01f));
        for (size_t i = 0; i < out.size(); ++i)
            out[i] *= (i + fade >= out.size() ? (float)(out.size() - 1 - i) / (float)fade : 1.0f) / top;
        return out;
    }

    constexpr Mode c_wood[] = { { 310.0f, 1.0f, 0.07f }, { 795.0f, 0.6f, 0.045f }, { 1420.0f, 0.4f, 0.03f }, { 2310.0f, 0.25f, 0.018f }, { 3400.0f, 0.12f, 0.012f } };
    constexpr Mode c_metal[] = { { 2100.0f, 1.0f, 0.05f }, { 3370.0f, 0.6f, 0.04f }, { 5230.0f, 0.35f, 0.025f }, { 7100.0f, 0.15f, 0.02f } };
    constexpr Mode c_crystal[] = { { 1175.0f, 0.4f, 0.3f }, { 2350.0f, 0.5f, 0.25f }, { 3520.0f, 0.35f, 0.2f }, { 5280.0f, 0.2f, 0.12f } };
    constexpr Mode c_tube[] = { { 185.0f, 0.6f, 0.09f }, { 430.0f, 0.3f, 0.05f }, { 760.0f, 0.15f, 0.03f } };
    constexpr Mode c_drum[] = { { 92.0f, 1.0f, 0.22f }, { 148.0f, 0.5f, 0.12f }, { 233.0f, 0.3f, 0.07f }, { 410.0f, 0.15f, 0.04f } };
    constexpr Mode c_glow[] = { { 660.0f, 1.0f, 0.5f }, { 990.0f, 0.6f, 0.45f }, { 1320.0f, 0.35f, 0.35f }, { 1980.0f, 0.15f, 0.25f } };

    // A stone that hits a stone: two short partials and a click.
    Buf rockClack(Rng& rng)
    {
        const float hz = rng.range(500.0f, 2600.0f);
        const Mode modes[] = { { hz, 1.0f, rng.range(0.008f, 0.03f) }, { hz * rng.range(1.4f, 1.9f), 0.6f, rng.range(0.006f, 0.02f) },
            { hz * rng.range(2.3f, 3.1f), 0.3f, 0.008f } };
        Buf out = modal(modes, 0.12f);
        const Buf click = noise(rng, 0.004f, White, High, 3000.0f, 3000.0f, 0.7f, 0.0002f, 0.0015f);
        for (size_t i = 0; i < click.size(); ++i)
            out[i] += click[i] * 0.5f;
        return out;
    }

    // A whistled note from hz0 to hz1 under a smooth envelope (a bird, a cricket's pulse, an owl). A real whistle is a
    // tone, but not a clean one: the pitch is never steady, and breath - a narrow band of noise at the same pitch -
    // goes with it.
    Buf whistle(float dur, float hz0, float hz1, float vibHz = 0.0f, float vibDepth = 0.0f, float harmonic = 0.15f)
    {
        Buf out(framesOf(dur));
        Rng r{ (uint32)(hz0 * 7.0f + hz1 * 3.0f + dur * 50000.0f) | 1u };
        Svf breath;
        float phase = 0.0f, unsteady = 0.0f;
        const float glide = std::log(hz1 / hz0);
        const float n = (float)glm::max(out.size(), (size_t)1);
        for (size_t i = 0; i < out.size(); ++i)
        {
            const float t = (float)i / c_sr, u = (float)i / n;
            unsteady += (r.bi() * 0.02f - unsteady) * 0.004f;
            const float hz = hz0 * std::exp(glide * u) * (1.0f + vibDepth * std::sin(c_tau * vibHz * t) + unsteady);
            phase += c_tau * hz / c_sr;
            if ((i & 7) == 0)
                breath.set(hz, 14.0f);
            breath.step(r.bi());
            out[i] = (std::sin(phase) * 0.7f + std::sin(phase * 2.0f) * harmonic + breath.bp * 1.6f) * hann(u);
        }
        return out;
    }

    // THE VOICE of a creature. The keys give, at their times: the pitch, the three formants (the shape of the mouth),
    // the loudness, the breath (noise in the source) and the roughness (every second pulse weaker: a growl).
    struct VocalKey { float t, pitch, f1, f2, f3, amp, breath, rough; };

    Buf vocal(Rng& rng, oc::span<const VocalKey> keys, float pitchScale = 1.0f, float timeScale = 1.0f, float drive = 2.2f)
    {
        // THE SOURCE is a throat, not an oscillator: soft pulses (the folds open for 40 % of a period: little above the
        // first formants, so no buzz) whose length and strength change from one to the next by `rough`, and breath -
        // noise that is always there and gets louder with every pulse. The formants are wide (a mouth, not a bell).
        Buf out(framesOf(keys.back().t * timeScale));
        Svf f1, f2, f3, body;
        Noise breathNoise;
        float phase = 0.0f, drift = 0.0f, periodScale = 1.0f, strength = 1.0f;
        size_t key = 0;
        body.set(300.0f, 0.7f);
        for (size_t i = 0; i < out.size(); ++i)
        {
            const float t = (float)i / c_sr / timeScale;
            while (key + 2 < keys.size() && t >= keys[key + 1].t)
                ++key;
            const VocalKey& a = keys[key];
            const VocalKey& b = keys[key + 1];
            const float u = glm::clamp((t - a.t) / glm::max(b.t - a.t, 1e-4f), 0.0f, 1.0f);
            const float rough = glm::mix(a.rough, b.rough, u);
            const float breath = glm::mix(a.breath, b.breath, u);
            drift += (rng.bi() * 0.06f - drift) * 0.0015f; // the pitch is never steady
            phase += glm::mix(a.pitch, b.pitch, u) * pitchScale * (1.0f + drift) * periodScale / c_sr;
            if (phase >= 1.0f)
            {
                phase -= 1.0f;
                periodScale = 1.0f + rng.bi() * rough * 0.18f; // the next pulse: longer or shorter, weaker or stronger
                strength = 1.0f - rng.uni() * rough * 0.55f;
            }
            const float open = phase < 0.4f ? std::sin(0.5f * c_tau * phase / 0.4f) : 0.0f;
            const float pulse = open * open * strength;
            const float source = pulse * (1.0f - 0.5f * glm::min(breath, 1.0f)) + breathNoise.next(rng, Pink) * (breath * 0.9f + pulse * (0.4f + breath));
            if ((i & 7) == 0)
            {
                f1.set(glm::mix(a.f1, b.f1, u), 4.5f);
                f2.set(glm::mix(a.f2, b.f2, u), 5.5f);
                f3.set(glm::mix(a.f3, b.f3, u), 6.5f);
            }
            f1.step(source);
            f2.step(source);
            f3.step(source);
            body.step(source);
            const float v = f1.bp * 2.0f + f2.bp * 1.4f + f3.bp * 0.7f + body.bp * 0.5f; // band passes only: the pulses' offset stays out
            out[i] = std::tanh(v * glm::mix(a.amp, b.amp, u) * drive * 0.6f);
        }
        return out;
    }

    // ---- Work on a layer ----

    void put(Buf& dst, const Buf& src, float start, float gain)
    {
        const size_t first = framesOf(start);
        if (dst.size() < first + src.size())
            dst.resize(first + src.size(), 0.0f);
        for (size_t i = 0; i < src.size(); ++i)
            dst[first + i] += src[i] * gain;
    }

    void lowpass(Buf& b, float hz)
    {
        const float a = 1.0f - std::exp(-c_tau * hz / c_sr);
        float s1 = 0.0f, s2 = 0.0f;
        for (float& v : b)
        {
            s1 += (v - s1) * a;
            s2 += (s1 - s2) * a;
            v = s2;
        }
    }

    void saturate(Buf& b, float drive)
    {
        const float norm = 1.0f / std::tanh(drive);
        for (float& v : b)
            v = std::tanh(v * drive) * norm;
    }

    void tremolo(Buf& b, float hz, float depth, bool square)
    {
        for (size_t i = 0; i < b.size(); ++i)
        {
            const float s = std::sin(c_tau * hz * (float)i / c_sr);
            b[i] *= 1.0f - depth * (square ? (s > 0.0f ? 0.0f : 1.0f) : 0.5f - 0.5f * s);
        }
    }

    // An uneven level: random values `hz` times a second, with straight lines between them. Where a regular tremolo
    // would sound like a machine (a saw's teeth, a gurgle).
    void flutter(Buf& b, Rng& rng, float hz, float depth)
    {
        const float step = hz / c_sr;
        float from = rng.uni(), to = rng.uni(), u = 0.0f;
        for (float& v : b)
        {
            u += step;
            if (u >= 1.0f)
            {
                u -= 1.0f;
                from = to;
                to = rng.uni();
            }
            v *= 1.0f - depth * glm::mix(from, to, u);
        }
    }

    float peakOf(const Buf& b)
    {
        float top = 1e-6f;
        for (const float v : b)
            top = glm::max(top, glm::abs(v));
        return top;
    }
    void level(Buf& b) // to a peak of 1: a layer's gain in the mix then means the same for every layer
    {
        const float scale = 1.0f / peakOf(b);
        for (float& v : b)
            v *= scale;
    }

    // ---- The creature sounds ----

    // VARIETY: no two variants of a sound are the same sound at another pitch. Each one rolls its own layers from its
    // own random stream, and `kind` picks the character, so a set of variants covers every character.

    // The pitches and the mouth of a voice, moved a little: every throat is its own.
    template <size_t N>
    void varyVoice(Rng& rng, VocalKey (&keys)[N], float pitchAmount, float formantAmount)
    {
        const float f1 = 1.0f + rng.bi() * formantAmount, f2 = 1.0f + rng.bi() * formantAmount, f3 = 1.0f + rng.bi() * formantAmount * 0.5f;
        for (VocalKey& key : keys)
        {
            key.pitch *= 1.0f + rng.bi() * pitchAmount;
            key.f1 *= f1;
            key.f2 *= f2;
            key.f3 *= f3;
        }
    }

    // kind: 0 blunt (a heavy blow), 1 claw (a slash through the air, then the cut), 2 wet (a blow that goes in),
    // 3 crunch (something breaks).
    Buf makeMeleeHit(Rng& rng, int kind)
    {
        kind &= 3;
        Buf out;
        const float weight = kind == 0 ? 1.0f : kind == 1 ? 0.45f : 0.8f;
        put(out, thump(0.18f, rng.range(130.0f, 200.0f), rng.range(48.0f, 68.0f), rng.range(0.015f, 0.03f), rng.range(0.035f, 0.07f), 2.5f), 0.0f, weight);
        const float slap = rng.range(1000.0f, 2400.0f);
        put(out, noise(rng, 0.12f, Pink, Band, slap, slap * rng.range(0.4f, 0.7f), rng.range(0.9f, 1.8f), 0.001f, rng.range(0.02f, 0.035f)), 0.0f, 2.2f);
        put(out, noise(rng, 0.004f, White, High, 3000.0f, 3000.0f, 0.7f, 0.0002f, 0.0015f), 0.0f, 0.5f);
        if (kind == 1)
        {
            const float top = rng.range(4500.0f, 7000.0f);
            put(out, noise(rng, 0.07f, White, Band, top, top * 0.35f, 2.0f, 0.012f, 0.02f), 0.0f, 2.0f);
            put(out, noise(rng, 0.03f, White, High, 5000.0f, 3000.0f, 1.0f, 0.0005f, 0.008f), 0.035f, 0.8f);
        }
        else if (kind == 2)
            put(out, noise(rng, 0.14f, White, Low, rng.range(1800.0f, 2800.0f), rng.range(250.0f, 400.0f), rng.range(3.0f, 5.0f), 0.003f, 0.04f), 0.0f, 0.6f);
        else if (kind == 3)
            for (int i = 0; i < 2 + rng.pick(2); ++i)
                put(out, rockClack(rng), 0.004f + 0.02f * (float)i * rng.uni(), rng.range(0.25f, 0.5f));
        level(out);
        return out;
    }

    // A DEATH GRUNT: the last breath goes out (0.2 - 0.4 s) and the body goes down. Not a scream either.
    // kind: 0 a grunt that falls, 1 a short one, 2 a long breath out, 3 a gurgle.
    Buf makeDeathCry(Rng& rng, float pitch, int kind = 0)
    {
        kind &= 3;
        VocalKey keys[] = {
            { 0.00f, 120.0f, 600.0f, 1050.0f, 2400.0f, 0.0f,  0.5f, 0.6f },
            { 0.03f, 130.0f, 650.0f, 1100.0f, 2400.0f, 1.0f,  0.5f, 0.6f },
            { 0.14f, 100.0f, 520.0f, 950.0f,  2300.0f, 0.75f, 0.7f, 0.8f },
            { 0.24f, 78.0f,  460.0f, 850.0f,  2250.0f, 0.3f,  0.9f, 0.9f },
            { 0.32f, 65.0f,  420.0f, 800.0f,  2200.0f, 0.0f,  1.0f, 1.0f } };
        if (kind == 2)
            for (VocalKey& key : keys)
                key.breath *= 1.6f;
        varyVoice(rng, keys, 0.08f, 0.15f);
        const float time = kind == 1 ? rng.range(0.6f, 0.75f) : kind == 2 ? rng.range(1.1f, 1.3f) : rng.range(0.85f, 1.1f);
        Buf out = vocal(rng, keys, pitch, time, 1.6f);
        if (kind == 3)
            flutter(out, rng, rng.range(14.0f, 22.0f), 0.8f);
        level(out);
        // The body goes down: as much of the sound as the voice is.
        put(out, thump(0.2f, rng.range(75.0f, 105.0f), 45.0f, 0.03f, 0.06f, 2.0f), 0.2f * time, 0.8f);
        Buf cloth = noise(rng, 0.1f, Pink, Low, rng.range(900.0f, 1500.0f), 400.0f, 0.8f, 0.002f, 0.03f);
        level(cloth);
        put(out, cloth, 0.2f * time, 0.4f);
        if (kind != 1) // the shadow comes apart: a short, soft hiss
        {
            Buf hiss = noise(rng, 0.25f, Pink, High, rng.range(2500.0f, 3500.0f), rng.range(5000.0f, 6500.0f), 0.7f, 0.05f, 0.09f);
            level(hiss);
            put(out, hiss, 0.12f * time, rng.range(0.1f, 0.2f));
        }
        level(out);
        return out;
    }

    // ---- The canvas: the sound itself ----

    struct Canvas
    {
        oc::vector<float>& samples;
        uint32 frames = 0;
        uint32 channels = 1;
        bool loop = false;

        void addAt(uint32 frame, uint32 channel, float value)
        {
            samples[(size_t)frame * channels + channel] += value;
        }

        void mix(const Buf& b, float start, float gain, float pan = 0.5f)
        {
            const uint32 first = framesOf(start);
            const float left = channels == 1 ? gain : gain * std::cos(pan * c_tau * 0.25f) * 1.4f;
            const float right = gain * std::sin(pan * c_tau * 0.25f) * 1.4f;
            for (size_t i = 0; i < b.size(); ++i)
            {
                uint32 frame = first + (uint32)i;
                if (frame >= frames)
                {
                    if (!loop)
                        return;
                    frame %= frames;
                }
                samples[(size_t)frame * channels] += b[i] * left;
                if (channels == 2)
                    samples[(size_t)frame * 2 + 1] += b[i] * right;
            }
        }

        // The room. room 0..1 = its size (the tail's length), damp = how fast the highs die, wet = how much of it.
        void reverb(float room, float damp, float wet)
        {
            constexpr int c_comb[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 }; // at 44.1 kHz
            constexpr int c_allpass[4] = { 556, 441, 341, 225 };
            const float feedback = 0.7f + 0.28f * glm::clamp(room, 0.0f, 1.0f);
            const float damping = glm::clamp(damp, 0.0f, 1.0f) * 0.4f;
            for (uint32 channel = 0; channel < channels; ++channel)
            {
                Buf comb[8], allpass[4];
                float store[8] = {};
                uint32 combAt[8] = {}, allpassAt[4] = {};
                for (int i = 0; i < 8; ++i)
                    comb[i].assign((size_t)((float)(c_comb[i] + (int)channel * 23) * c_sr / 44100.0f), 0.0f);
                for (int i = 0; i < 4; ++i)
                    allpass[i].assign((size_t)((float)(c_allpass[i] + (int)channel * 23) * c_sr / 44100.0f), 0.0f);
                for (int pass = loop ? 0 : 1; pass < 2; ++pass)
                    for (uint32 frame = 0; frame < frames; ++frame)
                    {
                        float& sample = samples[(size_t)frame * channels + channel];
                        const float in = sample * 0.015f;
                        float sum = 0.0f;
                        for (int i = 0; i < 8; ++i)
                        {
                            float& cell = comb[i][combAt[i]];
                            sum += cell;
                            store[i] = cell * (1.0f - damping) + store[i] * damping;
                            cell = in + store[i] * feedback;
                            if (++combAt[i] >= comb[i].size())
                                combAt[i] = 0;
                        }
                        for (int i = 0; i < 4; ++i)
                        {
                            float& cell = allpass[i][allpassAt[i]];
                            const float delayed = cell;
                            cell = sum + delayed * 0.5f;
                            sum = delayed - sum;
                            if (++allpassAt[i] >= allpass[i].size())
                                allpassAt[i] = 0;
                        }
                        if (pass == 1)
                            sample += sum * wet * 3.0f;
                    }
            }
        }

        void lowpassAll(float hz)
        {
            const float a = 1.0f - std::exp(-c_tau * hz / c_sr);
            for (uint32 channel = 0; channel < channels; ++channel)
            {
                float s1 = 0.0f, s2 = 0.0f;
                for (int pass = loop ? 0 : 1; pass < 2; ++pass)
                    for (uint32 frame = 0; frame < frames; ++frame)
                    {
                        float& sample = samples[(size_t)frame * channels + channel];
                        s1 += (sample - s1) * a;
                        s2 += (s1 - s2) * a;
                        if (pass == 1)
                            sample = s2;
                    }
            }
        }

        // The last step of every sound: no offset, no value that is not a number, soft edges on a one-shot, peak at 0.9.
        void finish()
        {
            const float a = 1.0f - std::exp(-c_tau * 20.0f / c_sr);
            bool finite = true;
            for (uint32 channel = 0; channel < channels; ++channel)
            {
                float offset = 0.0f;
                for (int pass = loop ? 0 : 1; pass < 2; ++pass)
                    for (uint32 frame = 0; frame < frames; ++frame)
                    {
                        float& sample = samples[(size_t)frame * channels + channel];
                        finite &= std::isfinite(sample);
                        offset += (sample - offset) * a;
                        if (pass == 1)
                            sample -= offset;
                    }
            }
            if (!finite)
            {
                Log::warning("GameSound: a synthesized sound is not finite - it stays silent");
                for (float& s : samples)
                    s = 0.0f;
                return;
            }
            if (!loop)
            {
                const uint32 fadeIn = glm::min(framesOf(0.0005f), frames), fadeOut = glm::min(framesOf(0.012f), frames);
                for (uint32 channel = 0; channel < channels; ++channel)
                {
                    for (uint32 i = 0; i < fadeIn; ++i)
                        samples[(size_t)i * channels + channel] *= (float)i / (float)fadeIn;
                    for (uint32 i = 0; i < fadeOut; ++i)
                        samples[(size_t)(frames - 1 - i) * channels + channel] *= (float)i / (float)fadeOut;
                }
            }
            const float scale = 0.9f / peakOf(samples);
            for (float& s : samples)
                s *= scale;
        }
    };

    // ---- A CROWD IS A TEXTURE, NOT A SUM OF EVENTS ----
    // Many voices at once are not heard as voices: the ear hears ONE sound with a colour (where the mouths are) and
    // a grain (how the syllables come). So a crowd is made as that one sound - a few STREAMS that never stop, each a
    // breath that pulses, through a mouth that moves, in bursts like syllables - and never as recorded events mixed
    // on top of each other (tried: 60 growls or 130 hits and cries in a loop. Each event keeps its own start and
    // shape, so the sum is a clutter of starts, or - when they are dense enough to hide the starts - a steady drone).

    // A value that walks between random targets, `hz` new ones a second: the unsteady hand on every knob below.
    struct Wander
    {
        float value = 0.5f, target = 0.5f;
        int left = 0;
        float next(Rng& rng, float hz)
        {
            if (--left <= 0)
            {
                target = rng.uni();
                left = (int)(c_sr / hz * rng.range(0.5f, 1.5f));
            }
            value += (target - value) * (hz * 4.0f / c_sr);
            return value;
        }
    };

    // One stream of a crowd's voice: breath that pulses at a low, unsteady rate (rough, with no tone), through two
    // wide formants that wander (the mouths open and shut), let through in bursts (the syllables).
    struct Murmur
    {
        float pitch = 70.0f;            // pulses a second
        float syllables = 4.0f;         // bursts a second
        float lowF1 = 350.0f, highF1 = 600.0f, lowF2 = 800.0f, highF2 = 1300.0f;
        Svf f1, f2;
        Noise breath;
        Wander burst, mouth1, mouth2;
        float phase = 0.0f, period = 1.0f;
        uint32 count = 0;

        float step(Rng& rng)
        {
            const float b = burst.next(rng, syllables);
            const float m1 = mouth1.next(rng, 1.5f), m2 = mouth2.next(rng, 1.5f);
            if ((count++ & 15) == 0)
            {
                f1.set(glm::mix(lowF1, highF1, m1), 4.0f);
                f2.set(glm::mix(lowF2, highF2, m2), 5.0f);
            }
            phase += pitch * period / c_sr;
            if (phase >= 1.0f)
            {
                phase -= 1.0f;
                period = 1.0f + rng.bi() * 0.25f;
            }
            const float open = phase < 0.5f ? std::sin(c_tau * phase) : 0.0f;
            const float source = breath.next(rng, Pink) * (0.3f + open * open);
            f1.step(source);
            f2.step(source);
            return (f1.bp + f2.bp * 0.7f) * b * b * b * 3.0f;
        }
    };

    // Sparse random impulses, `rate` a second, most of them small: what a filter turns into a patter (feet) or a
    // clatter (blows) with no single event to pick out.
    float dust(Rng& rng, float rate)
    {
        if (rng.uni() >= rate / c_sr)
            return 0.0f;
        const float a = rng.uni();
        return (rng.bi() > 0.0f ? 1.0f : -1.0f) * a * a * 40.0f;
    }

    // A texture has no whole cycles to close on: make it longer than the loop and fade its tail into its head.
    Buf closeLoop(const Buf& longer, uint32 frames)
    {
        Buf out(frames);
        const uint32 extra = (uint32)longer.size() - frames;
        for (uint32 i = 0; i < frames; ++i)
        {
            if (i < extra)
            {
                const float u = (float)i / (float)extra;
                out[i] = longer[i] * std::sqrt(u) + longer[frames + i] * std::sqrt(1.0f - u);
            }
            else
                out[i] = longer[i];
        }
        return out;
    }

    // A slow, smooth 0..1 curve that closes over the loop: a few sines with whole cycles in it.
    struct LoopLfo
    {
        float phase[4];
        int cycles[4];
        LoopLfo(Rng& rng, int c0, int c1, int c2, int c3) : cycles{ c0, c1, c2, c3 }
        {
            for (float& p : phase)
                p = rng.uni() * c_tau;
        }
        float at(float u) const // u = 0..1 over the loop
        {
            float s = 0.0f;
            for (int i = 0; i < 4; ++i)
                s += std::sin(c_tau * (float)cycles[i] * u + phase[i]);
            return glm::clamp(0.5f + s * 0.2f, 0.0f, 1.0f);
        }
    };
}

// The synthesis is slow (seconds) and its output is the same on every run, so GameSound keeps it in a file
// (Sound.cpp, load). The file must not outlive the code that made it: this stamp is the time THIS source file was last
// changed, so any edit here makes the next run synthesize again. No number to bump by hand.
uint32 synthGameSoundStamp()
{
    uint32 hash = 2166136261u;
    for (const char* c = __TIMESTAMP__; *c; ++c)
        hash = (hash ^ (uint8)*c) * 16777619u;
    return hash;
}

void synthGameSound(EGameSound sound, int variant, oc::vector<float>& out, bool& stereo, uint32& rate)
{
    rate = (uint32)c_sr;
    Rng rng{ 0x1234567u + (uint32)sound * 7919u + (uint32)variant * 104729u };
    const auto begin = [&](float seconds, bool isStereo, bool loop)
    {
        stereo = isStereo;
        const uint32 frames = framesOf(seconds);
        out.assign((size_t)frames * (isStereo ? 2u : 1u), 0.0f);
        return Canvas{ out, frames, isStereo ? 2u : 1u, loop };
    };

    switch (sound)
    {
    case EGameSound::MeleeHit:
    {
        Canvas c = begin(0.3f, false, false);
        c.mix(makeMeleeHit(rng, variant), 0.0f, 1.0f);
        c.reverb(0.15f, 0.6f, 0.08f);
        c.finish();
        break;
    }
    case EGameSound::UnitReveal:
    {
        // THE SIZZLE of a shadow that the light makes solid (it goes with the reveal smoke): a thin, high hiss that
        // spits unevenly and is gone in half a second, a duller breath of steam under it, a few tiny spits. No weight,
        // no voice: it must not read as a hit.
        const float length = rng.range(0.45f, 0.85f);
        Canvas c = begin(length + 0.1f, false, false);
        Buf hiss = noise(rng, length, White, High, rng.range(4500.0f, 6500.0f), rng.range(6500.0f, 9000.0f), 0.7f, rng.range(0.01f, 0.03f), length * rng.range(0.28f, 0.4f));
        flutter(hiss, rng, rng.range(60.0f, 130.0f), 0.75f); // it spits
        level(hiss);
        c.mix(hiss, 0.0f, 1.0f);
        Buf steam = noise(rng, length, Pink, Band, rng.range(2200.0f, 3200.0f), rng.range(4000.0f, 5500.0f), 0.9f, 0.04f, length * 0.3f);
        level(steam);
        c.mix(steam, 0.0f, rng.range(0.25f, 0.45f));
        for (int i = 0, spits = 2 + rng.pick(5); i < spits; ++i)
        {
            const float hz = rng.range(3500.0f, 8000.0f);
            Buf spit = noise(rng, rng.range(0.004f, 0.012f), White, Band, hz, hz, 2.0f, 0.0003f, 0.003f);
            level(spit);
            c.mix(spit, rng.range(0.0f, length * 0.6f), rng.range(0.2f, 0.5f));
        }
        c.finish();
        break;
    }
    case EGameSound::UnitDeath:
    {
        Canvas c = begin(0.8f, false, false);
        c.mix(makeDeathCry(rng, rng.range(0.8f, 1.25f), variant), 0.0f, 1.0f);
        c.reverb(0.2f, 0.6f, 0.08f);
        c.finish();
        break;
    }
    case EGameSound::ShotFired:
    {
        // A spit: a wet pop, the mouth's resonance falling, air after it.
        Canvas c = begin(0.45f, false, false);
        c.mix(thump(0.06f, rng.range(420.0f, 640.0f), 160.0f, 0.008f, 0.018f, 1.5f), 0.0f, 0.8f);
        c.mix(noise(rng, 0.16f, White, Band, rng.range(2200.0f, 3400.0f), rng.range(500.0f, 900.0f), rng.range(1.8f, 3.5f), 0.001f, 0.04f), 0.0f, 2.0f);
        c.mix(noise(rng, 0.2f, Pink, High, 3500.0f, 5000.0f, 0.7f, 0.01f, 0.06f), 0.0f, 0.6f);
        if (variant & 1) // a second, smaller spit right behind the first
            c.mix(noise(rng, 0.1f, White, Band, rng.range(2600.0f, 3800.0f), 900.0f, 3.0f, 0.001f, 0.025f), rng.range(0.04f, 0.07f), 1.0f);
        c.reverb(0.15f, 0.5f, 0.08f);
        c.finish();
        break;
    }
    case EGameSound::LobFired:
    {
        // A mortar: the charge, the puff, the tube that rings.
        Canvas c = begin(0.8f, false, false);
        c.mix(thump(0.45f, rng.range(120.0f, 165.0f), rng.range(38.0f, 48.0f), 0.035f, rng.range(0.11f, 0.17f), 3.0f), 0.0f, 1.0f);
        c.mix(noise(rng, 0.3f, Brown, Low, rng.range(700.0f, 1100.0f), 250.0f, 0.8f, 0.002f, 0.08f), 0.0f, 0.8f);
        c.mix(modal(c_tube, 0.4f, rng.range(0.85f, 1.1f)), 0.0f, rng.range(0.3f, 0.6f));
        c.reverb(0.3f, 0.5f, 0.15f);
        c.finish();
        break;
    }
    case EGameSound::ShotImpact:
    {
        // A splat: the hit, a few wet drops, the sizzle.
        Canvas c = begin(0.5f, false, false);
        c.mix(noise(rng, 0.1f, White, Low, rng.range(2500.0f, 4000.0f), 400.0f, rng.range(2.0f, 4.0f), 0.001f, 0.03f), 0.0f, 1.0f);
        c.mix(thump(0.08f, rng.range(190.0f, 290.0f), 90.0f, 0.01f, 0.025f, 1.5f), 0.0f, 0.6f);
        for (int i = 0, drops = 2 + rng.pick(5); i < drops; ++i)
        {
            const float hz = rng.range(900.0f, 2200.0f);
            c.mix(noise(rng, 0.05f, White, Band, hz, hz * 0.5f, 6.0f, 0.002f, 0.015f), 0.02f + 0.025f * (float)i, 1.5f);
        }
        c.mix(noise(rng, 0.3f, White, High, 4000.0f, 6000.0f, 0.7f, 0.01f, 0.09f), 0.0f, 0.25f);
        c.reverb(0.15f, 0.5f, 0.08f);
        c.finish();
        break;
    }
    case EGameSound::SplashImpact:
    {
        // An explosion: the crack, the body, the weight under it, what falls back down.
        Canvas c = begin(2.0f, false, false);
        c.mix(noise(rng, 0.02f, White, High, 2500.0f, 2500.0f, 0.7f, 0.0002f, 0.006f), 0.0f, 0.8f);
        Buf body = noise(rng, 1.2f, Brown, Low, rng.range(2800.0f, 4200.0f), rng.range(110.0f, 170.0f), 0.8f, 0.002f, rng.range(0.26f, 0.38f));
        level(body);
        saturate(body, 4.0f);
        c.mix(body, 0.0f, 1.0f);
        c.mix(thump(0.9f, rng.range(62.0f, 80.0f), 27.0f, 0.06f, 0.3f, 2.5f), 0.0f, 1.0f);
        for (int i = 0, debris = 16 + rng.pick(15); i < debris; ++i)
        {
            const float t = 0.08f + std::pow(rng.uni(), 1.5f) * 0.9f;
            c.mix(rockClack(rng), t, rng.range(0.1f, 0.35f) * (1.0f - t));
        }
        c.reverb(0.55f, 0.4f, 0.3f);
        c.finish();
        break;
    }
    case EGameSound::TurretFire:
    {
        // The light tower's strike, as a bolt: the crack, the air that tears (noise through a resonance that falls), the
        // thump, the crystal that answers, the air after. No tone anywhere. SHORT: about 0.25 s of sound and a small
        // room - a tower fires again and again, and a long tail runs into the next shot.
        Canvas c = begin(0.45f, false, false);
        Buf crack = noise(rng, 0.025f, White, High, 2500.0f, 2500.0f, 0.7f, 0.0002f, rng.range(0.004f, 0.008f));
        level(crack);
        c.mix(crack, 0.0f, 0.9f);
        const float top = rng.range(5500.0f, 7500.0f), bottom = rng.range(600.0f, 1000.0f), narrow = rng.range(2.0f, 4.5f);
        Buf tear = noise(rng, rng.range(0.08f, 0.13f), White, Band, top, bottom, narrow, 0.001f, rng.range(0.025f, 0.04f));
        level(tear);
        c.mix(tear, 0.0f, 0.8f);
        if (variant & 1) // a second, smaller bolt right behind the first
        {
            Buf second = noise(rng, 0.07f, White, Band, top * 0.8f, bottom, narrow, 0.001f, 0.02f);
            level(second);
            c.mix(second, rng.range(0.04f, 0.06f), 0.45f);
            c.mix(crack, rng.range(0.04f, 0.06f), 0.4f);
        }
        c.mix(thump(0.12f, rng.range(95.0f, 125.0f), 50.0f, 0.015f, 0.035f, 2.0f), 0.0f, 0.8f);
        c.mix(modal(c_crystal, 0.22f, rng.range(0.85f, 1.15f), 0.4f, 30.0f), 0.01f, rng.range(0.15f, 0.3f));
        Buf air = noise(rng, 0.18f, Pink, High, 3000.0f, 5000.0f, 0.7f, 0.01f, 0.05f);
        level(air);
        c.mix(air, 0.0f, 0.15f);
        c.reverb(0.2f, 0.5f, 0.12f);
        c.finish();
        break;
    }
    case EGameSound::StructurePlaced:
    {
        // A mallet on wood.
        Canvas c = begin(0.5f, false, false);
        if (variant & 1) // a stone set down
            c.mix(rockClack(rng), 0.0f, 0.9f);
        else
            c.mix(modal(c_wood, 0.3f, rng.range(0.85f, 1.3f)), 0.0f, 1.0f);
        c.mix(noise(rng, 0.006f, White, Band, 2500.0f, 2500.0f, 1.0f, 0.0002f, 0.002f), 0.0f, 0.5f);
        c.mix(thump(0.12f, rng.range(100.0f, 140.0f), 70.0f, 0.02f, 0.04f, 1.5f), 0.0f, 0.6f);
        c.reverb(0.2f, 0.6f, 0.1f);
        c.finish();
        break;
    }
    case EGameSound::StructureBuilt:
    {
        // The last piece goes in: two mallet blows, the second higher, the stone that settles, and a soft glow of air
        // after the second (noise through a few wide resonances: a breath with a colour, not a bell).
        Canvas c = begin(1.2f, false, false);
        const float wood = rng.range(0.9f, 1.1f);
        c.mix(thump(0.15f, 100.0f, 55.0f, 0.02f, 0.05f, 2.0f), 0.0f, 0.6f);
        c.mix(modal(c_wood, 0.25f, wood), 0.0f, 0.8f);
        c.mix(rockClack(rng), 0.0f, 0.3f);
        c.mix(thump(0.12f, 120.0f, 65.0f, 0.02f, 0.04f, 1.5f), 0.17f, 0.4f);
        c.mix(modal(c_wood, 0.25f, wood * (variant & 1 ? 1.33f : 1.25f)), 0.17f, 0.75f);
        c.mix(modal(c_glow, 0.8f, variant & 1 ? 0.9f : 1.0f, 1.0f, 14.0f), 0.17f, 0.12f);
        c.reverb(0.4f, 0.4f, 0.22f);
        c.finish();
        break;
    }
    case EGameSound::StructureCollapse:
    {
        // The beams snap, the walls come down, the stones settle, the dust hangs.
        Canvas c = begin(2.6f, false, false);
        const float snaps[] = { 0.0f, 0.07f, 0.16f };
        for (const float t : snaps)
        {
            c.mix(modal(c_wood, 0.2f, rng.range(0.7f, 1.3f)), t, 0.5f);
            c.mix(noise(rng, 0.01f, White, High, 2500.0f, 2500.0f, 0.7f, 0.0002f, 0.003f), t, 0.5f);
        }
        Buf rumble = noise(rng, 2.0f, Brown, Low, 260.0f, 90.0f, 0.7f, 0.05f, 0.7f);
        level(rumble);
        saturate(rumble, 2.0f);
        c.mix(rumble, 0.05f, 1.0f);
        c.mix(thump(0.6f, 60.0f, 28.0f, 0.08f, 0.25f, 2.0f), 0.15f, 0.8f);
        for (int i = 0; i < 45; ++i)
        {
            const float t = 0.1f + std::pow(rng.uni(), 1.6f) * 1.8f;
            c.mix(rockClack(rng), t, rng.range(0.15f, 0.5f) * (1.0f - t / 2.2f));
        }
        c.mix(noise(rng, 1.8f, Pink, High, 2500.0f, 4000.0f, 0.7f, 0.3f, 0.6f), 0.2f, 0.5f);
        c.reverb(0.5f, 0.45f, 0.25f);
        c.finish();
        break;
    }
    case EGameSound::Thunder:
    {
        // An even variant is near (it cracks first), an odd one is far (it only rolls).
        const bool nearStrike = (variant & 1) == 0;
        Canvas c = begin(6.5f, false, false);
        if (nearStrike)
            for (int i = 0; i < 6; ++i)
            {
                const float hz = rng.range(1500.0f, 3500.0f);
                Buf crack = noise(rng, rng.range(0.03f, 0.09f), White, Band, hz, hz * 0.4f, 0.8f, 0.0005f, rng.range(0.01f, 0.04f));
                level(crack);
                c.mix(crack, rng.range(0.0f, 0.18f), rng.range(0.4f, 0.8f));
            }
        Buf roll = noise(rng, 6.0f, Brown, Low, nearStrike ? 320.0f : 200.0f, 65.0f, 0.7f, 0.02f, 1.6f);
        // The roll comes in waves.
        float bumpAt[6], bumpGain[6];
        for (int i = 0; i < 6; ++i)
        {
            bumpAt[i] = rng.range(0.3f, 3.5f);
            bumpGain[i] = rng.range(0.4f, 1.0f);
        }
        for (size_t i = 0; i < roll.size(); ++i)
        {
            const float t = (float)i / c_sr;
            float wave = 0.5f;
            for (int k = 0; k < 6; ++k)
                wave += bumpGain[k] * std::exp(-(t - bumpAt[k]) * (t - bumpAt[k]) / 0.06f);
            roll[i] *= wave;
        }
        level(roll);
        c.mix(roll, 0.0f, 1.0f);
        c.mix(thump(1.5f, 55.0f, 30.0f, 0.2f, 0.6f, 1.5f), 0.05f, 0.5f);
        c.reverb(0.8f, 0.5f, 0.4f);
        c.finish();
        break;
    }
    case EGameSound::Alarm:
    {
        // A war drum, three blows: the skin (a struck body with low resonances), the weight under it, the stick's slap.
        Canvas c = begin(1.6f, false, false);
        const float blows[] = { 0.0f, 0.22f, 0.5f };
        for (const float t : blows)
        {
            c.mix(modal(c_drum, 0.6f, rng.range(0.97f, 1.03f), 1.0f, 12.0f), t, 1.0f);
            c.mix(thump(0.3f, 110.0f, 60.0f, 0.03f, 0.1f, 2.5f), t, 0.7f);
            Buf slap = noise(rng, 0.04f, Pink, Band, rng.range(1500.0f, 2100.0f), 1200.0f, 1.0f, 0.0005f, 0.012f);
            level(slap);
            c.mix(slap, t, 0.5f);
        }
        c.reverb(0.45f, 0.45f, 0.22f);
        c.finish();
        break;
    }
    case EGameSound::PlayerHurt:
    {
        // A blow to the body, and the breath it knocks out.
        const VocalKey grunt[] = {
            { 0.00f, 125.0f, 600.0f, 1000.0f, 2400.0f, 0.0f, 0.2f, 0.3f },
            { 0.03f, 135.0f, 650.0f, 1050.0f, 2400.0f, 1.0f, 0.3f, 0.3f },
            { 0.18f, 100.0f, 500.0f, 900.0f,  2300.0f, 0.0f, 0.5f, 0.4f } };
        Canvas c = begin(0.5f, false, false);
        c.mix(thump(0.25f, rng.range(90.0f, 125.0f), 42.0f, 0.03f, rng.range(0.06f, 0.1f), 3.0f), 0.0f, 1.0f);
        c.mix(noise(rng, 0.12f, Pink, Low, rng.range(900.0f, 1500.0f), 500.0f, 0.8f, 0.001f, 0.035f), 0.0f, 1.2f);
        Buf voice = vocal(rng, grunt, rng.range(0.85f, 1.1f), rng.range(0.8f, 1.2f), 1.5f);
        level(voice);
        c.mix(voice, 0.02f, 0.35f);
        c.finish();
        break;
    }
    case EGameSound::LoopWind:
    {
        // Air that moves: a wide band whose pitch wanders, a thin whistle in the gusts, a low body. The two sides have
        // their own noise and their own wander, and share the gusts.
        Canvas c = begin(10.0f, true, true);
        const LoopLfo gust(rng, 1, 2, 4, 7);
        for (uint32 channel = 0; channel < 2; ++channel)
        {
            const LoopLfo wander(rng, 2, 3, 5, 8), high(rng, 3, 5, 6, 9);
            Svf band, thin, low;
            Noise pink, brown;
            for (int pass = 0; pass < 2; ++pass)
            {
                Rng r{ 0xabcdefu + channel * 977u };
                for (uint32 i = 0; i < c.frames; ++i)
                {
                    const float u = (float)i / (float)c.frames;
                    const float g = gust.at(u);
                    if ((i & 15) == 0)
                    {
                        band.set(260.0f * std::exp(1.2f * wander.at(u)), 0.9f);
                        thin.set(1100.0f * std::exp(0.5f * high.at(u)), 7.0f);
                        low.set(120.0f, 0.7f);
                    }
                    const float p = pink.next(r, Pink);
                    band.step(p);
                    thin.step(p);
                    low.step(brown.next(r, Brown));
                    if (pass == 1)
                        c.addAt(i, channel, band.bp * (0.35f + 0.65f * g) + thin.bp * g * g * g * 0.5f + low.lp * 0.25f);
                }
            }
        }
        c.finish();
        break;
    }
    case EGameSound::LoopDay:
    {
        // A wood by day: leaves that move with the air, and three kinds of bird, near and far.
        Canvas c = begin(14.0f, true, true);
        const LoopLfo gust(rng, 1, 3, 4, 6);
        for (uint32 channel = 0; channel < 2; ++channel)
        {
            Svf leaves;
            Noise pink;
            leaves.set(2600.0f, 0.7f);
            for (int pass = 0; pass < 2; ++pass)
            {
                Rng r{ 0x51ed27u + channel * 31337u };
                for (uint32 i = 0; i < c.frames; ++i)
                {
                    leaves.step(pink.next(r, Pink));
                    const float g = gust.at((float)i / (float)c.frames);
                    if (pass == 1)
                        c.addAt(i, channel, leaves.hp * (0.1f + 0.9f * g * g) * 0.1f);
                }
            }
        }
        // NO bird is in this loop: a song at the same second of every round is heard as the same song again. The
        // birds are GameSound's perches (singBird), which never sing one song twice.
        c.reverb(0.5f, 0.4f, 0.2f);
        c.finish();
        break;
    }
    case EGameSound::LoopNight:
    {
        // The night's background only: a field of crickets far off, on each side its own, that swells and thins, and the
        // night air under it. The crickets and the owl one can point at are GameSound's perches.
        Canvas c = begin(10.0f, true, true);
        for (uint32 channel = 0; channel < 2; ++channel)
        {
            const LoopLfo swell(rng, 1, 2, 3, 5);
            Svf field, air;
            Noise brown;
            field.set(4600.0f + 300.0f * (float)channel, 12.0f);
            air.set(300.0f, 0.7f);
            for (int pass = 0; pass < 2; ++pass)
            {
                Rng r{ 0x77aa11u + channel * 4099u };
                for (uint32 i = 0; i < c.frames; ++i)
                {
                    field.step(r.bi());
                    air.step(brown.next(r, Brown));
                    if (pass == 1) // 50 pulses a second: a whole number of them in the loop
                        c.addAt(i, channel, field.bp * (0.5f - 0.5f * std::cos(c_tau * 50.0f * (float)i / c_sr)) * (0.15f + 0.3f * swell.at((float)i / (float)c.frames))
                            + air.lp * 0.12f);
                }
            }
        }
        c.reverb(0.7f, 0.3f, 0.3f);
        c.finish();
        break;
    }
    case EGameSound::LoopBattle:
    {
        // A FIGHT, some way off, as ONE TEXTURE (see A CROWD IS A TEXTURE): on each side of the head, the clatter of
        // blows (sparse impulses through a few resonances that wander - no single blow to pick out), their weight (the
        // same impulses, low), and one stream of angry voices (fast syllables, the mouths wide). The whole comes and goes
        // in surges. Far: dull, and not loud - the near, clear events are the one-shots on top of it.
        Canvas c = begin(12.0f, true, true);
        const uint32 extra = framesOf(1.5f);
        for (uint32 channel = 0; channel < 2; ++channel)
        {
            Rng r{ 0x3c3c3cu + channel * 8191u };
            Buf side(c.frames + extra);
            Svf clatter[3], weight;
            Wander centre[3], surge;
            Murmur voices;
            voices.pitch = channel == 0 ? 95.0f : 80.0f;
            voices.syllables = 6.0f;
            voices.lowF1 = 450.0f;
            voices.highF1 = 800.0f;
            voices.lowF2 = 1000.0f;
            voices.highF2 = 1700.0f;
            weight.set(150.0f, 1.0f);
            const float low[3] = { 500.0f, 1100.0f, 2000.0f }, high[3] = { 900.0f, 1900.0f, 3200.0f };
            for (uint32 i = 0; i < (uint32)side.size(); ++i)
            {
                const float s = 0.35f + 0.65f * surge.next(r, 0.4f);
                const float d = dust(r, 22.0f);
                float v = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float where = centre[k].next(r, 0.7f);
                    if ((i & 15) == 0)
                        clatter[k].set(glm::mix(low[k], high[k], where), 3.5f);
                    clatter[k].step(d);
                    v += clatter[k].bp * (k == 0 ? 1.0f : 0.6f);
                }
                weight.step(d);
                side[i] = (v + weight.lp * 1.5f + voices.step(r) * 2.5f) * s;
            }
            const Buf loop = closeLoop(side, c.frames);
            for (uint32 i = 0; i < c.frames; ++i)
                c.addAt(i, channel, loop[i]);
        }
        c.lowpassAll(3800.0f);
        c.reverb(0.45f, 0.6f, 0.15f);
        c.finish();
        break;
    }
    case EGameSound::LoopHorde:
    {
        // THE HORDE that comes, as ONE TEXTURE (see A CROWD IS A TEXTURE): the patter of very many feet (sparse impulses
        // through low filters: a soft, uneven rumble, no single step), and three streams of low voices - each with its
        // own pitch, mouth and tempo, slow syllables - that swell and thin by themselves, so the murmur is never level.
        Canvas c = begin(14.0f, false, true);
        const uint32 extra = framesOf(1.5f);
        Buf texture(c.frames + extra);
        Rng r{ 0x40de5u };
        Svf patter, ground;
        Wander swell, part[3];
        Murmur voices[3];
        const float pitch[3] = { 58.0f, 74.0f, 91.0f }, tempo[3] = { 2.6f, 3.4f, 4.3f };
        for (int k = 0; k < 3; ++k)
        {
            voices[k].pitch = pitch[k];
            voices[k].syllables = tempo[k];
            voices[k].lowF1 = 300.0f + 40.0f * (float)k;
            voices[k].highF1 = 520.0f + 60.0f * (float)k;
            voices[k].lowF2 = 700.0f + 100.0f * (float)k;
            voices[k].highF2 = 1100.0f + 150.0f * (float)k;
        }
        patter.set(140.0f, 1.2f);
        ground.set(420.0f, 0.7f);
        for (uint32 i = 0; i < (uint32)texture.size(); ++i)
        {
            const float d = dust(r, 60.0f);
            patter.step(d);
            ground.step(d);
            float v = (patter.bp * 1.2f + ground.lp * 0.35f) * (0.5f + 0.5f * swell.next(r, 0.3f));
            for (int k = 0; k < 3; ++k)
                v += voices[k].step(r) * 2.0f * part[k].next(r, 0.25f); // each stream comes forward and steps back
            texture[i] = v;
        }
        const Buf loop = closeLoop(texture, c.frames);
        for (uint32 i = 0; i < c.frames; ++i)
            out[i] = loop[i];
        c.lowpassAll(2600.0f);
        c.reverb(0.25f, 0.7f, 0.1f);
        c.finish();
        break;
    }
    case EGameSound::LoopHum:
    {
        // A field of light, as AIR that sings: noise through the resonances of a low chord (each a narrow band of noise
        // whose level wanders by itself - a drone like wind in a pipe, not an electric hum), a low body, and high bands
        // that come and go.
        Canvas c = begin(6.0f, false, true);
        constexpr int c_bands = 8;
        const float hz[c_bands] = { 98.0f, 147.0f, 196.0f, 294.0f, 392.0f, 1176.0f, 1568.0f, 2352.0f };
        const float gain[c_bands] = { 1.0f, 0.6f, 0.7f, 0.35f, 0.25f, 0.1f, 0.08f, 0.05f };
        const LoopLfo comeAndGo[3] = { LoopLfo(rng, 1, 2, 3, 5), LoopLfo(rng, 1, 3, 4, 7), LoopLfo(rng, 2, 3, 5, 6) };
        Svf band[c_bands], body;
        Noise pink, brown;
        for (int k = 0; k < c_bands; ++k)
            band[k].set(hz[k], k < 5 ? 40.0f : 25.0f);
        body.set(70.0f, 0.7f);
        for (int pass = 0; pass < 2; ++pass)
        {
            Rng r{ 0x600d1eu };
            for (uint32 i = 0; i < c.frames; ++i)
            {
                const float u = (float)i / (float)c.frames;
                const float p = pink.next(r, Pink);
                body.step(brown.next(r, Brown));
                float s = body.lp * 0.08f;
                for (int k = 0; k < c_bands; ++k)
                {
                    band[k].step(p);
                    s += band[k].bp * gain[k] * (k < 5 ? 1.0f : comeAndGo[k - 5].at(u));
                }
                if (pass == 1)
                    out[i] = s;
            }
        }
        c.reverb(0.4f, 0.4f, 0.2f);
        c.finish();
        break;
    }
    case EGameSound::LoopFire:
    {
        // A hearth: the roar that flickers, the dry crackle, a pop with weight now and then.
        Canvas c = begin(6.0f, false, true);
        const LoopLfo flicker(rng, 3, 7, 11, 17), flutter(rng, 5, 9, 13, 23);
        Svf roar, mid;
        Noise brown, pink;
        roar.set(500.0f, 0.7f);
        mid.set(900.0f, 1.0f);
        for (int pass = 0; pass < 2; ++pass)
        {
            Rng r{ 0xf12e00u };
            for (uint32 i = 0; i < c.frames; ++i)
            {
                const float u = (float)i / (float)c.frames;
                roar.step(brown.next(r, Brown));
                mid.step(pink.next(r, Pink));
                if (pass == 1)
                    out[i] = roar.lp * (0.4f + 0.6f * flicker.at(u)) * 0.8f + mid.bp * flutter.at(u) * 0.6f;
            }
        }
        for (int i = 0; i < 140; ++i)
        {
            const float dur = rng.range(0.002f, 0.012f);
            const float hz = rng.range(1200.0f, 6500.0f);
            const float loud = std::pow(rng.uni(), 3.0f);
            const float t = rng.range(0.0f, 6.0f);
            Buf crackle = noise(rng, dur, White, Band, hz, hz, 2.0f, 0.0002f, dur / 3.0f);
            level(crackle);
            c.mix(crackle, t, 0.08f + loud * 0.5f);
            if (loud > 0.5f)
                c.mix(thump(0.03f, 200.0f, 90.0f, 0.005f, 0.01f, 1.0f), t, 0.2f);
        }
        c.reverb(0.15f, 0.6f, 0.08f);
        c.finish();
        break;
    }
    case EGameSound::LoopWorks:
    {
        // A workshop: three strikes of a chisel, a mallet, then a saw - push, pull, push, pull.
        Canvas c = begin(4.0f, false, true);
        const float strikes[] = { 0.0f, 0.45f, 0.9f };
        for (const float t : strikes)
        {
            c.mix(modal(c_metal, 0.3f, rng.range(0.96f, 1.04f)), t, 0.5f);
            c.mix(modal(c_wood, 0.2f, 0.8f), t, 0.5f);
            c.mix(thump(0.1f, 130.0f, 70.0f, 0.015f, 0.035f, 1.5f), t, 0.4f);
        }
        for (int i = 0; i < 4; ++i)
        {
            const bool push = (i & 1) == 0;
            Buf stroke = noise(rng, 0.45f, White, Band, push ? 1700.0f : 2300.0f, push ? 2300.0f : 1700.0f, 1.5f, 0.06f, 0.3f);
            level(stroke);
            flutter(stroke, rng, 55.0f, 0.6f); // the teeth catch unevenly
            c.mix(stroke, 1.5f + 0.55f * (float)i, push ? 0.4f : 0.28f);
        }
        c.reverb(0.25f, 0.5f, 0.15f);
        c.finish();
        break;
    }
    case EGameSound::BirdSong:
    {
        // NOT a song: one NOTE. GameSound::singBird makes every song at run time from these six, each note at its own
        // time, pitch and level, so no song is ever heard twice. All are near 3.2 kHz: the bird's own pitch (0.7 - 1.35)
        // times the note's (0.6 - 1.7) puts them from 1.3 to 7 kHz. The canvas is short (the wood's answer is cut):
        // a trill starts a note every 50 ms, and each one holds a voice for the canvas' length.
        Buf song;
        float length = 0.3f;
        switch (variant % 6)
        {
        case 0: // a flat whistle
            put(song, whistle(0.12f, 3200.0f, 3150.0f, 30.0f, 0.004f), 0.0f, 1.0f);
            length = 0.4f;
            break;
        case 1: // a rising chirp
            put(song, whistle(0.07f, 2700.0f, 3900.0f), 0.0f, 1.0f);
            break;
        case 2: // a falling chirp
            put(song, whistle(0.07f, 3800.0f, 2800.0f), 0.0f, 1.0f);
            break;
        case 3: // a tick
            put(song, whistle(0.028f, 3300.0f, 3200.0f, 0.0f, 0.0f, 0.1f), 0.0f, 1.0f);
            length = 0.15f;
            break;
        case 4: // a long soft whistle that sinks a little
            put(song, whistle(0.24f, 3300.0f, 3000.0f, 6.0f, 0.01f), 0.0f, 1.0f);
            length = 0.5f;
            break;
        default: // a rough triple pulse
            for (int pulse = 0; pulse < 3; ++pulse)
                put(song, whistle(0.02f, 3000.0f, 3400.0f, 0.0f, 0.0f, 0.35f), 0.03f * (float)pulse, 1.0f);
            length = 0.2f;
            break;
        }
        Canvas c = begin(length, false, false);
        level(song);
        c.mix(song, 0.0f, 1.0f);
        c.reverb(0.4f, 0.4f, 0.15f); // the wood answers
        c.finish();
        break;
    }
    case EGameSound::CricketBout:
    {
        // One cricket, one bout: 1 - 3 s of chirps. The variant's random stream is the cricket: its pitch, its tempo, how
        // many pulses a chirp has. The chirps are not on a grid.
        const float length = rng.range(1.2f, 2.8f);
        Canvas c = begin(length + 0.3f, false, false);
        const float hz = rng.range(3800.0f, 5300.0f), gap = rng.range(0.14f, 0.34f), spacing = rng.range(0.013f, 0.019f);
        const int pulses = 2 + rng.pick(3);
        Buf chirp;
        for (int pulse = 0; pulse < pulses; ++pulse)
            put(chirp, whistle(0.009f, hz, hz, 0.0f, 0.0f, 0.0f), spacing * (float)pulse, 0.7f + 0.3f * (float)pulse / (float)pulses);
        for (float t = 0.0f; t < length; t += gap * rng.range(0.92f, 1.1f))
            c.mix(chirp, t, glm::smoothstep(0.0f, 0.25f, t) * (1.0f - glm::smoothstep(length - 0.4f, length, t)) * rng.range(0.8f, 1.0f));
        c.reverb(0.3f, 0.5f, 0.08f);
        c.finish();
        break;
    }
    case EGameSound::OwlCall:
    {
        // "hoo, hoo-hoo, hooo": low, breathy, in a wood that answers.
        Canvas c = begin(2.6f, false, false);
        const float hz = variant & 1 ? 330.0f : 372.0f;
        Buf owl;
        put(owl, whistle(0.35f, hz, hz * 0.97f, 0.0f, 0.0f, 0.2f), 0.0f, 1.0f);
        put(owl, whistle(0.22f, hz, hz * 0.95f, 0.0f, 0.0f, 0.2f), 0.5f, 0.8f);
        if (variant & 1)
            put(owl, whistle(0.22f, hz, hz * 0.95f, 0.0f, 0.0f, 0.2f), 0.8f, 0.8f);
        put(owl, whistle(0.5f, hz * 0.99f, hz * 0.91f, 0.0f, 0.0f, 0.2f), variant & 1 ? 1.15f : 0.85f, 0.9f);
        level(owl);
        lowpass(owl, 1500.0f);
        c.mix(owl, 0.0f, 1.0f);
        c.reverb(0.75f, 0.3f, 0.35f);
        c.finish();
        break;
    }
    case EGameSound::WindGust:
    {
        // One gust through the trees: the air comes up, opens and goes (a band of noise whose pitch rises and falls with
        // its level), with the leaves on top - they catch unevenly, and late.
        const float length = rng.range(2.4f, 4.2f);
        Canvas c = begin(length, false, false);
        const float low = rng.range(220.0f, 380.0f), rise = rng.range(1.6f, 2.6f), peak = rng.range(0.3f, 0.55f);
        Svf air, leaves;
        Noise pink, white;
        Buf rustle(c.frames);
        for (uint32 i = 0; i < c.frames; ++i)
        {
            const float u = (float)i / (float)c.frames;
            // Up fast, down slowly: the peak is before the middle.
            const float swell = u < peak ? glm::smoothstep(0.0f, 1.0f, u / peak) : 1.0f - glm::smoothstep(0.0f, 1.0f, (u - peak) / (1.0f - peak));
            const float late = glm::smoothstep(0.0f, 1.0f, glm::clamp((u - 0.08f) / peak, 0.0f, 1.0f)) * (u < peak + 0.1f ? 1.0f : 1.0f - glm::smoothstep(0.0f, 1.0f, (u - peak - 0.1f) / (0.9f - peak)));
            if ((i & 15) == 0)
            {
                air.set(low * std::pow(rise, swell), 0.8f);
                leaves.set(3200.0f + 1500.0f * swell, 0.7f);
            }
            air.step(pink.next(rng, Pink));
            leaves.step(white.next(rng, White));
            out[i] = air.bp * swell;
            rustle[i] = leaves.hp * late * late;
        }
        flutter(rustle, rng, rng.range(9.0f, 16.0f), 0.7f);
        level(rustle);
        const float body = 1.0f / peakOf(out);
        for (uint32 i = 0; i < c.frames; ++i)
            out[i] = out[i] * body + rustle[i] * (variant & 1 ? 0.45f : 0.2f); // an odd variant goes through more leaves
        c.finish();
        break;
    }
    case EGameSound::WaterLap:
    {
        // One small wave on the shore: the water comes up (a band of noise that rises), a few drops fall back (short
        // bands of noise, each with its own pitch, that rise as they close).
        const float length = rng.range(0.6f, 1.1f);
        Canvas c = begin(length + 0.5f, false, false);
        Buf slosh = noise(rng, length, Pink, Band, rng.range(450.0f, 700.0f), rng.range(1200.0f, 1900.0f), 1.2f, length * 0.35f, length * 0.4f);
        level(slosh);
        c.mix(slosh, 0.0f, 1.0f);
        for (int i = 0, drops = 3 + rng.pick(6); i < drops; ++i)
        {
            const float hz = rng.range(500.0f, 2400.0f);
            Buf drop = noise(rng, rng.range(0.03f, 0.07f), White, Band, hz, hz * rng.range(1.3f, 1.9f), rng.range(7.0f, 12.0f), 0.003f, 0.02f);
            level(drop);
            c.mix(drop, rng.range(0.15f, length), rng.range(0.15f, 0.45f));
        }
        Buf foam = noise(rng, length, White, High, 3500.0f, 5500.0f, 0.7f, length * 0.4f, length * 0.35f);
        level(foam);
        c.mix(foam, length * 0.2f, 0.12f);
        c.reverb(0.2f, 0.5f, 0.08f);
        c.finish();
        break;
    }
    case EGameSound::LoopWater:
    {
        // Water at a shore or in a bed: the body that comes and goes (waves; five in the loop, none like the other), the
        // babble of hundreds of small drops on top, the foam when a wave is up.
        Canvas c = begin(10.0f, false, true);
        const LoopLfo wave(rng, 2, 5, 7, 9), second(rng, 3, 4, 8, 11);
        Svf body, foam;
        Noise pink;
        body.set(700.0f, 0.7f);
        foam.set(3800.0f, 0.7f);
        for (int pass = 0; pass < 2; ++pass)
        {
            Rng r{ 0x0a7e2u };
            for (uint32 i = 0; i < c.frames; ++i)
            {
                const float u = (float)i / (float)c.frames;
                const float w = wave.at(u), w2 = second.at(u);
                const float p = pink.next(r, Pink);
                body.step(p);
                foam.step(r.bi());
                if (pass == 1)
                    out[i] = body.lp * (0.3f + 0.7f * w) * 1.6f + foam.hp * w * w2 * 0.12f;
            }
        }
        for (int i = 0; i < 260; ++i)
        {
            const float hz = rng.range(450.0f, 2600.0f);
            Buf drop = noise(rng, rng.range(0.03f, 0.08f), White, Band, hz, hz * rng.range(1.2f, 1.8f), rng.range(7.0f, 13.0f), 0.003f, 0.025f);
            level(drop);
            c.mix(drop, rng.range(0.0f, 10.0f), rng.range(0.03f, 0.16f));
        }
        c.reverb(0.2f, 0.5f, 0.08f);
        c.finish();
        break;
    }
    default:
        begin(0.05f, false, false);
        break;
    }
}
