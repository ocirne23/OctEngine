#version 450

// A big quad on the ground plane, centred under the camera, as two triangles from gl_VertexIndex.
layout (push_constant) uniform GridPC
{
    mat4 u_viewProj;
    vec4 u_cameraPosY; // xyz = camera position, w = the grid's height
    vec4 u_extents;    // x = half size of the quad, y = the fade distance
};

layout (location = 0) out vec3 out_worldPos;

void main()
{
    const vec2 corners[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    const vec2 corner = corners[gl_VertexIndex] * u_extents.x + u_cameraPosY.xz;
    out_worldPos = vec3(corner.x, u_cameraPosY.w, corner.y);
    gl_Position = u_viewProj * vec4(out_worldPos, 1.0);
}
