module;

#pragma warning(push, 3)
#pragma warning(disable: 4701 4702 4245 4244 4310)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#include <miniaudio/miniaudio.h>
#pragma warning(pop)

#include <phonon/phonon.h>

export module Audio:Engine;

import Core;
import Core.glm;

// Internal engine state shared by AudioBuffer/AudioSource/AudioSystem's implementations. Not part of
// the public API (never re-exported from Audio.ixx).

export constexpr uint32 MaxOneShotSources = 32;
export constexpr uint32 IplFrameSize = 256; // HRTF processing block (~5.3ms latency at 48kHz)
export constexpr uint32 SampleRate = 48000;

export struct SoundData
{
    oc::vector<float> frames; // interleaved f32
    uint32 channels = 1;
    uint32 sampleRate = SampleRate;
    uint32 frameCount() const { return uint32(frames.size() / channels); }
};

// Custom miniaudio node applying Steam Audio's HRTF: stereo in (downmixed to mono) -> binaural
// stereo out. Steam Audio processes fixed-size blocks while the node graph asks for arbitrary
// frame counts, so input/output are staged through one-block FIFOs.
export struct BinauralNode
{
    ma_node_base base; // must be first
    IPLBinauralEffect effect = nullptr;
    oc::atomic<float> dirX{ 0.0f }, dirY{ 0.0f }, dirZ{ -1.0f }; // listener-space unit direction to the source
    oc::atomic<float> blend{ 1.0f };                             // 0 = unspatialized passthrough (relative sources)

    float inFifo[IplFrameSize] = {};           // downmixed mono, waiting for a full block
    uint32 inCount = 0;
    float outBlock[IplFrameSize * 2] = {};     // interleaved stereo of the last processed block
    uint32 outRead = 0, outAvail = 0;
    float scratchL[IplFrameSize] = {}, scratchR[IplFrameSize] = {};
};

export struct SourceState
{
    ma_sound sound;
    ma_audio_buffer buffer;
    BinauralNode node;
    bool soundValid = false;
    bool nodeValid = false;
    bool spatial = false; // routed through the binaural node
    const SoundData* data = nullptr;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 velocity = glm::vec3(0.0f);
    float gain = 1.0f;
    float pitch = 1.0f;
    bool relative = false;
    bool looping = false;
    float refDist = 1.0f;
    float maxDist = FLT_MAX;
    float rolloff = 1.0f;
};

export struct SystemState
{
    ma_engine engine;
    IPLContext iplContext = nullptr;
    IPLHRTF hrtf = nullptr;
    IPLAudioSettings iplAudioSettings = { SampleRate, (IPLint32)IplFrameSize };
    oc::vector<oc::unique_ptr<SoundData>> buffers;
    oc::vector<oc::unique_ptr<SourceState>> sources;
    glm::vec3 listenerPos = glm::vec3(0.0f);
    glm::vec3 listenerFwd = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 listenerUp = glm::vec3(0.0f, 1.0f, 0.0f);
    glm::vec3 listenerVel = glm::vec3(0.0f);
};

export SystemState& audioState();
export SourceState* toState(uint64 handle);

// Recomputes the HRTF direction, distance attenuation and doppler for a source from the current
// listener state. Game thread; the audio thread picks the values up through the node's atomics.
export void updateSource(SourceState& s);
export void clearSourceSound(SourceState& s);
export void destroySourceState(SourceState& s);

export void binauralNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut);
export ma_node_vtable g_binauralNodeVTable = { binauralNodeProcess, nullptr, 1, 1, 0 };
