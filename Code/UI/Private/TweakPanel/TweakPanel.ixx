export module UI:TweakPanel;

import Core;

import Core;
import Settings.Tweaks;

// A lock click, COLLECTED like an onChange: var set = that row, else the whole section `lock`.
export struct TweakLockToggle
{
	uint32 lock;
	const TweakVar* var;
	bool locked;
};

// One tweak variable as its ImGui widget row (label + slider/checkbox/combo/...). Shared by the
// Tweaks editor panel and the main menu's Settings page. A changed var is only
// COLLECTED into deferredCallbacks (main-thread work - see TweakPanel::flushDeferredCallbacks).
// deferredLocks set: a row a lock covers gets its lock button in front (the panel; the Settings page
// passes none and only greys a locked row).
export void drawTweakVar(const TweakVar& var, int index, oc::vector<const TweakVar*>& deferredCallbacks,
	oc::vector<TweakLockToggle>* deferredLocks = nullptr);

export class TweakPanel
{
public:
	void render();

	// The widget pass runs OFF the main thread (see UI::update); onChange callbacks assume main
	// (renderer re-records, shader reloads with waitIdle, live box3d calls), so render() only
	// COLLECTS the changed vars and UI::flushMainThreadWork invokes them here, on main, after the
	// join. The value write itself already landed in render() - a callback one frame "late" only
	// re-reads the value it reacts to.
	void flushDeferredCallbacks();

	// The MainMenu's Settings page shares this list, so its onChange callbacks ride the same flush.
	oc::vector<const TweakVar*>& deferredCallbacks() { return m_deferredCallbacks; }

private:
	oc::vector<const TweakVar*> m_deferredCallbacks;
	oc::vector<TweakLockToggle> m_deferredLocks; // lock clicks: TweakRegistry::setLocked / setVarLocked on main
};
