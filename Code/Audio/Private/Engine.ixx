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
import :Reverb;

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
    // Set by setBuffer (game thread), taken by the audio thread at its next block: the FIFOs below still hold the END
    // of the sound the source played before, and without this the new sound starts with those few milliseconds.
    oc::atomic<bool> flush{ false };
    // THE DISTANCE, beyond the gain: `lowpass` is a one-pole coefficient on the dry mono signal (1 = open; the air
    // takes a far sound's highs) and `send` is how much of that signal goes to the shared reverb (SystemState's
    // send ring). Both are the game thread's to set (AudioSource::setLowpass / setReverbSend).
    oc::atomic<float> lowpass{ 1.0f };
    oc::atomic<float> send{ 0.0f };
    float lowpassState = 0.0f; // audio thread
    uint64 sendWrite = 0;      // audio thread: this node's place in the send ring's timeline

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

// The shared reverb's node: no input bus (the sends come through the ring below), one stereo output on the endpoint.
export struct ReverbNode
{
    ma_node_base base; // must be first
};

export constexpr uint32 SendRingSize = 8192; // frames, a power of two
// A send lands this far ahead of where the reverb reads: more than a device period plus an HRTF block, so a node
// that runs after the reverb node in a callback is still ahead of it. It is also the reverb's pre-delay (43 ms).
export constexpr uint32 SendLead = 2048;

export struct SystemState
{
    // THE REVERB BUS (audio thread only, except reverbLevel and FdnReverb::configure). Every BinauralNode adds its
    // dry block x send into sendRing at its own sendWrite; the reverb node reads from sendRead, clears what it read,
    // runs the reverb on it and plays it. One timeline for all: sendRead counts the frames the reverb node has played.
    float sendRing[SendRingSize] = {};
    uint64 sendRead = 0;
    FdnReverb reverb;
    ReverbNode reverbNode;
    bool reverbNodeValid = false;
    oc::atomic<float> reverbLevel{ 0.0f };

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
export void reverbNodeProcess(ma_node* pNode, const float** ppFramesIn, ma_uint32* pFrameCountIn, float** ppFramesOut, ma_uint32* pFrameCountOut);
export ma_node_vtable g_reverbNodeVTable = { reverbNodeProcess, nullptr, 0, 1, 0 };
