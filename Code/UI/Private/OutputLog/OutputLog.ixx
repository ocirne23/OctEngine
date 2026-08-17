export module UI:OutputLog;

import Core;
import Core.Log;

export class OutputLog
{
public:
	// prepare() = the log SNAPSHOT (copy on revision change) + the level/text FILTER pass, no
	// ImGui: UI::prepare runs it on a job. render() draws the filtered list (prepares inline if
	// nothing ran ahead). The filter settings it reads are what render's widgets set last frame.
	void prepare();
	void render();

private:
	bool   m_prepared = false; // prepare() ran for this UI frame (render clears it)
	bool   m_showVerbose  = true;
	bool   m_showInfo     = true;
	bool   m_showWarning  = true;
	bool   m_showError    = true;
	bool   m_autoScroll   = true;
	char   m_filterBuf[256] = {};

	uint32                   m_cachedRevision = UINT32_MAX;
	oc::vector<Log::Message> m_snapshot;
	oc::vector<uint32>       m_visible; // indices into m_snapshot passing the filters
};
