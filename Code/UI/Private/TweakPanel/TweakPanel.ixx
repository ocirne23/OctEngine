export module UI:TweakPanel;

import Core;

import Core;
import Core.Tweaks;

// One tweak variable as its ImGui widget row (label + slider/checkbox/combo/...). Shared by the
// Tweaks editor panel and the main menu's Settings page. A changed var with an onChange is only
// COLLECTED into deferredCallbacks (main-thread work - see TweakPanel::flushDeferredCallbacks).
export void drawTweakVar(const TweakVar& var, int index, oc::vector<const TweakVar*>& deferredCallbacks);

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
};
