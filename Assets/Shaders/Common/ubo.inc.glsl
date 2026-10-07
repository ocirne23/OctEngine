#ifndef UBO_INC_GLSL
#define UBO_INC_GLSL

#ifndef UBO_BINDING
#define UBO_BINDING 0
#endif

// NUM_SHADOW_CASCADES is injected by the engine from RendererVKLayout (Layout.ixx)

// THE FRAME UBO. Its declaration is GENERATED from the registered block (Code/RendererVK/Private/UboBlock.ixx: the
// root values of UboRoot.ixx, then the lockable values of Renderer::registerUboFields) and served by the shader
// includer; a copy for reading is written to Assets/Local/Shaders/ubo.generated.glsl. The ROOT members (u_views_*,
// u_sunDirection, u_screenSize, u_sceneFocus, ...) are what nearly every pass reads; each subject has its own FLAT
// fields (u_fog_density, u_ocean_amplitude, ...) next to LIVE ones (u_fog_waveBand, ...). While every tweak a field
// comes from is LOCKED, that field is a const here: read it exactly like the block member (UBO_LIVE_<name> reads the
// block member in both modes).
#include "ubo.generated.glsl"

#define VIEW_CENTER 0 // shared passes + desktop; the eyes are 1 (left) and 2 (right) in VR

// View index selecting which u_views_*[] entry the convenience macros / reconstruction helpers read. Defaults
// to VIEW_CENTER (0): shared world-space passes and the whole desktop path leave it there. The per-eye
// screen-space passes set it once at the top of main() from a push constant (1 = left eye, 2 = right eye).
int g_viewIndex = 0;

// Convenience accessors so call sites read the current view's matrices without spelling out u_views_*[..].
// Shared passes get the centre view for free (g_viewIndex == VIEW_CENTER). A pass that needs a specific
// view regardless of g_viewIndex (e.g. fog apply sampling the centre-built froxel volume) must index
// u_views_*[] explicitly instead of using these.
#define u_mvp        u_views_mvp[g_viewIndex]
#define u_invMvp     u_views_invMvp[g_viewIndex]
#define u_prevMvp    u_views_prevMvp[g_viewIndex]
#define u_prevInvMvp u_views_prevInvMvp[g_viewIndex]
#define u_reprojClip u_views_reprojClip[g_viewIndex]
#define u_viewPos    (u_views_viewPos[g_viewIndex].xyz)

#endif
