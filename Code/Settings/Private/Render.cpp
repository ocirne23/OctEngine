module Settings.Render;

import Core;
import Core.glm;
import Settings.Tweaks;

namespace
{
    constexpr oc::string_view s_tonemapperNames[] = { "Off", "Reinhard", "ACES", "AgX" };
    constexpr oc::string_view s_shadowDebugNames[] = { "Off", "Cascade index", "Cascade blend band", "Sun visibility", "Texel size" };
    constexpr oc::string_view s_cloudDebugNames[] = { "Off", "Step count", "Density only", "History rejection" };
    constexpr oc::string_view s_cloudShadowSplitNames[] = { "All texels per frame", "1/4 per frame", "1/16 per frame", "1/64 per frame" };
    constexpr oc::string_view s_dlssModeNames[] = { "Off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
    constexpr oc::string_view s_dlssPresetNames[] = { "Default", "J", "K", "L", "M" };
    constexpr oc::string_view s_treeDebugViews[] = { "Off", "Material", "Mesh (LOD)", "Fade side" };
    constexpr oc::string_view s_farTreePixelSkip[] = { "Off", "1 of 2 (checkerboard)", "1 of 4" };
    constexpr oc::string_view s_anisotropyNames[] = { "Off", "2x", "4x", "8x", "16x" };
    constexpr oc::string_view s_giDebugModeNames[] = { "Irradiance", "Cascade / LOD colour", "Update priority", "Relocation / backface", "Visibility" };
}

// The sun, the ambient and the up axis are the UBO's root (live): Runtime, so the "Sky" lock leaves them editable.
void Settings::registerSky(SkyParams& s)
{
    SkyParams* p = &s;
    Tweak::boolean("Sky", "Enabled", &s.enabled);
    Tweak::float3("Sky", "Sun Direction", &s.sunDirection, 0.01f, [p]() { p->sunDirection = glm::normalize(p->sunDirection); }, ETweakFlags::Runtime);
    Tweak::color3("Sky", "Sun Color", &s.sunColor, &s.sunIntensity, 0.0f, FLT_MAX, 0.05f, {}, ETweakFlags::Runtime);
    Tweak::color3("Sky", "Ambient", &s.ambientColor, &s.ambientIntensity, 0.0f, 0.2f, 0.001f, {}, ETweakFlags::Runtime);
    Tweak::color3("Sky", "Sky Radiance", &s.skyRadianceColor, &s.skyRadianceIntensity, 0.0f, 1.2f, 0.001f);
    Tweak::color3("Sky", "Ground Albedo", &s.groundColor, &s.groundIntensity, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Sky", "Ground Horizon", &s.groundHorizon, 0.0f, 0.5f, 0.01f);
    Tweak::floatVar("Sky/Sun", "Sun Angle Cos", &s.sunAngularCos, 0.9995f, 1.0f, 0.000001f);
    Tweak::floatVar("Sky/Sun", "Sun Glow", &s.sunGlow, 0.0, 5.0f, 0.01f);

    Tweak::floatVar("Sky/Atmosphere", "Scatter Boost", &s.scatterBoost, 0.0f, 32.0f);
    Tweak::floatVar("Sky/Atmosphere", "Rayleigh", &s.rayleighScatter, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Sky/Atmosphere", "Mie", &s.mieScatter, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Sky/Atmosphere", "Mie Anisotropy", &s.mieG, 0.0f, 0.99f);
    Tweak::floatVar("Sky/Atmosphere", "Rayleigh Height", &s.rayleighHeight, 1000.0f, 20000.0f, 10.0f);
    Tweak::floatVar("Sky/Atmosphere", "Mie Height", &s.mieHeight, 200.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Sky/Atmosphere", "Mie Extinction", &s.mieExtinction, 1.0f, 2.0f, 0.005f);
    Tweak::floatVar("Sky/Atmosphere", "Ozone", &s.ozone, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Density", &s.starDensity, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Stars", "Size", &s.starSize, 0.2f, 3.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Size Variation", &s.starSizeVar, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Brightness", &s.starBrightness, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Color Variation", &s.starColorVar, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Sky/Nebula", "Intensity", &s.nebulaIntensity, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Sky/Nebula", "Scale", &s.nebulaScale, 0.5f, 12.0f, 0.05f);
    Tweak::floatVar("Sky/Nebula", "Band Width", &s.nebulaBandWidth, 0.05f, 1.0f, 0.005f);
    Tweak::floatVar("Sky/Nebula", "Dust Lanes", &s.nebulaDust, 0.0f, 1.0f, 0.01f);
    Tweak::float3("Sky/Nebula", "Axis", &s.nebulaAxis, 0.01f, [p]() { p->nebulaAxis = glm::normalize(p->nebulaAxis); });
    Tweak::float3("Sky/Moon", "Direction", &s.moonDirection, 0.01f, [p]() { p->moonDirection = glm::normalize(p->moonDirection); });
    Tweak::floatVar("Sky/Moon", "Size", &s.moonSizeDeg, 0.05f, 10.0f, 0.01f);
    Tweak::floatVar("Sky/Moon", "Brightness", &s.moonBrightness, 0.0f, 2.0f);
    Tweak::float3("Sky", "Up Axis", &s.up, 0.01f, [p]() { p->up = glm::normalize(p->up); }, ETweakFlags::Runtime);
}

// THE wind is live under the "Sky" lock (ETweakFlags::Runtime): its UBO values are never baked.
void Settings::registerWind(WindParams& s)
{
    Tweak::floatVar("Sky/Wind", "Speed (m/s)", &s.speed, 0.0f, 40.0f, 0.1f, {}, ETweakFlags::Runtime);
    Tweak::floatVar("Sky/Wind", "Direction (deg)", &s.angleDeg, 0.0f, 360.0f, 1.0f, {}, ETweakFlags::Runtime);
    Tweak::floatVar("Sky/Wind", "Gust strength (m/s)", &s.gustStrength, 0.0f, 20.0f, 0.1f, {}, ETweakFlags::Runtime);
    Tweak::floatVar("Sky/Wind", "Gust size (m)", &s.gustSize, 2.0f, 200.0f, 1.0f, {}, ETweakFlags::Runtime);
}

void Settings::registerShadow(ShadowParams& s)
{
    Tweak::enumVar("Shadows", "Debug mode", &s.debugMode, s_shadowDebugNames); // SHADOW_DEBUG define
    Tweak::floatVar("Shadows", "Max distance (m)", &s.maxDistance, 25.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Shadows", "Split lambda", &s.splitLambda, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Shadows", "Caster pad (m)", &s.casterPad, 0.0f, 5000.0f, 10.0f);
    Tweak::floatVar("Shadows", "Depth bias", &s.depthBias, 0.0f, 0.005f, 0.0001f);
    Tweak::floatVar("Shadows", "Normal bias", &s.normalBias, 0.0f, 10.0f);
    Tweak::floatVar("Shadows", "Terrain march start (m)", &s.terrainMarchStart, 0.0f, 20000.0f, 50.0f);
    Tweak::floatVar("Shadows", "Terrain march bias (m)", &s.terrainMarchBias, 0.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Shadows", "Terrain march spread", &s.terrainMarchSpread, 0.002f, 0.1f, 0.001f);
}

void Settings::registerFoliage(FoliageParams& s)
{
    Tweak::enumVar("Trees", "Debug view", &s.debugView, s_treeDebugViews); // TREE_DEBUG define
    // In the Procedural TreeSystem's "Trees" category: the tree billboards are their only user.
    Tweak::floatVar("Trees", "Foliage crown normal", &s.crownNormal, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage shadow length (m)", &s.shadowLength, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Trees", "Foliage interior depth start", &s.interiorStart, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage interior depth end", &s.interiorEnd, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage interior view fade", &s.interiorViewFade, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage self shadow", &s.selfShadow, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage transmission self shadow", &s.transmissionSelfShadow, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage interior shadow", &s.interiorShadow, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage interior shadow top card scale", &s.interiorTopCardScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage transmission", &s.transmission, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage transmission focus", &s.transmissionFocus, 1.0f, 64.0f, 0.1f);
    Tweak::floatVar("Trees", "Foliage transmission glow", &s.transmissionGlow, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage transmission shadow", &s.transmissionShadow, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage min N.V", &s.minNoV, 0.0f, 0.9f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade start", &s.edgeFadeStart, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade end", &s.edgeFadeEnd, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade centre scale", &s.edgeFadeCentreScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade top card scale", &s.edgeFadeTopCardScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage shadow cascade margin (m)", &s.shadowCascadeMargin, 0.0f, 10000.0f, 1.0f);
    Tweak::floatVar("Trees", "RT range (m)", &s.rtRange, 0.0f, 10000.0f, 5.0f);
    Tweak::floatVar("Trees/Wind", "Bend", &s.windBend, 0.0f, 0.2f, 0.0005f);
    Tweak::floatVar("Trees/Wind", "Reference height (m)", &s.windRefHeight, 1.0f, 50.0f, 0.1f);
    Tweak::floatVar("Trees/Wind", "Sway", &s.windSway, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Trees/Wind", "Sway frequency (Hz)", &s.windSwayFrequency, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees/Wind", "Branch", &s.windBranch, 0.0f, 0.5f, 0.001f);
    Tweak::floatVar("Trees/Wind", "Branch frequency (Hz)", &s.windBranchFrequency, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Trees/Wind", "Leaf", &s.windLeaf, 0.0f, 0.5f, 0.001f);
    Tweak::floatVar("Trees/Wind", "Leaf frequency (Hz)", &s.windLeafFrequency, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Trees/Wind", "Branch fade start (m)", &s.windBranchFadeStart, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Trees/Wind", "Branch fade end (m)", &s.windBranchFadeEnd, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Trees/Wind", "Leaf fade start (m)", &s.windLeafFadeStart, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Trees/Wind", "Leaf fade end (m)", &s.windLeafFadeEnd, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Trees/Wind", "Trunk fade end (m)", &s.windTrunkFadeEnd, 0.0f, 20000.0f, 10.0f);
    Tweak::floatVar("Trees/Wind", "Billboard waves", &s.windBillboardWaves, 0.0f, 0.1f, 0.0005f);
}

void Settings::registerFarTree(FarTreeParams& s)
{
    Tweak::boolean("Trees", "Far volume", &s.enabled);
    Tweak::floatVar("Trees", "Far start (m)", &s.startDistance, 50.0f, 40000.0f, 5.0f);
    Tweak::floatVar("Trees", "Far end (m)", &s.endDistance, 100.0f, 100000.0f, 10.0f);
    Tweak::floatVar("Trees", "Far overlap (m)", &s.overlap, 1.0f, 5000.0f, 1.0f);
    Tweak::intVar("Trees", "Far angular resolution", (int*)&s.angularRes, 64, 8192, 1.0f);
    Tweak::intVar("Trees", "Far radial resolution", (int*)&s.radialRes, 16, 4096, 1.0f);
    Tweak::intVar("Trees", "Far slices", (int*)&s.slices, 2, 64, 1.0f);
    Tweak::floatVar("Trees", "Far height (m)", &s.height, 5.0f, 200.0f, 0.5f);
    Tweak::floatVar("Trees", "Far density", &s.densityScale, 0.0f, 10.0f, 0.01f);
    Tweak::floatVar("Trees", "Far blob shrink", &s.blobShrink, 0.0f, 2.0f, 0.001f);
    Tweak::floatVar("Trees", "Far step scale", &s.stepScale, 0.05f, 4.0f, 0.01f);
    Tweak::intVar("Trees", "Far max steps", (int*)&s.maxSteps, 8, 2048, 1.0f);
    Tweak::floatVar("Trees", "Far ambient", &s.ambient, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Trees", "Far sun scale", &s.sunScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Far self shadow", &s.selfShadow, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Trees", "Far normal strength", &s.normalStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Far ground darkening", &s.groundDarkening, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Far interior shadow", &s.interiorShadow, 0.0f, 20.0f, 0.01f);
    Tweak::floatVar("Trees", "Far interior radius", &s.interiorRadius, 0.0f, 0.5f, 0.001f);
    Tweak::floatVar("Trees", "Far forward scatter", &s.forwardScatter, -0.9f, 0.9f, 0.01f);
    Tweak::floatVar("Trees", "Far albedo scale", &s.albedoScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Far saturation scale", &s.saturationScale, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Trees", "Far temporal blend", &s.temporalBlend, 0.0f, 0.98f, 0.01f);
    Tweak::boolean("Trees", "Far half res", &s.halfRes);
    Tweak::enumVar("Trees", "Far pixel skip", &s.pixelSkip, s_farTreePixelSkip);
    Tweak::floatVar("Trees", "Far rebake distance (m)", &s.rebakeDistance, 1.0f, 2000.0f, 1.0f);
    Tweak::boolean("Trees", "Far bake ahead", &s.bakeAhead);
    Tweak::floatVar("Trees", "Far record detail (m)", &s.recordDetail, 0.0f, 40000.0f, 10.0f);
    Tweak::intVar("Trees", "Far bake frames", &s.bakeFrames, 1, 120, 1.0f);
    Tweak::floatVar("Trees", "Far swap time (s)", &s.swapTime, 0.0f, 10.0f, 0.01f);
    Tweak::intVar("Trees", "Far floor smoothing", &s.floorSmoothing, 0, 8, 1.0f);
    Tweak::floatVar("Trees", "Far rock extinction (1/m)", &s.rockExtinction, 0.0f, 50.0f, 0.05f);
}

void Settings::registerRock(RockParams& s)
{
    Tweak::floatVar("Rocks/Material", "UV scale (x terrain rock)", &s.uvScale, 0.1f, 32.0f, 0.05f);
    Tweak::floatVar("Rocks/Material", "Ground cover", &s.coverAmount, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Rocks/Material", "Cover start (normal y)", &s.coverSlopeStart, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Rocks/Material", "Cover full (normal y)", &s.coverSlopeFull, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Rocks/Material", "Cover patch size (m)", &s.coverPatchSize, 0.1f, 50.0f, 0.05f);
    Tweak::floatVar("Rocks/Material", "Contact height (m)", &s.contactHeight, 0.0f, 5.0f, 0.01f);
    Tweak::floatVar("Rocks/Material", "Contact blend", &s.contactBlend, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Rocks/Material", "Contact darkening", &s.contactDarkening, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Rocks/Material", "Contact fade distance (m)", &s.contactFadeDistance, 10.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Rocks/Material", "Cavity AO", &s.cavityAo, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Rocks/Material", "Cavity cover", &s.cavityCover, 0.0f, 1.0f, 0.01f);
}

void Settings::registerGrass(GrassParams& s)
{
    Tweak::boolean("Grass", "Enabled", &s.enabled);
    Tweak::intVar("Grass", "Blades per patch", &s.bladesPerPatch, 16, GrassParams::MAX_BLADES, 1.0f); // rebuilds the blade index buffer
    Tweak::floatVar("Grass", "Patch size (m)", &s.patchSize, 1.0f, 16.0f, 0.1f);
    Tweak::floatVar("Grass", "Range (m)", &s.range, 5.0f, 500.0f, 1.0f);
    Tweak::floatVar("Grass", "Range fade (m)", &s.rangeFade, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Grass/Blade", "Height (m)", &s.bladeHeight, 0.01f, 3.0f, 0.005f);
    Tweak::floatVar("Grass/Blade", "Height variation", &s.heightVariation, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Blade", "Width (m)", &s.bladeWidth, 0.002f, 0.5f, 0.001f);
    Tweak::floatVar("Grass/Blade", "Root sink (m)", &s.rootSink, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Grass/Blade", "Curvature", &s.curvature, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/LOD", "Thinning start (m)", &s.thinStart, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Grass/LOD", "Thinning exponent", &s.thinExponent, 0.0f, 3.0f, 0.01f);
    Tweak::floatVar("Grass/LOD", "Width compensation", &s.widthCompensation, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/LOD", "Max width scale", &s.maxWidthScale, 1.0f, 16.0f, 0.05f);
    Tweak::floatVar("Grass/LOD", "LOD 1 distance (m)", &s.lod1Distance, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Grass/LOD", "LOD 2 distance (m)", &s.lod2Distance, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Grass/LOD", "LOD 3 distance (m)", &s.lod3Distance, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Grass/LOD", "Min pixel width", &s.minPixelWidth, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Grass/LOD", "LOD morph band", &s.lodMorphBand, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Wind", "Bend (per m/s)", &s.windBend, 0.0f, 0.5f, 0.001f);
    Tweak::floatVar("Grass/Wind", "Ripple (per m/s)", &s.rippleBend, 0.0f, 0.5f, 0.001f);
    Tweak::floatVar("Grass/Wind", "Ripple size (m)", &s.rippleSize, 0.5f, 200.0f, 0.1f);
    Tweak::floatVar("Grass/Wind", "Sway frequency (Hz)", &s.swayFrequency, 0.0f, 5.0f, 0.01f);
    Tweak::floatVar("Grass/Wind", "Fade start (m)", &s.windFadeStart, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Grass/Wind", "Fade end (m)", &s.windFadeEnd, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Grass/Cover", "Clump size (m)", &s.clumpSize, 0.2f, 50.0f, 0.05f);
    Tweak::floatVar("Grass/Cover", "Patchiness", &s.patchiness, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Cover", "Bare fraction", &s.bareFraction, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Cover", "Grow band", &s.growBand, 0.01f, 1.0f, 0.005f);
    Tweak::floatVar("Grass/Cover", "Size by cover", &s.sizeByCover, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Cover", "Canopy thinning", &s.canopyThinning, 0.0f, 1.0f, 0.01f);
    Tweak::color3("Grass/Look", "Root colour", &s.rootColor);
    Tweak::color3("Grass/Look", "Tip colour", &s.tipColor);
    Tweak::color3("Grass/Look", "Dry colour", &s.dryColor);
    Tweak::floatVar("Grass/Look", "Colour variation", &s.colorVariation, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Cold darkening", &s.coldDarkening, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Cold temperature (C)", &s.coldTemperature, -30.0f, 40.0f, 0.1f);
    Tweak::floatVar("Grass/Look", "Warm temperature (C)", &s.warmTemperature, -30.0f, 40.0f, 0.1f);
    Tweak::floatVar("Grass/Look", "Dry amount", &s.dryAmount, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Roughness", &s.roughness, 0.05f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Root occlusion", &s.rootOcclusion, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Shadow bias (m)", &s.shadowBias, 0.0f, 5.0f, 0.01f);
    Tweak::floatVar("Grass/Shadows", "Canopy shadow", &s.canopyShadow, 0.0f, 4.0f, 0.005f);
    Tweak::floatVar("Grass/Shadows", "Fleck size (m)", &s.fleckSize, 0.01f, 2.0f, 0.005f);
    Tweak::floatVar("Grass/Shadows", "Fleck contrast", &s.fleckContrast, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Shadows", "Fleck fade distance (m)", &s.fleckFadeDistance, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Grass/Shadows", "Fleck stretch", &s.fleckStretch, 0.0f, 8.0f, 0.01f);
    Tweak::boolean("Grass/Shadows", "Near shadows", &s.nearShadows);
    Tweak::floatVar("Grass/Shadows", "Near shadow range (m)", &s.nearShadowRange, 1.0f, 100.0f, 0.1f);
    Tweak::floatVar("Grass/Shadows", "Near shadow bias (m)", &s.nearShadowBias, 0.0f, 0.5f, 0.001f);
    Tweak::floatVar("Grass/Shadows", "Near shadow strength", &s.nearShadowStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Transmission", &s.transmission, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Roundness", &s.roundness, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Ground normal blend", &s.groundBlend, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Grass/Look", "Ground normal blend distance (m)", &s.groundBlendDistance, 1.0f, 500.0f, 0.5f);
}

void Settings::registerFog(FogParams& s)
{
    Tweak::boolean("Fog", "Enabled", &s.enabled);
    Tweak::floatVar("Fog", "Global Density", &s.density, 0.0f, 1.0f, 0.001f);
    Tweak::floatVar("Fog", "Height Base", &s.heightBase, -200.0f, 500.0f);
    Tweak::floatVar("Fog", "Height Falloff", &s.heightFalloff, 0.0f, 1.0f, 0.002f);
    Tweak::floatVar("Fog", "Terrain Follow", &s.terrainFollow, 0.0f, 1.0f, 0.01f);
    Tweak::color3("Fog", "Albedo", &s.albedo, &s.albedoIntensity);
    Tweak::floatVar("Fog", "Sun scatter", &s.sunScatter, 0.0f, 50.0f, 0.05f);
    Tweak::floatVar("Fog/Shaft haze", "Density (1/m)", &s.shaftHazeDensity, 0.0f, 0.01f, 0.00001f);
    Tweak::floatVar("Fog/Shaft haze", "Height (m)", &s.shaftHazeHeight, 10.0f, 10000.0f, 10.0f);
    Tweak::floatVar("Fog/Aerial perspective", "Strength", &s.aerialStrength, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Fog/Aerial perspective", "Max distance (km)", &s.aerialMaxDistanceKm, 1.0f, 400.0f, 1.0f);
    Tweak::floatVar("Fog", "Anisotropy", &s.anisotropy, -0.9f, 0.95f, 0.01f);
    Tweak::floatVar("Fog", "Range", &s.range, 32.0f, 4096.0f, 32.0f);
    Tweak::boolean("Fog/Far Field", "Enabled", &s.farField);
    Tweak::floatVar("Fog/Far Field", "Density Scale", &s.farFieldDensity, 0.0f, 4.0f, 0.05f);
    Tweak::floatVar("Fog/Far Field", "Thickness Scale", &s.farFieldThickness, 0.1f, 20.0f, 0.1f);
    Tweak::intVar("Fog/Far Field", "Ground Steps", &s.farFieldSteps, 1, 32);
    Tweak::floatVar("Fog/Far Field", "Max distance (km)", &s.farFieldMaxDistanceKm, 0.0f, 400.0f, 0.5f);
    Tweak::floatVar("Fog", "Slice Power", &s.slicePower, 0.4f, 1.5f, 0.01f);
    Tweak::floatVar("Fog", "Noise Scale", &s.noiseScale, 0.005f, 1.0f, 0.005f);
    Tweak::floatVar("Fog", "Noise Strength", &s.noiseStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Fog", "Temporal Blend", &s.temporalBlend, 0.0f, 0.97f, 0.01f);
    Tweak::floatVar("Fog", "Region strength", &s.regionStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Fog", "Underwater density", &s.underwaterDensity, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Fog", "Lake underwater density", &s.inlandUnderwaterDensity, 0.0f, 20.0f, 0.1f); // lakes and rivers
    Tweak::floatVar("Fog", "Underwater wave offset", &s.underwaterWaveOffset, 0.0f, 2.0f, 0.01f); // x the live wave trough, lowers the boundary
    Tweak::floatVar("Fog", "Shaft boost", &s.shaftBoost, 0.0f, 100.0f, 0.1f);
    Tweak::floatVar("Fog", "Caustic strength", &s.causticStrength, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Fog", "Caustic depth fade", &s.causticDepthFade, 0.0f, 0.5f, 0.005f);
    Tweak::floatVar("Fog", "Caustic shore fade (m)", &s.causticShoreFade, 0.0f, 10.0f, 0.05f);
    Tweak::intVar("Fog/Quality", "Sun Rays", &s.sunRays, 1, 8);
    Tweak::floatVar("Fog/Quality", "Terrain Shadow Dist", &s.terrainShadowDist, 32.0f, 8192.0f, 16.0f);
    Tweak::floatVar("Fog/Quality", "Sun Softness", &s.sunSoftness, 0.0f, 0.2f, 0.005f);
    Tweak::boolean("Fog/Quality", "Spatial Filter", &s.spatialFilter);
    Tweak::boolean("Fog/Quality", "GI Ambient", &s.giAmbient);
    Tweak::boolean("Fog/Quality", "Light Shadows", &s.lightShadows);
}

// Enabled, the two shadow bools, Checkerboard and Debug mode are baked defines: the Renderer's listener reloads.
void Settings::registerClouds(CloudParams& s)
{
    CloudParams* p = &s;
    Tweak::boolean("Sky/Clouds", "Enabled", &s.enabled);
    Tweak::floatVar("Sky/Clouds", "Bottom (m)", &s.bottom, 0.0f, 12000.0f, 10.0f, [p]() { p->top = oc::max(p->top, p->bottom + 100.0f); }, ETweakFlags::Runtime);
    Tweak::floatVar("Sky/Clouds", "Top (m)", &s.top, 100.0f, 10000.0f, 10.0f, [p]() { p->bottom = oc::min(p->bottom, p->top - 100.0f); }, ETweakFlags::Runtime);
    // Live under the "Sky/Clouds" lock: the values it feeds (upper coverage, the shadow's mean) are never baked.
    Tweak::floatVar("Sky/Clouds", "Coverage", &s.coverage, 0.0f, 1.0f, 0.01f, {}, ETweakFlags::Runtime);
    Tweak::floatVar("Sky/Clouds", "Coverage variation", &s.coverageVariation, 0.0f, 2.0f, 0.01f, {}, ETweakFlags::Runtime);
    Tweak::floatVar("Sky/Clouds", "Density (1/m)", &s.densityScale, 0.001f, 0.1f, 0.001f);
    Tweak::floatVar("Sky/Clouds", "Weather size (km)", &s.weatherSizeKm, 5.0f, 500.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds", "Wind speed scale", &s.windSpeedScale, 0.0f, 100.0f, 0.1f); // x "Sky/Wind/Speed"
    Tweak::floatVar("Sky/Clouds", "Evolve speed (m/s)", &s.evolveSpeed, 0.0f, 100.0f);

    Tweak::floatVar("Sky/Clouds/Lighting", "Aerial perspective strength", &s.aerialStrength, 0.0f, 5.0f); // 0 = off
    Tweak::floatVar("Sky/Clouds/Lighting", "Multi-scatter", &s.multiScatter, 0.0f, 0.95f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Multi-scatter strength", &s.multiScatterStrength, 0.0f, 5.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Ambient", &s.ambient, 0.0f, 4.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Ground albedo", &s.groundAlbedo, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Ground light depth (m)", &s.groundLightDepth, 10.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Droplet size (um)", &s.dropletSize, 5.0f, 50.0f, 0.1f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Forward peak limit", &s.forwardPeakLimit, 0.8f, 1.0f, 0.001f);
    //Tweak::floatVar("Sky/Clouds/Lighting", "Powder", &s.powder, 0.0f, 1.0f, 0.01f); // CLOUD_POWDER while > 0 (a define: needs a Renderer listener)

    Tweak::floatVar("Sky/Clouds/Shape", "Type", &s.cloudType, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Type variation", &s.typeVariation, 0.0f, 2.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Base height variation", &s.baseVariation, 0.0f, 0.6f);
    Tweak::floatVar("Sky/Clouds/Shape", "Tower variation", &s.towerVariation, 0.0f, 0.9f);
    Tweak::floatVar("Sky/Clouds/Shape", "Top roundness", &s.topRoundness, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Base sharpness", &s.baseSharpness, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Tower core link", &s.towerCoreLink, 0.0f, 1.0f);
    Tweak::intVar("Sky/Clouds/Shape", "Shelf count", &s.shelfCount, 0, 3);
    Tweak::floatVar("Sky/Clouds/Shape", "Shelf strength", &s.shelfStrength, 0.0f, 3.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Shelf thickness", &s.shelfThickness, 0.005f, 0.2f, 0.005f);
    Tweak::floatVar("Sky/Clouds/Shape", "Shelf spacing", &s.shelfSpacing, 0.0f, 0.5f);
    Tweak::boolean("Sky/Clouds/Upper layer", "Enabled", &s.upperEnabled);
    Tweak::floatVar("Sky/Clouds/Upper layer", "Bottom (m)", &s.upperBottom, 0.0f, 12000.0f, 10.0f, [p]() { p->upperTop = oc::max(p->upperTop, p->upperBottom + 50.0f); });
    Tweak::floatVar("Sky/Clouds/Upper layer", "Top (m)", &s.upperTop, 50.0f, 15000.0f, 10.0f, [p]() { p->upperBottom = oc::min(p->upperBottom, p->upperTop - 50.0f); });
    Tweak::floatVar("Sky/Clouds/Upper layer", "Coverage", &s.upperCoverage, 0.0f, 2.0f); // x the main coverage
    Tweak::floatVar("Sky/Clouds/Upper layer", "Type", &s.upperType, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Upper layer", "Height variation", &s.upperHeightVariation, 0.0f, 0.8f);
    Tweak::floatVar("Sky/Clouds/Upper layer", "Density", &s.upperDensity, 0.0f, 2.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Erosion", &s.erosion, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Erosion cutoff", &s.erosionCutoff, 0.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds/Quality", "Curl (m)", &s.curl, 0.0f, 1000.0f, 1.0f);
    Tweak::intVar("Sky/Clouds/Quality", "Base repeats", &s.baseRepeats, 1, 64);
    Tweak::intVar("Sky/Clouds/Quality", "Detail repeats", &s.detailRepeats, 1, 64);

    Tweak::boolean("Sky/Clouds/Shadows", "Enabled", &s.shadows);
    Tweak::floatVar("Sky/Clouds/Shadows", "Strength", &s.shadowStrength, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shadows", "Near cascade (km)", &s.shadowNearKm, 1.0f, 50.0f, 0.1f, [p]() { p->shadowFarKm = oc::max(p->shadowFarKm, p->shadowNearKm * 2.0f); });
    Tweak::floatVar("Sky/Clouds/Shadows", "Far cascade (km)", &s.shadowFarKm, 2.0f, 400.0f, 0.5f, [p]() { p->shadowNearKm = oc::min(p->shadowNearKm, p->shadowFarKm * 0.5f); });
    Tweak::intVar("Sky/Clouds/Shadows", "Near steps", &s.shadowNearSteps, 4, 256);
    Tweak::intVar("Sky/Clouds/Shadows", "Far steps", &s.shadowFarSteps, 4, 256);
    Tweak::floatVar("Sky/Clouds/Shadows", "Far softness", &s.shadowFarSoftness, 0.0f, 4.0f);
    Tweak::enumVar("Sky/Clouds/Shadows", "Near update split", &s.shadowNearSplit, s_cloudShadowSplitNames);
    Tweak::enumVar("Sky/Clouds/Shadows", "Far update split", &s.shadowFarSplit, s_cloudShadowSplitNames);
    Tweak::boolean("Sky/Clouds/Shadows", "Self-shadow from map", &s.selfShadowFromMap);

    Tweak::intVar("Sky/Clouds/Quality", "Max steps", &s.maxSteps, 16, 1024);
    Tweak::floatVar("Sky/Clouds/Quality", "Max distance (km)", &s.maxDistanceKm, 1.0f, 400.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds/Quality", "Near step (m)", &s.nearStep, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds/Quality", "Min step (m)", &s.minStep, 0.25f, 50.0f, 0.25f);
    Tweak::intVar("Sky/Clouds/Quality", "Steps per ray", &s.stepsPerRay, 16, 1024);
    Tweak::intVar("Sky/Clouds/Quality", "Light steps", &s.lightSteps, 0, 16);
    Tweak::floatVar("Sky/Clouds/Quality", "Light distance (m)", &s.lightDistance, 50.0f, 20000.0f, 10.0f); // the cap on the per-sample reach
    Tweak::floatVar("Sky/Clouds/Quality", "Temporal blend", &s.temporalBlend, 0.0f, 0.98f);
    Tweak::floatVar("Sky/Clouds/Quality", "Sky map history (s)", &s.skyMapHistorySec, 0.0f, 30.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "GI sky history (s)", &s.giSkyHistorySec, 0.0f, 30.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Sky map min samples", &s.skyMapMinSamples, 0.0f, 64.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds/Quality", "GI sky observer radius (m)", &s.giSkyObserverRadius, 0.0f, 20000.0f, 10.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Near detail radius (m)", &s.nearDetailRadius, 0.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Detail distance (km)", &s.detailDistanceKm, 0.5f, 400.0f, 0.1f);
    Tweak::boolean("Sky/Clouds/Quality", "Checkerboard", &s.checkerboard); // CLOUD_CHECKERBOARD
    Tweak::enumVar("Sky/Clouds/Quality", "Debug mode", &s.debugMode, s_cloudDebugNames); // CLOUD_DEBUG_MODE
}

void Settings::registerRT(RTParams& s)
{
    Tweak::boolean("RT", "Enable RT", &s.enabled);
    Tweak::boolean("GI", "Enable GI", &s.giEnabled); // its own Graphics category (with the GiSettings knobs); still needs "RT/Enable RT"
    Tweak::boolean("RT", "RT Lights", &s.rtLightShadows);
    Tweak::boolean("RT", "RT Sun", &s.rtSunShadow);
    Tweak::intVar("RT", "RT Sun Rays", &s.sunShadowRays, 1, 8);
    Tweak::boolean("RT", "RT Sky Radiance", &s.rtSkyRadiance);
    Tweak::intVar("RT", "BLAS LOD level", &s.blasLodLevel, 0, 4);
    Tweak::boolean("RT", "BLAS compaction", &s.blasCompaction);
}

void Settings::registerRTAO(RTAOParams& s)
{
    Tweak::boolean("RTAO", "Enabled", &s.enabled);
    Tweak::intVar("RTAO", "Rays Per Pixel", &s.rays, 1, 32, 1.0f);
    Tweak::floatVar("RTAO", "Radius", &s.radius, 0.0f, 32.0f, 0.01f);
    Tweak::floatVar("RTAO", "Power", &s.power, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("RTAO", "Intensity", &s.intensity, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("RTAO", "Fade Start", &s.fadeStart, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("RTAO", "Max Distance", &s.maxDistance, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("RTAO", "Normal Bias", &s.normalBias, 0.0f, 0.2f, 0.001f);
    Tweak::floatVar("RTAO", "Distance Bias", &s.distanceBias, 0.0f, 0.01f, 0.0002f);
    Tweak::floatVar("RTAO", "Max History", &s.maxHistory, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("RTAO", "Blur Radius", &s.blurRadius, 0, 8, 1.0f);
    Tweak::boolean("RTAO", "Alpha Test", &s.alphaTest);
}

void Settings::registerTAA(TAAParams& s)
{
    Tweak::boolean("TAA", "Enabled", &s.taaEnabled);
    Tweak::floatVar("TAA", "History Feedback", &s.taaFeedback, 0.0f, 0.98f, 0.01f);
    Tweak::floatVar("TAA", "Ocean feedback", &s.taaOceanFeedback, 0.0f, 0.98f, 0.01f);
}

void Settings::registerDlss(DlssParams& s)
{
    Tweak::enumVar("Post/DLSS", "Mode", &s.mode, s_dlssModeNames);
    Tweak::enumVar("Post/DLSS", "Preset", &s.preset, s_dlssPresetNames);
    Tweak::boolean("Post/DLSS", "Mip bias", &s.mipBias);
    Tweak::floatVar("Post/DLSS", "Ocean current bias", &s.oceanBias, 0.0f, 1.0f, 0.01f); // the frame UBO's u_post_dlssOceanBias
    Tweak::boolean("Post/DLSS", "Verbose log (restart)", &s.verboseLog, {}, ETweakFlags::Saved);
}

void Settings::registerMotionBlur(MotionBlurParams& s)
{
    Tweak::boolean("Post/Motion blur", "Enabled", &s.enabled);
    Tweak::floatVar("Post/Motion blur", "Shutter", &s.shutter, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Post/Motion blur", "Max radius (px)", &s.maxRadius, 1.0f, 32.0f, 0.5f);
    Tweak::floatVar("Post/Motion blur", "Camera motion", &s.cameraScale, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("Post/Motion blur", "Samples", &s.samples, 4, 32, 1.0f);
}

void Settings::registerBloom(BloomParams& s)
{
    Tweak::boolean("Post/Bloom", "Enabled", &s.enabled);
    Tweak::floatVar("Post/Bloom", "Intensity", &s.intensity, 0.0f, 2.0f, 0.005f);
    Tweak::floatVar("Post/Bloom", "Threshold", &s.threshold, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Post/Bloom", "Knee", &s.knee, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Post/Bloom", "Radius", &s.radius, 0.0f, 1.0f, 0.01f);
    Tweak::intVar("Post/Bloom", "Levels", &s.levels, 2, 7, 1.0f);
}

void Settings::registerPost(PostParams& s)
{
    Tweak::floatVar("Post", "Exposure (EV)", &s.exposureEV, -8.0f, 8.0f, 0.05f);
    Tweak::enumVar("Post", "Tonemapper", &s.tonemapper, s_tonemapperNames);
    Tweak::boolean("Post", "Auto Exposure", &s.autoExposure);
    Tweak::floatVar("Post", "Adapt Speed (s)", &s.adaptTau, 0.05f, 5.0f, 0.05f);
    Tweak::floatVar("Post", "Adapt Key", &s.adaptKey, 0.02f, 0.5f, 0.005f);
    Tweak::floatVar("Post", "Adapt Min LogLum", &s.adaptMinLogLum, -12.0f, 0.0f, 0.1f);
    Tweak::floatVar("Post", "Adapt Max LogLum", &s.adaptMaxLogLum, 0.0f, 12.0f, 0.1f);
    Tweak::floatVar("Post", "Adapt Min EV", &s.adaptMinEV, -12.0f, 0.0f, 0.1f);
    Tweak::floatVar("Post", "Adapt Max EV", &s.adaptMaxEV, 0.0f, 12.0f, 0.1f);
}

void Settings::registerMeshLod(MeshLodParams& s)
{
    Tweak::boolean("LOD", "Enabled", &s.enabled);
    Tweak::floatVar("LOD", "Max error (px)", &s.maxErrorPixels, 0.05f, 3.0f, 0.01f);
    Tweak::floatVar("LOD", "Full-res pixels (Authored Lods)", &s.fullResPixels, 16.0f, 4096.0f, 1.0f);
    Tweak::intVar("LOD", "Bias", &s.bias, -6, 6);
    Tweak::floatVar("LOD", "Hysteresis", &s.hysteresis, 0.0f, 0.9f, 0.01f);
    Tweak::intVar("LOD", "Force LOD", &s.forceLod, -1, 6);
    Tweak::boolean("LOD", "Generate LODs", &s.generate);
    Tweak::intVar("LOD", "Generated levels", &s.generateLevels, 1, 6);
    Tweak::floatVar("LOD", "Generated reduction", &s.generateReduction, 0.05f, 0.75f, 0.01f);
    Tweak::intVar("LOD", "Min indices", &s.minIndices, 32, 4096);
}

void Settings::registerLightGrid(LightGridParams& s)
{
    Tweak::floatVar("LOD/Light grid", "LOD start (m)", &s.lodStart, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("LOD/Light grid", "LOD step (m)", &s.lodStep, 0.5f, 1000.0f, 0.5f);
    Tweak::floatVar("LOD/Light grid", "LOD power", &s.lodPower, 0.1f, 4.0f, 0.05f);
    Tweak::intVar("LOD/Light grid", "Min cell (log2 m)", &s.minCellLog2, 0, 5, 1.0f);
    Tweak::intVar("LOD/Light grid", "Max cell (log2 m)", &s.maxCellLog2, 0, 5, 1.0f);
    Tweak::intVar("LOD/Light grid", "Per-cell budget (cells)", &s.cellBudget, 8, 32768, 8.0f);
    // 0 off, 1 grid cells, 2 light count heat, 3 light ranges - the LIGHT_GRID_DEBUG define on the lit fragments.
    // Sun cascades: the baked "Shadows/Debug mode".
    Tweak::intVar("LOD/Light grid", "Debug Mode", &s.debugMode, 0, 3, 0.0f, {}, ETweakFlags::None);
}

void Settings::registerRenderer(RendererSettings& s)
{
    Tweak::boolean("Editor", "Wireframe", &s.wireframe);
    // The scene textures' max anisotropy (materials + terrain splat).
    Tweak::enumVar("Renderer/Textures", "Anisotropy", &s.anisotropyLevel, s_anisotropyNames, {}, ETweakFlags::Saved);
    Tweak::boolean("Decals", "Enabled", &s.decals);
    Tweak::boolean("Renderer", "Log pipeline stats", &s.logPipelineStats); // F5 re-creates the pipelines with it
    Tweak::boolean("Renderer", "Overlap compute", &s.overlapCompute);
    Tweak::boolean("Time", "VSync", &s.vsync, {}, ETweakFlags::Saved);
    Tweak::boolean("Terrain/Water", "Diffusion on", &s.terrainWetDiffusion);
}

// The grid shape and the volume are shader defines (every probe-sampling shader); the debug colour and radius are
// push constants of a cached secondary. The Renderer's listeners handle both.
void Settings::registerGi(GiSettings& s)
{
    GiGridConfig& grid = s.grid;
    Tweak::intVar("GI", "Cascades", &grid.numCascades, 1, 8, 1.0f);
    Tweak::intVar("GI", "Probes X (log2)", &grid.dimLog2X, 2, 6, 1.0f);
    Tweak::intVar("GI", "Probes Y (log2)", &grid.dimLog2Y, 2, 6, 1.0f);
    Tweak::intVar("GI", "Probes Z (log2)", &grid.dimLog2Z, 2, 6, 1.0f);
    Tweak::floatVar("GI", "Focus Y offset (m)", &grid.focusOffsetY, -128.0f, 128.0f, 0.5f);
    // The irradiance volume: its images + the GI_VOLUME / GI_VOLUME_RES defines - NOT the probe buffer, so a
    // volume change keeps the traced history.
    Tweak::boolean("GI", "Irradiance volume", &grid.volume);
    Tweak::intVar("GI", "Volume voxels per probe", &grid.volumeRes, 1, 2, 1.0f);

    Tweak::intVar("GI", "Rays Per Probe", &s.raysPerProbe, 1, 128);
    Tweak::floatVar("GI", "Update Interval Mult", &s.updateIntervalMult, 1.0f, 32.0f, 0.5f);
    Tweak::floatVar("GI", "Priority Distance (m)", &s.priorityDist, 1.0f, 512.0f, 1.0f);
    Tweak::floatVar("GI", "Priority Falloff", &s.priorityFalloff, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("GI", "Priority Frustum Weight", &s.priorityFrustumWeight, 1.0f, 8.0f, 0.1f);
    Tweak::floatVar("GI", "Temporal Alpha", &s.temporalAlpha, 0.0f, 0.05f, 0.001f);
    Tweak::floatVar("GI", "Max Ray Distance", &s.maxRayDist, 0.0f, 128.0f);
    Tweak::floatVar("GI", "Strength", &s.strength, 0.0f, 10.0f, 0.01f);
    Tweak::floatVar("RT", "TLAS Range", &s.tlasRange, 16.0f, 8192.0f, 16.0f);
    Tweak::floatVar("GI", "Vis Variance Floor", &s.visVarianceFloor, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("GI", "Vis Weight Floor", &s.visWeightFloor, 0.0f, 0.25f, 0.005f);
    Tweak::floatVar("GI", "Vis Mean Scale", &s.visMeanScale, 0.5f, 3.0f, 0.05f);

    Tweak::boolean("GI", "Debug probes", &s.debugEnabled);
    Tweak::enumVar("GI", "Debug probe colour", &s.debugMode, s_giDebugModeNames);
    Tweak::floatVar("GI", "Debug probe radius", &s.debugRadius, 0.02f, 1.0f, 0.01f);
}

void Settings::registerParticles(ParticleParams& s)
{
    Tweak::boolean("Particles", "Enabled", &s.enabled);
    Tweak::boolean("Particles", "Depth collision", &s.collision);
    Tweak::floatVar("Particles", "Time scale", &s.timeScale, 0.0f, 4.0f);
    Tweak::boolean("Particles", "Log stats", &s.logStats);
    Tweak::boolean("Particles", "Rain occlusion", &s.rainOcclusion);
    Tweak::floatVar("Particles", "Rain occlusion pad", &s.rainOcclusionCasterPad, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Particles", "Rain occlusion bias", &s.rainOcclusionTolerance, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Particles", "Rain occlusion foliage block", &s.rainOcclusionFoliageBlock, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Particles", "Streak camera blur", &s.streakCameraBlur, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Particles", "Anisotropy", &s.anisotropy, -0.9f, 0.95f, 0.01f);
    Tweak::floatVar("Particles", "Wind sheet contrast", &s.windSheetContrast, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Particles", "Wind sheet size", &s.windSheetSize, 2.0f, 200.0f, 1.0f);
    Tweak::floatVar("Particles", "Wind sheet drift", &s.windSheetDrift, 0.0f, 20.0f, 0.1f);
}

void Settings::registerOceanSpray(OceanSprayParams& s)
{
    Tweak::floatVar("Ocean", "Spray rate", &s.rate, 0.0f, 200.0f, 0.1f);
    Tweak::floatVar("Ocean", "Spray radius", &s.radius, 10.0f, 300.0f, 1.0f);
    Tweak::floatVar("Ocean", "Spray threshold", &s.threshold, 0.0005f, 0.025f, 0.0005f);
    Tweak::floatVar("Ocean", "Spray kick", &s.kick, 0.0f, 15.0f, 0.1f);
    Tweak::floatVar("Ocean", "Spray speed", &s.speed, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Ocean", "Spray forward offset", &s.forward, -5.0f, 5.0f, 0.05f);
    Tweak::floatVar("Ocean", "Spray height offset", &s.height, -2.0f, 2.0f, 0.01f);
}

void Settings::registerMeshStreaming(MeshStreamingSettings& s)
{
    Tweak::boolean("Mesh Streaming", "Enabled", &s.enabled);
    Tweak::intVar("Mesh Streaming", "Mesh budget (MB)", &s.budgetMB, 64, 8192);
    Tweak::intVar("Mesh Streaming", "Mesh cold frames", &s.coldFrames, 30, 3000);
    Tweak::intVar("Mesh Streaming", "Mesh max ops", &s.maxOpsInFlight, 1, 32);
    Tweak::intVar("Mesh Streaming", "Mesh max MB/frame", &s.maxStreamMBPerFrame, 4, 256);
}

void Settings::registerTextureStreaming(TextureStreamingSettings& s)
{
    Tweak::intVar("Texture Streaming", "Budget (MB)", &s.budgetMB, 64, 8192);
    Tweak::boolean("Texture Streaming", "Enabled", &s.enabled);
    Tweak::intVar("Texture Streaming", "Tail max dim", &s.tailMaxDim, 32, 512);
    Tweak::floatVar("Texture Streaming", "Mip bias", &s.mipBias, -4.0f, 4.0f);
    Tweak::floatVar("Texture Streaming", "Texel ratio", &s.texelRatio, 0.25f, 4.0f);
    Tweak::intVar("Texture Streaming", "Max ops in flight", &s.maxOpsInFlight, 0, 32);
    Tweak::floatVar("Texture Streaming", "Max MB/frame", &s.maxMBPerFrame, 1.0f, 64.0f);
    Tweak::boolean("Texture Streaming", "GPU mip copies", &s.gpuMipCopies);
    Tweak::intVar("Texture Streaming", "Demote hysteresis frames", &s.demoteHysteresisFrames, 1, 600);
    Tweak::intVar("Texture Streaming", "Decay frames", &s.decayFrames, 1, 3600);
    Tweak::boolean("Texture Streaming", "Debug rewrite all slots", &s.debugRewriteAllSlots);
}
