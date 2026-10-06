export module Settings.Network;

import Core;

// Client-side correction thresholds, read by NetworkComponent::update ("Network/Correction").
export struct NetSyncParams
{
    float posDeadzone = 0.05f;        // below: local state free-runs
    float posSnapThreshold = 2.0f;    // entering the push's CATCH-UP band (boosted gains/caps)
    float rotDeadzoneDeg = 0.5f;
    float rotSnapThresholdDeg = 45.0f;
    float blendRate = 10.0f;          // NON-physics entities: exponential blend rate toward the target
    bool extrapolate = true;          // dead-reckon the pose target by linVel * timeSinceSnapshot
    // REMOTE-OWNED entities (other players) skip the chase and replay the owner's recorded
    // trajectory this many snapshot ticks in the past, interpolating between buffered snapshots.
    // Tweak minimum is 2 - the cursor clamps to `newest - 1`, so 1 leaves it no headroom and
    // playback advances only on snapshot arrival. Loss gaps fall through to the push for a frame.
    int remoteInterpTicks = 2;

    // PHYSICAL PUSH (dynamic bodies): error becomes corrective velocity on top of the server's, as
    // bounded impulses through the sim. Past the snap threshold gains/caps are multiplied by
    // pushCatchUpBoost so multi-meter errors still correct THROUGH the sim - teleporting into an
    // occupied space would depenetration-fling both bodies into fresh desync. The teleport resync is
    // the LAST resort, position error only (a wrong orientation can't materialize inside anything).
    // INTERACTION GRACE: a server-owned body within this radius of one of OUR claim-driven bodies
    // suspends its corrections - they'd fight the shove the player is applying with the server's
    // RTT-old pre-push state. The twin gets the same push an RTT later. 0 = off.
    float interactionRadius = 1.5f;
    float interactionLinger = 0.5f;   // seconds the grace persists after leaving the radius

    float pushPosGain = 0.5f;         // corrective velocity per meter of position error (1/s)
    float pushRotGain = 5.0f;         // corrective angular velocity per radian of rotation error (1/s)
    float pushMaxVel = 10.0f;         // cap on the corrective (error-driven) velocity term (m/s)
    float pushMaxAngVel = 10.0f;      // cap on the corrective angular term (rad/s)
    float pushMaxAccel = 10.0f;       // how hard the push may change the body's velocity (m/s^2; keep > gravity)
    float pushMaxAngAccel = 60.0f;    // (rad/s^2)
    float pushCatchUpBoost = 4.0f;    // gain/cap multiplier in the catch-up band (snap..teleport threshold)
    // MASS SCALING: a light body's velocity answers every contact impulse strongly, so in a packed
    // crowd its correction and its neighbours' pushes compound into swinging. Gains, velocity caps
    // and acceleration limits scale by clamp(mass / pushMassReference, pushMassScaleMin, 1): a body
    // at or above the reference corrects at full strength, a lighter one proportionally gentler.
    float pushMassReference = 30.0f;  // kg; bodies this heavy or heavier get the full correction
    float pushMassScaleMin = 0.15f;   // floor so a very light body still converges
    float posTeleportThreshold = 10.0f; // beyond this the non-physical teleport resync fires after all
    // ARBITRATED OWNER (player-vs-player contact): the local feel comes from the local contact with
    // the opponent's replica - the server correction only reconciles REAL divergence (you actually
    // lost ground), so it runs with this much larger deadzone and halved gains; a tight deadzone
    // would micro-correct the pipeline lag and drag against the player's input for the whole window
    float arbitrateDeadzone = 0.4f;
};

// "Network": the NetworkManager's send policy, ownership transfer, claim validation and link simulation
// (the policy itself is documented at its use in Entity/NetworkManager.cpp).
export struct NetworkSettings
{
    float snapshotHz = 20.0f; // matches the physics fixed step
    bool quantize = true;
    // Snapshots are UNRELIABLE, so an oversized one is DROPPED: this stays under the transport's
    // single-packet budget - netMaxSinglePacketMessage(1200, encrypted) - 64, static_asserted in NetworkManager.cpp.
    int snapshotMaxBytes = 1105;
    int maxEntitiesPerTick = 300;
    int keyframeEveryTicks = 20;
    float nearRadius = 40.0f;
    float midRadius = 100.0f;
    int midEveryTicks = 2;
    int farEveryTicks = 4;
    int farKeyframeEveryTicks = 200;
    int asleepKeyframeEveryTicks = 100;
    int farMaxPerTick = 100;
    int spawnRecordsPerFrame = 128;
    int spawnQueueTarget = 64;
    float sendPosEpsilon = 0.001f;
    float sendRotEpsilonDeg = 0.1f;
    float maxVel = 50.0f;    // velocity quantization range (m/s); sent per message so both ends agree
    float maxAngVel = 50.0f; // angular velocity quantization range (rad/s)
    bool showStats = true;

    NetSyncParams correction;

    bool transferEnabled = true;
    float transferRadius = 2.5f;
    float releaseRadius = 4.0f;
    float releaseDelaySec = 1.0f;
    float arbitrateSec = 1.0f;
    float contestSec = 1.5f;

    float maxUpdateHz = 20.0f;
    float maxClaimSpeed = 60.0f;
    float twinFollowGain = 10.0f;
    float twinResyncDistance = 2.0f;
    float maxClaimVelocity = 50.0f;
    float maxClaimAngVel = 50.0f;
    float claimTeleportCap = 10.0f;
    bool claimPathRaycast = true;
    int forcedTicks = 30;
    float claimReanchorRadius = 2.0f;
    int claimRedundancy = 4;
    float ownerPredictTicks = 0.5f;

    // Outgoing link simulation; the NetworkManager pushes these into its NetHost.
    float simPacketLoss = 0.0f; // 0..1 chance to drop
    float simLatencyMs = 0.0f;
    float simJitterMs = 0.0f;
};

export namespace Settings
{
    void registerNetwork(NetworkSettings& s);
}
