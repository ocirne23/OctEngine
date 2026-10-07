export module Procedural:ClutterGenerator;

import Core;
import RendererVK;

import :ClutterType;

// The GROUND CLUTTER's meshes (Docs/GroundClutterPlan.md 6): one per variant, each a short LOD chain
// (RendererVKLayout::CLUTTER_LODS levels), at a nominal size of 1, y up, the lowest point at y = 0:
//   PEBBLE   - the rock SDF generator (RockGenerator: the type's rock keys) - its regular LOD chain, its cavity as AO;
//   BRANCH   - a fallen stick along X (length 1): a bent, tapering tube lying on the ground, its end grain capped, a few
//              side twigs lying flat;
//   MUSHROOM - a lathed stem + cap (Dome / Flat / Cone / Funnel) with gills underneath, in a group of up to 5.
// Flowers have no mesh (clutter_flower.vs.glsl builds them). Pure; any thread.
export namespace Procedural
{
	void generateClutterMeshes(const ClutterTypeDesc& type, uint32 gridResolution, oc::vector<Renderer::ClutterMesh>& out);
}
