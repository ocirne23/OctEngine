module Animation;

import Core;

AnimStateMachine::StateId AnimStateMachine::addClipState(const oc::string& name, const AnimationClip* pClip)
{
    const StateId id = (StateId)m_states.size();
    m_states.push_back(State{ name, pClip, nullptr, {} });
    return id;
}

AnimStateMachine::StateId AnimStateMachine::addBlendState(const oc::string& name, const BlendSpace1D* pBlendSpace, const oc::string& blendParam)
{
    const StateId id = (StateId)m_states.size();
    m_states.push_back(State{ name, nullptr, pBlendSpace, blendParam });
    return id;
}

void AnimStateMachine::addTransition(StateId from, StateId to, oc::vector<AnimCondition> conditions, float fadeSeconds, float exitTime)
{
    m_transitions.push_back(Transition{ from, to, oc::move(conditions), fadeSeconds, exitTime, false });
}

void AnimStateMachine::addAnyTransition(StateId to, oc::vector<AnimCondition> conditions, float fadeSeconds, float exitTime)
{
    m_transitions.push_back(Transition{ INVALID_STATE, to, oc::move(conditions), fadeSeconds, exitTime, true });
}

bool AnimStateMachine::conditionsMet(const oc::vector<AnimCondition>& conds) const
{
    for (const AnimCondition& c : conds)
    {
        switch (c.type)
        {
        case AnimCondition::Type::FloatGreater:
        {
            const auto it = m_floats.find(c.param);
            if (!(it != m_floats.end() && it->second > c.value)) return false;
            break;
        }
        case AnimCondition::Type::FloatLess:
        {
            const auto it = m_floats.find(c.param);
            if (!(it != m_floats.end() && it->second < c.value)) return false;
            break;
        }
        case AnimCondition::Type::BoolEquals:
        {
            const auto it = m_bools.find(c.param);
            const bool v = it != m_bools.end() && it->second;
            if (v != c.boolValue) return false;
            break;
        }
        case AnimCondition::Type::Trigger:
        {
            const auto it = m_triggers.find(c.param);
            if (!(it != m_triggers.end() && it->second)) return false;
            break;
        }
        }
    }
    return true;
}

void AnimStateMachine::enterState(StateId id, float fade)
{
    m_current = id;
    m_timeInState = 0.0f;
    const State& s = m_states[id];
    if (s.blend)
        m_pPlayer->playBlendSpace(s.blend, fade);
    else
        m_pPlayer->play(s.clip, fade);
}

void AnimStateMachine::update(float deltaSeconds)
{
    if (!m_pPlayer || m_states.empty())
        return;

    if (m_current == INVALID_STATE)
    {
        enterState(m_entry != INVALID_STATE ? m_entry : 0, 0.0f);
        return;
    }

    m_timeInState += deltaSeconds;

    // Evaluate transitions in declaration order (any-state ones included). The first whose source/exit-time
    // gate and conditions all pass fires.
    for (const Transition& t : m_transitions)
    {
        if (!t.fromAny && t.from != m_current)
            continue;
        if (t.to == m_current)
            continue;
        if (t.exitTime > 0.0f && m_pPlayer->getNormalizedTime() < t.exitTime)
            continue;
        if (!conditionsMet(t.conditions))
            continue;

        // Consume any triggers this transition tested.
        for (const AnimCondition& c : t.conditions)
            if (c.type == AnimCondition::Type::Trigger)
                m_triggers[c.param] = false;

        enterState(t.to, t.fade);
        break;
    }

    // Drive the blend axis of the (possibly just-entered) current state from its bound parameter.
    const State& s = m_states[m_current];
    if (s.blend)
    {
        const auto it = m_floats.find(s.blendParam);
        if (it != m_floats.end())
            m_pPlayer->setBlendParameter(it->second);
    }
}

const oc::string& AnimStateMachine::getCurrentStateName() const
{
    static const oc::string kNone = "<none>";
    return m_current != INVALID_STATE ? m_states[m_current].name : kNone;
}
