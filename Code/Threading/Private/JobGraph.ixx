export module Threading:JobGraph;

import Core;
import :Types;
import :JobSystem;

export class JobGraph;

export struct JobId
{
    uint32 index = UINT32_MAX;
};

// Returned by JobGraph::addJob to declare the job's data access and explicit ordering. Hazard
// edges are derived immediately, render-graph style, from declaration order: a reader depends on
// the resource's last writer; a writer depends on every reader since the last write (or the
// writer itself when there were none). Reads of the same resource run in parallel; no locks are
// ever taken - the schedule itself is the synchronization.
export class JobGraphBuilder final
{
public:

    JobGraphBuilder& reads(const JobResource& resource);
    JobGraphBuilder& writes(const JobResource& resource);
    JobGraphBuilder& after(JobId predecessor); // explicit edge for ordering not expressible as data access
    JobGraphBuilder& priority(EJobPriority priority);
    operator JobId() const { return { m_index }; }

private:

    friend class JobGraph;
    JobGraphBuilder(JobGraph& graph, uint32 index) : m_graph(&graph), m_index(index) {}
    JobGraph* m_graph;
    uint32 m_index;
};

// A reusable DAG of jobs. Declare jobs + accesses once, compile() once, then run()/wait() every
// frame: run() only resets the per-job atomic dependency counters and pushes the roots, so a
// steady-state graph costs zero allocations and no rebuild. Edges always point from an earlier
// declared job to a later one, so the graph is acyclic by construction. Callables are invoked
// once per run and must be re-invocable.
export class JobGraph final
{
public:

    JobGraph() = default;
    JobGraph(const JobGraph&) = delete;
    JobGraph& operator=(const JobGraph&) = delete;
    ~JobGraph()
    {
        assert(!isRunning());
        clear();
    }

    template<typename Func>
    JobGraphBuilder addJob(const char* name, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        assert(!isRunning());
        m_compiled = false;
        Job& job = allocateJob();
        setJobCallable(job, oc::forward<Func>(func));
        job.signal = &m_counter;
        job.successors = nullptr;
        job.numSuccessors = 0;
        job.pending.store(0, oc::memory_order_relaxed);
        job.initialPending = 0;
        job.costEmaUs = 0;
        job.rankUs = 0;
        job.priority = priority;
        job.effectivePriority = priority;
        job.flags = 0; // timed by default: measured cost drives the critical-path ranking in run()
        job.name = name;
        return JobGraphBuilder(*this, m_numJobs - 1);
    }

    // A node whose body fans out into a parallelFor over [0, countFunc()) - the range is
    // re-evaluated every run (live entity/item counts). The node's fiber parks at the internal
    // join, so budget one fiber per such node running concurrently (JobSystemDesc::numFibers).
    template<typename CountFunc, typename Func>
    JobGraphBuilder addParallelJob(const char* name, CountFunc&& countFunc, uint32 grainSize, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        return addJob(name, [countFunc = oc::forward<CountFunc>(countFunc), grainSize, func = oc::forward<Func>(func)]
            {
                Globals::jobSystem.parallelFor(0u, countFunc(), grainSize, func);
            }, priority);
    }

    void compile(); // dedupes edges, builds the successor arrays + dependency counts + root set
    void run();     // non-blocking; requires compile()d and not already running
    void wait() { Globals::jobSystem.wait(m_counter); }
    void runAndWait()
    {
        run();
        wait();
    }

    bool isRunning() const { return !m_counter.isDone(); }
    bool isCompiled() const { return m_compiled; }
    uint32 getNumJobs() const { return m_numJobs; }
    uint32 getNumEdges() const { return uint32(m_edges.size()); }

    // Measured cost EMA / critical-path rank of a node after at least one run (min 1us each) -
    // free per-node profiling for the frame graph.
    uint32 getJobCostUs(JobId id) const { return jobAt(id.index).costEmaUs; }
    uint32 getJobRankUs(JobId id) const { return jobAt(id.index).rankUs; }

    void clear(); // destroys the callables; the graph can be redeclared from scratch

private:

    friend class JobGraphBuilder;

    static constexpr uint32 JobsPerChunk = 64; // Job is non-movable (atomics), chunks keep addresses stable

    Job& jobAt(uint32 index) { return m_chunks[index / JobsPerChunk][index % JobsPerChunk]; }
    const Job& jobAt(uint32 index) const { return m_chunks[index / JobsPerChunk][index % JobsPerChunk]; }
    Job& allocateJob();
    void declareRead(uint32 jobIndex, uint32 resourceId);
    void declareWrite(uint32 jobIndex, uint32 resourceId);
    void addEdge(uint32 from, uint32 to);

    struct ResourceState
    {
        int32 lastWriter = -1;
        oc::vector<uint32> readersSinceWrite;
    };

    oc::vector<oc::unique_ptr<Job[]>> m_chunks;
    uint32 m_numJobs = 0;
    oc::vector<oc::pair<uint32, uint32>> m_edges;
    oc::vector<ResourceState> m_resources; // indexed by JobResource::id, build-time only
    oc::vector<Job*> m_successorStorage;   // CSR edge array the jobs' successor spans point into
    oc::vector<Job*> m_roots;
    JobCounter m_counter;
    uint32 m_lastMaxRankUs = 0; // previous run's critical-path length; this run's promotion threshold
    bool m_compiled = false;
};
