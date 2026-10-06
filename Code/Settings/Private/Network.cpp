module Settings.Network;

import Core;
import Settings.Tweaks;

void Settings::registerNetwork(NetworkSettings& s)
{
    Tweak::floatVar("Network", "Snapshot Hz", &s.snapshotHz, 1.0f, 60.0f, 0.5f);
    Tweak::boolean("Network", "Quantize", &s.quantize);
    Tweak::intVar("Network", "Snapshot max bytes", &s.snapshotMaxBytes, 128, 1400);
    Tweak::intVar("Network", "Max entities per tick", &s.maxEntitiesPerTick, 1, 4096);
    Tweak::intVar("Network", "Keyframe every ticks", &s.keyframeEveryTicks, 1, 255);
    Tweak::floatVar("Network/Relevance", "Near radius", &s.nearRadius, 0.0f, 1000.0f, 5.0f);
    Tweak::floatVar("Network/Relevance", "Mid radius", &s.midRadius, 0.0f, 1000.0f, 5.0f);
    Tweak::intVar("Network/Relevance", "Mid every ticks", &s.midEveryTicks, 1, 60);
    Tweak::intVar("Network/Relevance", "Far every ticks", &s.farEveryTicks, 1, 60);
    Tweak::intVar("Network/Relevance", "Far keyframe every ticks", &s.farKeyframeEveryTicks, 1, 4000);
    Tweak::intVar("Network/Relevance", "Asleep keyframe every ticks", &s.asleepKeyframeEveryTicks, 1, 4000);
    Tweak::intVar("Network/Relevance", "Far max per tick", &s.farMaxPerTick, 0, 4096);
    Tweak::intVar("Network/Spawn stream", "Records per frame", &s.spawnRecordsPerFrame, 1, 4096);
    Tweak::intVar("Network/Spawn stream", "Queue target", &s.spawnQueueTarget, 1, 512);
    Tweak::floatVar("Network", "Send pos epsilon", &s.sendPosEpsilon, 0.0f, 0.1f, 0.0005f);
    Tweak::floatVar("Network", "Send rot epsilon (deg)", &s.sendRotEpsilonDeg, 0.0f, 10.0f, 0.01f);
    Tweak::floatVar("Network", "Max vel (quantize m/s)", &s.maxVel, 1.0f, 500.0f, 1.0f);
    Tweak::floatVar("Network", "Max ang vel (quantize rad/s)", &s.maxAngVel, 1.0f, 200.0f, 1.0f);
    Tweak::boolean("Network", "Show stats", &s.showStats);

    NetSyncParams& c = s.correction;
    Tweak::floatVar("Network/Correction", "Pos deadzone", &c.posDeadzone, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Network/Correction", "Pos snap threshold", &c.posSnapThreshold, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Network/Correction", "Rot deadzone (deg)", &c.rotDeadzoneDeg, 0.0f, 30.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Rot snap threshold (deg)", &c.rotSnapThresholdDeg, 0.0f, 180.0f, 0.5f);
    Tweak::floatVar("Network/Correction", "Blend rate", &c.blendRate, 0.0f, 30.0f, 0.1f);
    Tweak::boolean("Network/Correction", "Extrapolate", &c.extrapolate);
    Tweak::intVar("Network/Correction", "Remote interp (ticks)", &c.remoteInterpTicks, 2, 8);
    Tweak::floatVar("Network/Correction", "Interaction radius", &c.interactionRadius, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Network/Correction", "Interaction linger", &c.interactionLinger, 0.0f, 3.0f, 0.05f);
    Tweak::floatVar("Network/Correction", "Push pos gain", &c.pushPosGain, 0.0f, 30.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Push rot gain", &c.pushRotGain, 0.0f, 30.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Push max vel", &c.pushMaxVel, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Network/Correction", "Push max ang vel", &c.pushMaxAngVel, 0.0f, 100.0f, 0.5f);
    Tweak::floatVar("Network/Correction", "Push accel limit", &c.pushMaxAccel, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Network/Correction", "Push ang accel limit", &c.pushMaxAngAccel, 0.0f, 500.0f, 1.0f);
    Tweak::floatVar("Network/Correction", "Push catch-up boost", &c.pushCatchUpBoost, 1.0f, 20.0f, 0.1f);
    Tweak::floatVar("Network/Correction", "Push mass reference (kg)", &c.pushMassReference, 0.1f, 1000.0f, 1.0f);
    Tweak::floatVar("Network/Correction", "Push mass scale min", &c.pushMassScaleMin, 0.01f, 1.0f, 0.01f);
    Tweak::floatVar("Network/Correction", "Arbitrate deadzone", &c.arbitrateDeadzone, 0.0f, 3.0f, 0.02f);
    Tweak::floatVar("Network/Correction", "Pos teleport threshold", &c.posTeleportThreshold, 0.0f, 100.0f, 0.5f);

    Tweak::boolean("Network/Ownership", "Transfer enabled", &s.transferEnabled);
    Tweak::floatVar("Network/Ownership", "Transfer radius", &s.transferRadius, 0.0f, 20.0f, 0.1f);
    Tweak::floatVar("Network/Ownership", "Release radius", &s.releaseRadius, 0.0f, 40.0f, 0.1f);
    Tweak::floatVar("Network/Ownership", "Release delay", &s.releaseDelaySec, 0.0f, 10.0f, 0.05f);
    Tweak::floatVar("Network/Ownership", "Arbitrate window", &s.arbitrateSec, 0.0f, 5.0f, 0.05f);
    Tweak::floatVar("Network/Ownership", "Contest window", &s.contestSec, 0.0f, 5.0f, 0.05f);

    Tweak::floatVar("Network/Player", "Max update Hz", &s.maxUpdateHz, 1.0f, 120.0f, 0.5f);
    Tweak::floatVar("Network/Validation", "Max speed", &s.maxClaimSpeed, 0.0f, 200.0f, 0.5f);
    Tweak::floatVar("Network", "Twin follow gain", &s.twinFollowGain, 0.0f, 40.0f, 0.5f);
    Tweak::floatVar("Network", "Twin resync (m)", &s.twinResyncDistance, 0.1f, 20.0f, 0.1f);
    Tweak::floatVar("Network/Validation", "Max velocity", &s.maxClaimVelocity, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Network/Validation", "Max ang velocity", &s.maxClaimAngVel, 0.0f, 500.0f, 0.5f);
    Tweak::floatVar("Network/Validation", "Teleport cap", &s.claimTeleportCap, 0.0f, 100.0f, 0.5f);
    Tweak::boolean("Network/Validation", "Path raycast", &s.claimPathRaycast);
    Tweak::intVar("Network/Validation", "Forced ticks", &s.forcedTicks, 1, 255);
    Tweak::floatVar("Network/Validation", "Re-anchor radius", &s.claimReanchorRadius, 0.1f, 10.0f, 0.1f);
    // NOT validation: redundancy is the owning CLIENT's send-side loss margin, and claim lead is a
    // SERVER emit-side timeline shift. Same miscategorisation "Claim rate Hz" had before it moved.
    Tweak::intVar("Network/Player", "Claim redundancy", &s.claimRedundancy, 1, 8);
    Tweak::floatVar("Network", "Owner predict (ticks)", &s.ownerPredictTicks, 0.0f, 2.0f, 0.05f);

    Tweak::floatVar("Network/Link sim", "Packet loss", &s.simPacketLoss, 0.0f, 1.0f, 0.005f);
    Tweak::floatVar("Network/Link sim", "Latency (ms)", &s.simLatencyMs, 0.0f, 1000.0f, 1.0f);
    Tweak::floatVar("Network/Link sim", "Jitter (ms)", &s.simJitterMs, 0.0f, 500.0f, 1.0f);
}
