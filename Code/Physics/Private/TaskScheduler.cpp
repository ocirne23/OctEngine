module;

#include <box3d/box3d.h>

module Physics;

import Core;
import Threading;

import :TaskScheduler;

// Copies into the slot's own storage. box3d's taskName does NOT survive the call -- it is built into
// a transient buffer -- and ProfileRecord keeps names by pointer for the panel to read back later,
// so the copy is what makes the name still be the name when the flame graph renders it.
void PhysicsTaskScheduler::copyName(char (&dst)[c_maxNameLen], const char* src)
{
    uint32 i = 0;
    for (; src[i] && i < c_maxNameLen - 1; ++i)
        dst[i] = src[i];
    dst[i] = 0;
}

// Every task body carries a marker named by box3d's own task name, so a step's fan-out reads as
// named spans on the worker tracks instead of anonymous job time.
//
// Unconditional, unlike the per-entity component updates: the record volume is bounded by the STEP
// rate, not the frame rate. box3d caps a step at B3_MAX_TASKS and the "Step Hz" tweak caps at 120,
// so the worst case is ~15.6K records/sec spread across every worker -- tens of seconds of ring
// history per thread, against a 512-frame panel window.
void PhysicsTaskScheduler::runSlot(TaskSlot& slot)
{
    ProfileScope scope(slot.name, EProfileCategory::Physics);
    slot.task(slot.context);
}

void* PhysicsTaskScheduler::enqueue(TaskFn task, void* taskContext, void* userContext, const char* taskName)
{
    // The interface deliberately cannot see box3d, so pin its assumptions about the ring here (in a
    // member, where c_maxTasks is accessible).
    static_assert(c_maxTasks == B3_MAX_TASKS, "the task ring must match box3d's per-step enqueue cap");
    static_assert((c_maxTasks & (c_maxTasks - 1)) == 0, "the task ring masks the cursor, so it must be a power of two");

    // Claim the slot even on the inline path below: its name buffer is the only storage that
    // outlives box3d's transient taskName, and a profile record made here is read back just as late.
    PhysicsTaskScheduler& scheduler = *static_cast<PhysicsTaskScheduler*>(userContext);
    TaskSlot& slot = scheduler.m_slots[scheduler.m_cursor.fetch_add(1, oc::memory_order_relaxed) & (c_maxTasks - 1)];
    assert(slot.counter.isDone() && "box3d enqueued more than B3_MAX_TASKS in one step - the ring wrapped onto a live task");
    slot.task = task;
    slot.context = taskContext;
    copyName(slot.name, taskName ? taskName : "Physics task");

    // No job system to fan out to: the tooling builds that never initialize one, and the window at
    // teardown where ~JobSystem (init_seg XCU5) has already run but the plain-XCU physics global has
    // not, so b3DestroyWorld still reaches here.
    if (!Globals::jobSystem.isInitialized())
    {
        runSlot(slot);
        return nullptr;
    }

    // High: the step blocks in finish() until these land, so they sit on the frame's critical path.
    // The job name is the slot's copy too, so JobGraph/stats diagnostics agree with the tracks (and
    // outlive the enqueue the same way -- submit stores the pointer, it does not copy either).
    Globals::jobSystem.submit([&slot] { runSlot(slot); }, EJobPriority::High, &slot.counter, slot.name);
    return &slot;
}

// One marker per fork/join point, on the thread driving b3World_Step: the row of these under
// "Physics step" IS the step's parallel structure, each one's width the cost of that phase. Note
// that wait() HELPS rather than sleeps here, so jobs the main thread picks up to burn the stall nest
// inside this scope with their own markers -- the same honest-but-broad attribution the engine's
// other main-thread waits have, e.g. "UI prepare wait".
void PhysicsTaskScheduler::finish(void* userTask, void*)
{
    ProfileScope scope("Physics fork/join", EProfileCategory::Physics);
    Globals::jobSystem.wait(static_cast<TaskSlot*>(userTask)->counter);
}

int PhysicsTaskScheduler::defaultWorkerCount()
{
    if (!Globals::jobSystem.isInitialized())
        return 1;
    // +1: box3d treats the thread driving the step as a worker and slices for it too.
    return int(oc::clamp<uint32>(Globals::jobSystem.getNumWorkers() + 1, 1u, uint32(B3_MAX_WORKERS)));
}
