#version 450

// BEAM CARDS (BeamCardPipeline): one camera-facing quad per beam (6 vertices, no vertex attributes), around the CAPSULE
// its glow reaches - the segment plus the halo's range on every side. Each end is offset in the plane facing the
// camera at that end: `range` across the beam's direction on screen, and `range` beyond the end along it, so the round
// end caps are covered too (and a beam seen end-on is a square around each end). The fragment shader evaluates the glow
// (beam_glow.inc.glsl); the card is only its coverage.
// THE CAMERA INSIDE THE REACH (within 1.5 x the range of the segment): the card is the whole screen, since corners next
// to and behind the camera do not bound the pixels any more. That pixel cost is one column's.
// Card data (BeamCardPipeline::Card, 4 vec4 each): foot.xyz + core radius, unit axis.xyz + length, colour.rgb + range,
// x = the tip fade.

#include "shared.inc.glsl"

layout (binding = 1, std430) readonly buffer BeamCards { vec4 bc_data[]; };

// View index (0 = centre / desktop, 1 / 2 = the eyes in VR).
layout (push_constant) uniform ViewPC { uint u_viewIndex; };

layout (location = 0) flat out uint v_card;

void main()
{
    g_viewIndex = int(u_viewIndex);
    const uint card = uint(gl_VertexIndex) / 6u;
    const uint vertex = uint(gl_VertexIndex) % 6u;
    // Two triangles (0, 1, 2) (2, 1, 3) over the corners: bit 1 = the end (0 the foot, 1 the tip), bit 0 = the side.
    const uint c = vertex < 3u ? vertex : (vertex == 3u ? 2u : (vertex == 4u ? 1u : 3u));
    const float end = float(c >> 1u);
    const float side = (c & 1u) == 0u ? -1.0 : 1.0;
    v_card = card;

    const vec4 footR = bc_data[card * 4u + 0u];
    const vec4 axisLen = bc_data[card * 4u + 1u];
    const float range = bc_data[card * 4u + 2u].w;
    const vec3 axis = axisLen.xyz;
    const float len = axisLen.w;

    // Camera-relative: the world coordinate's rounding stays out of the offsets.
    const vec3 eye = u_viewPos;
    const vec3 foot = footR.xyz - eye;
    const vec3 nearest = foot + axis * clamp(-dot(foot, axis), 0.0, len);
    if (dot(nearest, nearest) < 2.25 * range * range)
    {
        gl_Position = vec4(side, end * 2.0 - 1.0, 0.0, 1.0); // the whole render rect (the depth test is off)
        return;
    }

    const vec3 p = foot + axis * (len * end);
    const vec3 d = normalize(p);
    vec3 onScreen = axis - d * dot(axis, d); // the beam's direction across the view ray
    onScreen = dot(onScreen, onScreen) > 1e-6 ? normalize(onScreen)
                                              : normalize(cross(d, abs(d.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    const vec3 across = cross(d, onScreen);
    const vec3 corner = p + across * (side * range) + onScreen * ((end * 2.0 - 1.0) * range);
    gl_Position = u_mvp * vec4(corner + eye, 1.0);
    gl_Position.xy += u_taaJitter.xy * gl_Position.w; // TAA sub-pixel jitter (clip space), as every raster pass
}
