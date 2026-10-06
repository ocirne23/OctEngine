module RendererVK;

import Core;
import Settings.Tweaks;
import Core.Log;

import File;

import :Layout;
import :UboFields;

// Renderer: the TWEAK LOCKS. Every lockable UBO value (an entry of m_uboFields, registerUboFields at the end of
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

void Renderer::registerUboLocks()
{
    const auto onLockClick = [this]() { m_uboResolveDirty = true; m_uboLocksDirty = true; };
    for (uint32 s = 0; s < RendererVKLayout::NUM_UBO_LOCK_SECTIONS; ++s)
        m_uboLocks[s] = Tweak::lock(RendererVKLayout::c_uboLockSections[s].name, RendererVKLayout::c_uboLockSections[s].categories,
            /*lockedByDefault*/ true, onLockClick);
    TweakRegistry::get().addChangeListener(this, [this](const TweakVar& var)
    {
        if (TweakRegistry::get().isVarBakeable(var))
            m_uboLocksDirty = true;
    });

    registerUboFields(m_uboFields);
    assert(RendererVKLayout::UBO_FIELDS_OFFSET + m_uboFields.size() <= RendererVKLayout::UBO_RANGE && "the lockable UBO values outgrew UBO_RANGE");
    const size_t count = m_uboFields.entries().size();
    m_uboLocked.assign(count, 0);
    m_uboBaked.assign(count, 0);
    m_uboFieldValues.assign(m_uboFields.size(), 0);
    m_uboBakedValues.assign(m_uboFields.size(), 0);

    // THE startup bake: the declaration the pipelines are first compiled with (initPipelines). The live entries are
    // skipped - what they read may not exist yet; they are never baked anyway.
    m_uboFields.evaluate(m_uboFieldValues.data(), /*includeLive*/ false);
    resolveUboLocks();
    bakeUboValues();
    m_uboResolveDirty = false;
    m_uboLocksDirty = false;
}

void Renderer::setUboDeclaration()
{
    RendererVKLayout::g_uboDeclaration = RendererVKLayout::buildUboDeclaration(m_uboFields, m_uboBakedValues.data(), m_uboBaked);
    // A copy for reading (the includer serves the string itself).
    FileSystem::createDirectories("Local/Shaders", true);
    FileSystem::writeFileStr("Local/Shaders/ubo.generated.glsl", RendererVKLayout::g_uboDeclaration, true);
}

// Each entry's lock from its sources: locked while every source row is locked (or no lock covers it).
void Renderer::resolveUboLocks()
{
    ProfileScope scope("UBO lock resolve", EProfileCategory::Renderer);
    const TweakRegistry& tweaks = TweakRegistry::get();
    const oc::vector<UboFieldList::Entry>& entries = m_uboFields.entries();
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

// The consts from m_uboFieldValues: an entry is one while it is locked and its value is finite (a const cannot spell
// a NaN). Returns whether any const appeared, went or changed its value.
bool Renderer::bakeUboValues()
{
    const oc::vector<UboFieldList::Entry>& entries = m_uboFields.entries();
    bool changed = false;
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const UboFieldList::Entry& e = entries[i];
        const uint8* value = m_uboFieldValues.data() + e.offset;
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
    if (!m_initialized || !m_uboLocksDirty)
        return;
    ProfileScope scope("UBO locks", EProfileCategory::Renderer);
    m_uboLocksDirty = false;
    if (m_uboResolveDirty)
    {
        m_uboResolveDirty = false;
        resolveUboLocks();
    }
    // The values as of now (the change landed before this frame's build).
    m_uboFields.evaluate(m_uboFieldValues.data());
    // Rebuilding every shader reads the disk on main: an explicit stall, like F5.
    const FileSystem::AllowMainThreadIO allowIo;
    if (bakeUboValues())
        reloadShaders();
}
