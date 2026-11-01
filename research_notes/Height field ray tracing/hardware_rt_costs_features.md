# Hardware ray tracing costs and features for terrain

State checked: 2026-11-01. Each older source has its date. Tags: `[Certain]` = I read the primary text; `[Likely]` = a good source or a strong inference; `[Guessing]` = fills a gap. "Snippet only" = I saw the text in a search result and did not open the page.

## What does a triangle BLAS cost in bytes per triangle on NVIDIA, AMD and Intel, before and after compaction? Is there data for regular-grid or terrain meshes?

### Takeaway
After compaction with PREFER_FAST_TRACE, current numbers on one real scene are 18.8 B/tri (RTX 50), about 26 B/tri (RTX 20 to 40), 45 B/tri (Arc B580), 48 B/tri (RDNA4) and 57 B/tri (RDNA2/3). Before compaction, multiply by about 2 on NVIDIA. I found no published BLAS size for a regular-grid or terrain mesh, so a measurement on the engine's own grid is necessary.

### Cited Findings

**Compacted, PREFER_FAST_TRACE, Bistro (1.754M BLAS triangles, fp16 positions, end of March 2025 drivers, TLAS not included)** — `[Certain]` — [Zeux, "Measuring acceleration structures", 2025](https://zeux.io/2025/03/31/measuring-acceleration-structures/)

| GPU | BLAS size | B/tri |
|---|---|---|
| NVIDIA RTX 5070 (Blackwell) | 33 MB | 18.8 |
| NVIDIA RTX 4090 (Ada) | 45 MB | 25.7 |
| NVIDIA RTX 3050 (Ampere) | 45 MB | 25.7 |
| NVIDIA RTX 2080 (Turing) | 46 MB | 26.5 |
| Intel Arc B580 | 79 MB | 45.0 |
| AMD RX 9070 (RDNA4) | 84 MB | 47.9 |
| AMD 7900 GRE (RDNA3), 7950X iGPU (RDNA2) | 100 MB | 57.0 |
| Apple M4 | 93 MB | 53.0 |

- The driver changes the result much. RDNA3 went 88.4 B/tri (AMDVLK 2024.Q3), then 59.9 (2024.Q4), then 57.0 (2025.Q1). radv (Mesa 25.0) was 137.4 B/tri. The author connects the differences to fp16 box nodes, triangle pair compression and SBVH splits. `[Certain]` — [Zeux](https://zeux.io/2025/03/31/measuring-acceleration-structures/)
- The same article gives no uncompacted sizes. It warns that bytes per triangle is an approximate metric. It expects that denser meshes come nearer to the theoretical optimum but did not test this. It also expects that fp32 input gives different results and smaller vendor differences. `[Certain]` — [Zeux](https://zeux.io/2025/03/31/measuring-acceleration-structures/)

**Before and after compaction**
- RTX 2070 SUPER, driver 621.01: Vulkan 70.7 to 29.2 B/tri, DX12 71.0 to 29.5 B/tri (41% remains). The mesh is not described; BLAS sizes 2,048 to 131,072 triangles. This is a third-party probe through the wgpu allocator report, not a vendor source. `[Certain]` that it says this, `[Likely]` that the data is good — [gariasf/praxis PR 303](https://github.com/gariasf/praxis/pull/303)
- RTX 3000 series: average 52% reduction from compaction, standard deviation 2.8%. Zero Day 458.9 to 208.3 MB (55%), Emerald Square 189.7 to 106.1 MB (44%), Sun Temple 45.8 to 20.7 MB (55%). `[Certain]` — [NVIDIA RTXMU blog, July 2021](https://developer.nvidia.com/blog/reducing-acceleration-structure-memory-with-nvidia-rtxmu/)
- NVIDIA claims about a competitor (AMD 6000 series, 2021, not independent): AMD without compaction uses double the memory of NVIDIA; compaction removes about 75% on AMD against 50% on NVIDIA; after compaction NVIDIA is 3.26x smaller. `[Certain]` that NVIDIA says this — [NVIDIA RTXMU blog](https://developer.nvidia.com/blog/reducing-acceleration-structure-memory-with-nvidia-rtxmu/)
- The Tellusim figures are older and do not mention compaction, so I read them as uncompacted. Fast Trace, about 3.6M to 4.2M triangles: RTX 2080 Ti 224 to 255 MB (about 61 B/tri), RX 6700 XT 559 to 656 MB (about 156 B/tri), Apple M1 304 to 355 MB (about 84 B/tri). Scratch: NVIDIA about 4 B/tri, AMD about 200 B/tri. `[Certain]` for the MB values, the B/tri values are my division — [Tellusim, October 2021](https://tellusim.com/rt-anim/)
- AMD RDNA2/3-class builder, compacted: triangle leaves 32.0 to 35.8 B/tri and inner nodes 22.9 to 41.5 B/tri, total 54.9 to 77.3 B/tri over six scenes. The paper reads this from the open-source AMD builder. This agrees with a limit of two triangles per leaf, also stated in the paper. `[Certain]` — [Barczak, Benthin, McAllister, "DGF", July 2024](https://gpuopen.com/download/DGF.pdf)
- Layout: the Zeux article says that RDNA2/3 packs two triangles per 64-byte leaf and RDNA4 uses 8-wide nodes and primitive nodes. NVIDIA formats are not documented. `[Certain]` — [Zeux](https://zeux.io/2025/03/31/measuring-acceleration-structures/)
- Intel: the driver searches for quads greedily in blocks of 16 triangles. An unpaired triangle becomes a quad with a degenerate triangle. Adjacent triangles of a quad must be in the same geometry. The guide also says to always compact a static RTAS, and that lossless compression may wait until compaction. `[Certain]` — [Intel Arc ray tracing guide](https://www.intel.com/content/www/us/en/developer/articles/guide/real-time-ray-tracing-in-games.html)
- Suballocation: an acceleration structure needs 256 B alignment, a buffer has a 64 KB minimum. Many small BLASes profit most. `[Certain]` — [NVIDIA RTXMU blog](https://developer.nvidia.com/blog/reducing-acceleration-structure-memory-with-nvidia-rtxmu/)

**Grid or terrain**
- Searches found no measured BLAS size for a regular grid or a terrain mesh from any vendor, paper or engine team. `[Certain]` that these searches found nothing; this does not prove that none exists.

### Inferences
- RTX 50 against RTX 40 on Bistro is 18.8 / 25.7 = 0.73. This agrees with a secondary report of a 0.75x footprint ([WCCFTech, snippet only](https://wccftech.com/nvidia-blackwell-rtx-50-gpu-architecture-advanced-cores-dlss-4-next-gen-gaming-technologies/)). So Blackwell probably also compresses plain triangle BLAS, not only clusters. `[Likely]`
- A regular grid should be near the good end of each vendor's range: every triangle pairs perfectly along the shared diagonal (Intel quads, AMD pairs), triangles are uniform, and there are no long thin triangles. The fp16 result does not transfer directly: grid positions with fp32 may cost more. `[Likely]`, no measured value.
- Example cost for one node, assuming 64x64 quads (8,192 triangles; 65x65 vertices). This node size is an assumption, not read from the engine. It applies the Bistro values, so treat it as an estimate. `[Guessing]`

| GPU class | Compacted | Uncompacted estimate (x2.1 to x2.4) | Ratio to 65x65 R16 heights (8.45 KB) |
|---|---|---|---|
| RTX 50 | 154 KB | 320 to 370 KB | 18x |
| RTX 20 to 40 | 211 to 217 KB | 440 to 520 KB | 25x |
| Arc B580 | 369 KB | no compaction data | 44x |
| RDNA4 | 392 KB | no compaction data | 46x |
| RDNA2/3 | 467 KB | no compaction data | 55x |

- Without compaction the terrain BLAS costs about twice as much on NVIDIA, and possibly more on AMD. "Optional compaction" is thus the largest setting available in the current design. `[Likely]`
- AMD scratch was about 50x NVIDIA scratch in 2021 (Tellusim). A streaming terrain budget must include it. Current drivers may differ. `[Guessing]`

### Gaps
- No uncompacted B/tri on current AMD and Intel, and no compaction ratio for RDNA4 or Arc.
- No grid or terrain data. To close this gap, call `vkGetAccelerationStructureBuildSizesKHR` and a compaction size query on the real node mesh on each vendor. The compacted size can only be known after a build.
- All numbers except the DXR preview below are from 2021 to 2025. I found no re-run on 2026 drivers.

## What is the measured performance of AABB procedural geometry with ray query candidate handling or intersection shaders against triangles? Is inline AABB handling slower, and by how much?

### Takeaway
I found no published benchmark of inline (ray query) AABB handling against triangles, and none for a height field. The only numbers are for other workloads and the ray tracing pipeline: about 2x slower for analytic tubes on Turing, and 1.9x to 2.4x for a software triangle decoder against a software reference. Specifications and vendor guides agree on direction: shader intersection costs more than the fixed-function triangle test.

### Cited Findings
- Analytic tube intersection through an intersection shader was about a factor of two slower than ray-triangle tests, but with a much smaller memory footprint because the primitive count is lower. Hardware: RTX cards (TITAN RTX, RTX 2060 SUPER, Turing). The authors connect the triangle speed to hardware intersection. It uses OptiX, not ray query, and tubes, not a height march. `[Certain]` — [Kern et al., line rendering comparison, arXiv December 2019](https://arxiv.org/pdf/1912.08485)
- DGF paper, AMD GPU, 1080p diffuse path tracing, one procedural primitive per triangle or per DGF triangle slot. Reference intersection shader (index + vertex buffer): Telegraph 9.39 ms, Sponza 14.30 ms. DGF decode in a shader: 17.97 ms (1.89x), 34.09 ms (2.38x). DMM proxy, testing coarse base triangles through extruded-prism AABBs: 6.79 ms (0.72x), 13.90 ms (0.97x). L2 misses were 24% and 19% of the reference for the proxy. The authors say that the proxy underestimates the computational cost of DMM. `[Certain]` — [DGF paper, July 2024](https://gpuopen.com/download/DGF.pdf)
- That proxy is the nearest published data to a "coarse AABB + shader intersection" design. A smaller BVH over coarse primitives can beat a dense BVH even though it runs shader code, as long as the shader work is small. `[Certain]` for the data, `[Likely]` for the transfer to terrain.
- DXR specification: intersection shaders are less efficient than the built-in triangle test but more flexible. A BLAS can hold only one geometry type. For non-triangle geometry, `RayQuery::Proceed()` returns control to the shader, which must evaluate the intersection and transparency itself. The shader calls `CommitProceduralPrimitiveHit()`, and the shader must check the ray extents, because the system does not. Without the no-duplicate flag an intersection shader can be invoked redundantly for one ray and primitive. `[Certain]` — [DXR specification](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html)
- Intel: intersection shaders are expensive for the same reasons as any-hit shaders. Accept duplicates. Do not traverse in the shader: make leaf AABBs and let hardware traverse. Many tight, non-overlapping AABBs are better than a few large boxes with empty space, and the saved calls can outweigh the extra traversal. Ray queries are synchronous and give up the thread sorting unit (TSU) for coherent shading. An extra shader invocation costs more than a few more triangles. Age about four years according to a snippet; the page date was not confirmed. `[Certain]` for the content — [Intel Arc guide](https://www.intel.com/content/www/us/en/developer/articles/guide/real-time-ray-tracing-in-games.html)
- Intersection shader calls increase super-linearly with AABB width (RTX 2080 Ti, neighbour search). Snippet only. `[Likely]` — [RTNN, arXiv 2022](https://arxiv.org/pdf/2201.01366)
- AMD: with procedural primitives the driver built 4-wide uncompressed inner nodes at 128 B each. `[Certain]` — [DGF paper](https://gpuopen.com/download/DGF.pdf)
- AABB input stride is 24 B (input buffer only, not BLAS cost). `[Certain]` — [wgpu documentation](https://wgpu.rs/doc/wgpu/type.BlasAABBGeometrySizeDescriptor.html)

### Inferences
- On a ray query design, each AABB hit leaves fixed-function traversal and returns to the shader loop. All 7 shaders need the candidate branch, and a height march inside it. The cost then scales with the number of AABB candidates per ray multiplied by the march length. `[Likely]`
- Expect well above the 2x of the tube case if one coarse AABB covers a whole quadtree node, because a grazing ray may run many march steps per box, and rays through empty air above the terrain still enter the box. Expect less cost for many small flat AABBs per min-max tile, but then BLAS memory increases and part of the gain goes. `[Guessing]`, no data. Prototype needed.
- Memory for AABB BLAS: one AABB per node or per tile needs far fewer leaf primitives than 8,192 triangles, so the saving should be large. Height data must then also be available to the shader (already in memory for rendering, if the resolution is the same). `[Likely]`
- Mixed scenes are possible through the TLAS: terrain as AABB BLAS, the rest as triangle BLAS. A hit group or ray query branch must still handle both. `[Likely]`; BLAS single type is from the DXR specification, Vulkan rule not read directly.
- From memory, not opened here: Vulkan's matching call is `rayQueryGenerateIntersectionEXT` with candidate type `gl_RayQueryCandidateIntersectionAABBEXT`. `[Likely]`

### Gaps
- No measured ray query against intersection shader on one scene. No published height field against triangle grid benchmark on hardware RT.
- No measured bytes per AABB in a BLAS on any vendor. Searches gave only API descriptions. Measure with `vkGetAccelerationStructureBuildSizesKHR`.
- I could not find NVIDIA or AMD best-practice text on AABB against triangles. Searches did not return it.

## Which hardware accelerates displacement micromaps? Did RTX 50 (Blackwell) drop DMM hardware? What is the current status of VK_NV_displacement_micromap, VK_EXT_opacity_micromap and DXR opacity micromaps?

### Takeaway
The extension is gone at the API level: NVIDIA says it is no longer available, and Khronos marks it deprecated by the cluster extension. Only Ada has a documented DMM engine. NVIDIA's Blackwell whitepaper lists the Opacity Micromap Engine for both Ada and Blackwell and does not mention a DMM engine at all, but I found no primary sentence that says the hardware was removed. So treat "Blackwell dropped DMM hardware" as a strong inference, not a published fact. Do not plan on DMM.

### Cited Findings

**Primary statements on the extension**
- NVIDIA, on two archived repositories (archived 13 February 2025): "The vulkan extension VK_NV_displacement_micromap is no longer available." It recommends RTX Mega Geometry as similar function with more flexibility, and points to `vk_tessellated_clusters`. The notice names no GPU generation and gives no driver version. `[Certain]` — [Displacement-MicroMap-SDK](https://github.com/NVIDIAGameWorks/Displacement-MicroMap-SDK), [Displacement-MicroMap-Toolkit](https://github.com/NVIDIAGameWorks/Displacement-MicroMap-Toolkit)
- Khronos: deprecated by `VK_NV_cluster_acceleration_structure`, and the replacement is not one-to-one but is the preferred direction for high-tessellation geometry. Extension 398, revision 2, provisional, not ratified, depends on `VK_EXT_opacity_micromap`. `[Certain]` — [Vulkan documentation](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_displacement_micromap.html)
- Older README text in the Toolkit: introduced with RTX 40 (Ada); earlier RTX cards had support with lower performance. So the extension itself was not limited to Ada hardware. `[Certain]` — [Toolkit](https://github.com/NVIDIAGameWorks/Displacement-MicroMap-Toolkit)

**Hardware**
- Ada: third-generation RT Core with a Displaced Micro-Mesh Engine, claimed up to 10x faster BVH build and up to 20x less BVH space. "Up to" vendor values; snippets of NVIDIA pages, whitepaper not opened. `[Likely]` — [NVIDIA Ada page](https://www.nvidia.com/en-gb/geforce/ada-lovelace-architecture), [Ada whitepaper 1.1](https://images.nvidia.com/aem-dam/en-zz/Solutions/technologies/NVIDIA-ADA-GPU-PROVIZ-Architecture-Whitepaper_1.1.pdf)
- Blackwell: the whitepaper says the RT Cores in both Ada and Blackwell include an Opacity Micromap Engine. It adds a Triangle Cluster Intersection Engine (which also does standard triangle tests) and Linear Swept Spheres. Special cluster engines implement new compression for geometry and BVH data. Up to 2x the triangle intersection rate of third-generation RT Cores. It describes displaced subdivision surfaces only as a Mega Geometry use. `[Certain]` — [NVIDIA RTX PRO Blackwell architecture v1.0, section "Blackwell 4th Generation RT Cores"](https://www.nvidia.com/content/dam/en-zz/Solutions/design-visualization/quadro-product-literature/NVIDIA-RTX-Blackwell-PRO-GPU-Architecture-v1.0.pdf)
- My text search of that PDF, with spacing errors removed: "DMM" 0 hits, "micro-mesh" 0 hits, "micromap" 3 hits, all in the Opacity Micromap Engine text. `[Certain]`. Figure 8 (the RT Core block diagram) is an image, and I could not view it.
- AMD and Intel: I found no evidence of DMM hardware. The DGF paper discusses DMM only as an approach with limits on topology and uses a software proxy. `[Certain]` for the paper — [DGF paper](https://gpuopen.com/download/DGF.pdf)

**Opacity micromaps (no help for terrain memory)**
- DXR 1.2 OMM: all NVIDIA ray tracing hardware supports it from the start; other vendors later; WARP also. `[Certain]` — [Microsoft, February 2026](https://devblogs.microsoft.com/directx/?p=12882)
- A secondary table of February 2026 lists OMM as N/A for RX 9000 and Arc B, hardware accelerated on RTX 40 and newer and emulated on older RTX. Snippet only. `[Likely]` — [WCCFTech](https://wccftech.com/microsoft-shader-model-6-9-dxr-1-2-dx12-enhancements-agility-sdk-1-619/amp/)
- `VK_KHR_opacity_micromap` exists as a promotion of the EXT, with significant API changes: micromaps become acceleration structure objects, an optional lossy flag, and no host builds. `VK_GEOMETRY_TYPE_MICROMAP_KHR` is in the enum. `[Certain]` — [KHR proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_opacity_micromap.html), [VkGeometryTypeKHR](https://docs.vulkan.org/refpages/latest/refpages/source/VkGeometryTypeKHR.html)

### Inferences
- The evidence for a hardware removal is: the whitepaper names OMM for both generations but not DMM, cluster engines are the new unit, and the extension was withdrawn near the Blackwell launch. This is absence of a statement. `[Likely]` that Blackwell has no exposed DMM path; `[Guessing]` about silicon.
- Even on Ada, DMM has no future API path, and it is NVIDIA-only. It is not an option for this engine. `[Certain]` for the API position.
- A height field on a triangle is what DMM expressed well. The DGF paper notes that DMM works when the content fits a coarse base mesh with scalar displacement. Terrain fits, so the loss is real for this use case. `[Likely]`
- OMM removes any-hit calls for alpha. It does not reduce the terrain BLAS. `[Likely]`

### Gaps
- I could not find a driver version or a release note that removed the extension.
- I found no NVIDIA sentence that says "Blackwell has no DMM engine". I could not read Figure 8. My searches also found no secondary report of a removal.
- The gpuinfo device list returned an empty table, probably because it loads dynamically, so it does not show who reports the extension: [vulkan.gpuinfo.org](https://vulkan.gpuinfo.org/listdevices.php?extension=VK_NV_displacement_micromap). Inconclusive.
- I could not confirm whether DXR ever exposed DMM (through NVAPI or otherwise).
- The date of the Blackwell PRO whitepaper was not read. I also did not separately read the consumer version.

## What are the published memory and build-time gains for RTX Mega Geometry, cluster and partitioned acceleration structures? Which generations are supported? Is there an AMD or cross-vendor equivalent? Would cluster templates suit a shared-topology grid?

### Takeaway
The large published gains are for rebuild-heavy animated geometry; the only static figure is 27% less memory on an RX 9070. DXR now has a cross-vendor preview (28 October 2026), but in Vulkan I found only `VK_NV_*` and the provisional `VK_AMDX_dense_geometry_format`. Templates suit a shared grid well for build speed, but no source shows that they reduce the size of the traced CLAS.

### Cited Findings

**DXR Clustered Geometry and Partitioned TLAS preview, 28 October 2026** — `[Certain]` — [Microsoft DirectX blog](https://devblogs.microsoft.com/directx/clas-ptlas-preview/)
- Agility SDK 1.722.0-preview, Shader Model 6.10 preview, experimental features and Windows Developer Mode needed. Inline raytracing has a matching RayQuery flag and position intrinsics.
- CLAS limit: 256 triangles and 256 vertices. A Cluster BLAS is built from CLAS references, and TLAS instances reference the BLAS. Changing a CLAS invalidates its Cluster BLAS.
- Templates hold topology and non-position data. Instantiation with positions gives a CLAS. A template cannot be traced.
- Position options: float and normalized integer formats, mantissa truncation, and `COMPRESSED1` (shared anchor and power-of-two scale per cluster, packed integer offsets).
- Support: NVIDIA all RTX; AMD Radeon RX 9000; Intel Arc B-Series, Core Ultra 200V and Series 3; Qualcomm Snapdragon X2 targeted for Q1/Q2 2027; WARP. Older hardware is undetermined.
- Sample on RX 9070. Microsoft says it is a test playground, and that games should not expect the same gains:

| Metric | Traditional BLAS rebuild | Clustered |
|---|---|---|
| Frame rate | 1.2 FPS | 79 FPS |
| Per-frame AS build | 788 ms | 3.11 ms |
| Per-frame ray tracing | 72.6 ms | 9.27 ms |
| Static AS memory | 723 MiB | 527 MiB |
| Animated AS memory | 6,794 MiB | 244 MiB |
| Total AS memory (with TLAS) | 7,519 MiB | 774 MiB |
| Static build scratch | 1.83 MiB | 526 MiB |
| Animated build scratch | 8.25 MiB | 179 MiB |

- PTLAS domino sample: full TLAS rebuild 2.88 ms, partitioned with 4,702 writes 0.79 ms, trace slightly slower. No memory figure.

**NVIDIA**
- Mega Geometry runs on all RTX GPUs from Turing. Vulkan vendor extensions for clusters and PTLAS, DXR through NVAPI, OptiX 9.0. Blackwell's cluster engines and compression reduce VRAM for typical Nanite-type scenes by several hundred MB. Clusters in Nanite-type LOD have about 128 triangles. `[Certain]` — [Blackwell PRO whitepaper](https://www.nvidia.com/content/dam/en-zz/Solutions/design-visualization/quadro-product-literature/NVIDIA-RTX-Blackwell-PRO-GPU-Architecture-v1.0.pdf)
- `VK_NV_cluster_acceleration_structure`: extension 570, not ratified, revision 4 of 2025-07-16 (OMM build flag), needs `VK_KHR_acceleration_structure`. Limits come from `maxVerticesPerCluster` and `maxTrianglesPerCluster`. `[Certain]` — [Vulkan documentation](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_cluster_acceleration_structure.html)
- Driver 572.16 (30 January 2025) or later; `vk_lod_clusters` needs Vulkan SDK 1.4.341.0. Host-side CLAS size estimates are often 3x to 5x above actual. No B/tri given. Triangle indices shrink about 3x in its own storage with meshoptimizer's codec (this is scene storage, not traced data). `[Certain]` — [vk_lod_clusters](https://github.com/nvpro-samples/vk_lod_clusters)
- `VK_NV_partitioned_acceleration_structure`: not ratified; aims at reuse of unchanged TLAS parts and more instances. Shaders see it as a normal TLAS. `[Certain]` — [Proposal](https://github.com/KhronosGroup/Vulkan-Docs/blob/main/proposals/VK_NV_partitioned_acceleration_structure.adoc), [refpage](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_partitioned_acceleration_structure.html)
- NVIDIA's blog of February 2025 lists the samples and gives no gain numbers. `[Certain]` — [NVIDIA blog](https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples)
- Alan Wake 2 (snippets and aggregators, conflicting): about 300 MB less VRAM per NVIDIA at GDC 2026; 5% to 20% performance per NVIDIA as reported ([Igor's Lab](https://www.igorslab.de/die-geforce-plattform-im-wandel-ki-rekonstruktion-raytracing-und-neue-renderingstrukturen-als-grundlage-der-naechsten-spielegeneration-gdc-2026/4/)); 1 GB and 13% on RTX 4090 ([gaming-st, snippet only](https://gaming-st.com/news/rtx-mega-geometry-vram-test-may2026/)). Tom's Hardware's test page gave no readable text: [Tom's Hardware](https://tomshardware.com/pc-components/gpus/testing-nvidias-rtx-mega-geometry-tech-vram-reducing-tech-a-leap-forward-for-path-traced-rendering). `[Likely]` for "hundreds of MB"; the exact values are unresolved.

**Templates** — `[Certain]`
- A template is a partly built CLAS without position data, smaller for that reason. It cannot be traced or used as a build base. Positions are given at instantiation; ClusterID and geometry index can be offset. It has an optional bounding limit of six floats, and all instantiated clusters must lie inside it. This combines with mantissa-bit zeroing to reduce vertex storage. — [Khronos proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_NV_cluster_acceleration_structure.html)
- Precedent: `vk_tessellated_clusters` pre-generates templates per tessellation pattern with PREFER_FAST_TRACE and instantiates them each frame with displaced positions. No memory or build numbers published. — [vk_tessellated_clusters](https://github.com/nvpro-samples/vk_tessellated_clusters)

**AMD DGF**
- `VK_AMDX_dense_geometry_format`: provisional. 128 B blocks, max 64 triangles and 64 vertices. 24-bit anchors, 1 to 16 bits offsets per vertex, power-of-two scale. Attributes such as UVs and normals must be in sideband buffers. Device builds only; updates may change only positions. `[Certain]` — [Vulkan specification chapter](https://docs.vulkan.org/spec/latest/chapters/VK_AMDX_dense_geometry_format/dense_geometry_format.html), [proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_AMDX_dense_geometry_format.html)
- AMD: "future GPU architectures" get direct support. The proposal allows a fallback layer that decodes and builds a conventional structure. AMD names no GPU with native support. First driver: Adrenalin 25.10.25.02 preview. `[Certain]` — [GPUOpen, August 2025](https://gpuopen.com/learn/dense-geometry-format-amd-vulkan-extension/), [GPUOpen, February 2025](https://gpuopen.com/learn/problem_increasing_triangle_density/)
- A forum list of RX 9070 and RX 7000 models as supported was disputed in the same thread. Snippet only. `[Guessing]` — [Guru3D](https://forums.guru3d.com/threads/amd-software-adrenalin-edition-25-10-25-02-for-expanded-vulkan-extension-support.457312/)
- Input data: 2.9 to 3.73 B/tri for bike, crab, powerplant. The sources differ per model: the paper gives crab 2.9, bike 3.73, powerplant 3.05; GPUOpen gives bike 3.73, crab 3.05, powerplant 3.39. `[Certain]` for the range — [GPUOpen](https://gpuopen.com/learn/problem_increasing_triangle_density/), [DGF paper](https://gpuopen.com/download/DGF.pdf)
- BVH projection (not measured hardware): compacted AMD BVH 54.9 to 77.3 B/tri. DGF leaves cut leaf data 6x to 10x and the total about 50%. With wider leaves, totals are 11 to 12 B/tri at 8 triangles per leaf and 5 to 7 B/tri at 64. `[Certain]` — [DGF paper Table 4](https://gpuopen.com/download/DGF.pdf)
- The paper states that small grid topologies do not need adjacency information and can be stored more compactly (Benthin et al. 2021, offsets from a bilinear patch). DGF itself does not use this. `[Certain]` — [DGF paper](https://gpuopen.com/download/DGF.pdf)

**Vulkan cross-vendor status**
- The visible Khronos proposal list shows `VK_AMDX_dense_geometry_format`, and no KHR or EXT cluster or partitioned proposal. The listing was cut off by "View all files". `[Likely]` — [Vulkan-Docs proposals](https://github.com/KhronosGroup/Vulkan-Docs/tree/main/proposals)
- A search found no Khronos announcement of a KHR or EXT cluster extension as of November 2026. `[Likely]` that none is public.

### Inferences
- For a static or rarely changed terrain node, read the static row, not the headline: 723 to 527 MiB is 27% less on RX 9070, and scratch goes up about 290x in that test. A burst-build streaming system must budget for this. `[Likely]`; the sample is not terrain.
- Template fit: a grid has one topology for all nodes of a resolution, which matches the template model exactly. Square patch limit under 256 triangles: 11x11 quads = 242 triangles, 144 vertices. A 64x64-quad node is then 36 clusters plus one Cluster BLAS. `[Likely]`; the arithmetic is `[Certain]`, the node size is an assumption.
- Templates mainly save build time: a CLAS still needs its own positions and bounds because the template has none. So the memory saving comes from cluster format and position quantization, not the template. `[Likely]`, no published number. Height-only variation cannot be declared to the API: three-component positions are still given, even if X and Z repeat across nodes.
- Bounding limit + mantissa zeroing suit terrain, since a node's bounding box and vertical precision are known. `[Likely]`
- Vulkan reach today: NVIDIA Turing and newer only. No AMD or Intel path through clusters, so the engine still needs the plain BLAS path. DXR's preview shows that AMD and Intel drivers can implement the model, so a later Vulkan KHR or EXT extension seems possible. `[Guessing]`, no announcement.
- PTLAS could reduce TLAS update cost if each quadtree node is an instance. It does not reduce BLAS memory. `[Likely]`
- DGF today probably gives no trace-memory saving on a fallback driver, since a conventional structure is built. `[Likely]`. It may still save storage and build input. Grid positions with a shared anchor should compress well, but no terrain data was found.

### Gaps
- No published B/tri for CLAS on any GPU, and no measured grid template result. Measure the actual size returned by the build; do not use the host estimate.
- No measured static-geometry CLAS against compacted BLAS on NVIDIA.
- I could not confirm whether the Vulkan cluster extension works with ray query from the proposal text I read. The DXR preview has a RayQuery flag, and the Vulkan extension lists interaction with the ray tracing pipeline. Check the SPIR-V extension `SPV_NV_cluster_acceleration_structure` before relying on it.
- Reports of a "Mega Geometry 2.0" ([remio.ai](https://www.remio.ai/post/nvidia-rtx-mega-geometry-2-0-trades-fixed-geometry-for-on-demand-streaming)) were not verified, and no primary source was found.
- AMD's driver release notes for native DGF hardware were not found.

## Does any API have a true height-field primitive? Is one proposed?

### Takeaway
No such primitive was found. Vulkan's geometry enum has none and no proposal by that name is visible; DXR preview material does not mention one. The nearest options are AABB + shader, the withdrawn DMM, and clusters with quantized positions.

### Cited Findings
- `VkGeometryTypeKHR` values: triangles, AABBs, instances, `SPHERES_NV`, `LINEAR_SWEPT_SPHERES_NV`, `DENSE_GEOMETRY_FORMAT_TRIANGLES_AMDX`, `MICROMAP_KHR`. No height field. `[Certain]` — [Vulkan documentation](https://docs.vulkan.org/refpages/latest/refpages/source/VkGeometryTypeKHR.html)
- Visible Khronos proposal names have no match for "height". The listing is cut off. `[Likely]` — [Vulkan-Docs proposals](https://github.com/KhronosGroup/Vulkan-Docs/tree/main/proposals)
- The DXR specification describes procedural AABB primitives with application-defined intersection. The October 2026 preview has triangle clusters only, and it does not mention AABB or displacement. `[Certain]` for what I read — [DXR spec](https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html), [DXR preview](https://devblogs.microsoft.com/directx/clas-ptlas-preview/)
- A new fixed-function primitive has precedent: Blackwell added Linear Swept Spheres, replacing custom intersection for curves. `[Certain]` — [Blackwell PRO whitepaper](https://www.nvidia.com/content/dam/en-zz/Solutions/design-visualization/quadro-product-literature/NVIDIA-RTX-Blackwell-PRO-GPU-Architecture-v1.0.pdf)
- Height field research on GPU uses software casting with min-max structures, not an API primitive. `[Likely]`, snippet only — [Lux and Froehlich, stacked height fields](https://www.uni-weimar.de/de/medien/professuren/vr/research/scivis/vrc-shf011/)

### Inferences
- The options for the engine's Vulkan path are: (1) triangle BLAS with always-on compaction and a lower ray tracing LOD; (2) AABB BLAS over min-max tiles with a ray query loop in the 7 shaders; (3) on NVIDIA only, cluster templates. Only (1) and (2) are cross-vendor in Vulkan. `[Likely]`
- Hardware vendors seem to be moving toward compressed triangle clusters, not toward height fields, so a native primitive is not likely soon. `[Guessing]`

### Gaps
- I read only the first 100,000 characters of the 621,000-character DXR specification, so I did not read the enum `D3D12_RAYTRACING_GEOMETRY_TYPE` directly.
- I did not check Metal, OptiX primitive lists or console APIs. Console documentation is not public.
- Absence in a search is not proof that no private proposal exists in Khronos or Microsoft working groups.
