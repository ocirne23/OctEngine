module RendererVK;

import Core;
import Settings.Tweaks;
import Core.Log;

import File;

import :Layout;
import :UboBlock;
import :PushFields;
import :Device;

// Renderer: the TWEAK LOCKS. Every lockable UBO value (an entry of m_ubo, registerUboFields at the end of
// RendererUbo.cpp) is a GLSL const of its value in every shader while every tweak it is computed from is locked, so
// the compiler folds it; otherwise the shaders read its block member. The block is still uploaded whole every frame (a
// baked entry's slot is unread).
//
// EVENT-DRIVEN, never polled: registerUboLocks bakes once before the first pipeline compile (every setting is
// registered by then), and later only two events re-bake (applyUboLocks, which then reloads the shaders when a const
// really changed):
//  * a lock click (the locks' onChange) - the entries' locks are resolved again;
//  * a change of a row that feeds baked values (TweakRegistry's change listener, filtered by isVarBakeable). A locked
//    row is read-only in the panel, so that is an override, a synced value, or code that wrote a setting and called
//    Tweak::notifyChanged; an unlocked row only feeds block reads and never re-bakes.
// The same two events mark the PUSH lists (PushFieldList, m_pushFields) dirty: applyUboLocks re-bakes the ones with an
// onRebake and reloads only their passes; the far-tree volume re-bakes its own two in TreeVolumePipeline::prepare.

void Renderer::registerUboLocks()
{
    const auto onLockClick = [this]()
    {
        m_uboResolveDirty = true;
        m_uboLocksDirty = true;
        for (PushFieldOwner& owner : m_pushFields)
            owner.list->markDirty(/*resolve*/ true);
    };
    for (uint32 s = 0; s < RendererVKLayout::NUM_UBO_LOCK_SECTIONS; ++s)
        m_uboLocks[s] = Tweak::lock(RendererVKLayout::c_uboLockSections[s].name, RendererVKLayout::c_uboLockSections[s].categories,
            /*lockedByDefault*/ true, onLockClick);
    TweakRegistry::get().addChangeListener(this, [this](const TweakVar& var)
    {
        if (!TweakRegistry::get().isVarBakeable(var))
            return;
        m_uboLocksDirty = true;
        for (PushFieldOwner& owner : m_pushFields)
            owner.list->markDirty(/*resolve*/ false);
    });

    registerUboFields(m_ubo); // after the root values (UboRoot binds them at construction)
    registerPushFields();
    assert(m_ubo.size() <= RendererVKLayout::UBO_RANGE && "the frame UBO outgrew UBO_RANGE");

    // A lock means something only on a row a baked value is computed from: those rows get the lock button.
    TweakRegistry& tweaks = TweakRegistry::get();
    for (const UboBlock::Entry& e : m_ubo.entries())
        for (const TweakRegistry::Source& s : e.sources)
            tweaks.markLockable(s.address, s.size);
    for (const PushFieldOwner& owner : m_pushFields)
        for (const PushFieldList::Entry& e : owner.list->entries())
            for (const TweakRegistry::Source& s : e.sources)
                tweaks.markLockable(s.address, s.size);
    const size_t count = m_ubo.entries().size();
    m_uboLocked.assign(count, 0);
    m_uboBaked.assign(count, 0);
    m_uboBakedValues.assign(m_ubo.size(), 0);

    // THE startup bake: the declaration the pipelines are first compiled with (initPipelines). The live entries are
    // skipped - what they read may not exist yet; they are never baked anyway.
    m_ubo.evaluate(/*includeLive*/ false);
    resolveUboLocks();
    bakeUboValues();
    m_uboResolveDirty = false;
    m_uboLocksDirty = false;
    for (PushFieldOwner& owner : m_pushFields)
        (void)owner.list->update();
}

// Every lockable push value, per list. A list the Renderer re-bakes (applyUboLocks) names the reload that follows.
void Renderer::registerPushFields()
{
    m_eyeAdaptationPipeline.registerPushFields();
    m_pushFields.push_back(PushFieldOwner{ &m_eyeAdaptationPipeline.pushFields(), [this]
    {
        m_eyeAdaptationPipeline.reloadShaders();
        setHaveToRecordCommandBuffers();
    } });
    m_rainOcclusionPipeline.registerPushFields();
    m_pushFields.push_back(PushFieldOwner{ &m_rainOcclusionPipeline.pushFields(), [this] { m_rainOcclusionPipeline.reloadShaders(); } });
    m_giProbePipeline.registerDebugPushFields();
    m_pushFields.push_back(PushFieldOwner{ &m_giProbePipeline.debugPushFields(), [this]
    {
        m_giProbePipeline.reloadDebugShaders(m_perFrameData[0].sceneColor.getOpaqueRenderPass());
        setHaveToRecordCommandBuffers();
    } });
    // The far-tree volume: its bake and its march read different settings at the same time (a bake runs over frames
    // while the march shows the last one), so it updates its two lists itself (TreeVolumePipeline::prepare).
    m_treeVolume.registerPushFields(m_farTreeParams);
    m_pushFields.push_back(PushFieldOwner{ &m_treeVolume.bakePushFields(), {} });
    m_pushFields.push_back(PushFieldOwner{ &m_treeVolume.marchPushFields(), {} });
}

void Renderer::setUboDeclaration()
{
    RendererVKLayout::g_uboDeclaration = RendererVKLayout::buildUboDeclaration(m_ubo, m_uboBakedValues.data(), m_uboBaked);
    // A copy for reading (the includer serves the string itself).
    FileSystem::createDirectories("Local/Shaders", true);
    FileSystem::writeFileStr("Local/Shaders/ubo.generated.glsl", RendererVKLayout::g_uboDeclaration, true);
}

// Each entry's lock from its sources: locked while every source row is locked (or no lock covers it).
void Renderer::resolveUboLocks()
{
    ProfileScope scope("UBO lock resolve", EProfileCategory::Renderer);
    const TweakRegistry& tweaks = TweakRegistry::get();
    const oc::vector<UboBlock::Entry>& entries = m_ubo.entries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        ETweakSource source = ETweakSource::Locked;
        for (const TweakRegistry::Source& s : entries[i].sources)
        {
            const ETweakSource state = tweaks.sourceState(s.address, s.size);
            if (state == ETweakSource::Unknown)
                Log::warning(oc::format("Renderer: a source of UBO value {} is no registered tweak - it is ignored", entries[i].name));
            source = oc::max(source, state == ETweakSource::Unknown ? ETweakSource::Locked : state);
        }
        m_uboLocked[i] = !entries[i].live && source == ETweakSource::Locked ? 1 : 0;
    }
}

// The consts from the block's evaluated values: an entry is one while it is locked and its value is finite (a const
// cannot spell a NaN). Returns whether any const appeared, went or changed its value.
bool Renderer::bakeUboValues()
{
    const oc::vector<UboBlock::Entry>& entries = m_ubo.entries();
    bool changed = false;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const UboBlock::Entry& e = entries[i];
        const uint8* value = m_ubo.data() + e.offset;
        const bool bake = m_uboLocked[i] && RendererVKLayout::isUboValueFinite(e.type, value);
        if (!bake)
        {
            changed |= m_uboBaked[i] != 0;
            m_uboBaked[i] = 0;
            continue;
        }
        if (m_uboBaked[i] && std::memcmp(value, m_uboBakedValues.data() + e.offset, e.size) == 0)
            continue;
        std::memcpy(m_uboBakedValues.data() + e.offset, value, e.size);
        m_uboBaked[i] = 1;
        changed = true;
    }
    if (changed)
        setUboDeclaration();
    return changed;
}

void Renderer::applyUboLocks()
{
    if (!m_initialized)
        return;
    bool pushDirty = false;
    for (const PushFieldOwner& owner : m_pushFields)
        pushDirty |= owner.onRebake && owner.list->isDirty();
    if (!m_uboLocksDirty && !pushDirty)
        return;
    ProfileScope scope("UBO locks", EProfileCategory::Renderer);
    // The push lists first: a full reload below compiles every shader with their new defines.
    oc::small_vector<PushFieldOwner*, 4> rebaked;
    for (PushFieldOwner& owner : m_pushFields)
        if (owner.onRebake && owner.list->isDirty() && owner.list->update())
            rebaked.push_back(&owner);
    bool uboChanged = false;
    if (m_uboLocksDirty)
    {
        m_uboLocksDirty = false;
        if (m_uboResolveDirty)
        {
            m_uboResolveDirty = false;
            resolveUboLocks();
        }
        // The values as of now (the change landed before this frame's build).
        m_ubo.evaluate();
        uboChanged = bakeUboValues();
    }
    if (!uboChanged && rebaked.empty())
        return;
    // Rebuilding shaders reads the disk on main: an explicit stall, like F5.
    const FileSystem::AllowMainThreadIO allowIo;
    if (uboChanged)
    {
        reloadShaders();
        return;
    }
    (void)Globals::device.graphicsQueueWaitIdle();
    for (PushFieldOwner* owner : rebaked)
        owner->onRebake();
}
