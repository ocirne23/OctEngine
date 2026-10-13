export module Procedural:ClutterTexture;

import Core;

import :ClutterType;

// The ground clutter's generated TEXTURES (Docs/GroundClutterPlan.md): the alpha-tested cards of a flower type - a fern's
// FROND, a radial head's PETAL, a grain's EAR with its awns, an umbel's FLORET CLUSTER, a spike's PEA FLORET, a bell. RGBA8, sRGB, square; v along the part (0 = its base, 1 = its
// tip), u across it (0.5 = its axis). R = the part's own shading (x the type's Color), G = a mask toward the second
// colour (Color2: a grain's awns, a fern's rachis), alpha = the cover. clutter_flower.fs.glsl:
// mix(Color, Color2, G) x R, discarded below alpha 0.5. The strip the vertex shader lays it on has the part's full
// width, so the outline is all in the texture. The mip chain is TreeLeafTexture's (coverage-preserving).
export namespace Procedural
{
	// Level 0 of the type's texture, its size in `outSize`. False: the type has none (a Reed / Tuft / Rosette / Leafy
	// head, or `PetalShape None` on a Radial / Umbel / Spike / Bell). Pure; any thread.
	bool generateClutterTexture(const ClutterTypeDesc& type, uint32& outSize, oc::vector<uint8>& outRgba);
}
