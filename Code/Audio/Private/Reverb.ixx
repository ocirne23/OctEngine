module;

#include <cmath>

export module Audio:Reverb;

// THE SHARED REVERB: one space for every spatial source (Engine.ixx: each BinauralNode SENDS its dry mono signal here,
// the reverb node plays the answer). One reverb for all - not one baked into each sound - is what makes sounds share
// a place: a far sound is mostly its reverb, a near one mostly itself, and both ring in the same room.
//
// A FEEDBACK DELAY NETWORK: the input, smeared by four short allpasses (so a click is a burst, not a click, before it
// gets in), goes into eight delay lines of unlike lengths. Each line's output is low-passed (the highs die first, as
// in air), all eight are MIXED into each other (a Hadamard matrix: every line feeds every line, at equal weight and
// with signs that keep the energy) and fed back, each with the gain that makes its own length die over `seconds`.
// The echoes double with every trip through the lines, so after a few hundredths of a second there is no single echo
// left to hear, only a tail. (Not parallel comb filters - eight combs that do not feed each other ring at their own
// pitches: the metallic reverb of an old effect unit.)
//
// Plain data and no allocation: the audio thread runs it. configure() is the game thread's; it only writes numbers
// the audio thread reads the next sample (a torn read costs one sample of the wrong decay).
export struct FdnReverb
{
    static constexpr int Lines = 8;
    static constexpr int MaxDelay = 8192; // frames (170 ms at 48 kHz)
    static constexpr int Diffusers = 4;
    static constexpr int MaxDiffuser = 1024;

    float line[Lines][MaxDelay] = {};
    int length[Lines] = { 1493, 1789, 2111, 2417, 2741, 3109, 3547, 3989 };
    int at[Lines] = {};
    float tone[Lines] = {}; // each line's low pass state
    float gain[Lines] = {};
    float toneCoef = 0.5f;

    float diffuser[Diffusers][MaxDiffuser] = {};
    int diffuserLength[Diffusers] = { 227, 379, 541, 719 };
    int diffuserAt[Diffusers] = {};

    float lowCutState = 0.0f, lowCutCoef = 0.02f; // nothing under about 150 Hz goes in: a reverb of the lows is mud

    // seconds: the time to 60 dB down. toneHz: where the tail's highs start to die early. size: 0.5 .. 1.5, the room's
    // scale (the delay lengths: a bigger room's echoes are farther apart).
    void configure(float sampleRate, float seconds, float toneHz, float size)
    {
        // Lengths in ms at size 1, no two with a common rhythm. Odd frame counts.
        const float ms[Lines] = { 31.1f, 37.3f, 43.9f, 50.3f, 57.1f, 64.7f, 73.9f, 83.1f };
        const float diffuserMs[Diffusers] = { 4.7f, 7.9f, 11.3f, 14.9f };
        if (size < 0.3f) size = 0.3f;
        if (size > 1.9f) size = 1.9f;
        if (seconds < 0.1f) seconds = 0.1f;
        for (int i = 0; i < Lines; ++i)
        {
            int frames = (int)(ms[i] * size * sampleRate / 1000.0f) | 1;
            if (frames > MaxDelay - 1) frames = MaxDelay - 1;
            length[i] = frames;
            gain[i] = std::pow(10.0f, -3.0f * (float)frames / (seconds * sampleRate));
        }
        for (int i = 0; i < Diffusers; ++i)
        {
            int frames = (int)(diffuserMs[i] * sampleRate / 1000.0f) | 1;
            if (frames > MaxDiffuser - 1) frames = MaxDiffuser - 1;
            diffuserLength[i] = frames;
        }
        toneCoef = 1.0f - std::exp(-6.2831853f * toneHz / sampleRate);
        lowCutCoef = 1.0f - std::exp(-6.2831853f * 150.0f / sampleRate);
    }

    // Everything that rings, to nothing (the shape stays). The audio thread's: reverbNodeProcess calls it when a value
    // that is not finite got in.
    void clear()
    {
        for (int i = 0; i < Lines; ++i)
        {
            for (int k = 0; k < MaxDelay; ++k)
                line[i][k] = 0.0f;
            tone[i] = 0.0f;
        }
        for (int i = 0; i < Diffusers; ++i)
            for (int k = 0; k < MaxDiffuser; ++k)
                diffuser[i][k] = 0.0f;
        lowCutState = 0.0f;
    }

    void process(float in, float& outLeft, float& outRight)
    {
        // The low cut, then the diffusers.
        lowCutState += (in - lowCutState) * lowCutCoef;
        float x = in - lowCutState;
        for (int i = 0; i < Diffusers; ++i)
        {
            float& cell = diffuser[i][diffuserAt[i]];
            const float delayed = cell;
            const float fed = x + delayed * 0.6f;
            cell = fed;
            x = delayed - fed * 0.6f;
            if (++diffuserAt[i] >= diffuserLength[i])
                diffuserAt[i] = 0;
        }

        // The lines' outputs, each through its low pass.
        float out[Lines];
        for (int i = 0; i < Lines; ++i)
        {
            if (at[i] >= length[i]) // a shorter length came in
                at[i] = 0;
            tone[i] += (line[i][at[i]] - tone[i]) * toneCoef;
            out[i] = tone[i];
        }
        outLeft = (out[0] - out[2] + out[4] - out[6]) * 0.5f;
        outRight = (out[1] - out[3] + out[5] - out[7]) * 0.5f;

        // The mix: a fast Hadamard transform (three rounds of sums and differences), scaled to keep the energy.
        float mixed[Lines];
        for (int i = 0; i < Lines; ++i)
            mixed[i] = out[i];
        for (int step = 1; step < Lines; step <<= 1)
            for (int i = 0; i < Lines; i += step << 1)
                for (int k = i; k < i + step; ++k)
                {
                    const float a = mixed[k], b = mixed[k + step];
                    mixed[k] = a + b;
                    mixed[k + step] = a - b;
                }
        for (int i = 0; i < Lines; ++i)
        {
            line[i][at[i]] = x + mixed[i] * 0.35355339f * gain[i];
            if (++at[i] >= length[i])
                at[i] = 0;
        }
    }
};
