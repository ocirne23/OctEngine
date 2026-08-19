export module UI:TweakPanel;

import Core;

import Core;
import Core.Tweaks;

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

private:
	oc::vector<const TweakVar*> m_deferredCallbacks;
};
