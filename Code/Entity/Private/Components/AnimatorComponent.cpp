module Entity;

import Core;
import Core.Log;
import Core.Transform;
import :Entity;
import :AnimationDescription;
import Animation;
import RendererVK;

static AnimCondition toAnimCondition(const AnimatorDesc::Condition& c)
{
    using Op = AnimatorDesc::Condition::Op;
    switch (c.op)
    {
    case Op::Less:    return floatLess(c.param, c.value);
    case Op::Equal:   return boolIs(c.param, c.boolValue);
    case Op::Trigger: return trigger(c.param);
    case Op::Greater:
    default:          return floatGreater(c.param, c.value);
    }
}

void AnimatorComponent::spawn(Entity& entity, const SpawnInfo& info, const Transform& base)
{
    entity.setProfiled(); // animated entities carry a per-entity profile scope
    enabled = info.enabled;
    if (!info.desc || !info.skeleton || !info.clipSet)
        return;
    const AnimatorDesc& desc = *info.desc;
    clipSet = info.clipSet; // shared, World-cached library (clips already loaded + retargeted)

    player.initialize(info.skeleton);
    player.setClipLibrary(clipSet);

    // Blend spaces (reserve so the state machine can hold stable pointers into this vector).
    oc::unordered_map<oc::string, size_t> blendIndex;
    blendSpaces.reserve(desc.blendSpaces.size());
    for (const AnimatorDesc::BlendSpace& bs : desc.blendSpaces)
    {
        BlendSpace1D space;
        for (const AnimatorDesc::BlendSample& s : bs.samples)
            if (const AnimationClip* c = clipSet->find(s.clip))
                space.addSample(c, s.position);
        blendIndex[bs.name] = blendSpaces.size();
        blendSpaces.push_back(oc::move(space));
    }

    defaultSpeed = desc.speed;

    if (desc.stateMachine.present)
    {
        hasStateMachine = true;
        stateMachine.initialize(&player);

        for (const AnimatorDesc::Param& p : desc.params)
        {
            if (p.type == AnimatorDesc::ParamType::Float) stateMachine.setFloat(p.name, p.floatValue);
            else if (p.type == AnimatorDesc::ParamType::Bool) stateMachine.setBool(p.name, p.boolValue);
        }

        oc::unordered_map<oc::string, AnimStateMachine::StateId> stateIds;
        for (const AnimatorDesc::State& st : desc.stateMachine.states)
        {
            AnimStateMachine::StateId id = AnimStateMachine::INVALID_STATE;
            if (auto it = blendIndex.find(st.play); it != blendIndex.end())
                id = stateMachine.addBlendState(st.name, &blendSpaces[it->second], desc.blendSpaces[it->second].axisParam);
            else if (const AnimationClip* c = clipSet->find(st.play))
                id = stateMachine.addClipState(st.name, c);
            else
            {
                Log::warning("Animator '" + desc.name + "': state '" + st.name + "' plays unknown source '" + st.play + "'");
                continue;
            }
            stateIds[st.name] = id;
            stateSpeeds.push_back(st.speed); // StateIds are sequential in add order, so this aligns by index
        }

        if (auto it = stateIds.find(desc.stateMachine.entry); it != stateIds.end())
            stateMachine.setEntryState(it->second);

        for (const AnimatorDesc::Transition& tr : desc.stateMachine.transitions)
        {
            oc::vector<AnimCondition> conds;
            conds.reserve(tr.conditions.size());
            for (const AnimatorDesc::Condition& c : tr.conditions)
                conds.push_back(toAnimCondition(c));

            const auto toIt = stateIds.find(tr.to);
            if (toIt == stateIds.end())
                continue;
            if (tr.from.empty())
            {
                stateMachine.addAnyTransition(toIt->second, oc::move(conds), tr.fade, tr.exitTime);
            }
            else if (const auto fromIt = stateIds.find(tr.from); fromIt != stateIds.end())
            {
                stateMachine.addTransition(fromIt->second, toIt->second, oc::move(conds), tr.fade, tr.exitTime);
            }
        }
    }
    else if (clipSet->numClips() > 0)
    {
        player.play(clipSet->get(0)); // no state machine: just play the first clip
        if (defaultSpeed.hasConst)
            player.setSpeed(defaultSpeed.value); // no parameters without a state machine, so const only
    }

    built = true;
}

float AnimatorComponent::resolvePlaybackSpeed() const
{
    AnimStateMachine::StateId cur = stateMachine.getCurrentState();
    const AnimatorDesc::SpeedBinding* b =
        (cur != AnimStateMachine::INVALID_STATE && cur < stateSpeeds.size() && stateSpeeds[cur].isSet())
        ? &stateSpeeds[cur]
        : (defaultSpeed.isSet() ? &defaultSpeed : nullptr);
    if (!b)
        return 1.0f;
    if (!b->param.empty())
        return stateMachine.getFloat(b->param) * b->scale;
    return b->value;
}

void AnimatorComponent::update(Entity& entity, Renderer& renderer, float deltaSeconds)
{
    if (!enabled || !built)
        return;
    if (hasStateMachine)
    {
        stateMachine.update(deltaSeconds); // fires transitions + sets the blend axis (does not tick)
        player.setSpeed(resolvePlaybackSpeed()); // warp playback rate for the active state
    }
    player.tick(deltaSeconds);

    if (!player.getFiredEvents().empty())
    {
        for (const oc::string& e : player.getFiredEvents()) // notifies crossed this tick
        {
            if (onEvent) onEvent(e);
        }
    }

    if (RenderComponent* rc = getComponent<RenderComponent>(&entity); rc && rc->node.isSkinned())
        renderer.setSkinningPalette(rc->node.getSkinnedPaletteHandle(), player.getPalette());
}

void AnimatorComponent::destroy(Entity& entity, const SpawnInfo& info)
{
}

AnimatorComponent::~AnimatorComponent()
{
}

const AnimatorComponent::SpawnInfo* getAnimatorSpawnInfo(const Entity* entity)
{
    if (!entity->spawnTemplate || !hasComponent<AnimatorComponent>(entity))
        return nullptr;

    size_t idx = 0;
    for (uint16 i = 0; i < uint16(EComponentID_Animator); ++i)
        if (entity->typeBits & (1 << i))
            ++idx;
    if (idx >= entity->spawnTemplate->spawnInfos.size())
        return nullptr;
    return static_cast<const AnimatorComponent::SpawnInfo*>(entity->spawnTemplate->spawnInfos[idx].get());
}

void writeAnimatorSpawnInfo(const AnimatorComponent::SpawnInfo& info, AssetNode& out)
{
    if (info.animatorName.empty())
        return;
    out.set("Animator", info.animatorName);
    if (!info.enabled)
        out.set("Enabled", info.enabled);
}
