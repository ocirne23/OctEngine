module;

#include <box3d/box3d.h>

module Physics;

import Core;
import Threading;

import :TaskScheduler;

// ONE literal for every task, deliberately not box3d's `taskName`. ProfileRecord keeps names BY
// POINTER and the panel reads them back frames later, but box3d builds `taskName` into a TRANSIENT
// buffer -- so the pointer is dangling by the time the flame graph draws it. Copying the text into
// the slot does not fix it either: the slot is reused a step later and the copy is overwritten, so
// names on already-recorded blocks keep changing as you watch them. A literal is the only name that
// still means what it said; box3d's per-phase detail is not worth a string store on this path.
//
// Unconditional, unlike the per-entity component updates: the record volume is bounded by the STEP
// rate, not the frame rate. box3d caps a step at B3_MAX_TASKS and the "Step Hz" tweak caps at 120,
// so the worst case is ~15.6K records/sec spread across every worker -- tens of seconds of ring
// history per thread, against a 512-frame panel window.
void PhysicsTaskScheduler::runSlot(TaskSlot& slot)
{
    ProfileScope scope("Physics Job", EProfileCategory::Physics);
    slot.task(slot.context);
}

void* PhysicsTaskScheduler::enqueue(TaskFn task, void* taskContext, void* userContext, const char*)
{
    // The interface deliberately cannot see box3d, so pin its assumptions about the ring here (in a
    // member, where c_maxTasks is accessible).
    static_assert(c_maxTasks == B3_MAX_TASKS, "the task ring must match box3d's per-step enqueue cap");
    static_assert((c_maxTasks & (c_maxTasks - 1)) == 0, "the task ring masks the cursor, so it must be a power of two");

    // No job system to fan out to: the tooling builds that never initialize one, and the window at
    // teardown where ~JobSystem (init_seg XCU5) has already run but the plain-XCU physics global has
    // not, so b3DestroyWorld still reaches here.
    if (!Globals::jobSystem.isInitialized())
    {
        ProfileScope scope("Physics Job", EProfileCategory::Physics);
        task(taskContext);
        return nullptr;
    }

    PhysicsTaskScheduler& scheduler = *static_cast<PhysicsTaskScheduler*>(userContext);
    TaskSlot& slot = scheduler.m_slots[scheduler.m_cursor.fetch_add(1, oc::memory_order_relaxed) & (c_maxTasks - 1)];
    assert(slot.counter.isDone() && "box3d enqueued more than B3_MAX_TASKS in one step - the ring wrapped onto a live task");
    slot.task = task;
    slot.context = taskContext;
    // High: the step blocks in finish() until these land, so they sit on the frame's critical path.
    // Same literal as the job name, so JobGraph/stats diagnostics agree with the profiler tracks.
    Globals::jobSystem.submit([&slot] { runSlot(slot); }, EJobPriority::High, &slot.counter, "Physics Job");
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
