export module UI:PropertiesPanel;

import Core;
import Entity;

export class PropertiesPanel
{
public:
	void render(Entity* selected);
	// Appends into the frame's merged drain (UI::takeEntityChanges); clear() keeps this panel's capacity.
	void takeChanges(oc::vector<EntityChange>& out)
	{
		out.insert(out.end(), oc::make_move_iterator(m_changes.begin()), oc::make_move_iterator(m_changes.end()));
		m_changes.clear();
	}

private:
	oc::vector<EntityChange> m_changes; // enable-toggles: main-thread work, drained via UI::takeEntityChanges

	// The script's exposed ScriptData fields, edited LIVE (never serialized -- the .pre holds the initial value,
	// authored in the Entity Editor). Reads the layout from the loaded module's field table, so it shows nothing
	// until the script has compiled.
	void drawScriptDataFields(ScriptComponent& script);
};
