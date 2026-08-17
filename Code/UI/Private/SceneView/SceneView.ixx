export module UI:SceneView;

import Core;
import Core.Transform;
import Entity;

export class SceneView
{
public:

	void render(const oc::vector<EntityPtr>& rootEntities);

	Entity* getSelected() const          { return m_selected.get(); }
	void    setSelected(Entity* entity)  { m_selected = EntityPtr(entity); }

	oc::vector<EntityChange> takeChanges() { return oc::move(m_changes); }

private:

	void renderToolbar();
	void renderEntityNode(Entity* entity, bool ancestorLocked);  // scene entity (recurses children) or loose leaf
	void renderContextMenu(Entity* entity, bool locked);
	void beginRename(Entity* entity);

	bool dragSourceFor(Entity* entity);     // returns true while this entity is being dragged
	void dropTargetReparentUnder(Entity* parent); // accepts a dragged scene entity, reparents under `parent`
	void acceptAssetSpawnPayload(Entity* parent); // queues an "ASSET_FILE" drop to spawn under `parent`

	void applyPendingMutations();
	void pruneSelection(const oc::vector<EntityPtr>& rootEntities); // drops a selection deleted from outside the panel

	oc::vector<EntityChange> m_changes;                            // panel mutations queued for the app to drain

	// OWNING: the selection outlives whatever deleted the entity (a script, a debug key, another panel),
	// so nothing downstream - the gizmo above all - can be left holding freed memory. pruneSelection
	// then drops it on the next render once the entity is no longer in the world.
	EntityPtr m_selected;
	Entity* m_renamingEntity = nullptr;
	bool    m_panelFocused   = false;  // Scene panel (or a child of it) had keyboard focus this frame — gates F2/Delete
	bool    m_focusRenameNext = false;
	bool    m_worldOpen       = true;
	char    m_renameBuffer[256] = {};
	char    m_searchBuffer[256] = {};

	Entity* m_pendingDelete         = nullptr;
	bool    m_hasPendingReparent    = false;
	Entity* m_pendingReparentChild  = nullptr;
	Entity* m_pendingReparentTarget = nullptr;  // nullptr â†’ World root
};
