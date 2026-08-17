export module Entity:AnimationDescription;

import Core;
import File;

// .anm — one named animation clip descriptor: a source file + which track inside it. Clips are retargeted
// by bone name at load (see ISceneData::loadAnimations), so a rig and its animations can live in separate
// files (Mixamo-style). The clip library that uses these lives in a .apl animator.
export struct AnimationClipDesc
{
    oc::string name;     // global clip name (referenced from a .apl's `Clip ... Anim <name>`)
    oc::string source;   // source file path, or a registered ObjectContainer name
    oc::string track;    // track within the source (empty = first kept track)
    bool loop = true;     // false = one-shot (playback clamps + holds the last frame)
    oc::string skip;     // ignore tracks whose name contains this (e.g. "TPose")
    oc::vector<oc::pair<oc::string, float>> events; // notifies: name -> normalized time (0..1)
};

// .apl — an animator graph: parameters + a clip library + blend spaces + a state machine. Maps 1:1 onto
// the Animation runtime (AnimationPlayer + AnimStateMachine + BlendSpace1D). Stored as plain data; an
// AnimatorComponent instantiates the runtime objects against a given skeleton at spawn time.
export struct AnimatorDesc
{
    enum class ParamType : uint8 { Float, Bool, Trigger };
    struct Param
    {
        oc::string name;
        ParamType type = ParamType::Float;
        float floatValue = 0.0f;
        bool boolValue = false;
    };

    // A clip in this animator's library: a local name bound to a global .anm clip.
    struct ClipRef
    {
        oc::string localName; // name used by states / blend-space samples within this animator
        oc::string anmName;   // the .anm clip it resolves to
    };

    struct BlendSample { oc::string clip; float position = 0.0f; }; // clip = a local clip name
    struct BlendSpace
    {
        oc::string name;
        oc::string axisParam; // the float parameter that drives the blend axis each update
        oc::vector<BlendSample> samples;
    };

    struct Condition
    {
        enum class Op : uint8 { Greater, Less, Equal, Trigger };
        oc::string param;
        Op op = Op::Greater;
        float value = 0.0f;     // Greater / Less threshold
        bool boolValue = false; // Equal target
    };
    // Playback-rate scaling for a state (or an animator-wide default). Either a constant Speed, or a float
    // parameter (optionally multiplied by SpeedScale) that warps the clip rate each update — e.g. driving
    // run playback from the same "speed" parameter that drives the locomotion blend.
    struct SpeedBinding
    {
        oc::string param;     // float parameter driving playback speed (empty = none)
        float scale = 1.0f;    // multiplier on the parameter value
        bool hasConst = false; // a constant Speed was authored
        float value = 1.0f;    // constant playback speed (used when hasConst and no param)
        bool isSet() const { return !param.empty() || hasConst; }
    };

    struct State
    {
        oc::string name;
        oc::string play; // a local clip name or a blend-space name
        SpeedBinding speed;
    };
    struct Transition
    {
        oc::string from; // empty = an "any-state" transition
        oc::string to;
        oc::vector<Condition> conditions;
        float fade = 0.2f;
        float exitTime = 0.0f;
    };
    struct StateMachine
    {
        bool present = false;
        oc::string entry;
        oc::vector<State> states;
        oc::vector<Transition> transitions;
    };

    oc::string name;
    oc::vector<Param> params;
    oc::vector<ClipRef> clips;
    oc::vector<BlendSpace> blendSpaces;
    StateMachine stateMachine;
    SpeedBinding speed; // animator-wide playback speed default (states without their own Speed use this)
};

export bool toAnimationClipDesc(const AssetNode& node, AnimationClipDesc& out); // node.key must be "Animation"
export bool toAnimatorDesc(const AssetNode& node, AnimatorDesc& out);           // node.key must be "Animator"
