export module UI:PropertiesPanel;

import Core;
import Entity;

export class PropertiesPanel
{
public:
	void render(Entity* selected);
	oc::vector<EntityChange> takeChanges() { return oc::move(m_changes); }

private:
	oc::vector<EntityChange> m_changes; // enable-toggles: main-thread work, drained via UI::takeEntityChanges

	// The script's exposed ScriptData fields, edited LIVE (never serialized -- the .pre holds the initial value,
	// authored in the Entity Editor). Reads the layout from the loaded module's field table, so it shows nothing
	// until the script has compiled.
	void drawScriptDataFields(ScriptComponent& script);
};
