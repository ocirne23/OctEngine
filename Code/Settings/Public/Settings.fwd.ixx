export module Settings.fwd;

// Forward declarations of the settings types, for an INTERFACE that only names them (a reference, a pointer, a
// parameter). The defaults live in the domain modules' member initializers, so a default change rebuilds every
// importer of that module: a library interface that imports Settings.fwd instead keeps its BMI, and only the .cpp
// files that import Settings rebuild.
//
// A type listed here MUST be defined inside its domain module's `extern "C++"` block (attached to the global module):
// MSVC mangles a module-attached type with its module's name, so a forward declaration of a module-attached type is a
// DIFFERENT type, and the calls across the two views do not link. To add a domain, wrap its structs the same way.
export extern "C++"
{
    // Settings.Render
    struct WindParams;
    struct SkyParams;
    struct CloudParams;
    struct ShadowParams;
    struct FoliageParams;
    struct GrassParams;
    struct RockParams;
    struct FarTreeParams;
    struct FogParams;
    struct PostParams;
    struct RTParams;
    struct LightGridParams;
    struct RTAOParams;
    struct TAAParams;
    struct DlssParams;
    struct MotionBlurParams;
    struct BloomParams;
    struct MeshLodParams;
    struct ParticleParams;
    struct OceanSprayParams;
    struct ForceFieldParams;
    struct GiGridConfig;
    struct GiSettings;
    struct TextureStreamingSettings;
    struct MeshStreamingSettings;
    struct RendererSettings;

    // Settings.Clutter
    struct ClutterSettings;

    // Settings.Threading
    struct JobStressSettings;
    struct JobStatsSettings;
    struct ThreadingSettings;
}
