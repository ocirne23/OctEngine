#version 450

// FAR-TREE APPLY (TreeVolumePipeline): a fullscreen draw in the scene-colour pass, before the cloud / fog apply.
// The march is full res: out = in-scatter + scene x transmittance (blend One, SrcAlpha; alpha not written).

layout (binding = 0) uniform sampler2D u_trees;

layout (location = 0) out vec4 out_color;

void main()
{
    out_color = texelFetch(u_trees, ivec2(gl_FragCoord.xy), 0);
}
