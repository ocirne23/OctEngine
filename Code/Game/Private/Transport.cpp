module Game;

import Core;
import Core.glm;
import Entity;
import Threading;
import :Structures;

// THE CABLE TRANSPORT — see the block in Structures.ixx. This file is the tick: the boundary
// with the buildings' float stores (inject / apply, main thread) and the job (per run, in
// parallel across runs: the two BFS fields, then the offer / apply stencil sub-steps over the
// flat node graph — owner-only writes, no atomics).

// The port role of a building type in a medium: PRODUCERS push whole cells out of their store,
// CONSUMERS pull into their headroom, STORAGE does either by the fill of the cable next to its
// port (push while at/below the low mark, pull while at/above the high mark).
StructureSystem::ETransportRole StructureSystem::transportRoleOf(EStructureType t, int medium)
{
    using R = ETransportRole;
    switch (medium)
    {
    case 0: // energy
        if (t == EStructureType::Generator || t == EStructureType::Solar) return R::Producer;
        if (t == EStructureType::Battery || t == EStructureType::Base) return R::Storage; // the Base banks: its
        return R::Consumer;                                                                 // shield costs no draw
    case 1: // fuel
        if (t == EStructureType::Extractor) return R::Producer;
        if (t == EStructureType::FuelTank) return R::Storage;
        return R::Consumer;
    default: // minerals
        if (t == EStructureType::Extractor || t == EStructureType::Fabricator) return R::Producer;
        if (t == EStructureType::Base || t == EStructureType::MineralSilo) return R::Storage;
        return R::Consumer;
    }
}

void StructureSystem::addTransportSlot(int buildingIdx, int medium, uint32 node)
{
    const Ref& ref = m_frame[buildingIdx];
    TransportSlot slot;
    slot.state = ref.state;
    slot.node = node;
    slot.medium = (uint8)medium;
    slot.role = transportRoleOf(ref.type, medium);
    // METERED machines: the barracks' build rate and the turret's fire cadence are their cable
    // INTAKE — the store fills at that rate however many segments feed the junction.
    if (medium == 0 && isBarracksType(ref.type))
        slot.intakePerSec = m_barracksEnergyIntake;
    else if (medium == 0 && ref.type == EStructureType::Turret)
        slot.intakePerSec = GameStructureComponent::params.turretShotEnergy
            / glm::max(GameStructureComponent::params.turretFireInterval, 1e-3f);
    m_net.slots.push_back(slot);
}

// ---------------------------------------------------------------- the boundary (main thread)

void StructureSystem::transportInject(const TransportRun& run)
{
    const float period = transportTickPeriod();
    const float cellsPerSeg = (float)cellsPerSegmentOf(run.medium);
    const int substeps = glm::max(m_transportSubsteps, 1);
    for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
    {
        const TransportNode& node = m_net.nodes[n];
        if (!node.junction)
            continue;
        // The most this port can move in one tick: its node's out-rate over the tick (+1 so a
        // fractional rate still moves) — it also caps what a storage reserves out of its bank.
        const int maxPerTick = (int)((uint64)node.rateFp * (uint64)substeps / 1024u) + 1;
        for (uint32 s = node.slotFirst; s < node.slotFirst + node.slotCount; ++s)
        {
            TransportSlot& slot = m_net.slots[s];
            slot.supply = slot.demand = slot.taken = slot.given = slot.reserved = 0;
            GameStructureComponent* c = slot.state;
            if (!c || c->blueprint)
                continue;
            const int m = slot.medium;
            const float cap = c->capacity[m];
            if (cap <= 0.0f)
                continue;
            // A STORAGE offers at most what its port can take right now (its free space): a
            // reserved supply the network cannot move must never leave the store and bounce back.
            // A PRODUCER reserves nothing, so it offers up to its per-tick cap and the port takes
            // the cells over the sub-steps as space frees — at "Cells per segment" 1 a port would
            // otherwise cap every producer at one cell per tick.
            const int portSpace = glm::max((int)cellsPerSeg - (int)node.fill, 0);
            const auto push = [&](bool reserve) {
                int cells = glm::min((int)std::floor(glm::max(c->store[m], 0.0f)), maxPerTick);
                if (reserve)
                    cells = glm::min(cells, portSpace);
                if (cells <= 0)
                    return;
                slot.supply = cells;
                // A PRODUCER's store in this medium is drained by the transport alone, so the
                // taken cells simply come off at the join — no reservation, no visible dip.
                // STORAGE (the Base: siege load drains it meanwhile) reserves, refunded at the join.
                if (reserve)
                {
                    c->store[m] -= (float)cells;
                    slot.reserved = cells;
                } };
            const auto pull = [&](bool metered) {
                int cells = (int)std::floor(glm::max(cap - c->store[m], 0.0f)); // whole cells: capacities are whole numbers
                if (metered && slot.intakePerSec > 0.0f)
                {
                    // The meter accrues only while there is headroom (a full store banks no
                    // intake) and never past a whole cell or one tick's worth, whichever is more —
                    // a sub-cell rate (2/s at 10 Hz = 0.2 a tick) must be allowed to reach 1.
                    const float perTick = slot.intakePerSec * period;
                    if (cells > 0)
                        slot.intakeCarry = glm::min(slot.intakeCarry + perTick, glm::max(1.0f, perTick));
                    const int allowed = (int)std::floor(slot.intakeCarry);
                    slot.intakeCarry -= (float)allowed;
                    cells = glm::min(cells, allowed);
                }
                else
                    cells = glm::min(cells, maxPerTick);
                slot.demand = glm::max(cells, 0); };
            switch (slot.role)
            {
            case ETransportRole::Producer: push(false); break;
            case ETransportRole::Consumer: pull(true); break;
            case ETransportRole::Storage:
            {
                // The price signal is the CABLE next to the port — the mean fill of the junction's
                // neighbours — never the junction itself: a push fills the own junction to the
                // high mark, which then read as "pull it back" (the Base oscillated on exactly
                // that). TRUE HYSTERESIS: the port keeps its mode until the OPPOSITE mark is
                // crossed — pulling until the cable runs down to the low mark, pushing until it
                // fills to the high mark. (A per-tick band with a hold zone between the marks
                // idled a battery every other tick on 2-cell segments: one pulled cell dropped
                // the mean into the zone.)
                float fill = 0.0f;
                if (node.adjCount > 0)
                {
                    for (uint32 a = 0; a < node.adjCount; ++a)
                        fill += (float)m_net.nodes[m_net.adj[node.adjFirst + a].node].fill;
                    fill /= (float)node.adjCount * cellsPerSeg;
                }
                if (fill >= m_storageHighMark)
                    slot.storageMode = 1;
                else if (fill <= m_storageLowMark)
                    slot.storageMode = -1;
                if (slot.storageMode > 0)
                    pull(false);
                else if (slot.storageMode < 0)
                    push(true);
                break;
            }
            }
        }
    }
}

void StructureSystem::transportApplyBoundary()
{
    ProfileScope scope("Transport apply", EProfileCategory::Game);
    for (const uint32 r : m_net.dueRuns)
    {
        const TransportRun& run = m_net.runs[r];
        for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
        {
            const TransportNode& node = m_net.nodes[n];
            for (uint32 s = node.slotFirst; s < node.slotFirst + node.slotCount; ++s)
            {
                TransportSlot& slot = m_net.slots[s];
                GameStructureComponent* c = slot.state;
                if (!c)
                    continue;
                const int m = slot.medium;
                // Refund the reserved cells the network did not take, add the delivered ones; a
                // storage that self-generates (the Base) can overshoot by a trickle — clamp.
                c->store[m] += (float)(slot.reserved - slot.taken) + (float)slot.given;
                c->store[m] = glm::clamp(c->store[m], 0.0f, glm::max(c->capacity[m], 0.0f));
                // The gauge: the served fraction of what this port asked for this tick (a port
                // that asked nothing reads as served).
                const int asked = slot.role == ETransportRole::Producer || (slot.role == ETransportRole::Storage && slot.reserved > 0)
                    ? slot.taken + slot.supply : slot.given + slot.demand;
                const int served = slot.role == ETransportRole::Producer || (slot.role == ETransportRole::Storage && slot.reserved > 0)
                    ? slot.taken : slot.given;
                const float util = asked > 0 ? (float)served / (float)asked : 1.0f;
                c->flowUtil += (util - c->flowUtil) * 0.5f;
                slot.reserved = slot.supply = slot.demand = 0;
            }
        }
    }
    m_net.dueRuns.clear();
}

void StructureSystem::joinTransport()
{
    if (!m_transportKicked)
        return;
    {
        ProfileScope scope("Transport join", EProfileCategory::Wait);
        Globals::jobSystem.wait(m_transportCounter);
    }
    m_transportKicked = false;
    transportApplyBoundary();
}

void StructureSystem::kickTransport()
{
    if (m_transportKicked || m_net.runs.empty())
        return;
    const float period = transportTickPeriod();
    const int spread = glm::clamp(m_transportSpread, 1, 8);
    // Stagger groups: group g ticks at phase g/spread of the period, so a big base's runs land on
    // different frames. A hitch never spirals: a late group re-arms one period from NOW.
    if (m_transportTickIndex == 0)
        for (int g = 0; g < 8; ++g)
            m_transportGroupNext[g] = m_transportTime + period * (float)g / (float)spread;
    uint32 dueMask = 0;
    for (int g = 0; g < spread; ++g)
        if (m_transportTime >= m_transportGroupNext[g])
        {
            dueMask |= 1u << g;
            m_transportGroupNext[g] = glm::max(m_transportGroupNext[g] + period, m_transportTime);
        }
    if (dueMask == 0)
        return;
    ++m_transportTickIndex;
    m_statTransportTicks = (int)m_transportTickIndex;
    m_net.dueRuns.clear();
    {
        ProfileScope scope("Transport inject", EProfileCategory::Game);
        for (uint32 r = 0; r < (uint32)m_net.runs.size(); ++r)
        {
            const TransportRun& run = m_net.runs[r];
            if ((dueMask & (1u << (run.group % (uint8)spread))) == 0)
                continue;
            m_net.dueRuns.push_back(r);
            // Fold last tick's movement into the ~2 s average before the count restarts.
            const float avgAlpha = glm::min(period / 2.0f, 1.0f);
            for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
            {
                TransportNode& node = m_net.nodes[n];
                node.movedAvg += ((float)node.moved * m_transportTickHz - node.movedAvg) * avgAlpha;
                node.moved = 0;
            }
            transportInject(run);
        }
    }
    if (m_net.dueRuns.empty())
        return;
    Globals::jobSystem.submit([this] { transportTick(); },
        { "Transport tick", EProfileCategory::Game }, EJobPriority::Normal, &m_transportCounter);
    m_transportKicked = true;
}

// ---------------------------------------------------------------- the job

void StructureSystem::transportTick()
{
    TransportNet& net = m_net;
    const int cellsPerSegOf[3] = { cellsPerSegmentOf(0), cellsPerSegmentOf(1), cellsPerSegmentOf(2) };
    const int substeps = glm::max(m_transportSubsteps, 1);
    // A pass over ONE run's nodes: inline for a small run, fanned out for a large one (a big
    // base is usually one run — this is where the work splits when it is worth it).
    const auto forRunNodes = [&](const TransportRun& run, const char* name, auto&& fn)
    {
        if (run.numNode < 1024)
        {
            for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
                fn(n);
        }
        else
            Globals::jobSystem.parallelFor(run.firstNode, run.firstNode + run.numNode, 256u,
                JobProfile{ name, EProfileCategory::Game }, [&](uint32 begin, uint32 end)
            {
                for (uint32 n = begin; n < end; ++n)
                    fn(n);
            });
    };

    // OFFER: from the fill snapshot, node n decides what leaves it this sub-step — its own
    // slots' demand first, then its neighbours (toward demand, else filling away from supply,
    // space-weighted, integer remainder rotated), all within its out-budget — and what it takes
    // from its slots' supply into its free space. Writes only its own out/in scratch and carry.
    const auto offer = [&](uint32 n)
    {
        TransportNode& node = net.nodes[n];
        const int cellsPerSeg = cellsPerSegOf[glm::min((int)node.medium, 2)];
        // The out-budget accrues while a node has nothing to send (an idle line banks a burst, so
        // a front crosses a segment per sub-step instead of a segment per rate tick), CAPPED at
        // two ticks' worth — never minutes of banked throughput released at once.
        const uint32 carryCap = glm::max(node.rateFp * (uint32)substeps * 2u, 2048u);
        node.carryFp = glm::min(node.carryFp + node.rateFp, carryCap);
        int budget = (int)(node.carryFp >> 10); // whole cells; what is used comes off at the end
        int remaining = (int)node.fill;
        int outTotal = 0;
        // 1) local pull
        if (node.slotCount > 0 && remaining > 0 && budget > 0)
        {
            int totalDemand = 0;
            for (uint32 s = node.slotFirst; s < node.slotFirst + node.slotCount; ++s)
                totalDemand += net.slots[s].demand;
            int give = glm::min(glm::min(totalDemand, remaining), budget);
            for (uint32 k = 0; k < node.slotCount && give > 0; ++k)
            {
                const uint32 s = node.slotFirst + (node.rotate + k) % node.slotCount;
                TransportSlot& slot = net.slots[s];
                const int part = glm::min(slot.demand, give);
                net.outSlot[s] = (uint16)part;
                slot.demand -= part;
                slot.given += part;
                give -= part;
                remaining -= part;
                budget -= part;
                outTotal += part;
            }
        }
        // 2) FORWARD: everything it can, into neighbours with free space that are strictly
        //    DOWNHILL on the demand field (closer to a port that wants cells); with no such
        //    outlet, FILL: into cables (never ports) strictly farther from supply than this
        //    node. Space-weighted, integer remainder rotated. Cells therefore travel toward
        //    demand when there is any and otherwise fill the network outward from the producers
        //    until it is full — no fill gradient, no ping-pong, nothing drifts back toward a
        //    source or into another producer's dead end.
        if (node.adjCount > 0 && remaining > 0 && budget > 0)
        {
            bool fillMode = false;
            const auto spaceAt = [&](uint32 a) {
                const TransportNode& to = net.nodes[net.adj[node.adjFirst + a].node];
                if (fillMode ? (to.junction || to.sdist <= node.sdist || to.sdist == 0xFFFF)
                             : (node.dist == 0xFFFF || to.dist >= node.dist))
                    return 0;
                return glm::max(cellsPerSeg - (int)to.fill, 0); };
            int total = 0;
            for (uint32 a = 0; a < node.adjCount; ++a)
                total += spaceAt(a);
            if (total == 0 && node.sdist != 0xFFFF)
            {
                fillMode = true;
                for (uint32 a = 0; a < node.adjCount; ++a)
                    total += spaceAt(a);
            }
            if (total > 0)
            {
                const int move = glm::min(glm::min(total, remaining), budget);
                int assigned = 0;
                for (uint32 a = 0; a < node.adjCount; ++a)
                {
                    const int share = (int)((int64)move * spaceAt(a) / total);
                    net.outAdj[node.adjFirst + a] = (uint16)share;
                    assigned += share;
                }
                for (uint32 k = 0; k < node.adjCount && assigned < move; ++k) // the remainder, rotated
                {
                    const uint32 a = (node.rotate + k) % node.adjCount;
                    if (spaceAt(a) > (int)net.outAdj[node.adjFirst + a])
                    {
                        ++net.outAdj[node.adjFirst + a];
                        ++assigned;
                    }
                }
                remaining -= assigned;
                budget -= assigned;
                outTotal += assigned;
            }
        }
        // 3) take supply into the space left after this sub-step's outflow
        if (node.slotCount > 0)
        {
            int free = glm::max(cellsPerSeg - remaining, 0);
            for (uint32 k = 0; k < node.slotCount && free > 0; ++k)
            {
                const uint32 s = node.slotFirst + (node.rotate + k) % node.slotCount;
                TransportSlot& slot = net.slots[s];
                const int take = glm::min(slot.supply, free);
                net.inSlot[s] = (uint16)take;
                slot.supply -= take;
                slot.taken += take;
                free -= take;
            }
        }
        node.moved = (uint16)glm::min((int)node.moved + outTotal, 65535);
        node.carryFp -= (uint32)outTotal * 1024u; // the unused budget stays banked (capped above)
        ++node.rotate;
    };
    // APPLY: node n's fill = fill - what it sent - what it delivered + what its neighbours sent
    // it + what it took from its slots. Reads the neighbours' out-slots aimed here (their own
    // writes, finished behind the barrier), writes only its own fill. The neighbour out-slots
    // cannot be zeroed here — the neighbour's own apply may still need to read ours — so a third,
    // trivial pass clears them.
    const auto clearOut = [&](uint32 n)
    {
        const TransportNode& node = net.nodes[n];
        for (uint32 a = 0; a < node.adjCount; ++a)
            net.outAdj[node.adjFirst + a] = 0;
    };
    const auto apply = [&](uint32 n)
    {
        TransportNode& node = net.nodes[n];
        int fill = (int)node.fill;
        for (uint32 a = 0; a < node.adjCount; ++a)
        {
            const TransportAdj& adj = net.adj[node.adjFirst + a];
            fill -= (int)net.outAdj[node.adjFirst + a];
            if (adj.reverse != UINT32_MAX)
                fill += (int)net.outAdj[adj.reverse];
        }
        for (uint32 s = node.slotFirst; s < node.slotFirst + node.slotCount; ++s)
        {
            fill -= (int)net.outSlot[s];
            fill += (int)net.inSlot[s];
            net.outSlot[s] = net.inSlot[s] = 0;
        }
        node.fill = (uint16)glm::clamp(fill, 0, 65535);
    };
    // THE TWO FIELDS, once per tick per run: a BFS through cables only (ports never relay — a
    // line bridges a building cable-to-cable) from every port with unserved DEMAND (`dist`) and
    // from every port pushing SUPPLY (`sdist`). The offer pass moves cells strictly downhill on
    // the demand field, else strictly uphill on the supply field (filling up).
    const auto buildFields = [&](const TransportRun& run)
    {
        // The queue is THIS RUN'S SLICE of the shared scratch (a run's BFS only ever visits its
        // own node range, so the slice is owner-only): runs tick in parallel below, and no
        // thread_local / PerWorker is involved — a job may resume on another thread after any
        // wait, the nested parallelFor of a large run included.
        uint32* const queue = net.bfsQueue.data() + run.firstNode;
        const auto bfs = [&](uint16 TransportNode::* field, bool (*seeds)(const TransportSlot&))
        {
            uint32 tail = 0;
            for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
            {
                TransportNode& node = net.nodes[n];
                node.*field = 0xFFFF;
                if (!node.junction)
                    continue;
                for (uint32 s = node.slotFirst; s < node.slotFirst + node.slotCount; ++s)
                    if (seeds(net.slots[s]))
                    {
                        node.*field = 0;
                        queue[tail++] = n;
                        break;
                    }
            }
            for (uint32 head = 0; head < tail; ++head)
            {
                const TransportNode& node = net.nodes[queue[head]];
                for (uint32 a = 0; a < node.adjCount; ++a)
                {
                    TransportNode& to = net.nodes[net.adj[node.adjFirst + a].node];
                    if (to.*field != 0xFFFF || to.junction)
                        continue; // reached already, or a port (an endpoint, never a relay)
                    to.*field = (uint16)glm::min((int)(node.*field) + 1, 0xFFFE);
                    queue[tail++] = net.adj[node.adjFirst + a].node;
                }
            }
            // Unseeded ports must still SEND (a producer's port on the demand field) — one hop
            // above their best cable, so they read as uphill of it.
            for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
            {
                TransportNode& node = net.nodes[n];
                if (!node.junction || node.*field != 0xFFFF)
                    continue;
                int best = 0xFFFF;
                for (uint32 a = 0; a < node.adjCount; ++a)
                    best = glm::min(best, (int)(net.nodes[net.adj[node.adjFirst + a].node].*field));
                if (best != 0xFFFF)
                    node.*field = (uint16)glm::min(best + 1, 0xFFFE);
            }
        };
        bfs(&TransportNode::dist, [](const TransportSlot& s) { return s.demand > 0; });
        bfs(&TransportNode::sdist, [](const TransportSlot& s) { return s.supply > 0; });
    };
    // ONE RUN, start to finish: its fields, then the sub-steps. Runs share no state, so the due
    // runs tick IN PARALLEL (one job per run above a couple of runs); a single huge run fans its
    // passes out per node instead.
    const auto tickRun = [&](const TransportRun& run)
    {
        ProfileScope scope("Transport run", EProfileCategory::Game);
        buildFields(run);
        for (int sub = 0; sub < substeps; ++sub)
        {
            forRunNodes(run, "Transport offer", offer);
            forRunNodes(run, "Transport apply", apply);
            forRunNodes(run, "Transport clear", clearOut);
        }
    };
    const uint32 numDue = (uint32)net.dueRuns.size();
    if (numDue < 2)
    {
        for (const uint32 r : net.dueRuns)
            tickRun(net.runs[r]);
    }
    else
        Globals::jobSystem.parallelFor(0u, numDue, 1u, JobProfile{ "Transport runs", EProfileCategory::Game },
            [&](uint32 begin, uint32 end)
        {
            for (uint32 i = begin; i < end; ++i)
                tickRun(net.runs[net.dueRuns[i]]);
        });
}

// ---------------------------------------------------------------- readouts + mirror

bool StructureSystem::cableInfo(int index, CableInfo& out) const
{
    const auto it = m_net.nodeById.find(m_frame[index].state->structureId);
    if (it == m_net.nodeById.end())
        return false;
    const TransportNode& node = m_net.nodes[it->second];
    const TransportRun& run = m_net.runs[node.run];
    out.medium = node.medium;
    out.fill = node.fill;
    out.capacity = cellsPerSegmentOf(node.medium);
    out.movedPerSec = node.movedAvg; // ~2 s average, not the last tick's burst
    out.ratePerSec = m_cableThroughput[glm::min((int)node.medium, 2)];
    out.runFill = out.runCapacity = out.runSegments = 0;
    for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
    {
        const TransportNode& other = m_net.nodes[n];
        if (other.junction)
            continue;
        ++out.runSegments;
        out.runFill += other.fill;
        out.runCapacity += out.capacity;
    }
    return true;
}

void StructureSystem::collectCableFills(oc::vector<CableMirror>& out, uint32& cursor, int maxRecords) const
{
    const uint32 count = (uint32)m_net.nodes.size();
    if (count == 0)
        return;
    uint32 n = cursor % count;
    for (uint32 visited = 0; visited < count && (int)out.size() < maxRecords; ++visited, n = (n + 1) % count)
    {
        const TransportNode& node = m_net.nodes[n];
        if (node.junction)
            continue;
        const float rate = glm::max(m_cableThroughput[glm::min((int)node.medium, 2)], 1e-3f);
        out.push_back({ node.structureId, (uint8)glm::min((int)node.fill, 255),
            (uint8)glm::clamp(node.movedAvg / rate * 255.0f, 0.0f, 255.0f) });
    }
    cursor = n;
}

void StructureSystem::mirrorCableFill(uint32 id, uint8 fill, uint8 util)
{
    if (const auto it = m_net.nodeById.find(id); it != m_net.nodeById.end())
    {
        TransportNode& node = m_net.nodes[it->second];
        node.fill = fill;
        node.movedAvg = (float)util / 255.0f * m_cableThroughput[glm::min((int)node.medium, 2)];
    }
    else
        m_savedFills[id] = fill; // a segment the client's graph has not derived yet
}
