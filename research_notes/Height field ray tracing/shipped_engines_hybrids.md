# Terrain in ray tracing: shipped engines and hybrid designs

Label meaning in these notes:

- **[Dev]** = the developer or engine vendor says it, and I read the primary text (slide text or docs page).
- **[Excerpt]** = the primary source says it, but I saw only a search-tool excerpt, not the full document. Check before relying on it.
- **[Staff?]** = forum post by a person who appears to be vendor staff. The page does not label them.
- **[Third]** = third-party source.
- **[Inferred]** = my conclusion. No source says it.

Slide numbers are PDF page numbers. Slide text is paraphrased.

**Reading limit that affects these notes.** Three key decks are over the fetch tool's 10 MB limit, so I did NOT read them in full: Kuenlin (Snowdrop lighting pipeline), the AC Shadows SIGGRAPH 2025 deck, and the Lumen SIGGRAPH 2022 deck. Links are given below. I did not download them by other means, because this session has no user approval for file downloads. These three decks are the first things to read by hand. The AC Shadows talk also has a [video](https://www.youtube.com/watch?v=2K-rwy4aMmU), which I could not read.

## How does Unreal Engine handle Landscape in hardware ray tracing and in Lumen, and what are the known limits?

### Takeaway
In hardware ray tracing, Landscape is ordinary triangle geometry whose BLAS is rebuilt to follow the morphing LOD. This is a known cost, and the workaround causes visible dark splotches. In software Lumen, Landscape is handled by a heightfield path that is separate from mesh distance fields, but the public docs give little detail on it.

### Cited Findings

**Hardware ray tracing: Landscape BLAS**
- [Dev] Landscape is listed as supported geometry for hardware ray tracing. — [Epic: Hardware Ray Tracing](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-ray-tracing-in-unreal-engine)
- [Dev] Landscape is rebuilt to support its continuously morphing LOD. The guide gives `r.RayTracing.Geometry.Landscape.FractionalLODUpdateThreshold=0.3` to reduce how often it updates. — [Epic: RT Performance Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine)
- [Dev] NVIDIA's UE5 guide groups Landscape morphing LODs with skeletal meshes and Niagara as dynamic instances that rebuild their BLAS every frame, which lowers RT performance when triangle counts are high. — [NVIDIA: UE5 Raytracing Guideline](https://dlss.download.nvidia.com/uebinarypackages/Documentation/UE5+Raytracing+Guideline+v5.4.pdf)
- [Staff?] July 2026 thread: continuous updates keep the BLAS in sync as landscape geometry streams in and out. Without updates, the BLAS and the rendered geometry diverge. Workaround: `r.RayTracing.Geometry.Landscape.LODsUpdateEveryFrame=-1` and a higher `FractionalLODUpdateThreshold`, and the poster warns of dark splotches on the landscape. The original poster reports that disabling `r.RayTracing.Geometry.Landscape` causes artifacts. — [Epic forum: Landscape in hardware ray tracing](https://forums.unrealengine.com/t/landscape-in-hardware-ray-tracing/2745488)
- [Staff?] Same thread: 5.8 changes Nanite ray tracing, including landscape, but it is not yet "game ready" in 5.8 and is expected to mature in UE 6.x. It needs more memory, which likely does not fit the 8 GB budget Epic uses for current consoles and PC GPUs. The question of a static BLAS or a static-mesh duplicate was left open. — [same thread](https://forums.unrealengine.com/t/landscape-in-hardware-ray-tracing/2745488)

**Hardware ray tracing: range and proxies**
- [Dev] Default culling radius `r.RayTracing.Culling.Radius` = 10000 (100 m). City Sample used 15000 and angle 0.5. Epic says culling leaves coverage gaps and is best paired with Far Field. The page's angle text is inconsistent (default 1, text says 5 degrees). — [Epic: RT Performance Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine)
- [Dev] Generate Ray Tracing Proxies creates 2 LODs and keeps LOD1 resident, for use with Reference Based Residency (a BLAS stays in memory only if the TLAS references it; the pool size is a soft limit; meshes without LODs are permanently resident). With Nanite fallback meshes disabled, ray tracing streams proxies on demand. This text is about meshes. It does not mention Landscape. — [same guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine)
- [Dev] Far Field is hardware-only. It starts at Max Trace Distance (default 200 m), runs to `r.LumenScene.FarField.MaxtraceDistance` (default 1 km), and needs HLOD1 built. — [Epic: Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)
- [Excerpt] Each primitive has a Ray Tracing Far Field option, and the HLOD1 mesh is the far representation. — [Epic: Lumen GI and Reflections](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-global-illumination-and-reflections-in-unreal-engine)
- [Dev] Hit lighting evaluates at the hit point. The default for reflection rays is the Surface Cache. — [Epic: Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)

**Software Lumen**
- [Dev] Landscape terrain is one of the geometry types in the Lumen Scene. Software tracing uses mesh SDFs for the first 2 m, then the Global SDF. Lumen Scene covers about 200 m by default, up to 800 m. Past that distance, software tracing has only screen traces for GI. WPO is not supported. Walls under 10 cm can leak. — [Epic: Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)
- [Dev] Landscapes are handled separately by heightfields and are not affected by distance field resolution. — [Epic: Mesh Distance Fields](https://dev.epicgames.com/documentation/en-us/unreal-engine/mesh-distance-fields-in-unreal-engine)
- [Excerpt] For distance field AO, landscape is a heightfield with approximate cone intersection and no self-shadowing. My fetch of this page returned no body text, so this comes from the search excerpt only. — [Epic: Distance Field AO](https://dev.epicgames.com/documentation/unreal-engine/distance-field-ambient-occlusion-in-unreal-engine?lang=en-US)
- [Excerpt] Epic's stated reason for a software path includes that heavily overlapping meshes are slow in a two-level acceleration structure. — [Lumen SIGGRAPH 2022 deck](https://advances.realtimerendering.com/s2022/SIGGRAPH2022-Advances-Lumen-Wright%20et%20al.pdf)

### Inferences
- [Inferred] The Unreal design is the costly end of the range: the BLAS tracks the drawn LOD, so accuracy is good and cost is continuous. Epic's own workaround shows the failure when the two diverge.
- [Inferred] Far Field shows that Epic does not trace near Landscape triangles to the horizon. It hands over by distance to a separate coarse representation. It uses HLOD triangles, not a height field.

### Gaps
- I could not verify how Lumen's surface cache represents Landscape (card layout, merged cards), or whether the heightfield feeds the Global SDF directly. A [Polycount post](https://polycount.com/discussion/232083/ue4-landscape-does-it-support-mesh-distance-field) suggests that it does, but it is anecdotal. The Lumen 2022 deck may answer this, and the Netzel/Costa talk "Ray Tracing Open Worlds in UE5" ([course page](https://advances.realtimerendering.com/s2022/index.html)) may cover far field. I read neither.
- The `LODsUpdateEveryFrame` cvar and the 5.8 statements come from one forum post. I did not confirm them in release notes.
- I found no published memory figure for a Landscape BLAS.

## What do Frostbite, Snowdrop, Anvil, RE Engine, Decima, REDengine, Northlight, Dunia, 4A Engine, Unity HDRP, Godot and others do? Does any engine exclude terrain from the BVH or march a height map?

### Takeaway
Yes. Snowdrop (Avatar: Frontiers of Pandora) has no terrain triangles in its RT world and marches the height field during intersection. Metro Exodus marches the terrain height map in the raygen shader and uses the hit to shorten the real ray. Far Cry 6 does the opposite, with clipmapped terrain in the TLAS at a longer range than objects. For most other engines I found no public statement.

### Cited Findings

**Snowdrop (Avatar: Frontiers of Pandora). The closest match to the near-BVH plus marched-height design**
- [Dev] Exact statement: "Terrain does not have triangle representation for RT." The height field is ray-marched directly inside the intersection routine. — [Koshlo, GDC 2024, slide 16](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)
- [Dev] RT effects: indirect diffuse, reflections, shadows at long distance, audio queries. All run in 30 and 60 Hz console modes. It is a hybrid of screen space, hardware RT and ray-traced probes. — [Koshlo, slides 5-6](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)
- [Dev] The RT world uses fixed lower-detail geometry and average materials. PC uses DXR 1.1 inline trace. Consoles and non-RT PC hardware use a custom BVH. — [Koshlo, slide 11](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)
- [Dev] The scene is gathered in a 4 km radius around the camera. — [Koshlo, slide 14](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)
- [Excerpt] Terrain is marched separately and can use textures. Trace stages are listed as BVH trace, then terrain march. Screen-space trace comes first, BVH runs if that fails, and the probe cache is the fallback on a miss. Consoles trace at quarter resolution. The probe cascade's minimum height is offset from the terrain, and probes under the terrain are marked invalid. — [Kuenlin, GDC 2024](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Kuenlib_Quentin_Raytracing_In_Snowdrop.pdf) (excerpts only, so the exact order is not confirmed)

**4A Engine (Metro Exodus)**
- [Dev] After a screen-space pre-trace, the real ray spawns only if the pre-trace found no hit. The terrain height map is marched inside the raygen shader, and a hit limits the ray distance. The slide calls this almost free if done with care, due to GPU latency hiding. It warns about precision and tolerances. — [4A/NVIDIA, GTC 2019, slides 32-33](https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf)
- [Dev] BLAS meshes reuse simplified shadow meshes and are about 4x smaller than the real geometry. Slide claims: up to 30% RT performance gain in some scenes, ~1 GB instead of ~4 GB, close to zero quality difference. — [same deck, slide 28](https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf)
- [Dev] BLAS is updated only for skinned or animated instances. TLAS is always rebuilt and holds instances in an expanded frustum, with more than 100k active and under 5k surviving culling. — [same deck, slide 26](https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf)

**Dunia (Far Cry 6)**
- [Dev] Objects enter the TLAS only inside a radius around the player (radius of 100 on slide 72, no unit given). Clipmapped terrain is kept to a longer range. BLAS objects use a constant lower LOD. The pipeline has a "Collect Terrain" step. — [AMD/Ubisoft, GDC 2022, slides 21, 71, 72](https://gpuopen.com/gdc-presentations/2022/GDC_Performant_Reflective_Beauty_Hybrid_Ray_Traced_Reflections_In_Far_Cry_6.pdf)

**Anvil (Assassin's Creed Shadows)**
- [Excerpt] GI starts with a screen-space march, partly because on consoles it can be faster than tracing. The BVH world is very coarse, does not hold every object, and its geometry is simpler than the raster. Probe volumes cache secondary-hit GI. A topic "World Space texture atlas for terrain" is listed. — [Ubisoft, SIGGRAPH 2025](https://advances.realtimerendering.com/s2025/content/Advances%202025%20-%20Raytracing%20the%20world%20of%20Assassin's%20Creed%20Shadows.pdf)
- [Dev] PC modes: Selective (hideout only), Standard (GI), Extended (GI and reflections). GPUs without hardware RT use a proprietary software path. The Q&A does not mention terrain or BVH. — [Ubisoft Tech Q&A](https://www.ubisoft.com/en-gb/game/assassins-creed/news/4XbPPtFyQEtIMWrA9xVDmZ/assassins-creed-shadows-tech-qa)
- [Third] Terrain is built from height maps. — [PCGH](https://www.pcgameshardware.de/Assassins-Creed-Shadows-Spiel-74683/Specials/Test-Release-Review-Grafikkarten-Prozessoren-1468158/2/)

**Frostbite (Battlefield V)**
- [Dev] A text search of the GTC 2019 deck finds no terrain or height map statement. Culling is by the projected angle of the bounding sphere. A 4 degree cut took Rotterdam from 5000 to 400 BLAS rebuilds, 20000 to 2800 TLAS instances, and TLAS plus BLAS build from 64 ms to 14.5 ms. — [DICE, GTC 2019](https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s91023-it-just-works-ray-traced-reflections-in-battlefield-v.pdf)
- [Third] Battlefield 6 ships with no ray tracing, by the statement of its technical director. — [80 Level](https://80.lv/articles/battlefield-6-won-t-be-getting-ray-tracing-developers-confirm)

**RE Engine**
- [Dev] Uses inline ray tracing. Static meshes share one BLAS through instancing. Titles differ in which effects they use RT for. The deck has no statement on terrain, BLAS LOD, or range. — [Capcom, RE:2023](https://docswell.com/s/CAPCOM_RandD/524Y6M-RE2023)

**Unity HDRP**
- [Excerpt] `Terrain.enableHeightmapRayTracing` adds height-map geometry to the acceleration structures. — [Unity script reference](https://docs.unity3d.com/ScriptReference/Terrain-enableHeightmapRayTracing.html)
- [Excerpt] The HDRP Terrain Master Stack does not support ray tracing or path tracing. — [HDRP 17.5 docs](https://docs.unity3d.com/Packages/com.unity.render-pipelines.high-definition@17.5/manual/terrain-master-stack-reference.html)
- [Third] A 2020-era HDRP limit says that only the highest LOD of a renderer goes into the acceleration structure. It is old and not terrain-specific. — [Unity forum](https://discussions.unity.com/t/raytracing-limitations-version-to-version/857082)

**Godot**
- [Excerpt] Godot exposes BLAS/TLAS through RenderingDevice, as described in a 2026 talk. NVIDIA has an experimental path-tracing fork. — [GodotCon talk](https://talks.godotengine.org/godotcon-ams-2026/talk/JDYVQ3.ics), [CG Channel](https://www.cgchannel.com/tag/godot-fork/)
- [Excerpt] SDFGI follows the camera, and it does not support dynamic occluders. Neither source covers terrain. — [Godot SDFGI docs](https://docs.godotengine.org/en/stable/tutorials/3d/global_illumination/using_sdfgi.html)

### Inferences
- [Inferred] Metro's slides do not say whether terrain is also in the TLAS. The slide says the march limits the ray, which works either way, so "excludes terrain" is not proven for Metro. Snowdrop is the only explicit exclusion I found.
- [Inferred] Snowdrop's other titles (Star Wars Outlaws) may use the same design, because Snowdrop is a shared platform ([Simmons, slide 62](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Simmons_Joshua_Upgrading_the_Snowdrop.pdf)). I found no statement about Outlaws.
- [Inferred] The AC Shadows terrain atlas may be a surface-colour source for terrain hits. The excerpt only lists the topic.

### Gaps
- **Terrain handling in AC Shadows:** in the deck, which I did not read in full. Whether terrain is in the BVH is unverified.
- **REDengine (Cyberpunk 2077), Northlight, Decima, Witcher 3 RT update, Indiana Jones:** I found no public source on terrain in RT. This is an absence in my search, not evidence that they do any given thing. The [CDPR interview](https://www.nvidia.com/en-sg/geforce/news/cyberpunk-2077-ray-tracing-overdrive-mode-interview) has no terrain detail. No Digital Foundry source was found or read.
- **Capcom's "Advances in Ray Tracing" deck** ([link](https://docswell.com/s/CAPCOM_RandD/K24Y66-RE2023)) was not read. Dragon's Dogma 2 terrain is unverified.
- **Godot's HDDAGI, CryEngine SVOGI terrain:** the search returned nothing.

## Which designs trace hybrid rays (screen, near triangles, far height field or SDF), how do they hide the handover, and how do they merge hit distances?

### Takeaway
Screen trace first, then world-space trace, is near universal in this set. Only two designs add a height-field path to BVH rays: Snowdrop and Metro. The merge is simple in the Metro case: the height-map hit shortens the ray. No source I read describes a visible near-triangle to far-height-field handover by distance, because Snowdrop's terrain is one marched path at all distances.

### Cited Findings

| Design | Order of trace | Far / terrain path | Handover or merge method | Status |
|---|---|---|---|---|
| Snowdrop (Avatar) | Screen, BVH, terrain march, probe cache on miss | Height field, no triangles; 4 km BVH radius | Not described in text I read | [Dev] slide 16, [Excerpt] order |
| Metro Exodus | Screen pre-trace, height-map march in raygen, then real ray | Height map | Height-map hit limits ray distance | [Dev] slides 32-33 |
| Far Cry 6 | SSLR, then HW RT; particles separate | Terrain clipmap in TLAS at longer range | Confidence per ray; HW RT confidence falls to 0 at BVH edge; env map if neither | [Dev] slides 16-19, 71 |
| AC Shadows | Screen march, then BVH, probe volume for secondary hits | Unverified | Unverified | [Excerpt] |
| Lumen HW | Screen, near BVH, Far Field | HLOD1 triangles to 1 km | Far Field trace begins at Max Trace Distance | [Dev] |
| Lumen SW | Screen, mesh SDF (2 m), Global SDF | Global SDF; screen only past Lumen Scene | Fixed 2 m switch | [Dev] |
| Battlefield V | SSR, then RT | None stated | SS-hybridization section | [Dev], terrain absent |

- [Dev] Far Cry 6 confidence rules: high SSLR confidence uses SSLR only, mid confidence takes the trace with the highest confidence, no SSLR confidence uses HW RT only, and neither uses the environment map. Far distances are SSLR only, close distances are HW RT only. HW RT confidence falls gradually to 0% at the BVH edge, stated to prevent popping. — [Far Cry 6 deck, slides 16-19](https://gpuopen.com/gdc-presentations/2022/GDC_Performant_Reflective_Beauty_Hybrid_Ray_Traced_Reflections_In_Far_Cry_6.pdf)
- [Dev] The FidelityFX sample chooses hybrid or DXR per 8x8 tile from reprojected hit and miss counters. DXR rays nearly always report high confidence, so some pixels in a tile are randomly returned to hybrid. — [same deck, slides 75-76](https://gpuopen.com/gdc-presentations/2022/GDC_Performant_Reflective_Beauty_Hybrid_Ray_Traced_Reflections_In_Far_Cry_6.pdf)
- [Dev] Lumen screen traces come first, and a more reliable method follows when there is no hit or the ray passes behind a surface. — [Epic: Lumen Technical Details](https://dev.epicgames.com/documentation/en-us/unreal-engine/lumen-technical-details-in-unreal-engine)
- [Dev] A mismatch between screen and world paths is a known problem. Snowdrop disables small emissive objects in the RT world but keeps them in the main view, so screen-space hits find emission the BVH lacks. Its preferred fix traces a short ray around the emissive screen hit to verify that the object exists in the RT world. — [Koshlo, slides 71-72](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)
- [Dev] Snowdrop handles distant shadow casters with a similar non-triangle path: impostors in 256 m sectors, traced against impostor depth inside the traversal. On DXR each one is a procedural box; the custom BVH uses a sub-BVH. — [Koshlo, slides 52-53](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)

### Inferences
- [Inferred] With a height-field path, the natural merge is the minimum t: march first and use the hit as the BVH ray's tMax (Metro), or trace the BVH first and limit the march to the BVH hit (Snowdrop excerpt order). Metro's order helps the BVH ray terminate early. The second order avoids march cost when an object is hit early. Neither source compares them.
- [Inferred] Snowdrop has no terrain LOD handover in RT, because one path serves all ranges. This removes the terrain popping problem. It does not remove the mismatch of marched height against drawn terrain.
- [Inferred] For the context engine: this is close to Snowdrop's design, with cascades as the height source. Coarser cascades could be hidden with distance-dependent tolerance, but this is my suggestion, not a shipped method. A near triangles plus far height split (Metro-style tMax, with a confidence fade at the range limit as in Far Cry 6) is a combination of parts. I found no shipped game that documents exactly that split.

### Gaps
- I found no shipped source with a blend zone between near terrain triangles and far height field. The Kuenlin and AC decks could change this.
- March step count, height resolution, and hit refinement for Snowdrop and Metro are not in the text I read.

## Which terrain BLAS strategies are used, and what memory numbers were published?

### Takeaway
Published strategies range from no terrain BLAS (Snowdrop), through clipmap terrain in the TLAS (Far Cry 6), to a BLAS that follows the drawn LOD (Unreal). I found no published memory number specific to a terrain BLAS. The numbers below cover the full BVH.

### Cited Findings
- [Dev] Unreal: rebuild to follow the LOD, with a fractional threshold to reduce frequency. — [Epic: RT Performance Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine)
- [Dev] Far Cry 6: terrain clipmap at a longer range than objects. Static BLASes build once. Up to 16 BLAS rebuilds per frame, max 12 skinned. Non-overlapped BVH generation costs 0.4-0.8 ms more than async. — [Far Cry 6 deck, slides 22, 67, 71](https://gpuopen.com/gdc-presentations/2022/GDC_Performant_Reflective_Beauty_Hybrid_Ray_Traced_Reflections_In_Far_Cry_6.pdf)
- [Dev] Snowdrop: no terrain triangles. DXR queues only a handful of missing BLAS per frame, and a BLAS is ready after build and compaction. Top level is rebuilt each frame with cached sub-trees. — [Koshlo, slides 16, 26-27, 48](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)
- [Dev] Avatar PC package (disk, per slide title): BVH 3,396 MiB, Terrain 1,841 MiB, on a 32 km by 32 km map. Series S: reducing BVH streaming distance from 4 km to 1 km saves ~300 MB. CPU BVH updates 2.7 ms. GPU: RT GI 2.2 ms, RT reflections 1.7 ms, RT volumes 1.2 ms (platform not stated in extracted text). — [Simmons, GDC 2024, slides 4, 6, 19-20, 46, 61](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Simmons_Joshua_Upgrading_the_Snowdrop.pdf)
- [Dev] Metro: reusing simplified meshes gives about 4x smaller BLAS meshes, ~1 GB instead of ~4 GB. BLAS plus TLAS ~0.5 ms, hidden by async on an RTX 2080. — [Metro deck, slides 28, 73](https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf)
- [Dev] BLAS memory scales linearly with triangles and vertices, and exact size is platform-dependent. — [Epic: RT Performance Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine)
- [Dev] Traversal: rays that miss still pay the TLAS cost, so Far Cry 6 advises to keep far or low-value objects out. — [Far Cry 6 deck, slide 70](https://gpuopen.com/gdc-presentations/2022/GDC_Performant_Reflective_Beauty_Hybrid_Ray_Traced_Reflections_In_Far_Cry_6.pdf)

### Inferences
- [Inferred] A shared-topology terrain BLAS strategy (one index layout, per-tile positions) is a sensible option, but no source I found documents it for a shipped game. Treat it as untested by published evidence.
- [Inferred] Avatar's package figures show that the terrain's height data exists for raster anyway. Marching it adds little new data, which is one likely benefit of no terrain BLAS. Massive does not state this reason in the text I read.

### Gaps
- No published terrain-only BLAS size, build time, or refit-versus-rebuild comparison was found.
- Far Cry 6's terrain update policy as the clipmap moves is not stated.
- No published data on BLAS updates for streamed quadtree terrain tiles. The search returned only general clipmap background ([Hoppe](https://hhoppe.com/proj/geomclipmap/)).

## What artifacts come from a mismatch between the BLAS LOD and the drawn terrain, and what are the fixes?

### Takeaway
The published symptoms are wrong self-shadowing, dark splotches, and imprecise shadows. The published fixes are: keep the BLAS in sync (costly), raise the proxy resolution, lengthen the self-intersection skip distance, or do not use the coarse world for precise shadows at all. No shipped source I read gives a numeric terrain bias.

### Cited Findings
- [Dev] Snowdrop states that the RT world's geometry LOD does not match the raster world, so it cannot produce precise shadows. They use it only for long-distance shadows outside CSM range. — [Koshlo, slide 52](https://media.gdcvault.com/gdc2024/Slides/GDC+slide+presentations/Koshlo_Oleksandr_Snowdrop_Custom_BVH.pdf)
- [Staff?] Unreal Landscape: when updates are reduced and the BLAS diverges from rendered geometry, expect dark splotches on the landscape. — [Epic forum](https://forums.unrealengine.com/t/landscape-in-hardware-ray-tracing/2745488)
- [Dev] NVIDIA: the mismatch between Nanite and its fallback is unlikely to be noticed in diffuse effects, but it can be a problem in hard shadows and mirror reflections. For self-shadowing in hard shadows, increase `r.RayTracing.Shadows.AvoidSelfIntersectionTraceDistance`. For mirrors, lower Fallback Relative Error. — [NVIDIA: UE5 Raytracing Guideline](https://dlss.download.nvidia.com/uebinarypackages/Documentation/UE5+Raytracing+Guideline+v5.4.pdf)
- [Dev] Epic: rays hit fallback triangles and can cause artifacts. Fixes are to disable Nanite on the object or raise the Fallback Target. — [Epic: Hardware Ray Tracing](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-ray-tracing-in-unreal-engine)
- [Dev] Epic: WPO is ignored by default when building the RT scene, which can cause self-intersection. The same class of error comes from any raster displacement absent from the BLAS. — [Epic: RT Performance Guide](https://dev.epicgames.com/documentation/en-us/unreal-engine/ray-tracing-performance-guide-in-unreal-engine)
- [Dev] Metro: be careful with precision and tolerances. Floating point precision caused problems several times. — [Metro deck, slide 33](https://developer.download.nvidia.com/video/gputechconf/gtc/2019/presentation/s9985-exploring-ray-traced-future-in-metro-exodus.pdf)
- [Third, offline renderer] Blender Cycles offsets the ray start based on how far the smooth normal deviates from the geometric normal, and separates shading and geometry offsets. This is the terminator artifact on coarse meshes. — [Blender T68920](https://developer.blender.org/T68920), [commit](https://developer.blender.org/rB9c6a382)

### Inferences
- [Inferred] Sign of the error matters. If the BLAS or height field is above the drawn surface, rays start inside it and terrain self-shadows (dark splotches). If it is below, contact shadows and AO disappear and thin leaks occur. An offset fixes the first case at the cost of the second. It should scale with the local LOD error, not be constant.
- [Inferred] Effect sensitivity for the context engine, following NVIDIA's diffuse versus hard shadow split: GI probes, volumetric scatter and rain occlusion tolerate a coarse terrain; RTAO tolerates it at low range; RT sun shadow and mirror-like water reflection are the least tolerant. Snowdrop's slide 52 supports this for sun shadows.
- [Inferred] A height-field path that samples the same height data as the drawn terrain at the same LOD removes most of this mismatch at source. No source I read states this as a design goal.

### Gaps
- No shipped game publishes a numeric bias, or an LOD-error-driven offset, for terrain. I also did not find NVIDIA or AMD best-practice text on this topic in the search results.
- I did not read the standard self-intersection references. They are not cited here because I did not verify them in this session.
