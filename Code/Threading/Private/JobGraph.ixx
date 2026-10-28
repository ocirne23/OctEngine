export module Threading:JobGraph;

import Core;
import :JobTypes;
import :Job;
import :JobSystem;

// A reusable declared-access job DAG. Inside Threading only (the stress harness); nothing outside uses it.

export class JobGraph;

export struct JobId
{
    uint32 index = UINT32_MAX;
};

// A process-unique id for data that graph jobs declare read/write access on. Create once, keep alive.
export class JobResource final
{
public:

    explicit JobResource(const char* name = nullptr) : m_id(s_nextId.fetch_add(1, oc::memory_order_relaxed)), m_name(name) {}
    JobResource(const JobResource&) = delete;
    JobResource& operator=(const JobResource&) = delete;

    uint32 id() const { return m_id; }
    const char* name() const { return m_name; }

private:

    static inline oc::atomic<uint32> s_nextId = 0;
    uint32 m_id;
    const char* m_name;
};

// Declares a job's data access; hazard edges follow from declaration order (readers after the last
// writer, a writer after every reader since).
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

// A reusable DAG: declare + compile() once, run()/wait() every frame. Callables must be re-invocable.
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

    // A parallelFor over [0, countFunc()), re-evaluated every run. Holds a fiber while it runs.
    template<typename CountFunc, typename Func>
    JobGraphBuilder addParallelJob(const char* name, CountFunc&& countFunc, uint32 grainSize, Func&& func, EJobPriority priority = EJobPriority::Normal)
    {
        return addJob(name, [name, countFunc = oc::forward<CountFunc>(countFunc), grainSize, func = oc::forward<Func>(func)]
            {
                Globals::jobSystem.parallelFor(0u, countFunc(), grainSize, JobProfile{ name }, func);
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

    // Measured cost EMA / critical-path rank, after at least one run.
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
