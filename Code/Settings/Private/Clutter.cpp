module Settings.Clutter;

import Core;
import Settings.Tweaks;

// "Mesh resolution" regenerates the meshes and "Reload types" re-reads the files: ClutterSystem::initialize attaches
// those listeners. Everything else is a UBO value (live) or read per frame.
void Settings::registerClutter(ClutterSettings& s)
{
	Tweak::boolean("Clutter", "Enabled", &s.enabled);
	Tweak::boolean("Clutter", "Reload types", &s.reload);
	Tweak::floatVar("Clutter", "Density scale", &s.densityScale, 0.0f, 8.0f, 0.01f);
	Tweak::floatVar("Clutter", "Range scale", &s.rangeScale, 0.1f, 4.0f, 0.01f);
	Tweak::floatVar("Clutter", "Patch size (m)", &s.patchSize, 1.0f, 16.0f, 0.1f);
	Tweak::floatVar("Clutter", "Grow band", &s.growBand, 0.01f, 1.0f, 0.005f);
	Tweak::floatVar("Clutter", "Range fade", &s.rangeFade, 0.01f, 1.0f, 0.005f);
	Tweak::floatVar("Clutter/LOD", "LOD 1 size", &s.lod1Size, 0.0f, 0.2f, 0.0005f);
	Tweak::floatVar("Clutter/LOD", "LOD 2 size", &s.lod2Size, 0.0f, 0.2f, 0.0005f);
	Tweak::floatVar("Clutter/LOD", "Flower LOD 1 distance (m)", &s.flowerLod1Distance, 0.0f, 200.0f, 0.5f);
	Tweak::floatVar("Clutter/LOD", "Flower LOD 2 distance (m)", &s.flowerLod2Distance, 0.0f, 300.0f, 0.5f);
	Tweak::floatVar("Clutter/Look", "Flower transmission", &s.flowerTransmission, 0.0f, 4.0f, 0.01f);
	Tweak::floatVar("Clutter/Look", "Flower roughness", &s.flowerRoughness, 0.05f, 1.0f, 0.01f);
	Tweak::floatVar("Clutter/Look", "Contact darkening", &s.contactDarkening, 0.0f, 1.0f, 0.01f);
	Tweak::floatVar("Clutter/Look", "Contact height (m)", &s.contactHeight, 0.001f, 1.0f, 0.001f);
	Tweak::floatVar("Clutter/Floor map", "Rebake distance (m)", &s.floorRebakeDistance, 4.0f, 150.0f, 0.5f);
	Tweak::intVar("Clutter", "Mesh resolution", &s.meshResolution, 8, 64, 1.0f);
}
