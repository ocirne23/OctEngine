export module Animation:StateMachine;

import Core;
import :Clip;

// A condition on a named parameter. A transition fires when all of its conditions pass (AND). Triggers
// are one-shot: a trigger condition passes only while the trigger is set, and firing the transition
// consumes it.
export struct AnimCondition
{
    enum class Type : uint8 { FloatGreater, FloatLess, BoolEquals, Trigger };
    oc::string param;
    Type type = Type::FloatGreater;
    float value = 0.0f;     // FloatGreater / FloatLess threshold
    bool boolValue = false; // BoolEquals target
};

export inline AnimCondition floatGreater(const oc::string& p, float v) { return AnimCondition{ p, AnimCondition::Type::FloatGreater, v, false }; }
export inline AnimCondition floatLess(const oc::string& p, float v)    { return AnimCondition{ p, AnimCondition::Type::FloatLess, v, false }; }
export inline AnimCondition boolIs(const oc::string& p, bool b)        { return AnimCondition{ p, AnimCondition::Type::BoolEquals, 0.0f, b }; }
export inline AnimCondition trigger(const oc::string& p)               { return AnimCondition{ p, AnimCondition::Type::Trigger, 0.0f, false }; }

// A simple animation state machine that drives an AnimationPlayer. Each state plays a single clip or a 1D
// blend space (whose axis is bound to a float parameter). Transitions crossfade between states when their
// parameter conditions are met. The owner sets parameters (setFloat/setBool/setTrigger) and calls update()
// each frame BEFORE ticking the player (update only changes which source plays + the blend axis; it does
// not tick).
export class AnimStateMachine
{
public:
    using StateId = uint32;
    static constexpr StateId INVALID_STATE = UINT32_MAX;

    void initialize(AnimationPlayer* pPlayer) { m_pPlayer = pPlayer; }

    StateId addClipState(const oc::string& name, const AnimationClip* pClip);
    // A blend-space state: blendParam names the float parameter that drives the blend axis each update.
    StateId addBlendState(const oc::string& name, const BlendSpace1D* pBlendSpace, const oc::string& blendParam);
    void setEntryState(StateId id) { m_entry = id; }

    void addTransition(StateId from, StateId to, oc::vector<AnimCondition> conditions, float fadeSeconds = 0.2f, float exitTime = 0.0f);
    // A transition evaluable from any state (e.g. a death trigger). Checked before per-state transitions.
    void addAnyTransition(StateId to, oc::vector<AnimCondition> conditions, float fadeSeconds = 0.2f, float exitTime = 0.0f);

    void setFloat(const oc::string& name, float v) { m_floats[name] = v; }
    void setBool(const oc::string& name, bool v)   { m_bools[name] = v; }
    void setTrigger(const oc::string& name)        { m_triggers[name] = true; }

    float getFloat(const oc::string& name) const { const auto it = m_floats.find(name); return it == m_floats.end() ? 0.0f : it->second; }
    bool getBool(const oc::string& name) const   { const auto it = m_bools.find(name); return it == m_bools.end() ? false : it->second; }

    void update(float deltaSeconds);

    StateId getCurrentState() const { return m_current; }
    const oc::string& getCurrentStateName() const;
    float getTimeInState() const { return m_timeInState; }

private:
    struct State
    {
        oc::string name;
        const AnimationClip* clip = nullptr;
        const BlendSpace1D* blend = nullptr;
        oc::string blendParam;
    };
    struct Transition
    {
        StateId from = INVALID_STATE;
        StateId to = INVALID_STATE;
        oc::vector<AnimCondition> conditions;
        float fade = 0.2f;
        float exitTime = 0.0f; // if > 0, only fire once the current source reaches this normalized time
        bool fromAny = false;
    };

    bool conditionsMet(const oc::vector<AnimCondition>& conds) const;
    void enterState(StateId id, float fade);

    AnimationPlayer* m_pPlayer = nullptr;
    oc::vector<State> m_states;
    oc::vector<Transition> m_transitions;
    oc::unordered_map<oc::string, float> m_floats;
    oc::unordered_map<oc::string, bool> m_bools;
    oc::unordered_map<oc::string, bool> m_triggers;
    StateId m_entry = INVALID_STATE;
    StateId m_current = INVALID_STATE;
    float m_timeInState = 0.0f;
};
