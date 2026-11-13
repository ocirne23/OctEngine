module;

#pragma warning(push, 3)
#pragma warning(disable: 4701 4702 4245 4244 4310)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#include <miniaudio/miniaudio.h>
#pragma warning(pop)

#include <phonon/phonon.h>

module Audio;

import Core;
import Core.glm;

import :Engine;

constexpr float SpeedOfSound = 343.0f;

static IPLVector3 toIpl(const glm::vec3& v) { return IPLVector3{ v.x, v.y, v.z }; }

// "Is this a number": by its BITS (the exponent is not all ones). The build is /fp:fast, under which the compiler may
// take a test like isNumber(x) or x == x as always true and remove it. This one it cannot.
static bool isNumber(float value)
{
    uint32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

SystemState& audioState()
{
    static SystemState state;
    return state;
}

SourceState* toState(uint64 handle) { return reinterpret_cast<SourceState*>(handle); }

void binauralNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
    BinauralNode& node = *reinterpret_cast<BinauralNode*>(pNode);
    const float* in = ppFramesIn ? ppFramesIn[0] : nullptr;
    float* out = ppFramesOut[0];
    const uint32 inAvail = (pFrameCountIn && in) ? *pFrameCountIn : 0;
    const uint32 outWanted = *pFrameCountOut;
    if (node.flush.exchange(false, oc::memory_order_relaxed)) // a new sound on this source: drop what the last one left
    {
        node.inCount = 0;
        node.outRead = 0;
        node.outAvail = 0;
    }

    uint32 framesIn = 0;
    uint32 framesOut = 0;
    while (framesOut < outWanted)
    {
        if (node.outAvail > 0) // drain the staged output block first
        {
            out[framesOut * 2 + 0] = node.outBlock[node.outRead * 2 + 0];
            out[framesOut * 2 + 1] = node.outBlock[node.outRead * 2 + 1];
            node.outRead++;
            node.outAvail--;
            framesOut++;
            continue;
        }
        while (node.inCount < IplFrameSize && framesIn < inAvail) // stage input (stereo -> mono downmix)
        {
            node.inFifo[node.inCount++] = (in[framesIn * 2 + 0] + in[framesIn * 2 + 1]) * 0.5f;
            framesIn++;
        }
        if (node.inCount < IplFrameSize)
            break; // not enough input for a full block yet

        // The distance filter on the dry block (the direct sound and the send both get it), then the send.
        const float lowpass = node.lowpass.load(oc::memory_order_relaxed);
        if (lowpass < 0.999f)
        {
            for (uint32 i = 0; i < IplFrameSize; ++i)
            {
                node.lowpassState += (node.inFifo[i] - node.lowpassState) * lowpass;
                node.inFifo[i] = node.lowpassState;
            }
        }
        else
            node.lowpassState = node.inFifo[IplFrameSize - 1];
        const float send = node.send.load(oc::memory_order_relaxed);
        if (send > 0.0001f)
        {
            SystemState& state = audioState();
            // This node's blocks follow each other in the ring. Only a node that was silent for a while (far behind),
            // or one that got far ahead, is put back at the lead: a re-sync on every block would leave gaps, because
            // a node emits its 256-frame blocks in a rhythm that is not the device's.
            if (node.sendWrite + 1024 < state.sendRead + SendLead || node.sendWrite > state.sendRead + SendRingSize - 2 * IplFrameSize)
                node.sendWrite = state.sendRead + SendLead;
            for (uint32 i = 0; i < IplFrameSize; ++i)
            {
                const float sample = node.inFifo[i] * send;
                if (isNumber(sample)) // never a bad value into the reverb: it would stay there (reverbNodeProcess)
                    state.sendRing[(node.sendWrite + i) & (SendRingSize - 1)] += sample;
            }
            node.sendWrite += IplFrameSize;
        }

        IPLBinauralEffectParams params{};
        params.direction = IPLVector3{ node.dirX.load(oc::memory_order_relaxed), node.dirY.load(oc::memory_order_relaxed), node.dirZ.load(oc::memory_order_relaxed) };
        params.interpolation = IPL_HRTFINTERPOLATION_BILINEAR;
        params.spatialBlend = node.blend.load(oc::memory_order_relaxed);
        params.hrtf = audioState().hrtf;
        float* inChannels[1] = { node.inFifo };
        IPLAudioBuffer inBuffer{ 1, (IPLint32)IplFrameSize, inChannels };
        float* outChannels[2] = { node.scratchL, node.scratchR };
        IPLAudioBuffer outBuffer{ 2, (IPLint32)IplFrameSize, outChannels };
        iplBinauralEffectApply(node.effect, &params, &inBuffer, &outBuffer);

        // THE LAST LINE: no value that is not finite leaves this node. One NaN at the endpoint is silence or a blast
        // from the device for as long as it lasts. A bad block is dropped whole, and the effect is reset (its own
        // state holds the NaN too).
        bool finite = true;
        for (uint32 i = 0; i < IplFrameSize; ++i)
            finite &= isNumber(node.scratchL[i]) && isNumber(node.scratchR[i]);
        if (!finite)
        {
            iplBinauralEffectReset(node.effect);
            node.lowpassState = 0.0f;
        }
        for (uint32 i = 0; i < IplFrameSize; ++i)
        {
            node.outBlock[i * 2 + 0] = finite ? node.scratchL[i] : 0.0f;
            node.outBlock[i * 2 + 1] = finite ? node.scratchR[i] : 0.0f;
        }
        node.outRead = 0;
        node.outAvail = IplFrameSize;
        node.inCount = 0;
    }
    for (; framesOut < outWanted; ++framesOut) // upstream dry: pad with silence
    {
        out[framesOut * 2 + 0] = 0.0f;
        out[framesOut * 2 + 1] = 0.0f;
    }
    *pFrameCountOut = outWanted;
    if (pFrameCountIn)
        *pFrameCountIn = framesIn;
}

// The shared reverb: what the sources sent (the ring), through the reverb, to the endpoint.
void reverbNodeProcess(ma_node*, const float**, ma_uint32*, float** ppFramesOut, ma_uint32* pFrameCountOut)
{
    SystemState& state = audioState();
    float* out = ppFramesOut[0];
    const uint32 frames = *pFrameCountOut;
    const float level = state.reverbLevel.load(oc::memory_order_relaxed);
    bool finite = true;
    for (uint32 i = 0; i < frames; ++i)
    {
        float& cell = state.sendRing[(state.sendRead + i) & (SendRingSize - 1)];
        float left = 0.0f, right = 0.0f;
        state.reverb.process(isNumber(cell) ? cell : 0.0f, left, right);
        cell = 0.0f;
        finite &= isNumber(left) && isNumber(right);
        out[i * 2 + 0] = left * level;
        out[i * 2 + 1] = right * level;
    }
    state.sendRead += frames;
    // A REVERB FEEDS ITSELF: one value in it that is not finite is in every sample from then on. If that ever
    // happens, this block is silence and the reverb starts again from nothing - a gap of a tail, not the end of the
    // audio.
    if (!finite)
    {
        state.reverb.clear();
        for (uint32 i = 0; i < frames * 2; ++i)
            out[i] = 0.0f;
    }
}

void updateSource(SourceState& s)
{
    if (!s.soundValid)
        return;
    SystemState& state = audioState();
    float attenuation = 1.0f;
    float doppler = 1.0f;
    if (s.spatial)
    {
        if (s.relative)
        {
            s.node.blend.store(0.0f, oc::memory_order_relaxed);
        }
        else
        {
            const glm::vec3 toSource = s.position - state.listenerPos;
            const float dist = glm::length(toSource);
            // A SOURCE AT THE LISTENER HAS NO DIRECTION. iplCalculateRelativeDirection divides by the distance: at 0 it
            // gives NaN, the HRTF then writes NaN samples, and those go to the endpoint AND into the reverb, which
            // feeds itself - the whole output is gone until the engine restarts. So: inside 5 cm, or with any value
            // that is not finite, the source is straight ahead and the HRTF is blended out (it plays as it is).
            IPLVector3 dir{ 0.0f, 0.0f, -1.0f };
            float blend = 0.0f;
            if (dist > 0.05f && isNumber(dist))
            {
                dir = iplCalculateRelativeDirection(state.iplContext,
                    toIpl(s.position), toIpl(state.listenerPos), toIpl(state.listenerFwd), toIpl(state.listenerUp));
                if (isNumber(dir.x) && isNumber(dir.y) && isNumber(dir.z))
                    blend = glm::clamp((dist - 0.05f) / 0.45f, 0.0f, 1.0f); // in over the next 45 cm: no jump at the edge
                else
                    dir = IPLVector3{ 0.0f, 0.0f, -1.0f };
            }
            s.node.dirX.store(dir.x, oc::memory_order_relaxed);
            s.node.dirY.store(dir.y, oc::memory_order_relaxed);
            s.node.dirZ.store(dir.z, oc::memory_order_relaxed);
            s.node.blend.store(blend, oc::memory_order_relaxed);

            const float clamped = glm::clamp(dist, s.refDist, s.maxDist);
            attenuation = s.refDist / (s.refDist + s.rolloff * (clamped - s.refDist));

            if (dist > 0.001f)
            {
                const glm::vec3 d = toSource / dist;
                const float vListener = glm::dot(state.listenerVel, d); // toward the source
                const float vSource = -glm::dot(s.velocity, d);         // toward the listener
                doppler = glm::clamp((SpeedOfSound + vListener) / glm::max(SpeedOfSound - vSource, 1.0f), 0.5f, 2.0f);
            }
        }
    }
    ma_sound_set_volume(&s.sound, s.gain * attenuation);
    ma_sound_set_pitch(&s.sound, s.pitch * doppler);
}

void clearSourceSound(SourceState& s)
{
    if (!s.soundValid)
        return;
    ma_sound_uninit(&s.sound);
    ma_audio_buffer_uninit(&s.buffer);
    s.soundValid = false;
    s.spatial = false;
    s.data = nullptr;
}

void destroySourceState(SourceState& s)
{
    clearSourceSound(s);
    if (s.nodeValid)
    {
        ma_node_uninit(&s.node.base, nullptr);
        iplBinauralEffectRelease(&s.node.effect);
        s.nodeValid = false;
    }
}
