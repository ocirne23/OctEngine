module RendererVK;

import Core;
import Core.glm;
import Core.Tweaks;

namespace
{
    constexpr oc::string_view s_tonemapperNames[] = { "Off", "Reinhard", "ACES", "AgX" };
    constexpr oc::string_view s_shadowDebugNames[] = { "Off", "Cascade index", "Cascade blend band", "Sun visibility", "Texel size" };
    constexpr oc::string_view s_cloudDebugNames[] = { "Off", "Step count", "Density only", "History rejection" };
    constexpr oc::string_view s_cloudShadowSplitNames[] = { "All texels per frame", "1/4 per frame", "1/16 per frame", "1/64 per frame" };
    constexpr oc::string_view s_dlssModeNames[] = { "Off", "DLAA", "Quality", "Balanced", "Performance", "Ultra Performance" };
    constexpr oc::string_view s_dlssPresetNames[] = { "Default", "J", "K", "L", "M" };
}

void SkyParams::registerTweaks()
{
    Tweak::float3("Sky", "Sun Direction", &sunDirection, 0.01f, [&]() { sunDirection = glm::normalize(sunDirection); });
    Tweak::color3("Sky", "Sun Color", &sunColor, &sunIntensity);
    Tweak::color3("Sky", "Ambient", &ambientColor, &ambientIntensity, 0.0f, 0.2f, 0.001f);
    Tweak::color3("Sky", "Sky Radiance", &skyRadianceColor, &skyRadianceIntensity, 0.0f, 1.2f, 0.001f);
    Tweak::color3("Sky", "Ground Albedo", &groundColor, &groundIntensity, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Sky", "Ground Horizon", &groundHorizon, 0.0f, 0.5f, 0.01f);
    Tweak::floatVar("Sky/Sun", "Sun Angle Cos", &sunAngularCos, 0.9995f, 1.0f, 0.000001f);
    Tweak::floatVar("Sky/Sun", "Sun Glow", &sunGlow, 0.0, 5.0f, 0.01f);
    Tweak::floatVar("Sky/Sun", "Highlight Rolloff", &sunRolloff, 0.0f, 2.0f);
    Tweak::floatVar("Sky/Sun", "Rolloff Knee", &sunRolloffKnee, 0.1f, 1.0f, 0.005f);
    Tweak::floatVar("Sky/Sun", "Rolloff Headroom", &sunRolloffHeadroom, 0.5f, 32.0f, 0.05f);

    Tweak::floatVar("Sky/Atmosphere", "Scatter Boost", &scatterBoost, 0.0f, 32.0f);
    Tweak::floatVar("Sky/Atmosphere", "Rayleigh", &rayleighScatter, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Sky/Atmosphere", "Mie", &mieScatter, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Sky/Atmosphere", "Mie Anisotropy", &mieG, 0.0f, 0.99f);
    Tweak::floatVar("Sky/Atmosphere", "Rayleigh Height", &rayleighHeight, 1000.0f, 20000.0f, 10.0f);
    Tweak::floatVar("Sky/Atmosphere", "Mie Height", &mieHeight, 200.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Sky/Atmosphere", "Mie Extinction", &mieExtinction, 1.0f, 2.0f, 0.005f);
    Tweak::floatVar("Sky/Atmosphere", "Ozone", &ozone, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Density", &starDensity, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Stars", "Size", &starSize, 0.2f, 3.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Size Variation", &starSizeVar, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Brightness", &starBrightness, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Sky/Stars", "Color Variation", &starColorVar, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Sky/Nebula", "Intensity", &nebulaIntensity, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Sky/Nebula", "Scale", &nebulaScale, 0.5f, 12.0f, 0.05f);
    Tweak::floatVar("Sky/Nebula", "Band Width", &nebulaBandWidth, 0.05f, 1.0f, 0.005f);
    Tweak::floatVar("Sky/Nebula", "Dust Lanes", &nebulaDust, 0.0f, 1.0f, 0.01f);
    Tweak::float3("Sky/Nebula", "Axis", &nebulaAxis, 0.01f, [&]() { nebulaAxis = glm::normalize(nebulaAxis); });
    Tweak::float3("Sky/Moon", "Direction", &moonDirection, 0.01f, [&]() { moonDirection = glm::normalize(moonDirection); });
    Tweak::floatVar("Sky/Moon", "Size", &moonSizeDeg, 0.05f, 10.0f, 0.01f);
    Tweak::floatVar("Sky/Moon", "Brightness", &moonBrightness, 0.0f, 2.0f);
    Tweak::float3("Sky", "Up Axis", &up, 0.01f, [&]() { up = glm::normalize(up); });
}

void CloudParams::registerTweaks(const oc::function<void()>& onDefinesChanged)
{
    Tweak::boolean("Sky/Clouds", "Enabled", &enabled, onDefinesChanged);
    Tweak::floatVar("Sky/Clouds", "Bottom (m)", &bottom, 0.0f, 12000.0f, 10.0f, [&]() { top = oc::max(top, bottom + 100.0f); });
    Tweak::floatVar("Sky/Clouds", "Top (m)", &top, 100.0f, 10000.0f, 10.0f, [&]() { bottom = oc::min(bottom, top - 100.0f); });
    Tweak::floatVar("Sky/Clouds", "Coverage", &coverage, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds", "Coverage variation", &coverageVariation, 0.0f, 2.0f);
    Tweak::floatVar("Sky/Clouds", "Density (1/m)", &densityScale, 0.001f, 0.1f, 0.001f);
    Tweak::floatVar("Sky/Clouds", "Weather size (km)", &weatherSizeKm, 5.0f, 500.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds", "Wind speed (m/s)", &windSpeed, 0.0f, 100.0f, 0.1f);
    Tweak::floatVar("Sky/Clouds", "Wind angle (deg)", &windAngleDeg, 0.0f, 360.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds", "Evolve speed (m/s)", &evolveSpeed, 0.0f, 100.0f);

    Tweak::floatVar("Sky/Clouds/Lighting", "Aerial perspective strength", &aerialStrength, 0.0f, 5.0f); // 0 = off
    Tweak::floatVar("Sky/Clouds/Lighting", "Multi-scatter", &multiScatter, 0.0f, 0.95f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Multi-scatter strength", &multiScatterStrength, 0.0f, 5.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Ambient", &ambient, 0.0f, 4.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Ground albedo", &groundAlbedo, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Ground light depth (m)", &groundLightDepth, 10.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Droplet size (um)", &dropletSize, 5.0f, 50.0f, 0.1f);
    Tweak::floatVar("Sky/Clouds/Lighting", "Forward peak limit", &forwardPeakLimit, 0.8f, 1.0f, 0.001f);
    //Tweak::floatVar("Sky/Clouds/Lighting", "Powder", &powder, 0.0f, 1.0f, 0.01f, onDefinesChanged); // CLOUD_POWDER while > 0

    Tweak::floatVar("Sky/Clouds/Shape", "Type", &cloudType, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Type variation", &typeVariation, 0.0f, 2.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Base height variation", &baseVariation, 0.0f, 0.6f);
    Tweak::floatVar("Sky/Clouds/Shape", "Tower variation", &towerVariation, 0.0f, 0.9f);
    Tweak::floatVar("Sky/Clouds/Shape", "Top roundness", &topRoundness, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Base sharpness", &baseSharpness, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Tower core link", &towerCoreLink, 0.0f, 1.0f);
    Tweak::intVar("Sky/Clouds/Shape", "Shelf count", &shelfCount, 0, 3);
    Tweak::floatVar("Sky/Clouds/Shape", "Shelf strength", &shelfStrength, 0.0f, 3.0f);
    Tweak::floatVar("Sky/Clouds/Shape", "Shelf thickness", &shelfThickness, 0.005f, 0.2f, 0.005f);
    Tweak::floatVar("Sky/Clouds/Shape", "Shelf spacing", &shelfSpacing, 0.0f, 0.5f);
    Tweak::boolean("Sky/Clouds/Upper layer", "Enabled", &upperEnabled);
    Tweak::floatVar("Sky/Clouds/Upper layer", "Bottom (m)", &upperBottom, 0.0f, 12000.0f, 10.0f, [&]() { upperTop = oc::max(upperTop, upperBottom + 50.0f); });
    Tweak::floatVar("Sky/Clouds/Upper layer", "Top (m)", &upperTop, 50.0f, 15000.0f, 10.0f, [&]() { upperBottom = oc::min(upperBottom, upperTop - 50.0f); });
    Tweak::floatVar("Sky/Clouds/Upper layer", "Coverage", &upperCoverage, 0.0f, 2.0f); // x the main coverage
    Tweak::floatVar("Sky/Clouds/Upper layer", "Type", &upperType, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Upper layer", "Height variation", &upperHeightVariation, 0.0f, 0.8f);
    Tweak::floatVar("Sky/Clouds/Upper layer", "Density", &upperDensity, 0.0f, 2.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Erosion", &erosion, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Erosion cutoff", &erosionCutoff, 0.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds/Quality", "Curl (m)", &curl, 0.0f, 1000.0f, 1.0f);
    Tweak::intVar("Sky/Clouds/Quality", "Base repeats", &baseRepeats, 1, 64);
    Tweak::intVar("Sky/Clouds/Quality", "Detail repeats", &detailRepeats, 1, 64);

    Tweak::boolean("Sky/Clouds/Shadows", "Enabled", &shadows, onDefinesChanged);
    Tweak::floatVar("Sky/Clouds/Shadows", "Strength", &shadowStrength, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds/Shadows", "Near cascade (km)", &shadowNearKm, 1.0f, 50.0f, 0.1f, [&]() { shadowFarKm = oc::max(shadowFarKm, shadowNearKm * 2.0f); });
    Tweak::floatVar("Sky/Clouds/Shadows", "Far cascade (km)", &shadowFarKm, 2.0f, 400.0f, 0.5f, [&]() { shadowNearKm = oc::min(shadowNearKm, shadowFarKm * 0.5f); });
    Tweak::intVar("Sky/Clouds/Shadows", "Near steps", &shadowNearSteps, 4, 256);
    Tweak::intVar("Sky/Clouds/Shadows", "Far steps", &shadowFarSteps, 4, 256);
    Tweak::floatVar("Sky/Clouds/Shadows", "Far softness", &shadowFarSoftness, 0.0f, 4.0f);
    Tweak::enumVar("Sky/Clouds/Shadows", "Near update split", &shadowNearSplit, s_cloudShadowSplitNames);
    Tweak::enumVar("Sky/Clouds/Shadows", "Far update split", &shadowFarSplit, s_cloudShadowSplitNames);
    Tweak::boolean("Sky/Clouds/Shadows", "Self-shadow from map", &selfShadowFromMap, onDefinesChanged);

    Tweak::intVar("Sky/Clouds/Quality", "Max steps", &maxSteps, 16, 1024);
    Tweak::floatVar("Sky/Clouds/Quality", "Max distance (km)", &maxDistanceKm, 1.0f, 400.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds/Quality", "Near step (m)", &nearStep, 1.0f, 200.0f, 0.5f);
    Tweak::floatVar("Sky/Clouds/Quality", "Min step (m)", &minStep, 0.25f, 50.0f, 0.25f);
    Tweak::intVar("Sky/Clouds/Quality", "Steps per ray", &stepsPerRay, 16, 1024);
    Tweak::intVar("Sky/Clouds/Quality", "Light steps", &lightSteps, 0, 16);
    Tweak::floatVar("Sky/Clouds/Quality", "Light distance (m)", &lightDistance, 50.0f, 20000.0f, 10.0f); // the cap on the per-sample reach
    Tweak::floatVar("Sky/Clouds/Quality", "Temporal blend", &temporalBlend, 0.0f, 0.98f);
    Tweak::floatVar("Sky/Clouds/Quality", "Sky map history (s)", &skyMapHistorySec, 0.0f, 30.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Near detail radius (m)", &nearDetailRadius, 0.0f, 2000.0f, 5.0f);
    Tweak::floatVar("Sky/Clouds/Quality", "Detail distance (km)", &detailDistanceKm, 0.5f, 400.0f, 0.1f);
    Tweak::boolean("Sky/Clouds/Quality", "Checkerboard", &checkerboard, onDefinesChanged); // CLOUD_CHECKERBOARD
    Tweak::enumVar("Sky/Clouds/Quality", "Debug mode", &debugMode, s_cloudDebugNames, onDefinesChanged); // CLOUD_DEBUG_MODE
}

void ShadowParams::registerTweaks(const oc::function<void()>& onReloadShaders)
{
    Tweak::enumVar("Shadows", "Debug mode", &debugMode, s_shadowDebugNames, onReloadShaders); // SHADOW_DEBUG define
    Tweak::floatVar("Shadows", "Max distance (m)", &maxDistance, 25.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Shadows", "Split lambda", &splitLambda, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Shadows", "Caster pad (m)", &casterPad, 0.0f, 5000.0f, 10.0f);
    Tweak::floatVar("Shadows", "Depth bias", &depthBias, 0.0f, 0.005f, 0.0001f);
    Tweak::floatVar("Shadows", "Normal bias", &normalBias, 0.0f, 10.0f);
    Tweak::floatVar("Shadows", "Terrain march start (m)", &terrainMarchStart, 0.0f, 20000.0f, 50.0f);
    Tweak::floatVar("Shadows", "Terrain march bias (m)", &terrainMarchBias, 0.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Shadows", "Terrain march spread", &terrainMarchSpread, 0.002f, 0.1f, 0.001f);
}

void FoliageParams::registerTweaks()
{
    // In the Procedural TreeSystem's "Trees" category: the tree billboards are their only user.
    Tweak::floatVar("Trees", "Foliage depth offset", &depthOffset, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage crown normal", &crownNormal, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage shadow length (m)", &shadowLength, 0.0f, 20.0f, 0.05f);
    Tweak::floatVar("Trees", "Foliage interior shadow", &interiorShadow, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage interior inner radius", &interiorInner, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage interior outer radius", &interiorOuter, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage interior shadow top card scale", &interiorTopCardScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage transmission", &transmission, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage transmission focus", &transmissionFocus, 1.0f, 64.0f, 0.1f);
    Tweak::floatVar("Trees", "Foliage transmission glow", &transmissionGlow, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage transmission shadow", &transmissionShadow, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade start", &edgeFadeStart, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade end", &edgeFadeEnd, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade centre scale", &edgeFadeCentreScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Foliage edge fade top card scale", &edgeFadeTopCardScale, 0.0f, 4.0f, 0.01f);
}

void FarTreeParams::registerTweaks()
{
    Tweak::boolean("Trees", "Far volume", &enabled);
    Tweak::floatVar("Trees", "Far start (m)", &startDistance, 50.0f, 40000.0f, 5.0f);
    Tweak::floatVar("Trees", "Far end (m)", &endDistance, 100.0f, 100000.0f, 10.0f);
    Tweak::floatVar("Trees", "Far overlap (m)", &overlap, 1.0f, 5000.0f, 1.0f);
    Tweak::intVar("Trees", "Far angular resolution", (int*)&angularRes, 64, 8192, 1.0f);
    Tweak::intVar("Trees", "Far radial resolution", (int*)&radialRes, 16, 4096, 1.0f);
    Tweak::intVar("Trees", "Far slices", (int*)&slices, 2, 64, 1.0f);
    Tweak::floatVar("Trees", "Far height (m)", &height, 5.0f, 200.0f, 0.5f);
    Tweak::floatVar("Trees", "Far density", &densityScale, 0.0f, 10.0f, 0.01f);
    Tweak::floatVar("Trees", "Far blob shrink", &blobShrink, 0.0f, 2.0f, 0.001f);
    Tweak::floatVar("Trees", "Far step scale", &stepScale, 0.05f, 4.0f, 0.01f);
    Tweak::intVar("Trees", "Far max steps", (int*)&maxSteps, 8, 2048, 1.0f);
    Tweak::floatVar("Trees", "Far ambient", &ambient, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Trees", "Far sun scale", &sunScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Far self shadow", &selfShadow, 0.0f, 8.0f, 0.01f);
    Tweak::floatVar("Trees", "Far normal strength", &normalStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Far ground darkening", &groundDarkening, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Trees", "Far interior shadow", &interiorShadow, 0.0f, 20.0f, 0.01f);
    Tweak::floatVar("Trees", "Far interior radius", &interiorRadius, 0.0f, 0.5f, 0.001f);
    Tweak::floatVar("Trees", "Far forward scatter", &forwardScatter, -0.9f, 0.9f, 0.01f);
    Tweak::floatVar("Trees", "Far albedo scale", &albedoScale, 0.0f, 4.0f, 0.01f);
    Tweak::floatVar("Trees", "Far temporal blend", &temporalBlend, 0.0f, 0.98f, 0.01f);
    Tweak::boolean("Trees", "Far half res", &halfRes);
    static constexpr oc::string_view PIXEL_SKIP[] = { "Off", "1 of 2 (checkerboard)", "1 of 4" };
    Tweak::enumVar("Trees", "Far pixel skip", &pixelSkip, PIXEL_SKIP);
    Tweak::floatVar("Trees", "Far rebake distance (m)", &rebakeDistance, 1.0f, 2000.0f, 1.0f);
}

void FogParams::registerTweaks()
{
    Tweak::boolean("Fog", "Enabled", &enabled);
    Tweak::floatVar("Fog", "Global Density", &density, 0.0f, 1.0f, 0.001f);
    Tweak::floatVar("Fog", "Height Base", &heightBase, -200.0f, 500.0f);
    Tweak::floatVar("Fog", "Height Falloff", &heightFalloff, 0.0f, 1.0f, 0.002f);
    Tweak::floatVar("Fog", "Terrain Follow", &terrainFollow, 0.0f, 1.0f, 0.01f);
    Tweak::color3("Fog", "Albedo", &albedo, &albedoIntensity);
    Tweak::floatVar("Fog", "Sun scatter", &sunScatter, 0.0f, 50.0f, 0.05f);
    Tweak::floatVar("Fog/Shaft haze", "Density (1/m)", &shaftHazeDensity, 0.0f, 0.01f, 0.00001f);
    Tweak::floatVar("Fog/Shaft haze", "Height (m)", &shaftHazeHeight, 10.0f, 10000.0f, 10.0f);
    Tweak::floatVar("Fog", "Anisotropy", &anisotropy, -0.9f, 0.95f, 0.01f);
    Tweak::floatVar("Fog", "Range", &range, 32.0f, 4096.0f, 32.0f);
    Tweak::boolean("Fog/Far Field", "Enabled", &farField);
    Tweak::floatVar("Fog/Far Field", "Density Scale", &farFieldDensity, 0.0f, 4.0f, 0.05f);
    Tweak::floatVar("Fog/Far Field", "Thickness Scale", &farFieldThickness, 0.1f, 20.0f, 0.1f);
    Tweak::intVar("Fog/Far Field", "Ground Steps", &farFieldSteps, 1, 32);
    Tweak::floatVar("Fog/Far Field", "Max distance (km)", &farFieldMaxDistanceKm, 0.0f, 400.0f, 0.5f);
    Tweak::floatVar("Fog", "Slice Power", &slicePower, 0.4f, 1.5f, 0.01f);
    Tweak::floatVar("Fog", "Noise Scale", &noiseScale, 0.005f, 1.0f, 0.005f);
    Tweak::floatVar("Fog", "Noise Strength", &noiseStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Fog", "Wind Speed", &windSpeed, 0.0f, 20.0f);
    Tweak::floatVar("Fog", "Temporal Blend", &temporalBlend, 0.0f, 0.97f, 0.01f);
    Tweak::floatVar("Fog", "Region strength", &regionStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Fog", "Underwater density", &underwaterDensity, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Fog", "Underwater wave offset", &underwaterWaveOffset, 0.0f, 2.0f, 0.01f); // x the live wave trough, lowers the boundary
    Tweak::floatVar("Fog", "Shaft boost", &shaftBoost, 0.0f, 100.0f, 0.1f);
    Tweak::floatVar("Fog", "Caustic strength", &causticStrength, 0.0f, 2.0f, 0.05f);
    Tweak::floatVar("Fog", "Caustic depth fade", &causticDepthFade, 0.0f, 0.5f, 0.005f);
    Tweak::floatVar("Fog", "Caustic shore fade (m)", &causticShoreFade, 0.0f, 10.0f, 0.05f);
    Tweak::intVar("Fog/Quality", "Sun Rays", &sunRays, 1, 8);
    Tweak::floatVar("Fog/Quality", "Terrain Shadow Dist", &terrainShadowDist, 32.0f, 8192.0f, 16.0f);
    Tweak::floatVar("Fog/Quality", "Sun Softness", &sunSoftness, 0.0f, 0.2f, 0.005f);
    Tweak::boolean("Fog/Quality", "Spatial Filter", &spatialFilter);
    Tweak::boolean("Fog/Quality", "GI Ambient", &giAmbient);
    Tweak::boolean("Fog/Quality", "Light Shadows", &lightShadows);
}

void PostParams::registerTweaks(const oc::function<void()>& onReRecord)
{
    Tweak::floatVar("Post", "Exposure (EV)", &exposureEV, -8.0f, 8.0f, 0.05f, onReRecord);
    Tweak::enumVar("Post", "Tonemapper", &tonemapper, s_tonemapperNames, onReRecord);
    Tweak::boolean("Post", "Auto Exposure", &autoExposure, onReRecord);
    Tweak::floatVar("Post", "Adapt Speed (s)", &adaptTau, 0.05f, 5.0f, 0.05f);
    Tweak::floatVar("Post", "Adapt Key", &adaptKey, 0.02f, 0.5f, 0.005f);
    Tweak::floatVar("Post", "Adapt Min LogLum", &adaptMinLogLum, -12.0f, 0.0f, 0.1f);
    Tweak::floatVar("Post", "Adapt Max LogLum", &adaptMaxLogLum, 0.0f, 12.0f, 0.1f);
    Tweak::floatVar("Post", "Adapt Min EV", &adaptMinEV, -12.0f, 0.0f, 0.1f);
    Tweak::floatVar("Post", "Adapt Max EV", &adaptMaxEV, 0.0f, 12.0f, 0.1f);
}

void RTParams::registerTweaks(const oc::function<void()>& onReRecord, const oc::function<void()>& onReloadLitShaders)
{
    Tweak::boolean("RT", "Enable RT", &enabled, onReloadLitShaders);
    Tweak::boolean("GI", "Enable GI", &giEnabled, onReRecord); // its own Graphics category (with the GIProbePipeline knobs); still needs "RT/Enable RT"
    Tweak::boolean("RT", "RT Lights", &rtLightShadows, onReloadLitShaders);
    Tweak::boolean("RT", "RT Sun", &rtSunShadow, onReloadLitShaders);
    Tweak::intVar("RT", "RT Sun Rays", &sunShadowRays, 1, 8);
    Tweak::boolean("RT", "RT Sky Radiance", &rtSkyRadiance);
    Tweak::intVar("RT", "BLAS LOD level", &blasLodLevel, 0, 4);
    Tweak::boolean("RT", "BLAS compaction", &blasCompaction);
}

void LightGridParams::registerTweaks(const oc::function<void()>& onReloadLitShaders)
{
    Tweak::floatVar("LOD/Light grid", "LOD start (m)", &lodStart, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("LOD/Light grid", "LOD step (m)", &lodStep, 0.5f, 1000.0f, 0.5f);
    Tweak::floatVar("LOD/Light grid", "LOD power", &lodPower, 0.1f, 4.0f, 0.05f);
    Tweak::intVar("LOD/Light grid", "Min cell (log2 m)", &minCellLog2, 0, 5, 1.0f);
    Tweak::intVar("LOD/Light grid", "Max cell (log2 m)", &maxCellLog2, 0, 5, 1.0f);
    Tweak::intVar("LOD/Light grid", "Per-cell budget (cells)", &cellBudget, 8, 32768, 8.0f);
    // 0 off, 1 grid cells, 2 light count heat, 3 light ranges (LightGridParams::debugMode) - the
    // LIGHT_GRID_DEBUG define on the lit fragments. Sun cascades: the baked "Shadows/Debug mode".
    Tweak::intVar("LOD/Light grid", "Debug Mode", &debugMode, 0, 3, 0.0f, onReloadLitShaders, ETweakFlags::None);
}

void RTAOParams::registerTweaks(const oc::function<void()>& onReRecord, const oc::function<void()>& onReloadShaders)
{
    Tweak::boolean("RTAO", "Enabled", &enabled, onReRecord);
    Tweak::intVar("RTAO", "Rays Per Pixel", &rays, 1, 32, 1.0f, onReRecord);
    Tweak::floatVar("RTAO", "Radius", &radius, 0.0f, 32.0f, 0.01f, onReRecord);
    Tweak::floatVar("RTAO", "Power", &power, 0.0f, 8.0f, 0.01f, onReRecord);
    Tweak::floatVar("RTAO", "Intensity", &intensity, 0.0f, 4.0f, 0.01f, onReRecord);
    Tweak::floatVar("RTAO", "Fade Start", &fadeStart, 0.0f, 200.0f, 0.5f, onReRecord);
    Tweak::floatVar("RTAO", "Max Distance", &maxDistance, 0.0f, 200.0f, 0.5f, onReRecord);
    Tweak::floatVar("RTAO", "Normal Bias", &normalBias, 0.0f, 0.2f, 0.001f, onReRecord);
    Tweak::floatVar("RTAO", "Distance Bias", &distanceBias, 0.0f, 0.01f, 0.0002f, onReRecord);
    Tweak::floatVar("RTAO", "Max History", &maxHistory, 0.0f, 1.0f, 0.01f, onReRecord);
    Tweak::intVar("RTAO", "Blur Radius", &blurRadius, 0, 8, 1.0f, onReRecord);
    Tweak::boolean("RTAO", "Alpha Test", &alphaTest, onReloadShaders);
}

void TAAParams::registerTweaks(const oc::function<void()>& onReRecord)
{
    Tweak::boolean("TAA", "Enabled", &taaEnabled, onReRecord);
    Tweak::floatVar("TAA", "History Feedback", &taaFeedback, 0.0f, 0.98f, 0.01f, onReRecord);
    Tweak::floatVar("TAA", "Ocean feedback", &taaOceanFeedback, 0.0f, 0.98f, 0.01f, onReRecord);
}

void DlssParams::registerTweaks(const oc::function<void()>& onResize, const oc::function<void()>& onReRecord)
{
    Tweak::enumVar("Post/DLSS", "Mode", &mode, s_dlssModeNames, onResize);
    Tweak::enumVar("Post/DLSS", "Preset", &preset, s_dlssPresetNames, onReRecord);
    Tweak::boolean("Post/DLSS", "Mip bias", &mipBias, onResize);
    Tweak::floatVar("Post/DLSS", "Ocean current bias", &oceanBias, 0.0f, 1.0f, 0.01f, onReRecord); // a push constant of the cached mvec pass
    Tweak::boolean("Post/DLSS", "Verbose log (restart)", &verboseLog, {}, ETweakFlags::Saved);
}

void MotionBlurParams::registerTweaks(const oc::function<void()>& onReRecord)
{
    // All of it rides the cached secondaries (push constants) or gates them: every change re-records.
    Tweak::boolean("Post/Motion blur", "Enabled", &enabled, onReRecord);
    Tweak::floatVar("Post/Motion blur", "Shutter", &shutter, 0.0f, 2.0f, 0.01f, onReRecord);
    Tweak::floatVar("Post/Motion blur", "Max radius (px)", &maxRadius, 1.0f, 32.0f, 0.5f, onReRecord);
    Tweak::floatVar("Post/Motion blur", "Camera motion", &cameraScale, 0.0f, 1.0f, 0.01f, onReRecord);
    Tweak::intVar("Post/Motion blur", "Samples", &samples, 4, 32, 1.0f, onReRecord);
}

void BloomParams::registerTweaks(const oc::function<void()>& onReRecord)
{
    // Baked into the cached secondaries (the pass gate, the chain length, the composite's push constants).
    Tweak::boolean("Post/Bloom", "Enabled", &enabled, onReRecord);
    Tweak::floatVar("Post/Bloom", "Intensity", &intensity, 0.0f, 2.0f, 0.005f, onReRecord);
    Tweak::floatVar("Post/Bloom", "Threshold", &threshold, 0.0f, 8.0f, 0.01f, onReRecord);
    Tweak::floatVar("Post/Bloom", "Knee", &knee, 0.0f, 4.0f, 0.01f, onReRecord);
    Tweak::floatVar("Post/Bloom", "Radius", &radius, 0.0f, 1.0f, 0.01f, onReRecord);
    Tweak::intVar("Post/Bloom", "Levels", &levels, 2, 7, 1.0f, onReRecord);
}

void MeshLodParams::registerTweaks()
{
    Tweak::boolean("LOD", "Enabled", &enabled);
    Tweak::floatVar("LOD", "Max error (px)", &maxErrorPixels, 0.05f, 3.0f, 0.01f);
    Tweak::floatVar("LOD", "Full-res pixels (Authored Lods)", &fullResPixels, 16.0f, 4096.0f, 1.0f);
    Tweak::intVar("LOD", "Bias", &bias, -6, 6);
    Tweak::floatVar("LOD", "Hysteresis", &hysteresis, 0.0f, 0.9f, 0.01f);
    Tweak::intVar("LOD", "Force LOD", &forceLod, -1, 6);
    Tweak::boolean("LOD", "Generate LODs", &generate);
    Tweak::intVar("LOD", "Generated levels", &generateLevels, 1, 6);
    Tweak::floatVar("LOD", "Generated reduction", &generateReduction, 0.05f, 0.75f, 0.01f);
    Tweak::intVar("LOD", "Min indices", &minIndices, 32, 4096);
}

void ParticleParams::registerTweaks()
{
    Tweak::boolean("Particles", "Enabled", &enabled);
    Tweak::boolean("Particles", "Depth collision", &collision);
    Tweak::floatVar("Particles", "Time scale", &timeScale, 0.0f, 4.0f);
    Tweak::boolean("Particles", "Log stats", &logStats);
    Tweak::boolean("Particles", "Rain occlusion", &rainOcclusion);
    Tweak::floatVar("Particles", "Rain occlusion pad", &rainOcclusionCasterPad, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Particles", "Rain occlusion bias", &rainOcclusionTolerance, 0.0f, 2.0f, 0.01f);
    Tweak::floatVar("Particles", "Streak camera blur", &streakCameraBlur, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Particles", "Anisotropy", &anisotropy, -0.9f, 0.95f, 0.01f);
    Tweak::floatVar("Particles", "Wind speed", &windSpeed, 0.0f, 40.0f, 0.1f);
    Tweak::floatVar("Particles", "Wind angle", &windAngleDeg, 0.0f, 360.0f, 1.0f);
    Tweak::floatVar("Particles", "Wind gust strength", &windGustStrength, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Particles", "Wind gust size", &windGustSize, 2.0f, 200.0f, 1.0f);
    Tweak::floatVar("Particles", "Wind sheet contrast", &windSheetContrast, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Particles", "Wind sheet size", &windSheetSize, 2.0f, 200.0f, 1.0f);
    Tweak::floatVar("Particles", "Wind sheet drift", &windSheetDrift, 0.0f, 20.0f, 0.1f);
}

void OceanSprayParams::registerTweaks()
{
    Tweak::floatVar("Ocean", "Spray rate", &rate, 0.0f, 200.0f, 0.1f);
    Tweak::floatVar("Ocean", "Spray radius", &radius, 10.0f, 300.0f, 1.0f);
    Tweak::floatVar("Ocean", "Spray threshold", &threshold, 0.0005f, 0.025f, 0.0005f);
    Tweak::floatVar("Ocean", "Spray kick", &kick, 0.0f, 15.0f, 0.1f);
    Tweak::floatVar("Ocean", "Spray speed", &speed, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Ocean", "Spray forward offset", &forward, -5.0f, 5.0f, 0.05f);
    Tweak::floatVar("Ocean", "Spray height offset", &height, -2.0f, 2.0f, 0.01f);
}