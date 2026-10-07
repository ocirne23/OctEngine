module Settings;

import Core;

void Settings::registerAll()
{
    EngineSettings& s = Globals::settings;
    registerTime(s.time);

    registerSky(s.sky);
    registerWind(s.wind);
    registerShadow(s.shadow);
    registerFoliage(s.foliage);
    registerFarTree(s.farTree);
    registerRock(s.rock);
    registerGrass(s.grass);
    registerFog(s.fog);
    registerClouds(s.clouds);
    registerRT(s.rt);
    registerRTAO(s.rtao);
    registerTAA(s.taa);
    registerDlss(s.dlss);
    registerMotionBlur(s.motionBlur);
    registerBloom(s.bloom);
    registerPost(s.post);
    registerMeshLod(s.lod);
    registerLightGrid(s.lightGrid);
    registerRenderer(s.renderer);
    registerGi(s.gi);
    registerParticles(s.particles);
    registerOceanSpray(s.oceanSpray);
    registerMeshStreaming(s.meshStreaming);
    registerTextureStreaming(s.textureStreaming);

    registerWorld(s.world);
    registerPhysics(s.physics);
    registerAudio(s.audio);
    registerSpatial(s.spatial);
    registerNetwork(s.network);
    registerNav(s.nav);
    registerThreading(s.threading);

    registerHud(s.hud);
    registerParticleSystem(s.particleSystem);
    registerForce(s.force, s.forceSystem);
    registerFreeFlyCamera(s.freeFlyCamera);
    registerAppControls(s.appControls);
    registerGizmo(s.gizmo);

    registerTerrain(s.terrain);
    registerTerrainCollider(s.terrainCollider);
    registerClutter(s.clutter);
    registerTrees(s.trees);
    registerTreeWorld(s.treeWorld);
    registerRocks(s.rockSystem);
    registerOcean(s.ocean);

    registerGame(s.game);
}
