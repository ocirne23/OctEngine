# GPU height-field ray tracing and hierarchical acceleration structures

Source status (read this first):

| Source | Status |
|---|---|
| Dick, Krueger, Westermann 2009; Oh, Ki, Lee 2006; Dummer 2006; Thonat et al. RMIP 2023; Jung et al. 2020; NVIDIA Micro-Mesh slides | Full text read |
| Policarpo and Oliveira, GPU Gems 3 ch. 18; Hoetzlein 2025 | Read through a summarising fetch tool, NOT line by line |
| Tevs et al. 2008 | ABSTRACT ONLY. The PDF is above the fetch size limit. Facts come from the abstract and from papers that cite it |
| Thonat et al. TFDM 2021 | Abstract, plus the numbers that RMIP reports about it |
| Baboud et al. 2012; Timakov and Mikhaylyuk 2023; Ogaki 2023 | Abstract only |
| Drobot, GPU Pro 2010 (QDM) | NOT READ. Citation and second-hand forum text only |
| Lumen SIGGRAPH 2022 deck | Search excerpt only |

## What do Tevs, Dick, Policarpo/Oliveira, Dummer, Drobot and newer papers propose?

### Takeaway
There are three families: fixed-step search (relief mapping), per-texel precomputed safety shapes (cone, relaxed cone, Baboud safety shapes), and bound hierarchies (maximum or min-max mips: Oh, Tevs, Dick, TFDM, RMIP). Only the hierarchies are exact without an offline bake, and only they are cheap to rebuild at run time. Current work adds tighter bounds per ray (RMIP) and places the height-field traversal under a hardware TLAS/BLAS as custom geometry.

### Cited Findings

Fixed-step and safety-shape methods:
- Relief mapping = linear search, then binary search. Linear search "is also prone to aliasing in the presence of thin structures" — [Policarpo and Oliveira, GPU Gems 3 ch. 18](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping)
- Cone step mapping (CSM): one cone per texel, tip on the surface. The value stored is `Cone_Ratio` (range 0..1). The step is `dsc = ht / (iz/Cone_Ratio - z)`. The author stores sqrt(ratio) in 8 bits because most values cluster at the low end. The RGBA layout is height, sqrt(ratio), dF/dx, dF/dy — [Dummer 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/ConeStepMapping.pdf)
- The cone is conservative. With cone steps only, the ray approaches the surface asymptotically. The `_exact` variant adds a step of 1/texture_size per iteration so that the ray pierces the surface, then backs off one step (this adds a one-texel feature-size limit) — [Dummer 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/ConeStepMapping.pdf)
- Relaxed cone stepping relaxes the cone radius rule to get wider cones, then finishes with binary search. The listing uses 15 cone steps + 6 binary steps — [GPU Gems 3 ch. 18](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping)
- Baboud et al. store one safety shape per texel, inside which a ray cannot cross the surface twice, for static height fields. A companion text claims little memory overhead and better accuracy and speed than existing methods. Numbers not verified — [Baboud, Eisemann, Seidel, TVCG 2012](https://graphics.tudelft.nl/~eisemann/publications/Baboud2011HeightField/index.html); [Baboud and Eisemann, record](https://publications.graphics.tudelft.nl/papers/390)

Hierarchies:
- Oh et al. (pyramidal displacement mapping, 2006): each pyramid texel stores the highest height of the 4 children. Traversal goes root to leaf. A ray stops at the node boundary when an advance would cross it ("node crossing"). This predates Tevs — [Oh, Ki, Lee 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Pyramidal-Displacement-Mapping.pdf)
- Tevs et al. (maximum mipmaps, I3D 2008): a hierarchical structure with negligible precomputation, view-dependent LOD, and hierarchical ray-stepping for large height fields — [abstract on hgpu](https://hgpu.org/?p=1125). The PDF is at [pure.mpg.de](https://pure.mpg.de/rest/items/item_1325622_5/component/file_3590464/content) (not read).
- Dick et al. (2009) call their traversal "more efficient and numerically stable" than Tevs. Mirror the ray so that Dir x,y >= 0, replicate the loop for 8 sign cases to reduce branches and registers, step texel to texel, and force the crossed edge coordinate to the exact integer to prevent infinite loops from round-off. An ascending ray tests the texel entry height, a descending ray tests the exit height. Start at the 2x2 level; on a hit step down, on a miss move to the exit point and step up when the ray leaves the 2x2 block — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Dick et al. choose the max pyramid over cone maps because it builds on the GPU when a tile becomes visible and adds nothing to disk. Cone maps need an offline pass and "tremendous amounts of disk space and disk bandwidth" at several hundred gigasamples — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Quadtree displacement mapping (Drobot): chapter in GPU Pro, pp. 117-148. It is listed in the [gems database](https://www.gameenginegems.com/gemsdb/article.php?id=1065). A forum post says that it needs a min-built mip chain ([Polycount, second-hand](https://polycount.com/discussion/comment/1788414)).
- Hi-Z tracing in screen space (Uludag, GPU Pro 5) is the same min/max mip walk on a depth buffer. Forum reports say that the chapter text and snippets disagree on min versus min/max and that the code was reconstructed by readers — [gamedev.net thread](https://www.gamedev.net/topic/658702-help-with-gpu-pro-5-hi-z-screen-space-reflections/)

Newer:
- TFDM (Thonat et al. 2021) maps a displacement-specific structure onto the mesh instead of tessellating it. A min-max mip gives bounds, and affine arithmetic gives 3D boxes — [TFDM page](https://perso.telecom-paristech.fr/boubek/papers/TFDM/); mechanism described in [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- RMIP (Thonat et al. 2023) names three reasons why a min-max quadtree is slow: a fixed tree that ignores the data, square nodes against an anisotropic ray footprint, and only yes/no pruning from loose boxes. It stores min-max for all 2^i x 2^j rectangles; a query takes 4 fetches (a 2D range-minimum decomposition) and returns bounds for any axis-aligned rectangle. Traversal goes front to back and stops at the first hit — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- Ogaki 2023 traverses and tests in texture space with nonlinear rays (degree-2 rational functions) — [abstract](https://history.siggraph.org/?p=214452)
- Jung et al. 2020 use dynamic programming with maximum mips for soft shadows on curved terrain, and pre-displace a mesh to give rays a start position — [Jung et al. 2020](https://arxiv.org/pdf/2005.06671)
- A differentiable height-field path tracer uses maximum mips in both the forward pass and the discontinuity search — [SIGGRAPH 2023 paper](https://www.dgp.toronto.edu/~hsuehtil/pdf/diff_hpt.pdf)

### Inferences
- [Likely] For a planar height field, most RMIP cost does not apply: the Newton inversion and the curve splitting exist because of curved base meshes. The ray is a straight line in texture space, so only the RMIP bound structure and front-to-back split remain useful.
- [Likely] The engine's existing cone-march stays usable for sun shadows as a first coarse pass, but a cone value is valid only for the data it was baked from. A camera-centred cascade that scrolls must treat out-of-window terrain conservatively.

### Gaps
- Tevs full method (bilinear patch intersection, step tables, clipmap text): not verified. My recollection, NOT verified: it refines on the finest level with a bilinear or triangle test.
- Drobot chapter: no primary text obtained; no convergence numbers verified.
- Policarpo et al. 2005 relief mapping paper and Policarpo and Oliveira 2006 non-height-field paper: not read directly.

## What are the memory figures per texel, and the measured step counts and times?

### Takeaway
A max-only chain adds 1/3 of the base format. A 16-bit max pyramid over 16-bit height totals 2.67 B/texel (arithmetic; Dick states "16 bits/sample + mipmaps" and "1/3"). RMIP numbers suggest that a TFDM-type min-max mip costs about 5.3 B/texel. Measured traversal: Dick reports at most about 50 steps on a 512 tile, against up to 1024 without the pyramid, and a speed gain of about 5.

### Cited Findings

Memory:
- The max pyramid "only introduces additional GPU memory requirements of 1/3". Tile format: 16 bits per sample, plus mips; tile N = 512 — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Comparison in the same paper: triangle list 96 bits/triangle, against the height field. Ray-caster geometry memory on Vorarlberg: almost constant at about 128 MB for about 1 TB of source data. The rasterizer used more and fluctuated. On the smoother Utah set, the rasterizer used LESS memory and was faster — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- CSM: 4 x 8-bit channels (height, cone, 2 slopes) = 4 B/texel. Mips are not part of the method — [Dummer 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/ConeStepMapping.pdf)
- Relaxed cones: one extra channel per texel; ratios clamped to 1.0 for integer storage — [GPU Gems 3 ch. 18](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping)
- Full RMIP is N^2 (1 + log2 N)^2 values, which the authors reduce to the order of a mip chain by building it at low resolution and texel-marching below that — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- TFDM data structure: 21 Mb for a 2k map, 85 Mb for a 4k map. RMIP at x0.012-0.048 (low-res), x1.3-1.6, or x7.6-30 (high-res) of that — [RMIP 2023, Table 1](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- Micro-Mesh hardware format: 64 micro-triangles in 64 bytes, 256 in 128 bytes, 1024 in 128 bytes; blocks of 512 or 1024 bits; up to level 5 — [Vulkan extension docs](https://hackage.haskell.org/package/vulkan-3.26.1/docs/Vulkan-Extensions-VK_NV_displacement_micromap.html); [NVIDIA slides](https://developer.download.nvidia.com/ProGraphics/nvpro-samples/slides/Micro-Mesh_Basics.pdf)

Traversal and time (hardware and resolution given per item):
- Dick: GeForce GTX 280 1 GB, Core 2 Quad Q9450, far plane 600 km, tolerance 2/3 pixel. Heat map scale 0-50 steps at the 1920x1080 scene of Figure 1; "only relatively few rays come close to 50"; "up to 1024 steps" expected without the pyramid at 512x512. Speed gain about 5 from the pyramid, about 2 from tile-to-tile occlusion — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Dick: about 30 fps at 1920x1080 on Vorarlberg (1 m DEM, 56 x 85 km, 860 GB). At 1280x1024 about twice the speed of a rasterizer that delivers about 350 M triangles/s. Utah (5 m): about 40-50 fps, slower than the rasterizer. Performance scales linearly with screen pixels and does not depend on the data set — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Follow-up: hybrid rasterize + ray-cast per tile, reported as 2-4x over either alone at high resolution — [TUM page, abstract level](https://www.cs.cit.tum.de/cg/research/publications/2010/gpu-aware-hybrid-terrain-rendering)
- Oh: Radeon X1900 512 MB, 512x512 image, 2 triangles. 480.06 fps at a 256^2 map, 437.29 fps (432.29 in the body text; the paper disagrees with itself) at 512^2, 421.35 fps at 1024^2, 375.53 fps at 2048^2. Against 50.13 fps for 131,072 micro-polygons. Pyramid build about 2.3 ms (on a GeForce 7800 GTX path, map size not stated) — [Oh et al. 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Pyramidal-Displacement-Mapping.pdf)
- CSM: `_exact` is about the speed of relief mapping but more accurate, and needs no per-map tuning. `_fixed` = 10 iterations is fastest, and a fur map looks bad at 10. GeForce 6600. No fps table — [Dummer 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/ConeStepMapping.pdf)
- Relaxed cones: 15 + 6 steps "seems to be sufficient"; preprocess O(n^2), up to 128 steps per ray in the preprocess shader; no fps or GPU figures in the chapter — [GPU Gems 3 ch. 18](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping)
- Jung: GTX 970M, 1920x1080, tile N 1024-2048: 5.4 ms (N = {5,5,5}) and 6.2 ms ({5,5,5,3}) against 18.2 ms for 100 uniform steps, so "up to 237%" faster. Iris 540: 50.5 Hz at 1920x1080. Pre-displacing the mesh reduces view ray steps by nearly 100x for comparable accuracy (figure taken from a fetch-tool summary of the introduction) — [Jung et al. 2020](https://arxiv.org/pdf/2005.06671)
- RMIP: Ryzen 9 5950X, RTX 3080 10 GB, Vulkan Ray Tracing custom geometry. Average 5x over TFDM at equal memory (3x standard deviation). About 10x fewer traversal steps; heat-map scale goes to 200 steps for TFDM. Build about 5 ms for a 4k map; parameter changes below 1 ms. Showcase: 11x faster, 3x less memory than TFDM. Render resolution for Table 1 is not stated in the main text — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)

RMIP Table 1 extract, GPU render speed-up and memory relative to TFDM (x1):

| Scene (map, tiling) | TFDM mem / time | Uniform tessellation mem / speed | RMIP mid mem / speed |
|---|---|---|---|
| Alien Sphere (2k, 6x8) | 21 Mb / 56 ms | x186 / x91 | x1.3 / x3.4 |
| Creature (4k, 1x1) | 85 Mb / 115 ms | x40 / x232 | x1.6 / x7.8 |
| Ninja Head (2k, 5x5) | 21 Mb / 182 ms | x103 / x340 | x1.3 / x9.2 |
| Desert Tire (4k x 3, 252 and 6x1) | 256 Mb / 606 ms | x10 / x1151 | x0.32 / x11 |
| Glass (2k, 1x1) | 42 Mb / 41 ms | x51 / x12 | x1.3 / x2.7 |

Source: [RMIP 2023, Table 1](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf). The tessellation rate of "Uniform" is not in the main text (the supplement holds the visual comparison).

- Hoetzlein 2025, RTX 4090, 2048^2, 1 spp: 20-60 ms per beauty image, 80-400 Mrays/s, about 108 extra bytes per base face. Two beauty rows, RMIP / PDM / Micro-Mesh: Asteroid 145.6 / 60.8 / 48.3 ms; Tree Stump 166.2 / 18.1 / 19.5 ms. The column alignment was ambiguous in the extract; verify before quoting. Preprint, author comparison — [arXiv 2502.02011](https://arxiv.org/html/2502.02011v1)

### Inferences
Arithmetic, base sizes only, 2048^2 = 4.19 M texels. [Certain as arithmetic; formats are candidates, not recommendations from a paper]

| Layout | Bytes per texel | 2048^2 |
|---|---|---|
| RGBA32F, no mips (cascade format as given) | 16 | 67 MB |
| RGBA32F with full mips | 21.33 | 89 MB |
| R32F height + R32F max mips | 5.33 | 22 MB |
| R16 height + R16 max mips | 2.67 | 11 MB |
| R16 height + RG16 min-max mips (level 0 = height) | 3.33 | 14 MB |
| RG16 min-max, full chain with level 0 duplicated | 5.33 | 22 MB |
| R32F height + RG32F min-max mips | 6.67 | 28 MB |

- [Likely] TFDM 21 Mb at 2k and 85 Mb at 4k match RG16 with a full chain: 4 B x 4/3 x 4.19 M = 22.4 MB = 21.3 MiB; x 16.8 M = 85.3 MiB. The paper text read does not state the format.
- [Guessing] Glass at 42 Mb for a 2k map would match RG32F with mips (42.7 MiB).
- [Likely] A height-field method is about 2 orders below a uniform tessellated BLAS in memory, and it is also slower: RMIP is about 4x to 100x slower than uniform triangles in Table 1 (Glass 12/2.7 = 4.4x; Creature 232/7.8 = 30x; Desert Tire 1151/11 = 105x). This is the trade the engine would make. The uniform meshes may be lower quality than the map, so the true gap is uncertain.
- [Likely] RMIP authors state the limit plainly: with moderate-resolution maps, limited tiling and static content, "pre-tessellation, pre-displacement, and hardware ray tracing remains the method of choice" — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf). Streamed terrain differs: tiles change, and terrain is exactly a planar height field, but this must be measured.
- [Likely] Dick's Utah result warns that the ray-cast path can lose both speed and memory on smooth terrain when the alternative is an adaptive mesh. Generated terrain with an adaptive mesh may be nearer to that case.

### Gaps
- Tevs measured steps and fps: not verified.
- Baboud per-texel bytes and timings: PDF host refused the connection; not verified.
- No modern (2018+) paper found with a measured plain-terrain compute traversal cost in ms per Mray at game scale.
- No verified bytes-per-triangle figure for a driver-built BLAS over a terrain grid in this research. Measure with the engine's own `vkGetAccelerationStructureBuildSizesKHR` values.

## How do they handle grazing rays, thin ridges, watertightness, normals, and rays leaving a clipmap or cascade?

### Takeaway
A max or min-max walk does not miss a ridge at its stored resolution, whatever the angle, and its cost is highest at grazing angles. Cone and fixed-step methods trade exactness for speed there. Watertightness inside one regular grid is automatic for texel-box walking; it becomes a problem only between LOD levels. No paper found treats rays that leave a camera-centred cascade.

### Cited Findings

Grazing:
- Dick: frame rate peaks when the camera looks down, because few steps are needed; sky and terrain coverage cause the fluctuation — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- CSM: rays pointing straight down arrive in one iteration; near-horizontal rays need many more. Listed future work: elliptical or slope-aware cones — [Dummer 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/ConeStepMapping.pdf)
- Oh: linear search misses intersections at grazing angles and binary search is not a complete fix; the pyramid gives correct images "even at steep grazing angles" — [Oh et al. 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Pyramidal-Displacement-Mapping.pdf)
- When there are no peaks to skip, a hierarchy gives little: in the discontinuity search of the differentiable renderer the walk goes along every texel in the worst case — [SIGGRAPH 2023 paper](https://www.dgp.toronto.edu/~hsuehtil/pdf/diff_hpt.pdf)
- RMIP: rays near-parallel to the displacement direction have constant cost; anisotropic ray footprints are the case where square mips give loose bounds — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- PDM quality degrades at grazing angles when the sample spacing is large, and it suggests jitter for features thinner than the spacing — [arXiv 2502.02011](https://arxiv.org/html/2502.02011v1)

Thin features and accuracy:
- Linear search aliases on thin structures; conservative cone steps stop early and give wrong coordinates; relaxed cones with binary search greatly reduce this but can still distort at low step counts — [GPU Gems 3 ch. 18](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping)
- CSM `_fixed` shows darkening on silhouettes and needs more steps for high-frequency maps — [Dummer 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/ConeStepMapping.pdf)
- Cone-map mips: conventional filtering gives wrong intersections. Use a manual minimum or nearest sampling — [GPU Gems 3 ch. 18](https://developer.nvidia.com/gpugems/gpugems3/part-iii-rendering/chapter-18-relaxed-cone-stepping-relief-mapping)
- Dick: with screen-space error below one pixel, piecewise constant height gives no visual quality loss, which avoids the costly bilinear intersection of Tevs. Dick's tile LOD pyramid is built by AVERAGING 2x2 samples (the max pyramid exists only inside one tile) — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Oh bilinear reconstruction: from the point-sampled hit, sample bilinear height half a texel back and forward along the ray and interpolate; if the forward sample shows no crossing, advance and re-traverse. Oh notes that the bilinear path is what makes it slower — [Oh et al. 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Pyramidal-Displacement-Mapping.pdf)
- Jung: inaccurate view-ray hits gave artifacts, which they combat by pre-displacing a mesh. Approximating maximum delta heights along shadow rays with a max mip also gives artifacts, "largely mitigated" by starting at finer mip levels and concatenating several runs to cover long shadows — [Jung et al. 2020](https://arxiv.org/pdf/2005.06671)
- Lumen: a heightfield representation was tried for software tracing: fast with high spatial resolution, but unreliable coverage caused leaking; Mesh SDFs were chosen (seen through a search excerpt; deck not read) — [Wright et al. 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf)

Watertightness:
- Dick: a per-texel box walk with the exact edge snap; no numeric gap between texels. Tiles are "visually continuous" through the error tolerance; the paper describes no separate crack treatment for ray-casting — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Jung stitches visible tiles into one texture to prevent seams — [Jung et al. 2020](https://arxiv.org/pdf/2005.06671)
- RMIP: prisms give true space subdivision (neighbouring bounds do not overlap), and every location inside a prism can be projected back to texture space — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- PDM: no holes when the minimum displacement exceeds the sample spacing — [arXiv 2502.02011](https://arxiv.org/html/2502.02011v1)
- Micro-Mesh: adjacent base triangles may differ by a factor of 2 along an edge, with edge decimate flags, and the hardware ensures watertight results — [NVIDIA slides](https://developer.download.nvidia.com/ProGraphics/nvpro-samples/slides/Micro-Mesh_Basics.pdf)

Hit normals:
- Dick: partial derivatives by central differences at the hit texel (forward or backward at tile borders); used for texture footprint — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- CSM stores slopes per texel so the depth scale can change — [Dummer 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/ConeStepMapping.pdf)
- Oh stores an averaged normal in the RGB of every pyramid level — [Oh et al. 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Pyramidal-Displacement-Mapping.pdf)
- Jung keeps a separate normal texture per tile — [Jung et al. 2020](https://arxiv.org/pdf/2005.06671)
- RMIP reconstructs micro-geometry locally per texel at the hit — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)

Rays leaving a domain:
- Dick: a ray ends at the tile edge and the fragment is discarded; the next tile in front-to-back order handles that pixel. A depth-based tile ID pass prevents more than one hit per pixel. This is a primary-ray design with rasterized tile bounds — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)

### Inferences
- [Likely] Dick's "piecewise constant is enough" rests on a sub-pixel error for primary rays. It does not transfer to secondary rays. A box surface gives stair steps in reflections and self-intersection at glancing light, so shadow, AO and GI rays need a bias or a linear reconstruction at the last step.
- [Likely] Height quantization should round max up and min down so that bounds stay conservative. R16 over 8192 m gives 0.125 m per step; over 1024 m, 1.6 cm (arithmetic).
- [Likely] A coarse cascade built by averaging can remove a ridge that the near level holds, which gives leaks at cascade transitions. A max-built coarse level (or both) prevents missed occlusion, at the cost of false hits above real terrain.
- [Likely] A ray from cascade 0 into cascade 1 needs a start offset, because the surface height differs between levels. Without one, a ray can start below the coarse surface and hit at once. The transition design is an engine question; none of the papers read solves it.
- [Guessing] Lumen's "unreliable coverage" came from general meshes projected into heightfields (overhangs). Terrain does not have that defect, but terrain with caves or arches would.

### Gaps
- No paper found on traversal across nested camera-centred clipmap levels for secondary rays.
- No paper found that measures miss rate on thin ridges for cone versus max mip at game-terrain scale.
- Dick does not quantify grazing-case step counts beyond the heat map.

## How do they handle multiple resolution levels, cascades, or streamed tiles?

### Takeaway
Tile trees are the proven pattern (Dick: one pyramid per tile, built on the GPU at upload). LOD is per tile by screen-space error, or per ray by limiting the walk depth (Oh, TFDM, RMIP). Stitching into one texture works but gave frame-time spikes (Jung).

### Cited Findings
- Dick: tile tree (each tile covers 4 finer tiles), N = 512, asynchronous IO thread, tiles selected per frame by frustum and screen-space error (2/3 pixel), front-to-back. Only tiles not already in graphics memory upload; on upload the compressed mesh is rasterized into a 16-bit height texture, and the max pyramid is built on the GPU — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf)
- Dick notes that a rasterizer wins on low and medium resolution data, and that per-tile choice of method is possible by triangle count and distance — [Dick et al. 2009](https://www.cs.cit.tum.de/fileadmin/w00cfj/cg/Research/Publications/2009/GPU_Ray-Casting/EG09AreasTerrain.pdf); the follow-up builds it — [TUM 2010](https://www.cs.cit.tum.de/cg/research/publications/2010/gpu-aware-hybrid-terrain-rendering)
- Oh: LOD = stop the walk at a max level. The surface flattens as the limit falls (shown for limits 8 down to 3) — [Oh et al. 2006](https://faculty.digipen.edu/~gherron/references/References/SurfaceDetail/Pyramidal-Displacement-Mapping.pdf)
- Tevs: view-dependent LOD is claimed at abstract level — [abstract](https://hgpu.org/?p=1125)
- Jung: a cube virtual texture pages in visible height tiles on demand; tiles are stitched into a framebuffer object, max mips are generated at run time when the frustum changes enough, and frame-time spikes coincide with stitching — [Jung et al. 2020](https://arxiv.org/pdf/2005.06671)
- RMIP and TFDM: fractional per-ray LoD through hardware interpolation across levels, and tiling; RMIP layers are copied so that hardware sampling can blend levels — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- Micro-Mesh: subdivision per base triangle, neighbours within a factor of 2 — [NVIDIA slides](https://developer.download.nvidia.com/ProGraphics/nvpro-samples/slides/Micro-Mesh_Basics.pdf)
- Dübel et al. 2015 add a beam-based fast start over maximum mips, and test on a GTX TITAN through a task-based CUDA ray tracer — [Duebel et al.](https://vca.informatik.uni-rostock.de/~schumann/papers/2014+/VCSS15_paper_3.pdf)

### Inferences
- [Likely] For the engine, the closest published pattern to streamed quadtree terrain is Dick's: pyramid per node at residency time. A secondary ray then needs a way to find the neighbour tile. Options are a hardware TLAS over tile AABBs or a page table in compute. Dick does not need this, because rasterized tile bounds serve primary rays only.
- [Likely] A quadtree parent tile is not a bound of its children when it is built by averaging, as in Dick. A ray that uses the parent's pyramid as a coarse skip over its children can then miss a child ridge, unless the bake stores a max (or min-max) of the finest data in the parent.
- [Likely] A cascade that scrolls toroidally can update only the new strip and walk its parents upward. Cost grows with strip length, not with the area. No paper read reports this; RMIP's 5 ms for a full 4k rebuild is the upper reference (RTX 3080, naive implementation).

### Gaps
- No paper found with a toroidal min-max update or a mip chain under a clipmap with measured costs.
- Whether Tevs treats clipmaps explicitly: not verified.

## Is there recent work (2018 onward) that integrates height-field traversal with hardware ray tracing?

### Takeaway
Yes for displacement on meshes (TFDM, RMIP, PDM, as custom geometry under a hardware BVH) and as a hardware primitive (displacement micromaps, now deprecated by NVIDIA in favour of cluster acceleration structures). Terrain-specific work is thin: one Vulkan paper found, abstract only. None found that combines clipmaps with hardware RT.

### Cited Findings
- RMIP runs inside Embree and Vulkan Ray Tracing as custom geometry: one AABB per prism, a static BVH over prisms, traversal in the intersection program. Tests on RTX 3080 — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- RMIP ablation: replacing RMIP by a standard min-max mip at equal memory lowers performance; point-wise inversion helps less on GPU than on CPU (their guess: SIMD execution) — [RMIP 2023](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- Hoetzlein PDM: prisms in an OptiX hardware BVH with custom bounding and intersection programs. No bottom-level structure. Author states it is NOT intended for massive static scenes or terrain — [arXiv 2502.02011](https://arxiv.org/html/2502.02011v1)
- Timakov and Mikhaylyuk 2023 (Vulkan, C++, GLSL; Puget Sound height field): the intersection shader rejects AABBs early, and a "transparent AABB" defers costly tests to the any-hit shader. No numbers in the abstract — [Programming and Computer Software](https://journals.rcsi.science/0132-3474/article/view/137627)
- VK_NV_displacement_micromap: displacement in the BLAS with a compact format. Provisional, and deprecated by VK_NV_cluster_acceleration_structure, which is called the preferred direction but not a one-to-one replacement — [Vulkan extension docs](https://hackage.haskell.org/package/vulkan-3.26.1/docs/Vulkan-Extensions-VK_NV_displacement_micromap.html)
- Displacement micromap slides: reduces BVH build time and memory "significantly"; shell bounds come from per-vertex bias and scale fitted at BLAS build — [NVIDIA slides](https://developer.download.nvidia.com/ProGraphics/nvpro-samples/slides/Micro-Mesh_Basics.pdf)
- NVIDIA's sample says RTX Mega Geometry supersedes displaced Micro-Meshes and the repository is archived — [vk_raytrace_displacement](https://github.com/nvpro-samples/vk_raytrace_displacement). Mega Geometry: clusters of up to 256 triangles, GPU-driven builds — [NVIDIA blog](https://developer.nvidia.com/blog/nvidia-rtx-mega-geometry-now-available-with-new-vulkan-samples)
- Micro-mesh construction work notes hardware tracing of compressed displacement without pre-tessellation (seen as a citation in RMIP) — [Maggiordomo, Moreton, Tarini 2023, cited in RMIP](https://perso.telecom-paristech.fr/boubek/papers/RMIP/RMIP.pdf)
- Lumen default software path traces distance fields; Landscape belongs to the Lumen Scene — [Epic docs](https://dev.epicgames.com/documentation/unreal-engine/lumen-technical-details-in-unreal-engine). A heightfield path was tried and dropped for general meshes — [Wright et al. 2022](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf)

### Inferences
- [Likely] Three engine designs follow, and memory falls in this order:
  1. Keep triangles and reduce them: lower terrain BLAS LOD or cluster BLASes (vendor dependency if NV extensions).
  2. Custom geometry: one AABB per quadtree tile in the TLAS or one BLAS, and a min-max walk in an intersection shader (RMIP pattern, simplified for planar data). Scene rays stay unified with other geometry; ray-query paths need procedural handling.
  3. Pure compute walk for terrain only, and combine the result with a hardware query for non-terrain. This adds no BVH, but the caller must merge two hit distances for each ray.
- [Likely] A hardware micromap path is not portable and is deprecated, so it is a poor basis for new work.
- [Guessing] Intersection shaders can carry a higher per-call cost than a compute loop on some GPUs; I found no measured numbers to support it. Test on target hardware, AMD included.

### Gaps
- No verified measure of intersection-shader overhead against inline compute for a height-field walk.
- Timakov full text, Ogaki timings, TFDM's own tables: not read.
- No published game postmortem found that describes replacing a terrain BLAS with a height-field walk under hardware RT. Far Cry and Horizon terrain RT were not found in the searches made.
