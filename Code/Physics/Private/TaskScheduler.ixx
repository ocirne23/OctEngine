export module Physics:TaskScheduler;

import Core;
import Threading;

// box3d fans its solver phases out through two callbacks instead of spawning threads of its own
// (which is what b3WorldDef::workerCount > 1 WITHOUT callbacks does -- a second, private thread pool
// competing with the engine's for the same cores). One of these objects is handed to box3d as
// b3WorldDef::userTaskContext and comes back through the userContext parameter of both callbacks, so
// every bit of scheduler state is per-world and none of it is global.
//
// box3d has ALREADY sliced each phase into `workerCount` pieces and baked the slice index into every
// taskContext, so a task is an opaque closure here: run it exactly once, on any thread, with the
// pointer box3d gave. That is why the callback carries no worker index, unlike box2d.
//
// THE CALLING THREAD PARTICIPATES AS A WORKER. b3World_Step holds its stack across every fork/join,
// so finish() MUST block until the task ran, and box3d's own rule is "do not call b3World_Step and
// park that fiber" -- the step has to run somewhere whose blocking wait HELPS rather than suspends.
// PhysicsWorld::update is main-thread-only and JobSystem::wait() helps on the main thread (it runs
// ready jobs, reaching the High queue these tasks land in before anything else), which is exactly
// that. Calling the step from a job fiber would satisfy the JobSystem but violate box3d's rule.
export class PhysicsTaskScheduler final
{
public:

    // The plain C spelling of b3TaskCallback*. Identical to it once the typedef expands, so
    // b3WorldDef accepts the members below without a cast and box3d still never reaches a Physics
    // interface (this partition deliberately does not include box3d at all).
    using TaskFn = void (*)(void* taskContext);

    // -> b3WorldDef::enqueueTask / ::finishTask. userContext is the scheduler the world was built
    // with. enqueue returns nullptr -- box3d's "this task is already complete, do not call
    // finishTask" signal -- when there is no job system to fan out to.
    static void* enqueue(TaskFn task, void* taskContext, void* userContext, const char* taskName);
    static void finish(void* userTask, void* userContext);

    // -> b3WorldDef::workerCount: the job system's workers PLUS the thread driving the step, which
    // box3d counts as a worker too (it hands that thread a slice and expects finishTask to run it).
    // Clamped to [1, B3_MAX_WORKERS]. box3d's own guidance is to size this by PHYSICAL core count --
    // hyper-threads and efficiency cores buy it little and can cost -- so on a machine where the job
    // system spans logical cores, the "Worker count" tweak is the knob to trim it back.
    static int defaultWorkerCount();

private:

    static constexpr uint32 c_maxTasks = 256; // B3_MAX_TASKS, static_asserted against it in the .cpp

    // One slot per in-flight box3d task. box3d never enqueues more than B3_MAX_TASKS per world step
    // and finishes every one of them inside that same step, so a monotonic cursor masked into a
    // B3_MAX_TASKS-sized ring can never hand out a slot whose task is still running: any reuse is at
    // least a whole step old. That is what keeps the claim a single fetch_add -- no free list, no
    // lock -- and what makes the slot addresses handed back to box3d stable for the world's life.
    struct TaskSlot
    {
        TaskFn task = nullptr;
        void* context = nullptr;
        JobCounter counter;
    };

    static void runSlot(TaskSlot& slot);

    TaskSlot m_slots[c_maxTasks];
    oc::atomic<uint32> m_cursor = 0;
};
