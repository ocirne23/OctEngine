export module RendererVK:LightGridComputePipeline;

import Core;
import Core.glm;

import :VK;
import :Buffer;
import :CommandBuffer;
import :ComputePipeline;
import :DescriptorSet;
import :GridClaim;
import :Layout;
import :Settings;

// The clustered light grid. The BUILD is split three ways, none of it on the main thread:
//  1. addLight, INLINE on whatever thread adds the light (the entity pass, the force update, ...):
//     the light's bounds -> its cull record; every 32^3 grid its box covers is claimed in the
//     lock-free CPU hash table (GridClaim: the GPU's hash, bit for bit; the grid's distance-LOD
//     cell size is picked at the claim), the grid's candidate count is bumped, and one (grid
//     slot, light) touch is appended (GridTouches). The LARGE decision is made here too. This is
//     the bulk of the work, and it happens the moment the light exists.
//  2. build, a job kicked right after the last light of the frame (Renderer::kickGridBuilds,
//     from the App loop after the force update): prefix sums over the grids, the scatter of the
//     touches into per-grid lists, the data offsets, the workgroup list. O(grids + touches).
//  3. upload: the same job when the capacity fits; after the growth on the main thread when not.
// The GPU (light_grid.cs.glsl) then GATHERS: one thread per cell loops its grid's candidates and
// writes the cell's list with no atomics. Readers (forward pass, ocean, particles, fog, GI trace)
// are unchanged.
export class LightGridComputePipeline final
{
public:
	// What one frame needs, from build(): the renderer grows the GPU-side buffers (and this class's
	// host buffers, resizeHostBuffers) BEFORE upload() when any of it outruns the capacity. Grid and
	// touch demand count the FAILED claims too, so growth fits an overflowed frame.
	struct Demand
	{
		uint32 numGrids = 0;
		size_t gridDataBytes = 0;  // the bump-allocated grid data, incl. every cell of every grid
		uint32 numWorkgroups = 0;
		uint32 lightListSize = 0;
	};

	void initialize();
	void reloadShaders();
	struct RecordParams
	{
		DescriptorSet& descriptorSet;
		Buffer& ubo;
		Buffer& outLightGridBuffer;
	};
	void record(CommandBuffer& commandBuffer, uint32 frameIdx, RecordParams& recordParams);

	// Frame start (the renderer's begin-frame job, before any light is added): snapshots the LOD
	// params + view position every addLight of the frame uses, clears the claim table and counts.
	void beginFrame(uint32 frameIdx, const LightGridParams& params, const glm::vec3& viewPos);
	// Per light, from ANY thread, as soon as the light is added.
	void addLight(uint32 lightIdx, const RendererVKLayout::LightInfo& light);
	// After the last addLight of the frame (a job). `lights` re-walks the lights whose claims did
	// not fit (grown for the next frame).
	Demand build(oc::span<const RendererVKLayout::LightInfo> lights);
	bool hostBuffersFit(const Demand& demand) const;
	// Recreates every frame's host buffers (and the claim table) to fit: the caller has waited for
	// the GPU and re-records.
	void resizeHostBuffers(const Demand& demand);
	// Writes the grid jobs, the workgroup list, the light list, the hash table (into tableBuffer:
	// header {numGrids, gridDataCounter, tableSize} + the slots) and the indirect dispatch.
	void upload(Buffer& tableBuffer, uint32 tableEntries);
	vk::DescriptorSetLayout getDescriptorSetLayout() const { return m_computePipeline.getDescriptorSetLayout(); }

	// One grid's build record, LIGHT_GRID_JOB_STRIDE uints in light_grid.cs.glsl: keep in sync.
	// cellSize 0 = a DEAD slot (lost a claim race): no workgroups, not in the table.
	struct GridJob
	{
		glm::ivec3 pos;
		uint32 cellSize;
		uint32 dataOffset; // uints into the grid data buffer
		uint32 lightBegin;
		uint32 lightCount; // per-cell candidates
		uint32 largeBegin;
		uint32 largeCount; // evaluated by every pixel of the grid
	};
	static_assert(sizeof(GridJob) == 9 * sizeof(uint32));

private:
	struct PerFrameData
	{
		Buffer inIndirectCommandBuffer;
		oc::span<vk::DispatchIndirectCommand> mappedIndirectCommands;
		Buffer inLightCullBuffer;    // 3 vec4 per light, MAX_LIGHTS
		oc::span<glm::vec4> mappedLightCull;
		Buffer inGridJobsBuffer;
		oc::span<GridJob> mappedGridJobs;
		Buffer inWorkgroupsBuffer;   // 2 uints per workgroup
		oc::span<uint32> mappedWorkgroups;
		Buffer inLightListBuffer;
		oc::span<uint32> mappedLightList;
	};
	using Touch = GridTouches::Touch;
	struct LightWalk // a light's grid range plus what the touch needs
	{
		glm::ivec3 gridMin, gridMax;
		glm::ivec3 boxMinCell, boxMaxCell; // floor of the box, in world units
		bool largeEverywhere;
		bool valid;
	};
	void buildComputeLayout(ComputePipelineLayout& layout);
	void createHostBuffers();
	void createClaimTables();
	LightWalk walkOf(uint32 lightIdx, const RendererVKLayout::LightInfo& light);
	bool isLargeIn(const LightWalk& walk, const glm::ivec3& gridPos, uint32 cellSize) const;
	// Claims + touches the walk's grids into out[]; false = a claim met the grid capacity (the counts
	// bumped so far are retracted; the caller discards the block).
	bool touchGrids(const LightWalk& walk, uint32 lightIdx, Touch* out);

	oc::array<PerFrameData, RendererVKLayout::NUM_FRAMES_IN_FLIGHT> m_perFrameData;
	ComputePipeline m_computePipeline;

	uint32 m_gridCapacity = 1024;
	uint32 m_workgroupCapacity = 16384;
	uint32 m_lightListCapacity = 65536;

	// The frame's addLight inputs (beginFrame snapshots; only read while lights are being added).
	uint32 m_frameIdx = 0;
	LightGridParams m_params;
	glm::vec3 m_viewPos{ 0.0f };

	// The lock-free claim state (payload = the grid's cell size) + per-slot candidate counts.
	GridClaim m_claim;
	GridTouches m_touches;
	oc::vector<uint32> m_cellCounts;
	oc::vector<uint32> m_largeCounts;
	oc::vector<Touch> m_extraTouches; // build's serial re-walk of the overflow lights

	// Build scratch, reused every frame.
	oc::vector<GridJob> m_grids;
	oc::vector<uint32> m_cellCursor;
	oc::vector<uint32> m_largeCursor;
	oc::vector<uint32> m_lightList;
	oc::vector<uint32> m_workgroups;
	uint32 m_numGrids = 0; // slots in use this frame (dead ones included)
	Demand m_demand;
};
