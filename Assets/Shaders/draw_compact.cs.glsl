#version 460

// Draw-list compaction, right after a cull (DrawCompactPipeline). The culls accumulate into PER-MESH-SLOT
// sequences (one per registered mesh, LOD levels included), so the executes walked every slot - ~20k draws
// per list, nearly all with instanceCount 0, each still a front-end draw. This pass moves the non-empty
// sequences to the front of the SAME buffer, in slot order, and writes each list's count for the execute
// (DGC sequenceCountAddress / drawIndexedIndirectCount countBuffer).
// ONE workgroup walks the slots chunk by chunk: the order stays the slot order every frame (the transparent
// list is not depth sorted - a per-frame order change would flicker its overlaps). In place is safe: a kept
// sequence moves to an index <= its own slot, the chunk's reads finish before its writes (barrier), and the
// later chunks are never written. Everything past a count is stale; the cull's fill clears it next frame.

#extension GL_EXT_buffer_reference : require
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require

#ifndef NUM_LISTS
#define NUM_LISTS 1
#endif
#define GROUP_SIZE 1024
#define MAX_SUBGROUPS (GROUP_SIZE / 8)

layout (local_size_x = GROUP_SIZE) in;

// RendererVKLayout::IndirectDrawSequence: the execution-set pipelineIndex + a VkDrawIndexedIndirectCommand.
struct Sequence
{
    uint pipelineIndex;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int  vertexOffset;
    uint firstInstance;
};
layout (buffer_reference, std430, buffer_reference_align = 4) buffer SequenceList { Sequence seq[]; };
layout (buffer_reference, std430, buffer_reference_align = 4) buffer UintList { uint v[]; };

layout (push_constant) uniform Push
{
    UintList slotCount;  // [0] = the registered mesh count (the slots the cull may have written)
    UintList drawCounts; // out: [k] = list k's sequence count
    SequenceList lists[NUM_LISTS];
} pc;

shared uint s_subgroupKept[NUM_LISTS][MAX_SUBGROUPS];
shared uint s_base[NUM_LISTS];

void main()
{
    const uint t = gl_LocalInvocationIndex;
    const uint numSlots = pc.slotCount.v[0];
    if (t < NUM_LISTS)
        s_base[t] = 0u;
    barrier();

    for (uint chunk = 0u; chunk < numSlots; chunk += GROUP_SIZE)
    {
        const uint slot = chunk + t;
        bool keep[NUM_LISTS];
        uint rank[NUM_LISTS];
        for (uint k = 0u; k < NUM_LISTS; ++k)
        {
            // The two words that decide it first; the whole sequence only for the few that are kept.
            keep[k] = slot < numSlots && pc.lists[k].seq[slot].instanceCount != 0u && pc.lists[k].seq[slot].indexCount != 0u;
            const uvec4 ballot = subgroupBallot(keep[k]);
            rank[k] = subgroupBallotExclusiveBitCount(ballot);
            if (subgroupElect())
                s_subgroupKept[k][gl_SubgroupID] = subgroupBallotBitCount(ballot);
        }
        Sequence kept[NUM_LISTS];
        for (uint k = 0u; k < NUM_LISTS; ++k)
            if (keep[k])
                kept[k] = pc.lists[k].seq[slot];
        barrier(); // every read of this chunk is done, the subgroup counts are visible

        for (uint k = 0u; k < NUM_LISTS; ++k)
        {
            if (!keep[k])
                continue;
            uint dst = s_base[k] + rank[k];
            for (uint s = 0u; s < gl_SubgroupID; ++s)
                dst += s_subgroupKept[k][s];
            pc.lists[k].seq[dst] = kept[k];
        }
        barrier(); // s_base read by everyone before it advances

        if (t < NUM_LISTS)
        {
            uint total = 0u;
            for (uint s = 0u; s < gl_NumSubgroups; ++s)
                total += s_subgroupKept[t][s];
            s_base[t] += total;
        }
        barrier();
    }

    if (t < NUM_LISTS)
        pc.drawCounts.v[t] = s_base[t];
}
