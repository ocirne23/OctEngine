export module RendererVK:UboRoot;

import Core;
import Core.glm;

import :Layout;
import :UboBlock;

// THE FRAME UBO'S ROOT VALUES: the handles the frame build (RendererUbo.cpp) writes through - what nearly every pass
// reads (the views, the sun, the screen, the focus) and each subject's LIVE group (cloudsLive.enabled in C++,
// u_cloudsLive_enabled in GLSL): everything that depends on the camera, the clock, the sun, the wind or a value the
// outside pushes per frame. Never baked. The lockable values follow them in the block (Renderer::registerUboFields).
//
// EACH MEMBER BINDS ITSELF where it is declared (UboGroup), so this struct IS the layout: declaration order is std140
// packing order - a vec3 then a float share 16 bytes. A new value is one line here, written with `ubo.set(h, v)`.
export struct UboRoot
{
    explicit UboRoot(UboBlock& block) : u{ block, "" } {}
    UboGroup u;

    // The camera views, one array per matrix: [VIEW_CENTER] = the centre / combined view (the only one on desktop; in
    // VR sized to the union of both eyes' FOV, so the shared world-space passes cover what either eye sees); [1] = left
    // eye, [2] = right eye. Shaders pick through g_viewIndex (ubo.inc.glsl's u_mvp, u_viewPos, ...).
    UboArray<glm::mat4> viewMvp = u("views_mvp", RendererVKLayout::NUM_UBO_VIEWS);
    UboArray<glm::mat4> viewInvMvp = u("views_invMvp", RendererVKLayout::NUM_UBO_VIEWS);         // inverse(mvp), in double: the world position from depth + screen uv
    UboArray<glm::mat4> viewPrevMvp = u("views_prevMvp", RendererVKLayout::NUM_UBO_VIEWS);       // last frame's mvp: a world position to last frame's screen
    UboArray<glm::mat4> viewPrevInvMvp = u("views_prevInvMvp", RendererVKLayout::NUM_UBO_VIEWS); // last frame's inverse(mvp): last frame's world position
    // prevMvp * inverse(mvp), fused in double: current NDC + depth -> last frame's clip, near-identity at any camera
    // position (the world round trip's float32 error grows with the distance from the origin: temporal-history jitter).
    UboArray<glm::mat4> viewReprojClip = u("views_reprojClip", RendererVKLayout::NUM_UBO_VIEWS);
    UboArray<glm::vec4> viewPos = u("views_viewPos", RendererVKLayout::NUM_UBO_VIEWS); // xyz = world position

    UboArray<glm::vec4> frustumPlanes = u("frustumPlanes", 6); // the centre view's frustum (Frustum::planes)
    // The sun cascades. The bottom row is structurally [0 0 0 1], so m[0][3] = the far distance and m[1][3] = the texel
    // size ride it (the shaders' cascadeMatrix restores it).
    UboArray<glm::mat4> cascadeViewProj = u("cascadeViewProj", RendererVKLayout::NUM_SHADOW_CASCADES);
    UboValue<glm::vec4> cascadeSunSizeTexels = u("cascadeSunSizeTexels"); // per cascade: the PCF disc radius (texels) per unit of depth gap
    UboValue<glm::vec4> screenSize = u("screenSize");     // xy = the render target (px), zw = 1 / xy
    UboValue<glm::vec4> viewportRect = u("viewportRect"); // xy = the render rect's min, zw = its size, in [0,1] of the target
    // xy = this frame's TAA jitter (NDC), zw = last frame's. Every raster pass applies xy; the mvps stay unjittered.
    UboValue<glm::vec4> taaJitter = u("taaJitter");
    UboValue<glm::vec3> sunDirection = u("sunDirection");         // normalized, toward the sun
    UboValue<uint32> frameIndex = u("frameIndex");                // monotonic (RNG / temporal rotation)
    UboValue<glm::vec3> sunColor = u("sunColor");                 // the sun irradiance (colour x intensity)
    UboValue<float> timeSeconds = u("timeSeconds");               // SIM time (s): stops with the global pause
    UboValue<glm::vec3> sunTransmittance = u("sunTransmittance"); // the atmosphere toward the sun at ground level (CPU Chapman)
    UboValue<float> sunVisible = u("sunVisible");                 // the eclipse's visible sun fraction
    UboValue<glm::vec3> skyUp = u("skyUp");                       // the sky's up axis (normalized)
    UboValue<float> mipPixelScale = u("mipPixelScale");           // px per (size / distance): the LOD metric
    UboValue<glm::vec3> ambientColor = u("ambientColor");         // the flat minimum ambient radiance
    UboValue<glm::vec3> sceneFocus = u("sceneFocus");             // every distance-based quality falloff measures from it

    // The clouds' per-frame values (the clock, the wind, the camera, the sun, the game's suppression).
    struct CloudsLive
    {
        UboGroup g;
        // The Beer shadow map's cascades: xyz = the frozen centre relative to the CENTRE view's camera (m), w = 1 / the
        // extent (1/m).
        UboArray<glm::vec4> shadowCascade = g("shadowCascade", 2);
        UboValue<glm::vec3> shadowAxis0 = g("shadowAxis0");               // the light-space axes
        UboValue<float> enabled = g("enabled");                           // the march ran this frame (0/1)
        UboValue<glm::vec3> shadowAxis1 = g("shadowAxis1");
        UboValue<float> shadowRendered = g("shadowRendered");             // the map was rendered this frame (0/1)
        UboValue<glm::vec3> windStep = g("windStep");                     // the field's world displacement this frame (m; the temporal reprojection)
        UboValue<float> skyHistory = g("skyHistory");                     // the sky-map clouds' history weight this frame
        UboValue<glm::vec3> groundBounceAlbedo = g("groundBounceAlbedo"); // the sky's ground colour x the cloud "Ground albedo"
        UboValue<float> giSkyHistory = g("giSkyHistory");                 // the GI sky clouds' history weight this frame
        UboValue<glm::vec2> noiseOrigin = g("noiseOrigin");               // camera XZ - wind, wrapped by the weather period (m)
        UboValue<float> detailDrift = g("detailDrift");                   // the detail's vertical drift (m, wrapped)
    } cloudsLive{ { u.block, "cloudsLive_" } };

    // GI's per-frame values.
    struct GiLive
    {
        UboGroup g;
        UboValue<glm::vec3> prevFocus = g("prevFocus");   // LAST frame's scene focus (the previous clipmap window -> probe freshness)
        UboValue<float> temporalAlpha = g("temporalAlpha"); // this frame's blend: "GI/Temporal Alpha" compounded over the wall delta
        UboValue<float> fullBake = g("fullBake");         // a full irradiance-volume bake this frame (0/1)
    } giLive{ { u.block, "giLive_" } };

    // The fog's values that ride the ocean (its readback, its world scale).
    struct FogLive
    {
        UboGroup g;
        UboValue<float> waveBand = g("waveBand");                 // the waterline band gating the FFT wave taps (m; 0 = ocean off)
        UboValue<float> boundaryOffset = g("boundaryOffset");     // the underwater boundary off the local water surface (m)
        UboValue<float> causticDepthFade = g("causticDepthFade"); // 1/m
        UboValue<float> causticShoreFade = g("causticShoreFade"); // m (0 = off)
    } fogLive{ { u.block, "fogLive_" } };

    // The ocean's per-frame values: the wind, the sea level, the readback, the camera, the foam field's levels.
    struct OceanLive
    {
        UboGroup g;
        // xy = the level origin (drifted coords of texel (0,0)'s corner, m), zw = the whole texels it moved since last frame.
        UboArray<glm::vec4> foamLevels = g("foamLevels", RendererVKLayout::OCEAN_FOAM_LEVELS);
        UboValue<glm::vec3> bubbleSun = g("bubbleSun");               // the bubble cloud's per-frame factors (oceanBubbleRadianceFrame): the sun's ...
        UboValue<float> seaLevel = g("seaLevel");                     // world Y
        UboValue<glm::vec3> bubbleSky = g("bubbleSky");               // ... and the sky's path down to "Bubble depth" x the albedo
        UboValue<float> windSpeed = g("windSpeed");                   // U10 (m/s)
        UboValue<glm::vec2> windDirection = g("windDirection");       // unit
        UboValue<glm::vec2> foamDrift = g("foamDrift");               // the foam field's accumulated drift (m; its coords = rest XZ - drift)
        UboValue<float> shoreFoamDepth = g("shoreFoamDepth");         // the surf band's width at the waterline (m; follows the wind)
        UboValue<float> shoreFoamMax = g("shoreFoamMax");             // the surf's opacity cap (follows the wind)
        UboValue<float> swashReach = g("swashReach");                 // the CPU's run-up estimate (m)
        UboValue<float> displacementExtent = g("displacementExtent"); // how far the VS moves a vertex off its lattice (m; 0 with the ocean off)
        UboValue<float> cameraUnderwater = g("cameraUnderwater");     // 0/1: gates the underside path
        UboValue<float> sprayDt = g("sprayDt");                       // the sim delta (s)
        UboValue<uint32> sprayEmitter = g("sprayEmitter");            // the spray's particle emitter slot (UINT32_MAX = off)
    } oceanLive{ { u.block, "oceanLive_" } };

    // The terrain's world, its baked height map, its splat material set, the wetness clipmap's tick.
    struct TerrainLive
    {
        UboGroup g;
        // The ground / rock CLIMATE BOX in (t01, h01): xy = the temperature range, zw = the humidity range (unused for
        // beach / snow).
        UboArray<glm::vec4> splatClimate = g("splatClimate", RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS);
        // Slot s: [s >> 2][s & 3] = the BC5 HEIGHT + AO texture, 0xFFFF = none.
        UboArray<glm::uvec4> splatHeightTex = g("splatHeightTex", RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS / 4);
        // Slot s: [s >> 1].xy (even s) / .zw (odd): x = diffuse | normal << 16, y = 1 when the normal is BC5.
        UboArray<glm::uvec4> splatTex = g("splatTex", RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS / 2);
        // Slot s: [s >> 2][s & 3] = its grass amount (0..1).
        UboArray<glm::vec4> splatGrass = g("splatGrass", RendererVKLayout::MAX_TERRAIN_SPLAT_MATERIALS / 4);
        UboValue<glm::vec2> mapCentre = g("mapCentre");         // the baked height map's world centre XZ (both cascades)
        UboValue<float> mapInvNearSize = g("mapInvNearSize");   // 1 / the near cascade's world size (0 = no map)
        UboValue<float> mapInvFarSize = g("mapInvFarSize");     // 1 / the far cascade's (0 = near only)
        UboValue<float> mapSeaLevel = g("mapSeaLevel");         // the map's baked sea level
        UboValue<float> seaLevel = g("seaLevel");               // world Y, live from the streamer
        UboValue<float> meshRadius = g("meshRadius");           // the streamed mesh's coverage radius (m; 0 = none - fences the ocean land cull)
        UboValue<float> lapseRate = g("lapseRate");             // C per world metre above sea level (<= 0; terrainTemperatureAt)
        UboValue<float> splatBase = g("splatBase");             // the splat set: the base material (< 0 = none: the flat colour)
        UboValue<float> numGround = g("numGround");
        UboValue<float> numRock = g("numRock");
        UboValue<float> hasBeach = g("hasBeach");               // 0/1: the entry after the rock
        UboValue<float> hasSnow = g("hasSnow");                 // 0/1: the LAST entry
        UboValue<float> wetDecay = g("wetDecay");               // the wetness pass this tick: exp(-dt / dry time)
        UboValue<float> wetRain = g("wetRain");                 // rain wetting added
        UboValue<float> wetIn = g("wetIn");                     // wet-in added under water
        UboValue<float> wetSpread = g("wetSpread");             // the diffusion's mix fraction
        UboValue<float> wetLayer = g("wetLayer");               // the ping / pong layer written (the readers sample it)
        UboValue<glm::vec2> wetOrigin = g("wetOrigin");         // the clipmap window's origin (lattice coord, ints as floats)
        UboValue<glm::vec2> wetPrevOrigin = g("wetPrevOrigin"); // the previous tick's (texels that scrolled in start dry)
    } terrainLive{ { u.block, "terrainLive_" } };

    // The grass's per-frame values (the camera, the sun, the clock).
    struct GrassLive
    {
        UboGroup g;
        UboValue<glm::mat4> shadowViewProj = g("shadowViewProj");     // THE NEAR GRASS CASCADE: an ortho box ahead of the camera (standard Z)
        UboValue<glm::vec2> nearCentre = g("nearCentre");             // its box centre XZ
        UboValue<float> nearRange = g("nearRange");                   // its half size (m; 0 = off)
        UboValue<float> nearTexel = g("nearTexel");                   // m
        UboValue<float> minWidthPerMetre = g("minWidthPerMetre");     // the pixel floor as world width per metre of distance
        UboValue<float> canopyExtinction = g("canopyExtinction");     // the canopy's base extinction (1/m; 0 = no grass)
        UboValue<float> prevTime = g("prevTime");                     // LAST frame's u_timeSeconds (the motion vectors)
    } grassLive{ { u.block, "grassLive_" } };

    // The trees' per-frame values.
    struct FoliageLive
    {
        UboGroup g;
        UboValue<glm::vec2> handoverCentre = g("handoverCentre"); // the far-tree volume's HAND-OVER: the new bake's centre
        UboValue<float> handoverFade = g("handoverFade");         // the fraction of rays that pick the new bake
        UboValue<float> farMarched = g("farMarched");             // 1 while the far-tree volume marched this frame (the fog apply composites it)
        UboValue<float> windPrevTime = g("windPrevTime");         // LAST frame's u_timeSeconds (the motion vectors)
        UboValue<float> windReach = g("windReach");               // the culls' bound growth (m): the sway's reach at the strongest gust
    } foliageLive{ { u.block, "foliageLive_" } };

    // The force fields' per-frame values (the emitters, the camera, the view).
    struct ForceLive
    {
        UboGroup g;
        UboArray<glm::vec4> teamColors = g("teamColors", RendererVKLayout::MAX_FORCE_TEAMS); // rgb = linear team colour
        UboValue<glm::vec3> bakeMin = g("bakeMin");             // the sampled shell tier: the bake volume's world min
        UboValue<float> bakeThreshold = g("bakeThreshold");     // the reach threshold an emitter marches the volume at
        UboValue<glm::vec3> bakeInvSize = g("bakeInvSize");     // 1 / the volume's world size
        UboValue<float> bakeEnabled = g("bakeEnabled");         // 0/1
        UboValue<float> shellLodScale = g("shellLodScale");     // the shell march's LOD: (px per radius / dist) / the full-detail px (0 = off)
        UboValue<float> unionPxScale = g("unionPxScale");       // px per (radius / dist): the union march's distance LOD
        UboValue<float> cameraInside = g("cameraInside");       // the camera is inside a bubble (0/1)
    } forceLive{ { u.block, "forceLive_" } };

    // THE wind ("Sky/Wind": the particles, the vegetation, the fog), the weather volumes' camera values and the rain
    // occlusion map.
    struct Weather
    {
        UboGroup g;
        UboValue<glm::mat4> rainOcclusionViewProj = g("rainOcclusionViewProj"); // a plain top-down ortho over the rain volume (standard Z)
        UboValue<glm::vec3> windVelocity = g("windVelocity");                   // the mean wind (m/s, horizontal)
        UboValue<float> gustStrength = g("gustStrength");                       // m/s
        UboValue<glm::vec3> cameraVelocity = g("cameraVelocity");               // the centre view's velocity this frame (m/s)
        UboValue<float> invGustSize = g("invGustSize");                         // 1/m
        UboValue<glm::vec2> windDirection = g("windDirection");                 // unit XZ (valid at zero speed)
        UboValue<float> windSpeed = g("windSpeed");                             // m/s
        UboValue<float> cameraWaterY = g("cameraWaterY");                       // the LIVE water surface world Y under the camera
        UboValue<float> cameraWaterValid = g("cameraWaterValid");               // 0/1
        UboValue<float> rainOcclusionPresent = g("rainOcclusionPresent");       // the map exists this frame (0/1)
        UboValue<float> rainOcclusionInvRange = g("rainOcclusionInvRange");     // 1 / its depth range (1/m)
    } weather{ { u.block, "weather_" } };

    // Known only in present(): the claims made after the begin-frame build. present() uploads this group's bytes alone
    // (presentBegin / presentSize: its members are contiguous).
    struct Present
    {
        UboGroup g;
        UboValue<uint32> treeRangeBase = g("treeRangeBase");         // the BAKED TREE RECORDS (tree_cull.inc): this frame's first instance
        UboValue<uint32> treeRangeLength = g("treeRangeLength");     // its length (0 = no trees)
        UboValue<uint32> treeThreads = g("treeThreads");             // the culls' thread count (their dispatch)
        UboValue<uint32> treeCount = g("treeCount");                 // the listed pieces
        UboValue<float> treeFarScale = g("treeFarScale");            // the far distance scale
        UboValue<float> treeForceFar = g("treeForceFar");            // 0/1
        UboValue<float> treeVolumeStart = g("treeVolumeStart");      // the far-tree volume's start (m; 0 = no volume)
        UboValue<float> treeShadowMargin = g("treeShadowMargin");    // m: the shadow cull drops a tree from the cascades whose split + this it lies beyond
        UboValue<uint32> giTlasNumInstances = g("giTlasNumInstances"); // the TLAS-instance writer's live count
        uint32 begin() const { return treeRangeBase.offset; }
        uint32 size() const { return giTlasNumInstances.offset + 4u - treeRangeBase.offset; }
    } present{ { u.block, "present_" } };
};
