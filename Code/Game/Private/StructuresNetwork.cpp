module Game;

import Core;
import Core.glm;
import Entity;
import :Structures;

// THE NETWORK REBUILD (see Structures.ixx, "CELL OCCUPANCY + DERIVED LINKS"): from the cell hash
// to the flat transport graph Transport.cpp ticks — union-find over the built segments, crossing
// conduction, building attachment and bridging, the run-contiguous node layout with CSR edges and
// the port slots — plus the cable arm visuals that follow the same adjacency.

void StructureSystem::rebuildNetworks()
{
    if (!m_linksDirty)
        return;
    m_linksDirty = false;
    joinTransport(); // the graph the job indexes is about to change
    ProfileScope scope("Structures network rebuild", EProfileCategory::Game);
    const auto capacityIn = [&](EStructureType t, int medium) {
        return medium == 1 ? fuelCapacityOf(t) : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t); };
    const auto cellOf = [](const glm::vec3& p) {
        return glm::ivec2((int)std::lround(p.x / GridCellSize - 0.5f),
                          (int)std::lround(p.z / GridCellSize - 0.5f)); };

    // 1) Collect the BUILT cable segments and union-find them over 4-neighbour adjacency of equal
    //    medium (blueprint segments are non-conductive: the path is broken until they finish).
    struct Seg { int frameIdx; glm::ivec2 cell; uint8 medium; };
    oc::vector<Seg> segs;
    oc::unordered_map<uint64, int> segAtCell; // the segment's OWN cell (under-cables included)
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        if (!isCableType(m_frame[i].type) || m_frame[i].state->blueprint)
            continue;
        const glm::ivec2 cell = cellOf(m_frame[i].entity->pos);
        segAtCell[cellKey(cell.x, cell.y)] = (int)segs.size();
        segs.push_back(Seg{ i, cell, (uint8)cableMediumOf(m_frame[i].type) });
    }
    oc::vector<int> parent(segs.size());
    for (int i = 0; i < (int)parent.size(); ++i)
        parent[i] = i;
    const auto find = [&](int i) { while (parent[i] != i) i = parent[i] = parent[parent[i]]; return i; };
    const auto unite = [&](int a, int b) { parent[find(a)] = find(b); };
    const auto segAt = [&](int cx, int cz) -> int {
        const auto it = segAtCell.find(cellKey(cx, cz));
        return it != segAtCell.end() ? it->second : -1; };
    for (int i = 0; i < (int)segs.size(); ++i)
        for (const glm::ivec2 d : { glm::ivec2(1, 0), glm::ivec2(0, 1) })
            if (const int n = segAt(segs[i].cell.x + d.x, segs[i].cell.y + d.y);
                n >= 0 && segs[n].medium == segs[i].medium)
                unite(i, n);

    // 2) CROSSINGS conduct THEIR OWN MEDIUM ONLY: each BUILT crossing resolves what sits just
    //    beyond its two END cells along its axis — a cable run, an already-conducting crossing, or
    //    a building — and ignores everything of another medium. Runs of its medium on both ends
    //    union through it; a run on one end and a building holding that medium on the other
    //    attaches the building. A fixpoint loop serves crossing chains.
    // Each END accepts connections from THREE sides: straight out along the axis plus the two
    // laterals (only the raised MIDDLE cell is pass-through-only). Candidate scan order is fixed
    // (outward, +lateral, -lateral) so every instance resolves identically.
    struct Cross { int frameIdx; glm::ivec2 end[2]; glm::ivec2 axis; uint8 medium; int seg = -1; }; // seg = a run member it joined
    oc::vector<Cross> crossings;
    for (int i = 0; i < (int)m_frame.size(); ++i)
    {
        if (!isCrossingType(m_frame[i].type) || m_frame[i].state->blueprint)
            continue;
        const glm::ivec2 center = cellOf(m_frame[i].entity->pos);
        const glm::ivec2 ext = footprintExtent(m_frame[i].type, m_frame[i].entity->rot);
        const glm::ivec2 axis = ext.x == 3 ? glm::ivec2(1, 0) : glm::ivec2(0, 1);
        crossings.push_back(Cross{ i, { center - axis, center + axis }, axis,
            (uint8)crossingMediumOf(m_frame[i].type) });
    }
    // The three cells an end connects through: outward continues the axis, the laterals let a
    // perpendicular cable enter at the side.
    const auto endCandidates = [](const Cross& c, int e, glm::ivec2 out[3])
    {
        const glm::ivec2 outward = e == 0 ? -c.axis : c.axis;
        const glm::ivec2 lateral(c.axis.y, c.axis.x);
        out[0] = c.end[e] + outward;
        out[1] = c.end[e] + lateral;
        out[2] = c.end[e] - lateral;
    };
    const auto crossAtOut = [&](const glm::ivec2& cell) -> int { // a conducting crossing whose END
        for (int c = 0; c < (int)crossings.size(); ++c)          // touches this candidate cell
        {
            if (crossings[c].seg < 0)
                continue;
            if (cell == crossings[c].end[0] || cell == crossings[c].end[1])
                return c;
        }
        return -1; };
    oc::vector<oc::pair<int, int>> crossAttach; // (seg slot, building frameIdx) via a crossing end
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Cross& c : crossings)
        {
            if (c.seg >= 0)
                continue;
            const int medium = c.medium;
            oc::fixed_vector<int, 3> endRuns[2]; // run members OF THE CROSSING'S MEDIUM at each end
            int building[2] = { -1, -1 };        // first building reachable at each end
            for (int e = 0; e < 2; ++e)
            {
                glm::ivec2 cand[3];
                endCandidates(c, e, cand);
                for (const glm::ivec2& cell : cand)
                {
                    if (const int seg = segAt(cell.x, cell.y); seg >= 0)
                    {
                        if (segs[seg].medium == medium)
                            endRuns[e].push_back(seg); // another medium's cable is just in the way
                    }
                    else if (const int cc = crossAtOut(cell); cc >= 0)
                    {
                        if (crossings[cc].medium == medium)
                            endRuns[e].push_back(crossings[cc].seg);
                    }
                    else if (const auto it = m_cells.find(cellKey(cell.x, cell.y));
                        it != m_cells.end() && building[e] < 0)
                    {
                        const int idx = structureIndexById(it->second.id);
                        if (idx >= 0 && !isCableOrCrossing(m_frame[idx].type))
                            building[e] = idx;
                    }
                }
            }
            // Runs on BOTH ends conduct; every run touching either end joins one union.
            int chosen = -1;
            if (!endRuns[0].empty() && !endRuns[1].empty())
                chosen = endRuns[0].front();
            // Fallback: run(s) on one side only + a building holding the medium on the other.
            if (chosen < 0)
            {
                const int e = endRuns[0].empty() ? 1 : 0;
                const int far = building[1 - e];
                if (!endRuns[e].empty() && far >= 0 && capacityIn(m_frame[far].type, medium) > 0.0f)
                {
                    chosen = endRuns[e].front();
                    crossAttach.push_back({ chosen, far });
                }
            }
            if (chosen >= 0)
            {
                for (int e = 0; e < 2; ++e)
                    for (const int r : endRuns[e])
                        unite(r, chosen);
                c.seg = chosen;
                changed = true;
            }
        }
    }

    // 3) Building attachment: every run cell's 4-neighbours that hold a building with capacity in
    //    the run's medium (blueprint buildings pre-wire — the flow already gates on their flag).
    //    Collected as raw (seg slot, building) pairs first: the bridge step below still unions.
    oc::vector<oc::pair<int, int>> attachPairs; // (seg slot, building frame index)
    for (int i = 0; i < (int)segs.size(); ++i)
        for (const glm::ivec2 d : { glm::ivec2(1, 0), glm::ivec2(-1, 0), glm::ivec2(0, 1), glm::ivec2(0, -1) })
        {
            const auto it = m_cells.find(cellKey(segs[i].cell.x + d.x, segs[i].cell.y + d.y));
            if (it == m_cells.end())
                continue;
            const int idx = structureIndexById(it->second.id);
            if (idx >= 0 && !isCableOrCrossing(m_frame[idx].type)
                && capacityIn(m_frame[idx].type, segs[i].medium) > 0.0f)
                attachPairs.push_back({ i, idx });
        }
    for (const auto& [segSlot, buildingIdx] : crossAttach)
        attachPairs.push_back({ segSlot, buildingIdx });

    // 3b) BUILDINGS BRIDGE: a BUILT building conducts every medium it holds — two same-medium runs
    //     touching it merge into one (a power line with an emitter cut into the middle carries
    //     through), exactly like a cable cell would. Buildings still never connect DIRECTLY to
    //     each other (adjacency alone derives nothing — a link always needs cable in between), and
    //     a BLUEPRINT building does not bridge (consistent with blueprint cables breaking the
    //     path), though it still attaches to each run for the pre-wire.
    {
        oc::unordered_map<uint64, int> firstSlot; // (building << 2 | medium) -> first seg slot seen
        for (const auto& [segSlot, buildingIdx] : attachPairs)
        {
            if (m_frame[buildingIdx].state->blueprint)
                continue;
            const uint64 key = (uint64)buildingIdx << 2 | segs[segSlot].medium;
            const auto [it, inserted] = firstSlot.insert({ key, segSlot });
            if (!inserted)
                unite(segSlot, it->second);
        }
    }

    // 3c) Bucket the attachments by the FINAL union roots (dedup — several pairs can name the same
    //     building through different segments).
    oc::unordered_map<int, oc::vector<int>> runBuildings; // union root -> building frame indices
    for (const auto& [segSlot, buildingIdx] : attachPairs)
    {
        oc::vector<int>& list = runBuildings[find(segSlot)];
        bool known = false;
        for (const int existing : list)
            known |= existing == buildingIdx;
        if (!known)
            list.push_back(buildingIdx);
    }

    // 4) THE TRANSPORT GRAPH. Nodes, run-contiguous: every built segment, every conducting
    //    crossing, and one JUNCTION per (built building, medium) that bridges — the building's
    //    port. Edges: segment 4-adjacency, crossing <-> the run members at its ends, junction <->
    //    every segment/crossing touching the building. Slots (the ports) hang on the junctions;
    //    a BLUEPRINT building gets no node and no slot (it flows nothing) but is still stamped
    //    `attachedMask` for the "no cable" badge. Old fills carry over by structure id.
    for (const TransportNode& n : m_net.nodes)
        if (!n.junction && n.fill != 0)
            m_savedFills[n.structureId] = n.fill;
    m_net.nodes.clear();
    m_net.adj.clear();
    m_net.slots.clear();
    m_net.runs.clear();
    m_net.nodeById.clear();
    m_net.dueRuns.clear();
    for (const Ref& s : m_frame)
        s.state->attachedMask = 0;
    for (const auto& [segSlot, buildingIdx] : attachPairs)
        m_frame[buildingIdx].state->attachedMask |= uint8(1u << segs[segSlot].medium);

    // Node ids per union root, in a stable order: segments, crossings, then junctions. A junction
    // exists per (building, medium) root; `junctionOf` finds it for the edge pass.
    oc::unordered_map<int, uint32> runOfRoot;        // union root -> run index
    oc::vector<int> rootOfSeg(segs.size());
    for (int i = 0; i < (int)segs.size(); ++i)
        rootOfSeg[i] = find(i);
    oc::vector<oc::vector<int>> runSegs;             // per run: seg slots
    oc::vector<oc::vector<int>> runCross;            // per run: crossing slots
    oc::vector<oc::vector<int>> runJunctions;        // per run: building frame indices (built)
    for (int i = 0; i < (int)segs.size(); ++i)
    {
        auto [it, inserted] = runOfRoot.insert({ rootOfSeg[i], (uint32)m_net.runs.size() });
        if (inserted)
        {
            TransportRun run;
            run.medium = segs[i].medium;
            run.group = (uint8)(m_net.runs.size() % (size_t)glm::clamp(m_transportSpread, 1, 8));
            m_net.runs.push_back(run);
            runSegs.emplace_back();
            runCross.emplace_back();
            runJunctions.emplace_back();
        }
        runSegs[it->second].push_back(i);
    }
    for (int c = 0; c < (int)crossings.size(); ++c)
        if (crossings[c].seg >= 0)
            runCross[runOfRoot[rootOfSeg[crossings[c].seg]]].push_back(c);
    for (const auto& [root, buildings] : runBuildings)
        for (const int b : buildings)
            if (!m_frame[b].state->blueprint)
                runJunctions[runOfRoot[root]].push_back(b);

    oc::vector<uint32> nodeOfSeg(segs.size(), UINT32_MAX);
    oc::vector<uint32> nodeOfCross(crossings.size(), UINT32_MAX);
    oc::unordered_map<uint64, uint32> junctionOf;    // (building << 2 | medium) -> node
    for (uint32 r = 0; r < (uint32)m_net.runs.size(); ++r)
    {
        TransportRun& run = m_net.runs[r];
        run.firstNode = (uint32)m_net.nodes.size();
        const auto pushNode = [&](uint32 structureId, bool junction) {
            TransportNode n;
            n.structureId = structureId;
            n.run = (uint16)r;
            n.medium = run.medium;
            n.junction = junction ? 1 : 0;
            if (!junction)
                if (const auto sf = m_savedFills.find(structureId); sf != m_savedFills.end())
                {
                    n.fill = sf->second;
                    m_savedFills.erase(sf);
                }
            m_net.nodes.push_back(n);
            return (uint32)m_net.nodes.size() - 1; };
        for (const int i : runSegs[r])
            nodeOfSeg[i] = pushNode(m_frame[segs[i].frameIdx].state->structureId, false);
        for (const int c : runCross[r])
            nodeOfCross[c] = pushNode(m_frame[crossings[c].frameIdx].state->structureId, false);
        for (const int b : runJunctions[r])
            junctionOf[(uint64)b << 2 | run.medium] = pushNode(m_frame[b].state->structureId, true);
        run.numNode = (uint32)m_net.nodes.size() - run.firstNode;
    }
    for (uint32 n = 0; n < (uint32)m_net.nodes.size(); ++n)
        if (!m_net.nodes[n].junction)
            m_net.nodeById[m_net.nodes[n].structureId] = n;

    // Edges, collected per node then laid out CSR with reverse indices.
    oc::vector<oc::vector<uint32>> edges(m_net.nodes.size());
    const auto connect = [&](uint32 a, uint32 b) {
        if (a == b || a == UINT32_MAX || b == UINT32_MAX)
            return;
        for (const uint32 e : edges[a])
            if (e == b)
                return;
        edges[a].push_back(b);
        edges[b].push_back(a); };
    for (int i = 0; i < (int)segs.size(); ++i)
        for (const glm::ivec2 d : { glm::ivec2(1, 0), glm::ivec2(0, 1) })
            if (const int n = segAt(segs[i].cell.x + d.x, segs[i].cell.y + d.y);
                n >= 0 && segs[n].medium == segs[i].medium)
                connect(nodeOfSeg[i], nodeOfSeg[n]);
    for (int c = 0; c < (int)crossings.size(); ++c)
    {
        const Cross& cr = crossings[c];
        if (cr.seg < 0)
            continue;
        for (int e = 0; e < 2; ++e) // the same three cells per end the union step resolved
        {
            glm::ivec2 cand[3];
            endCandidates(cr, e, cand);
            for (const glm::ivec2& cell : cand)
            {
                if (const int seg = segAt(cell.x, cell.y); seg >= 0)
                {
                    if (segs[seg].medium == cr.medium)
                        connect(nodeOfCross[c], nodeOfSeg[seg]);
                }
                else if (const int cc = crossAtOut(cell); cc >= 0 && crossings[cc].medium == cr.medium)
                    connect(nodeOfCross[c], nodeOfCross[cc]);
            }
        }
    }
    // Junction edges: every attachment pair (segment/crossing end, building) of a BUILT building —
    // AND every pair of conductors touching the same building, so a line THROUGH a building
    // bridges cable-to-cable. The junction itself is a pure port: it accepts cells only while a
    // slot on it wants some, so nothing ever parks in (or relays through) a producer's port.
    oc::unordered_map<uint64, oc::vector<uint32>> touching; // (building << 2 | medium) -> conductor nodes
    for (const auto& [segSlot, buildingIdx] : attachPairs)
    {
        if (m_frame[buildingIdx].state->blueprint)
            continue;
        const uint64 key = (uint64)buildingIdx << 2 | segs[segSlot].medium;
        if (const auto it = junctionOf.find(key); it != junctionOf.end())
            connect(it->second, nodeOfSeg[segSlot]);
        touching[key].push_back(nodeOfSeg[segSlot]);
    }
    for (const auto& [segSlot, buildingIdx] : crossAttach)
    {
        if (m_frame[buildingIdx].state->blueprint)
            continue;
        // The crossing that attached this building: the one whose union member is segSlot and
        // whose end touches the building — scan is fine, crossings are few.
        const uint64 key = (uint64)buildingIdx << 2 | segs[segSlot].medium;
        const auto it = junctionOf.find(key);
        for (int c = 0; c < (int)crossings.size(); ++c)
            if (crossings[c].seg == segSlot)
            {
                if (it != junctionOf.end())
                    connect(it->second, nodeOfCross[c]);
                touching[key].push_back(nodeOfCross[c]);
            }
    }
    for (const auto& [key, nodes] : touching)
        for (size_t a = 0; a < nodes.size(); ++a)
            for (size_t b = a + 1; b < nodes.size(); ++b)
                connect(nodes[a], nodes[b]);
    for (uint32 n = 0; n < (uint32)m_net.nodes.size(); ++n)
    {
        TransportNode& node = m_net.nodes[n];
        node.adjFirst = (uint32)m_net.adj.size();
        node.adjCount = (uint32)edges[n].size();
        for (const uint32 e : edges[n])
            m_net.adj.push_back({ e, UINT32_MAX });
        // Out-rate per sub-step: the medium's cells/s over tick x substeps; a junction relays
        // every direction, so it scales with its degree (the cable stays the bottleneck).
        const float perSub = m_cableThroughput[glm::min((int)node.medium, 2)]
            / (glm::max(m_transportTickHz, 1.0f) * (float)glm::max(m_transportSubsteps, 1));
        node.rateFp = (uint32)glm::max(perSub * 1024.0f * (node.junction ? (float)glm::max(node.adjCount, 1u) : 1.0f), 1.0f);
    }
    for (uint32 n = 0; n < (uint32)m_net.nodes.size(); ++n)
    {
        const TransportNode& node = m_net.nodes[n];
        for (uint32 a = 0; a < node.adjCount; ++a)
        {
            TransportAdj& adj = m_net.adj[node.adjFirst + a];
            const TransportNode& other = m_net.nodes[adj.node];
            for (uint32 b = 0; b < other.adjCount; ++b)
                if (m_net.adj[other.adjFirst + b].node == n)
                {
                    adj.reverse = other.adjFirst + b;
                    break;
                }
        }
    }
    // Slots: one per (built building, medium) junction, in node order so a node's slots are
    // contiguous.
    for (uint32 n = 0; n < (uint32)m_net.nodes.size(); ++n)
    {
        TransportNode& node = m_net.nodes[n];
        if (!node.junction)
            continue;
        node.slotFirst = (uint32)m_net.slots.size();
        const int idx = structureIndexById(node.structureId);
        if (idx >= 0)
            addTransportSlot(idx, node.medium, n);
        node.slotCount = (uint32)m_net.slots.size() - node.slotFirst;
    }
    m_net.outAdj.assign(m_net.adj.size(), 0);
    m_net.outSlot.assign(m_net.slots.size(), 0);
    m_net.inSlot.assign(m_net.slots.size(), 0);
    m_net.bfsQueue.assign(m_net.nodes.size(), 0);
    m_statTransportNodes = (int)m_net.nodes.size();

    for (const Ref& s : m_frame)
        updateArms(s);
}

void StructureSystem::updateArms(const Ref& s)
{
    if (!isCableType(s.type))
        return;
    const int medium = cableMediumOf(s.type);
    const int cx = (int)std::lround(s.entity->pos.x / GridCellSize - 0.5f);
    const int cz = (int)std::lround(s.entity->pos.z / GridCellSize - 0.5f);
    static constexpr glm::ivec2 dirs[4] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }; // PX NX PZ NZ
    for (int a = 0; a < 4; ++a)
    {
        if (!s.arms[a])
            continue;
        // An arm shows toward anything the segment VISUALLY joins: a same-medium cable (blueprint
        // included — adjacency, not conduction), a same-medium crossing, or a building holding
        // the medium.
        bool on = false;
        const int nx = cx + dirs[a].x, nz = cz + dirs[a].y;
        if (const int seg = cableSegmentAt(nx, nz, /*builtOnly*/ false);
            seg >= 0 && cableMediumOf(m_frame[seg].type) == medium)
            on = true;
        else if (const auto it = m_cells.find(cellKey(nx, nz)); it != m_cells.end())
        {
            // A same-medium crossing — on top, or an end bridged under another crossing's middle.
            for (const uint32 id : { it->second.id, it->second.underId })
                if (const int idx = id != 0 ? structureIndexById(id) : -1;
                    idx >= 0 && crossingMediumOf(m_frame[idx].type) == medium)
                    on = true;
            const int idx = structureIndexById(it->second.id);
            if (!on && idx >= 0 && !isCableOrCrossing(m_frame[idx].type))
            {
                const EStructureType t = m_frame[idx].type;
                const float cap = medium == 1 ? fuelCapacityOf(t)
                                : medium == 2 ? mineralCapacityOf(t) : energyCapacityOf(t);
                on = cap > 0.0f;
            }
        }
        s.arms[a]->setEnabled(on);
    }
}
