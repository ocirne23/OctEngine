#version 450

layout (push_constant) uniform GridPC
{
    mat4 u_viewProj;
    vec4 u_cameraPosY; // xyz = camera position, w = the grid's height
    vec4 u_extents;    // x = half size of the quad, y = the fade distance
};

layout (location = 0) in vec3 in_worldPos;

layout (location = 0) out vec4 out_color;

// 1 on a line of the given spacing, antialiased by the screen-space derivative. posWidth = fwidth(pos), taken by the caller: the spacing jumps between two
// pixels where the level changes, and a derivative of pos / spacing would be huge there (a bright ring around the camera).
float gridLines(vec2 pos, vec2 posWidth, float spacing)
{
    const vec2 p = pos / spacing;
    const vec2 distToLine = abs(fract(p - 0.5) - 0.5) / max(posWidth / spacing, vec2(1e-5));
    return 1.0 - min(min(distToLine.x, distToLine.y), 1.0);
}

void main()
{
    const vec2 pos = in_worldPos.xz;
    const float dist = length(in_worldPos - u_cameraPosY.xyz);
    // The spacing follows the camera distance in powers of ten, the finer level fading out before its lines merge.
    const float level = log(max(dist, 1e-3)) / log(10.0) - 1.0;
    const float base = floor(level);
    const float fine = pow(10.0, base);
    const float fineFade = 1.0 - (level - base);
    // The same strength on both sides of a level change: a coarse line becomes the next level's fine line.
    const vec2 posWidth = fwidth(pos);
    const float lines = max(gridLines(pos, posWidth, fine) * fineFade, gridLines(pos, posWidth, fine * 10.0)) * 0.75;

    const float fade = 1.0 - smoothstep(u_extents.y * 0.4, u_extents.y, dist);
    vec3 color = vec3(0.62);
    // The X axis red, the Z axis blue.
    const vec2 axisDist = abs(pos) / max(fwidth(pos), vec2(1e-5));
    float alpha = lines;
    if (axisDist.y < 1.5) { color = vec3(0.85, 0.22, 0.2); alpha = 1.0; }
    if (axisDist.x < 1.5) { color = vec3(0.22, 0.4, 0.9); alpha = 1.0; }
    alpha *= fade;
    if (alpha <= 0.002)
        discard;
    out_color = vec4(color, alpha);
}
