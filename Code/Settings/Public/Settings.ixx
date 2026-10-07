export module Settings;

export import Settings.Tweaks;
export import Settings.Time;
export import Settings.Render;
export import Settings.World;
export import Settings.Physics;
export import Settings.Audio;
export import Settings.Spatial;
export import Settings.Network;
export import Settings.Nav;
export import Settings.Threading;
export import Settings.Terrain;
export import Settings.Clutter;
export import Settings.Trees;
export import Settings.Rocks;
export import Settings.Ocean;
export import Settings.Force;
export import Settings.ParticleSystem;
export import Settings.Hud;
export import Settings.Input;
export import Settings.App;
export import Settings.Game;

import Core;
import Core.Time;

// THE SETTINGS: every tweak's value, in one aggregate. Each domain module (Settings.<Domain>) holds its plain-data
// types and registers them (Settings::register<Domain>); registerAll runs them all once at startup, before any system
// initializes. Systems read Globals::settings directly and attach their reactions to a change with Tweak::onChange.
export struct EngineSettings
{
    TimeSettings time;

    // Settings.Render
    WindParams wind;
    SkyParams sky;
    CloudParams clouds;
    ShadowParams shadow;
    FoliageParams foliage;
    GrassParams grass;
    RockParams rock;
    FarTreeParams farTree;
    FogParams fog;
    PostParams post;
    RTParams rt;
    LightGridParams lightGrid;
    RTAOParams rtao;
    TAAParams taa;
    DlssParams dlss;
    MotionBlurParams motionBlur;
    BloomParams bloom;
    MeshLodParams lod;
    ParticleParams particles;
    OceanSprayParams oceanSpray;
    ForceFieldParams force;
    GiSettings gi;
    TextureStreamingSettings textureStreaming;
    MeshStreamingSettings meshStreaming;
    RendererSettings renderer;

    WorldSettings world;
    PhysicsSettings physics;
    AudioSettings audio;
    SpatialSettings spatial;
    NetworkSettings network;
    NavSettings nav;
    ThreadingSettings threading;

    TerrainSettings terrain;
    TerrainColliderSettings terrainCollider;
    ClutterSettings clutter;
    TreeSettings trees;
    TreeWorldSettings treeWorld;
    RockSettings rockSystem;
    OceanSettings ocean;

    ForceSystemSettings forceSystem;
    ParticleSystemSettings particleSystem;
    HudSettings hud;
    FreeFlyCameraSettings freeFlyCamera;
    GizmoSettings gizmo;
    AppControlsSettings appControls;

    GameSettings game;
};

OC_INIT_SEG(OC_SEG_SETTINGS)
export namespace Globals
{
    EngineSettings settings;
}

export namespace Settings
{
    // Every tweak, once: main, before any system initializes (a system's Tweak::onChange needs its row) and before
    // TweakRegistry::loadSaved. The order is the panel's row order inside a shared category.
    void registerAll();
}
