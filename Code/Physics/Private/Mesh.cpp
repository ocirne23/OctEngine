module;

#include <box3d/box3d.h>

module Physics;

import :PhysicsTypes;

void PhysicsMesh::destroy()
{
    if (m_data == 0)
        return;
    b3DestroyMesh(reinterpret_cast<b3MeshData*>(m_data));
    m_data = 0;
}
