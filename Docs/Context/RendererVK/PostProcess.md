# Post-processing and motion vectors

> Part of the RendererVK documentation ([`Docs/Context/RendererVK/CONTEXT.md`](CONTEXT.md)) — read that first.

## Motion vectors

Per-pixel OBJECT motion, so the temporal passes follow moving meshes (skinned characters, moving nodes, wind)
instead of only the camera.

* **The motion target** (`SceneColor`, `SCENE_MOTION_FORMAT` = RGBA16F, one per frame slot and eye): colour
  attachment 1 of the depth-writing stages only (see "Frame order" ([CONTEXT.md](CONTEXT.md#frame-order))). Value (`shared.inc.glsl`): xy = this
  frame's minus last frame's UNJITTERED full-frame uv, z = the surface's hardware depth LAST frame (<= 0:
  behind last frame's camera), w = 1. **Cleared to 0: w = 0 = no object motion**, and the readers then keep
  the exact camera-only reprojection (`prevScreenUVClip`). So only a point that really moved stores a uv
  (fp16 rounding: sub-pixel below ~0.1 of the screen per frame). **The NO-AO flag** (`motionVectorNoAo` /
  `motionNoAo`): w = 2 (object motion) or w = -1 (camera alone) marks a pixel RTAO does not trace - see "RTAO and
  normals" ([Lighting.md](Lighting.md)). Every reader goes through `motionIsObject` (w > 0.5), so the flag changes
  nothing else.
  Readers: `prevScreenUVMotion(uvUnjit, depth, motion, valid)`; a disocclusion test compares last frame's
  depth against `worldPosFromDepthMat(prevUv, motion.z, u_prevInvMvp)` - where the point WAS - instead of
  this frame's position.
* **Who writes it** (`GraphicsPipelineLayout::motionTarget` + `writeMotion` / `PipelineVariant::writeMotion`;
  otherwise the second blend state is write-masked): the variants that can draw moving meshes - LitOpaque,
  UnlitOpaque, LitMasked, LitFoliage, LitTree - plus the grass and the clutter flowers (wind), and the rigid
  clutter (static: only the NO-AO flag). Masked, and
  correct because static: terrain, rocks, sky, GI probe debug. Masked, keeping the opaque surface behind: the
  transparent family, the gizmos. **The ocean cannot write it**: dual-source blending allows one fragment
  output location (`maxFragmentDualSrcAttachments`), so the waves keep TAA's ocean feedback cap, and an ocean
  pixel over a moving underwater object carries that object's motion (known limitation).
* **TWO DGC EXECUTION SETS.** A set needs ONE fragment output interface for every pipeline in it
  (VUID-vkUpdateIndirectExecutionSetPipelineEXT-initialPipeline-11147), and a dual-source blend may not
  write location 1. So the variants split in two families (`RendererVKLayout::PIPELINE_TRANSPARENT_MASK`,
  injected as a define): the TRANSPARENT family (LitTransparent, UnlitTransparent - compiled with
  `NO_MOTION_OUTPUT` - Ocean, TerrainOverlay, River) writes location 0 only; every other variant writes
  locations 0 and 1 (masked where it does not use it). The cull routes a draw into the opaque or the
  transparent sequence by this FAMILY, not by its alpha mode, and `StaticMeshGraphicsPipeline` executes each
  sequence with its own set (`IndirectExecutionSet::initialize(pipeline, name, variantMask)`; the other slots
  stay unwritten). Before each execute the set's INITIAL pipeline must be bound (`getInitialVariant`).
  **A new variant: pick its family in the mask, and give its fragment shader that family's outputs.**
* **The vertex side** (`instanced_indirect.vs.glsl` `prevWorldDelta`): the WORLD offset of the point from
  where it was last frame, interpolated to `motion_vector.inc.glsl`. A world offset, not a clip position:
  exactly 0 on a still instance (the fragment then writes 0), small, and computed as a DIFFERENCE of the two
  transforms' terms (the translations cancel first), so it keeps its precision far from the origin. The
  fragment adds the camera through `u_reprojClip` and the object through `u_prevMvp`'s linear part only.
* **The delta costs the lit FS 3 inputs.** Tried and reverted: rebuilding the world position in the FS from
  `gl_FragCoord` + the depth to win them back. An interpolant costs no register until it is read, but the
  rebuilt position stayed live across the whole lit core: spills grew 16 -> 64 B and the static-mesh pass got
  slower (the pass is occupancy-bound on registers).
* **Last frame's instance transform** (`instanced_indirect.cs.glsl` `prevInstanceTransform`, into the
  cull's `OutMeshInstance`, 64 B): after the main cull the primary copies this slot's node transforms +
  pass masks into ONE device-local previous set (`InstanceStream::recordPrevCopy`; the other slot cannot
  serve - the CPU may write it while the GPU reads it), which next frame's cull reads (bindings 18/19). A
  pass mask carries its push frame above the PASS_* byte (`InstanceStream::stampedPassMask`, from
  `m_uboFrameIndex`, the UBO's `u_frameIndex`); a node not pushed LAST frame (off screen, or just spawned into a recycled transform
  slot) draws with no motion instead of a stale transform. prevScale 0 = the node did not move: the vertex
  shader then keeps the exact current transform (the previous quaternion is snorm16, moving nodes only).
* **Skinned meshes**: the output region is 2 x vertexCount (`spawnSkinnedNode`, freed the same way). The
  skin pass copies each position it is about to overwrite into the second half; `MeshInfo::prevVertexDelta`
  = vertexCount, copied into every LOD level's info (they index the same region), 0 = not skinned. The lit
  vertex shader reads the previous position from the vertex SSBO (binding 14, also in the vertex stage).
  `SkinningJob::prevValid` = 0 on a job's first frame (fresh or re-acquired bundle: the region holds no last
  frame), set by `SkinnedMeshRegistry::markJobsUploaded` after the upload.
* **Readers**: TAA (the motion of the NEAREST surface in the 3x3, so a moving silhouette's history follows
  the object), the AO temporal pass, the forward pass's AO read (its own `MOTION_WORLD_DELTA`, no
  target read), the motion blur, and DLSS's motion vector pass (`dlss_mvec`). Not: the cloud temporal
  pass, the particle collision.
* Cost: +8 B per pixel per slot and eye (~30 MB per slot at 1440p), +16 B per culled instance, one extra
  varying (vec3) on the lit/unlit vertex path.

## TAA off bypasses the pass completely

The secondary CB is not recorded, the dispatch and its GPU scope are skipped, and eye adaptation and
composite sample this frame's SceneColor directly (`resolvedLayout` SHADER_READ_ONLY instead of TAA's
GENERAL storage image; **the scene-colour barrier gains the fragment stage**, since the composite then
reads it). "TAA/Enabled" re-records the command buffers on change (`Tweak::onChange` in
`RendererSettings.cpp`), so toggling rebuilds the descriptors either way.

## DLSS (NVIDIA Streamline 2.14.1, "Post/DLSS" tweaks)

DLSS Super Resolution + DLAA, **desktop only; any mode but Off REPLACES TAA** (`taaActive()` = TAA on AND no
DLSS; `resolveActive()` = either). No frame generation / Reflex.

**Resizes apply on RELEASE** (`setResizeHold`, App's resize hold): while a window-border or ImGui-splitter drag holds
the left mouse button, the viewport rect and the window size stay the last ones, and an out-of-date swapchain skips
the frame (`m_swapchainStale`) instead of rebuilding; the one rebuild runs when the hold ends (that frame is skipped
too: its UBO was built for the old size). A rebuild per frame of the drag would recreate the DLSS feature every frame.

* **Optional, never linked** (`RendererVK:Streamline`): `sl.interposer.dll` is signature-checked
  (`sl_security.h`, hence `wintrust` + `crypt32`) and `LoadLibrary`'d from the exe's folder; it loads
  `sl.common.dll`, `sl.dlss.dll` and the model `nvngx_dlss.dll` from there (the App POST_BUILD copy). No DLL,
  no NVIDIA GPU, VR, or any SL failure = the plain path, and `Streamline::dlssAvailable()` is false.
* **MANUAL HOOKING.** `Streamline::load()` runs BEFORE the instance (slInit needs to precede it), the
  instance / device take DLSS's extensions, 1.2/1.3 features and extra queues (`appendInstanceExtensions`,
  `appendDeviceExtensions`, `mergeDeviceFeatures`, `extraGraphicsQueues`; Device drops its
  `VK_EXT_buffer_device_address` - SL lists it next to the KHR one, and both at once is a validation error; the feature
  is core), `onDeviceCreated` calls
  `slSetVulkanInfo`. **While SL is loaded the five swapchain calls MUST go through its proxies**
  (`Streamline::createSwapchain / destroySwapchain / getSwapchainImages / acquireNextImage / queuePresent`,
  used by SwapChain and Framebuffers) - SL's per-frame `presentCommon` hangs off them. A new swapchain call
  site uses the wrappers too. `slShutdown` is in `Device::destroy`, before the device. No OTA: the shipped
  `nvngx_dlss.dll` is the model. `eUseFrameBasedResourceTagging` is REQUIRED: without it SL rejects
  `slSetTagForFrame`. `sl.common` always loads `NvLowLatencyVk.dll` (Reflex), so it ships too. Its warnings
  "Hook sl.common:Vulkan:CmdBindPipeline / CmdBindDescriptorSets / BeginCommandBuffer is NOT supported" are
  expected: they are command-buffer state-tracking hooks, which manual hooking does not use.
* **No SL log file** (`pathToLogsAndData` null, no `sl.log`): SL's messages reach the engine log through the log
  callback only, and the console gets only its warnings and errors. "Verbose log (restart)" (saved) sets SL's verbose
  level, read once at slInit.
* **The OUTPUT must start at (0, 0)**: SL never sets NGX's `DLSS.Enable.Output.Subrects`, and NGX fails the
  evaluate (0xbad00005, "the output subrect base must be set to 0 ...") for any other output offset. So a viewport
  at the origin (the game) is written straight into TAA's resolved image; any other (the editor panel) goes to
  `DlssPipeline`'s swapchain-size output image at (0, 0), then one `copyImage` to the viewport's place (hence
  TRANSFER_DST on the resolved images, and the copy stage in `beginExternalWrite` / `endExternalWrite`). The
  INPUT sub-rects (colour, depth, motion vectors) are fine at any offset.
* **Two more NGX `InvalidParameter` traps** (both from the sl.dlss source): SL describes a Vulkan
  image to NGX as a COLOR subresource unless an `sl::SubresourceRange` is chained to it, so the depth tag
  carries one (`Streamline::Image::aspect`); and SL creates the NGX feature at the optimal render size for the
  output size, so with DLSS active `m_renderRect`'s SIZE is `getDlssRenderSize(mode, viewport size)` (only its
  origin scales), never the rounded swapchain ratio, which could come out 1 px larger than the feature.
* **RENDER RESOLUTION.** The render-size targets - SceneColor (colour, depth, motion), RTAO, the clouds, the
  force interval target, the DLSS motion vectors - are `m_renderExtent` = the swapchain extent x
  `m_renderScale` (the DLSS optimal size for the whole swapchain; 1 for Off / DLAA). The scene draws through
  `m_renderRect` = `m_viewportRect` x the same scale, and **the UBO's `u_screenSize` / `u_viewportRect` are the
  render target and the render rect**, so every scene / screen-space shader works unchanged. Everything
  after the resolve - TAA's resolved image (the DLSS output), motion blur, eye adaptation, bloom, composite -
  stays at the swapchain extent and `m_viewportRect` (the CPU passes it; nothing post reads the UBO rect
  except the motion blur, which is off while upscaling). The jitter is one RENDER pixel. The projection
  aspect and `m_mipPixelScale` stay on the output viewport. **A new render-size image joins
  `recreateRenderTargets()`; a new scene pass sizes from `renderExtent()` / `m_renderRect`, never the
  swapchain.**
* **The renderer never reads the "Mode" tweak live**: `m_dlssMode` is the APPLIED mode, set only in
  `updateRenderExtent` with the sizes it implies. The UI writes the tweak a frame or more before its `onChange`
  runs; reading it live renders one frame with the new mode and the old sizes (NGX creates the feature at the new
  optimal size, the input is bigger: 0xbad00005) or, from Off, executes the never-recorded DLSS secondary
  (device lost, black screen).
* **Mode change** ("Mode" tweak): GPU idle + `applyRenderResolution()` - the targets re-created only when the
  size changes, the DLSS history reset, the scene texture sampler's LOD bias = log2(render / output) ("Mip
  bias"; the static-mesh sampler only: materials + terrain; decals keep 0).
* **Per frame** (`recordDlssEvaluate`, in the primary, where TAA would run): the cached `DlssPipeline`
  secondary (`dlss_mvec.cs.glsl`: full motion vectors - the motion target has object motion only, so a w = 0
  pixel reprojects through the camera from its depth, RG16F render px, current -> previous, unjittered; plus
  the ocean bias mask below),
  `TaaPipeline::beginExternalWrite`, `Streamline::evaluateDlss` (options only on a change, a frame token, the
  constants, the four tags `eValidUntilEvaluate`, `slEvaluateFeature` into the primary), `endExternalWrite`.
  Auto exposure is on: the engine's exposure is computed after the upscale. No command buffer state to restore:
  every pass after it is a secondary.
* **Jitter sign**: DLSS gets the engine's NDC jitter in render pixels with y down: (+x, -y) x size / 2.
  The UE convention (-x, +y) wobbles and softens the image (A/B on screen).
* **The ocean** (no motion vectors: dual-source blend) gets TAA's ocean feedback cap as DLSS's
  **bias-current-colour mask** (`kBufferTypeBiasCurrentColorHint`: lerp(history, current, bias)): the mvec pass
  also writes an R16F mask (NOT R8_UNORM: SL's Vulkan format table has no entry for it and logs "Cannot have
  undefined format" - a new tagged format must be in `sl.chi` `Vulkan::getFormat`), "Ocean current bias" (0.8 = TAA's 0.2 history weight) on TAA's ocean flag pixels
  (scene colour alpha < 0.004, depth > 0), 0 elsewhere. It reads the scene colour (only where depth > 0: the
  sky is never ocean), so it runs after the colour's barrier to the resolve.
* **Preset default M** (`Post/DLSS/Preset`; sl_dlss.h: L / M are the newer models with less ghosting, M near K's
  cost). DLAA's own default, K, smears the trees' alpha-tested foliage under camera rotation; L and M do not.
  A far-tree bias in the ocean mask was tried for it and removed: it did not help.
* Not while upscaling: **motion blur** (`motionBlurEnabled()`: its velocity and gather assume one resolution).
  **Under DLAA the mvec pass writes the motion blur velocity + sub-tiles** (the side product TAA writes;
  `motion_blur_tiles` runs only with no resolve at all: `velocityPass = !resolveActive()`). The sub-tile grid
  starts at pixel 0, so with the blur on the pass dispatches the WHOLE target (`pc.base` = 0, as TAA), otherwise
  only the render rect. Its opening barrier's source stages include the fragment stage (the composite's gather).
* The GPU profiler shows the mvec pass as "DLSS mvec" and the upscale as "DLSS". Cost (RTX 4090, DLAA
  1920x1080): DLSS mvec ~0.03 ms, DLSS ~0.31 ms.
* **A viewport that does not fit the swapchain skips the upscale** (the editor layout before it adapts to the
  window, e.g. on a command-line start). Everything else crops it, but DLSS cannot:
  the render rect clamps below the feature's dynamic range (NGX InvalidParameter), and **`slEvaluateFeature`
  still returns OK when NGX fails inside it**, so the output copy would run out of bounds: device lost.

## Motion blur (`MotionBlurPipeline`, "Post/Motion blur" tweaks)

Desktop only (`motionBlurEnabled()`: off in VR, and bypassed when off - its pass is not recorded, not executed,
and TAA / the composite skip their parts). **FUSED into the passes around it: it has no full-res pass of its
own.** The shared code is `motion_blur.inc.glsl`; **eye adaptation and bloom keep the unblurred colour.** Its
images (RG16F) are used within the frame only - one set for every slot, GENERAL for life.

1. **TAA writes the velocity** (`motionBlurVelocity`; TAA already reads the depth and the motion target; under
   DLAA, DLSS's `dlss_mvec` pass does the same - see "DLSS") and
   the longest velocity per 8 x 8 SUB-tile (`MOTION_BLUR_SUBTILE`, TAA's workgroup; a shared-memory reduction
   at the top of the shader, before any early return). Velocity = (this - last frame) uv x "Shutter" in px,
   clamped to 2 x "Max radius (px)": the camera part from the depth (`prevScreenUVClip`), the object part = the
   motion target - the camera part, so "Camera motion" scales the camera's share alone; a frame motion over
   ~30 % of the screen diagonal is a cut or a teleport: no blur. With TAA OFF, `motion_blur_tiles` does it.
   TAA's opening storage transition (source stages compute + fragment) orders last frame's reads first.
2. `motion_blur_neighbor` (the "Motion blur" scope) - per `MOTION_BLUR_TILE` (32) tile the longest velocity
   of its 3 x 3 tiles, read straight from the sub-tiles. **The radius is capped at the tile size**, so that
   neighbourhood holds every blur that can reach a pixel: that is what lets a moving object smear past its
   silhouette.
3. **The COMPOSITE gathers** (`motionBlurGather`): a pixel whose tile neighbourhood moves runs McGuire et
   al. 2012's reconstruction filter, "Samples" taps along the neighbourhood velocity (interleaved-gradient
   jitter per frame), each weighted by whether its blur covers this pixel or this pixel's blur covers it, with
   soft depth tests (2 % of the distance, on 1 / the scene depth) choosing the front one; any other pixel reads
   the resolved colour once, as without the blur. So no blurred image is written or read. The composite binds
   the frame UBO for it (binding 6, `UBO_BINDING`).

"Shutter" is the exposure as a fraction of the frame, so a higher frame rate blurs less (physical); > 1
exaggerates. The ocean writes no motion vectors, so its waves blur only with the camera.

## Colour

Everything upstream of composite is **linear HDR**. `EyeAdaptationPipeline` (log-luminance histogram
auto-exposure) feeds `CompositePipeline` (HDR → display into the swapchain before ImGui; tonemap
off / Reinhard / ACES / AgX / AgX Punchy - the default). **AgX Punchy** = AgX + Blender's "Punchy" look after the
sigmoid (display space, before the outset): a power for contrast and a saturation around the luma, "Post/AgX punchy
contrast" (1.3; Blender's 1.35) / "saturation" (1.0; Blender's 1.4) - plain AgX's very gentle shoulder rarely let a sunlit surface reach white. The histogram bins EVERY viewport pixel (the reduce divides by the area),
4×4 pixels per thread: a run of equal bins is one shared atomic and only non-empty bins go global (one
pixel per thread was atomic-bound). `PIXELS_PER_THREAD` in the shader and the dispatch's group size must
match. A thread's block is 2 x 2 QUADS of 2 x 2 pixels, because each quad is also one texel of bloom level 0
(below).

## Bloom (`BloomPipeline`, "Post/Bloom" tweaks)

Desktop only (`bloomEnabled()`), in HDR before the exposure, in two modes:

* **"Threshold" > 0 (default 1):** a SOFT threshold (the Unity / Unreal knee, "Knee" half width) on the
  brightest channel in EXPOSED units (1 = display white before the tonemap), so only light that will show
  as bright goes into the blur, which the composite ADDS: `scene + blur * intensity`. Dark and mid tones
  never blur. The threshold uses LAST frame's auto exposure (the histogram pass runs before the reduce;
  eye adaptation's first barrier makes last frame's write visible) - the adaptation is smoothed over seconds.
* **"Threshold" 0:** the physical, energy-conserving `scene * (1 - intensity) + blur * intensity`: every pixel
  spreads a share of its light, so the whole image softens as the intensity rises (keep it small).
* **"Radius"** weighs the levels, level k by 2^(k (2 radius - 1)) (0.5 = equal; the default 0.75 favours the
  wide levels), normalized by the weights' sum (`getNormalize`). With equal weights the half- and quarter-res
  levels, barely blurred, read as a haze over the whole image.

**Built to add no full-res pass:**

* **Level 0 (half res) is written by the eye-adaptation HISTOGRAM pass**, which already reads every pixel of
  the resolved colour: each 2 x 2 quad of its block becomes one texel, a Karis average (weights 1 / (1 + luma),
  values clamped finite), so one very bright pixel cannot make a flickering blob; the threshold is applied
  to that average (once per quad). It reads TAA's output, not
  the motion blur's (bloom is wide anyway). Off = the pass skips the writes (a push-constant flag).
* **The chain** (`record`, after eye adaptation): downsamples to the smallest of "Levels" levels
  (`bloom_downsample`, Jimenez's 13-tap filter - stable under sub-pixel motion), then upsamples back
  (`bloom_upsample`, 3 x 3 tent + add, IN PLACE into the larger level, each level times its "Radius" weight -
  the smallest one on the first step). Level 0 then holds the WEIGHTED SUM of every level; the composite
  divides by the weights' sum.
* **The composite** mixes level 0 in with one bilinear fetch (`u_bloomUv` maps full-frame uv to level 0).
* ONE B10G11R11 image with a mip chain for all frame slots, sized to the full render target; each level uses
  only its VIEWPORT region (from the origin, taps clamped inside it), so a viewport change needs no re-create.
  Last frame's reads of level 0 are ordered before the histogram's write by eye adaptation's first barrier.
