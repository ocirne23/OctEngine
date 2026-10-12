#ifndef UBO_INC_GLSL
#define UBO_INC_GLSL

#ifndef UBO_BINDING
#define UBO_BINDING 0
#endif

// NUM_SHADOW_CASCADES is injected by the engine from Layout.ixx.

// THE FRAME UBO. Its declaration is GENERATED (Code/RendererVK/Private/UboBlock.ixx) and served by the shader includer;
// a copy for reading is in Assets/Local/Shaders/ubo.generated.glsl. While every tweak a field comes from is LOCKED, the
// field is a const: read it exactly like the block member (UBO_LIVE_<name> reads the block member in both modes).
#include "ubo.generated.glsl"

#define VIEW_CENTER 0 // shared passes + desktop; the eyes are 1 (left) and 2 (right) in VR

// The u_views_*[] entry the accessors below read. Per-eye screen-space passes set it at the top of main() from a push
// constant; everything else leaves it at VIEW_CENTER.
int g_viewIndex = 0;

// A pass that needs one view regardless of g_viewIndex (e.g. fog apply sampling the centre-built froxel volume)
// indexes u_views_*[] itself.
#define u_mvp        u_views_mvp[g_viewIndex]
#define u_invMvp     u_views_invMvp[g_viewIndex]
#define u_prevMvp    u_views_prevMvp[g_viewIndex]
#define u_prevInvMvp u_views_prevInvMvp[g_viewIndex]
#define u_reprojClip u_views_reprojClip[g_viewIndex]
#define u_viewPos    (u_views_viewPos[g_viewIndex].xyz)

// THE VIEW RAY through an NDC position: normalize(vec3(ndc, 1.0) * u_rayFromNdc) (the inverse of u_mvp's x / y / w
// rows, in double on the CPU: exact, translation-free, steady, unlike a reconstruction through u_invMvp). Unjittered:
// subtract u_taaJitter.xy from the NDC to match a rasterized pixel. u_viewForward = the ray through NDC (0, 0), normalized.
#define u_rayFromNdc  (mat3(u_views_rayFromNdc[g_viewIndex]))
#define u_viewForward (u_views_rayFromNdc[g_viewIndex][3].xyz)

#endif
