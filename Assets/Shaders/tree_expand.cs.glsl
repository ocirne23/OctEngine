#version 460

// Procedural tree expansion (TreeExpandPipeline): one thread per placed piece writes its instance records into
// the range the CPU claimed for this frame - TREE_EXPAND_RECORDS_PER_PIECE (3) per piece: bark, leaves,
// billboard - plus its node's stamped pass mask and LOD state bias. The decision mirrors the CPU preview path
// (TreeSystem::update): the mesh before the crossfade band, the billboard after it, inside it the mesh on its
// fade-OUT materials AND the billboard (the lit FS's dither splits the pixels). A record the piece does not
// draw points at the dummy node, whose pass mask is 0: every cull skips it at its first read.
// THE BILLBOARD STANDS IN FOR THE PIECE IN EVERY PASS BUT MAIN: the mesh records sit on the piece's node, MAIN
// only; the billboard record on its own node (same transform), always written - PASS_ALL while it shows, else
// SHADOW | GI. So the shadow, the TLAS (GI, RT shadows) and the shadow cull see the cheap cards at every distance
// and never the mesh. A piece without a billboard (Far mode None) keeps its mesh in every pass.
// The layouts mirror TreeExpandPipeline.ixx - keep them in step.

#extension GL_EXT_buffer_reference : require

layout (local_size_x = 64) in;

struct Record
{
    uint meshMaterial;  // meshIdx | materialIdx << 16; ABSENT = none
    uint pipelineAlpha; // pipelineIdx | alphaMode << 16
};
struct PieceType
{
    Record bark;
    Record barkFade;
    Record leaves;
    Record leavesFade;
    Record billboard;
    float farDistance;
    float fadeWidth;
};
struct Piece
{
    vec3 centre;
    float radius;
    uint transformIdx;
    uint lodStateBase;
    uint typeIdx;
    uint billboardNode;
};
// RendererVKLayout::InMeshInstance (instanced_indirect.cs.glsl's view of it).
struct MeshInstance
{
    uint renderNodeIdx;
    uint instanceOffsetIdx;
    uint meshIdxMaterialIdx;
    uint pipelineIdxAlphaMode;
};

layout (buffer_reference, std430, buffer_reference_align = 16) readonly buffer PieceList { Piece p[]; };
layout (buffer_reference, std430, buffer_reference_align = 4) readonly buffer TypeList { PieceType t[]; };
layout (buffer_reference, std430, buffer_reference_align = 16) writeonly buffer InstanceList { MeshInstance i[]; };
layout (buffer_reference, std430, buffer_reference_align = 4) writeonly buffer UintList { uint v[]; };
layout (buffer_reference, std430, buffer_reference_align = 4) writeonly buffer IntList { int v[]; };

layout (push_constant) uniform Push
{
    PieceList pieces;
    TypeList types;
    InstanceList instances;
    UintList passMasks;
    IntList lodStateBias;
    uint numPieces;
    uint baseInstance;
    vec4 cameraPosScale; // xyz camera, w = the far distance scale
    uint forceFar;
    uint stampedAll;   // the stamped pass masks: PASS_ALL,
    uint stampedMain;  // MAIN (a mesh beside a billboard),
    uint stampedProxy; // SHADOW | GI (a billboard standing in for the mesh)
    uint dummyNode;
    uint identityOffset;
} pc;

const uint ABSENT = 0xFFFFFFFFu;

void emit(uint slot, Record r, uint node)
{
    MeshInstance m;
    if (r.meshMaterial == ABSENT)
    {
        m.renderNodeIdx = pc.dummyNode;
        m.instanceOffsetIdx = pc.identityOffset;
        m.meshIdxMaterialIdx = 0u;
        m.pipelineIdxAlphaMode = 0u;
    }
    else
    {
        m.renderNodeIdx = node;
        m.instanceOffsetIdx = pc.identityOffset;
        m.meshIdxMaterialIdx = r.meshMaterial;
        m.pipelineIdxAlphaMode = r.pipelineAlpha;
    }
    pc.instances.i[slot] = m;
}

void main()
{
    const uint idx = gl_GlobalInvocationID.x;
    if (idx == 0u)
        pc.passMasks.v[pc.dummyNode] = 0u; // per frame slot, so every frame
    if (idx >= pc.numPieces)
        return;

    const Piece piece = pc.pieces.p[idx];
    const PieceType type = pc.types.t[piece.typeIdx];
    const uint base = pc.baseInstance + idx * 3u;

    const bool hasBillboard = type.billboard.meshMaterial != ABSENT;
    bool mesh = true, fade = false, billboard = false;
    if (hasBillboard)
    {
        const float switchDistance = type.farDistance * pc.cameraPosScale.w;
        const float bandStart = max(switchDistance - type.fadeWidth * 0.5, 0.0);
        const float bandEnd = bandStart + type.fadeWidth;
        // A pixel's distance varies by up to the piece radius from the centre's (the material's band is per pixel).
        const float dist = distance(pc.cameraPosScale.xyz, piece.centre);
        if (pc.forceFar != 0u)
        {
            mesh = false;
            billboard = true;
        }
        else if (switchDistance <= 0.0 || dist + piece.radius < bandStart)
        {
        }
        else if (dist - piece.radius > bandEnd)
        {
            mesh = false;
            billboard = true;
        }
        else
        {
            fade = true;
            billboard = true;
        }
    }

    const Record none = Record(ABSENT, 0u);
    emit(base + 0u, mesh ? (fade ? type.barkFade : type.bark) : none, piece.transformIdx);
    emit(base + 1u, mesh ? (fade ? type.leavesFade : type.leaves) : none, piece.transformIdx);
    emit(base + 2u, hasBillboard ? type.billboard : none, piece.billboardNode);
    pc.passMasks.v[piece.transformIdx] = hasBillboard ? pc.stampedMain : pc.stampedAll;
    pc.passMasks.v[piece.billboardNode] = billboard ? pc.stampedAll : pc.stampedProxy;
    // The cull's LOD hysteresis slot = instance index + bias: record k -> lodStateBase + k (both nodes).
    pc.lodStateBias.v[piece.transformIdx] = int(piece.lodStateBase) - int(base);
    pc.lodStateBias.v[piece.billboardNode] = int(piece.lodStateBase) - int(base);
}
