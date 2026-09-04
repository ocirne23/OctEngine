module Game;

import Core;
import Core.glm;
import Core.Log;
import Entity;
import Threading;
import :Structures;

// THE CABLE TRANSPORT — see the block in Structures.ixx. This file is the tick: the boundary
// with the buildings' float stores (inject / apply, main thread) and the job (offer / apply
// stencil passes over the flat node graph, owner-only writes, no atomics).

// The port role of a building type in a medium: PRODUCERS push whole cells out of their store,
// CONSUMERS pull into their headroom, STORAGE does either by the fill of its own port node
// (push while the cable there is at/below the low mark, pull while at/above the high mark).
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
    slot.structureId = ref.state->structureId;
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
    const float cellsPerSeg = (float)glm::max(m_cellsPerSegment, 1);
    const int substeps = glm::max(m_transportSubsteps, 1);
    for (uint32 n = run.firstNode; n < run.firstNode + run.numNode; ++n)
    {
        const TransportNode& node = m_net.nodes[n];
        if (!node.junction)
            continue;
        // The most this port can move in one tick: its node's out-rate over the tick (+1 so a
        // fractional rate still moves). Pushed cells are RESERVED out of the store now and the
        // unused part refunded at the join, so the store never reads more than it holds; the
        // cap keeps a battery from parking its whole bank in transit (its own consumers — the
        // Base's shield — keep reading a live store).
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
            const auto push = [&]() {
                const int cells = glm::min((int)std::floor(glm::max(c->store[m], 0.0f)), maxPerTick);
                if (cells <= 0)
                    return;
                c->store[m] -= (float)cells;
                slot.supply = slot.reserved = cells; };
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
            case ETransportRole::Producer: push(); break;
            case ETransportRole::Consumer: pull(true); break;
            case ETransportRole::Storage:
            {
                // The price signal is the CABLE next to the port — the mean fill of the junction's
                // neighbours — never the junction itself: a push fills the own junction to the
                // high mark, which then read as "pull it back" (the Base oscillated on exactly
                // that).
                float fill = 0.0f;
                if (node.adjCount > 0)
                {
                    for (uint32 a = 0; a < node.adjCount; ++a)
                        fill += (float)m_net.nodes[m_net.adj[node.adjFirst + a].node].fill;
                    fill /= (float)node.adjCount * cellsPerSeg;
                }
                if (fill <= m_storageLowMark)
                    push();
                else if (fill >= m_storageHighMark)
                    pull(false);
                break;
            }
            }
        }
    }
}

void StructureSystem::transportApplyBoundary()
{
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
    const int cellsPerSeg = glm::max(m_cellsPerSegment, 1);
    const int substeps = glm::max(m_transportSubsteps, 1);
    // A pass over every due run's nodes: inline for small runs, fanned out for large ones.
    const auto forDueNodes = [&](const char* name, auto&& fn)
    {
        for (const uint32 r : net.dueRuns)
        {
            const TransportRun& run = net.runs[r];
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
        }
    };

    // OFFER: from the fill snapshot, node n decides what leaves it this sub-step — its own
    // slots' demand first, then lower-filled neighbours (half the difference each, weighted by
    // deficit, integer remainder rotated), all within its out-budget — and what it takes from
    // its slots' supply into its free space. Writes only its own out/in scratch and its carry.
    const auto offer = [&](uint32 n)
    {
        TransportNode& node = net.nodes[n];
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
        // 2) FORWARD: everything it can, into neighbours with free space that did not feed it
        //    last sub-step (the conveyor rule — never back where it came from). Space-weighted,
        //    integer remainder rotated. No fill gradient is needed: a line runs full end to end,
        //    a dead end fills up and stops, and equal neighbours never ping-pong.
        if (node.adjCount > 0 && remaining > 0 && budget > 0)
        {
            const auto spaceAt = [&](uint32 a) {
                if (a < 32 && (node.inMask & (1u << a)) != 0)
                    return 0;
                return glm::max(cellsPerSeg - (int)net.nodes[net.adj[node.adjFirst + a].node].fill, 0); };
            int total = 0;
            for (uint32 a = 0; a < node.adjCount; ++a)
                total += spaceAt(a);
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
    const auto applyKeepOut = [&](uint32 n)
    {
        TransportNode& node = net.nodes[n];
        int fill = (int)node.fill;
        uint32 inMask = 0;
        for (uint32 a = 0; a < node.adjCount; ++a)
        {
            const TransportAdj& adj = net.adj[node.adjFirst + a];
            fill -= (int)net.outAdj[node.adjFirst + a];
            if (adj.reverse != UINT32_MAX && net.outAdj[adj.reverse] != 0)
            {
                fill += (int)net.outAdj[adj.reverse];
                if (a < 32)
                    inMask |= 1u << a;
            }
        }
        for (uint32 s = node.slotFirst; s < node.slotFirst + node.slotCount; ++s)
        {
            fill -= (int)net.outSlot[s];
            fill += (int)net.inSlot[s];
            net.outSlot[s] = net.inSlot[s] = 0;
        }
        node.fill = (uint16)glm::clamp(fill, 0, 65535);
        node.inMask = inMask;
    };
    for (int sub = 0; sub < substeps; ++sub)
    {
        forDueNodes("Transport offer", offer);
        forDueNodes("Transport apply", applyKeepOut);
        forDueNodes("Transport clear", clearOut);
    }
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
    out.capacity = glm::max(m_cellsPerSegment, 1);
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
