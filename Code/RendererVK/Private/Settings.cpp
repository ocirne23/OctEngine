module RendererVK;

import Core;
import Core.glm;
import Core.Tweaks;

namespace
{
    constexpr oc::string_view s_tonemapperNames[] = { "Off", "Reinhard", "ACES", "AgX" };
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
    Tweak::floatVar("Sky/Clouds", "Coverage", &cloudCoverage, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds", "Height", &cloudHeight, 0.0f, 8000.0f);
    Tweak::floatVar("Sky/Clouds", "Thickness", &cloudThickness, 20.0f, 250.0f);
    Tweak::floatVar("Sky/Clouds", "Scale", &cloudScale, 0.1f, 5.0f);
    Tweak::floatVar("Sky/Clouds", "Wind Speed", &cloudWindSpeed, 0.0f, 10.0f);
    Tweak::floatVar("Sky/Clouds", "Wind Angle", &cloudWindAngle, 0.0f, 6.2832f);
    Tweak::floatVar("Sky/Clouds", "Softness", &cloudSoftness, 0.05f, 2.0f);
    Tweak::floatVar("Sky/Clouds", "Density", &cloudDensity, 0.2f, 25.0f);
    Tweak::floatVar("Sky/Clouds", "Sharpness", &cloudSharpness, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds", "Height Variation", &cloudBaseVar, 0.0f, 1.0f);
    Tweak::floatVar("Sky/Clouds", "Sun Shading", &cloudShading, 0.0f, 6.0f);
    Tweak::float3("Sky", "Up Axis", &up, 0.01f, [&]() { up = glm::normalize(up); });
}

void ShadowParams::registerTweaks()
{
    Tweak::floatVar("Shadows", "Max distance (m)", &maxDistance, 25.0f, 5000.0f, 5.0f);
    Tweak::floatVar("Shadows", "Split lambda", &splitLambda, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Shadows", "Caster pad (m)", &casterPad, 0.0f, 5000.0f, 10.0f);
    Tweak::floatVar("Shadows", "Depth bias", &depthBias, 0.0f, 0.005f, 0.0001f);
    Tweak::floatVar("Shadows", "Normal bias", &normalBias, 0.0f, 10.0f);
    Tweak::floatVar("Shadows", "Terrain march start (m)", &terrainMarchStart, 0.0f, 20000.0f, 50.0f);
    Tweak::floatVar("Shadows", "Terrain march bias (m)", &terrainMarchBias, 0.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Shadows", "Terrain march spread", &terrainMarchSpread, 0.002f, 0.1f, 0.001f);
}

void FogParams::registerTweaks()
{
    Tweak::boolean("Fog", "Enabled", &enabled);
    Tweak::floatVar("Fog", "Global Density", &density, 0.0f, 1.0f, 0.001f);
    Tweak::floatVar("Fog", "Height Base", &heightBase, -200.0f, 500.0f);
    Tweak::floatVar("Fog", "Height Falloff", &heightFalloff, 0.0f, 1.0f, 0.002f);
    Tweak::floatVar("Fog", "Terrain Follow", &terrainFollow, 0.0f, 1.0f, 0.01f);
    Tweak::color3("Fog", "Albedo", &albedo, &albedoIntensity);
    Tweak::floatVar("Fog", "Anisotropy", &anisotropy, -0.9f, 0.95f, 0.01f);
    Tweak::floatVar("Fog", "Range", &range, 32.0f, 4096.0f, 32.0f);
    Tweak::boolean("Fog/Far Field", "Enabled", &farField);
    Tweak::floatVar("Fog/Far Field", "Density Scale", &farFieldDensity, 0.0f, 4.0f, 0.05f);
    Tweak::floatVar("Fog/Far Field", "Thickness Scale", &farFieldThickness, 0.1f, 20.0f, 0.1f);
    Tweak::intVar("Fog/Far Field", "Ground Steps", &farFieldSteps, 1, 32);
    Tweak::floatVar("Fog", "Slice Power", &slicePower, 0.4f, 1.5f, 0.01f);
    Tweak::floatVar("Fog", "Noise Scale", &noiseScale, 0.005f, 1.0f, 0.005f);
    Tweak::floatVar("Fog", "Noise Strength", &noiseStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Fog", "Wind Speed", &windSpeed, 0.0f, 20.0f);
    Tweak::floatVar("Fog", "Temporal Blend", &temporalBlend, 0.0f, 0.97f, 0.01f);
    Tweak::floatVar("Fog", "Region strength", &regionStrength, 0.0f, 1.0f, 0.01f);
    Tweak::floatVar("Fog", "Underwater density", &underwaterDensity, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Fog", "Underwater offset (m)", &underwaterOffset, -5.0f, 5.0f, 0.1f);
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

void RTParams::registerTweaks()
{
    Tweak::boolean("RT", "Enable RT", &enabled);
    Tweak::boolean("RT/GI", "Enable GI", &giEnabled);
    Tweak::boolean("RT", "RT Lights", &rtLightShadows);
    Tweak::boolean("RT", "RT Sun", &rtSunShadow);
    Tweak::intVar("RT", "RT Sun Rays", &sunShadowRays, 1, 8);
    Tweak::boolean("RT", "RT Sky Radiance", &rtSkyRadiance);
    Tweak::intVar("RT", "BLAS LOD level", &blasLodLevel, 0, 4);
    Tweak::boolean("RT", "BLAS compaction", &blasCompaction);
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