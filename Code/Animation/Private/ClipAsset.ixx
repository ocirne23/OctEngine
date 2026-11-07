export module Animation:ClipAsset;

import Core;
import Core.glm;
import :Skeleton;
import :Clip;

// ---- The .clip asset: one animation clip as KEYFRAMES BY BONE NAME ----
// What the Animation Editor edits and saves, and what every clip the runtime plays is built from: a .clip file directly, and an .anm's FBX through the
// importer (ISceneData::importClipAssets). It knows no skeleton: buildAnimationClip resolves the track names against one, so a clip plays on every rig
// with those bone names. Core-only, like the rest of the library: the caller reads and writes the text.

export enum class ClipChannel : uint8 { Position, Rotation, Scale };
export constexpr uint32 c_numClipChannels = 3;

export struct ClipTrack
{
    oc::string bone;                     // the node name in the rig (FBX pivot helpers included: "<bone>_$AssimpFbx$_Rotation")
    oc::vector<PositionKey> positionKeys; // each sorted by time, no two keys at the same time
    oc::vector<RotationKey> rotationKeys;
    oc::vector<ScaleKey>    scaleKeys;

    bool empty() const { return positionKeys.empty() && rotationKeys.empty() && scaleKeys.empty(); }
    uint32 numKeys(ClipChannel channel) const
    {
        switch (channel)
        {
        case ClipChannel::Position: return (uint32)positionKeys.size();
        case ClipChannel::Rotation: return (uint32)rotationKeys.size();
        default:                    return (uint32)scaleKeys.size();
        }
    }
};

export struct ClipAsset
{
    float duration = 1.0f;    // seconds
    // The editor's key grid: the duration cut into this many equal steps (keys snap to them). No frame rate: playback is continuous, and the grid only
    // grows when the editor subdivides it or extends the clip by whole steps.
    uint32 steps = 10;
    bool loop = true;
    oc::string previewRig;    // the ObjectContainer the editor previews the clip on (editor only)
    oc::vector<AnimationEventKey> events; // normalized time, like .anm
    oc::vector<ClipTrack> tracks;

    float step() const { return duration > 1e-6f ? duration / float(steps > 0 ? steps : 1u) : 0.1f; } // seconds per grid step

    int32 findTrack(const oc::string& bone) const
    {
        for (uint32 i = 0; i < (uint32)tracks.size(); ++i)
            if (tracks[i].bone == bone)
                return (int32)i;
        return -1;
    }
    uint32 numKeys() const
    {
        uint32 n = 0;
        for (const ClipTrack& t : tracks)
            n += (uint32)(t.positionKeys.size() + t.rotationKeys.size() + t.scaleKeys.size());
        return n;
    }
};

// The text form. parse returns false with a line-numbered reason; the asset is left partly filled.
export bool parseClipAsset(oc::string_view text, ClipAsset& out, oc::string& error);
export oc::string writeClipAsset(const ClipAsset& clip);

// The runtime clip on one skeleton: a channel per track whose bone the skeleton has (the other tracks are left out).
export AnimationClip buildAnimationClip(const ClipAsset& asset, const Skeleton& skeleton, const oc::string& name);

// Keyframe reduction: drops every Linear key that the line between its kept neighbours reproduces within the tolerances (at that key and at every key
// dropped beside it), and a channel that never moves keeps one key. Step and Smooth keys stay. Returns how many keys were removed.
export struct ClipReduceTolerance
{
    float position = 0.0005f;   // model units
    float rotationDegrees = 0.1f;
    float scale = 0.0005f;
};
export uint32 reduceClipKeys(ClipAsset& clip, const ClipReduceTolerance& tolerance);

// Sorts every channel by time and merges keys closer than minGapSeconds (the later one in the vector wins). Run after any edit that moves keys.
export void sortClipKeys(ClipAsset& clip, float minGapSeconds = 1e-5f);
