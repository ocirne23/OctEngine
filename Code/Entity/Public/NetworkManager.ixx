export module Entity:NetworkManager;

import Core;
import :Entity;
import :NetworkManagerState; // the session, the wire protocol and the registry (the protocol is described there)

export enum class ENetRole : uint8
{
    None,   // single player: no socket, fireNetworkEvent degrades to a local fireEvent
    Server, // authoritative: accepts clients, streams snapshots
    Client, // corrects local entities toward server snapshots
};

// Server-authoritative entity sync over the Network library's reliable-UDP NetHost (Globals::networkManager). One manager per process, role picked at
// startup: Server opens a listening host, Client connects out, None (single player) leaves everything inert. The "Network" settings are
// Globals::settings.network (Settings.Network). The state, the wire protocol and the registry are in NetworkManagerState.ixx.
export class NetworkManager final : public NetworkManagerState
{
public:

    // ---- Lifetime and the session ----

    void initialize(); // attaches the link-simulation listeners; once at startup regardless of role
    // Must be set BEFORE startServer/startClient - the transport reads it at open() and it cannot change on a live host. Both ends must match or the
    // handshake denies (a clean failure).
    static void setEncryption(bool enabled);
    bool startServer(uint16 port);
    bool startClient(const oc::string& address, uint16 defaultPort); // "ip[:port]" or "hostname[:port]" (blocking DNS); defaultPort fills a missing port
    void shutdown(); // idempotent (a never-started host closes as a no-op); also runs from ~NetworkManager
    ~NetworkManager() { shutdown(); } // init_seg XCU6: after ~World's NetworkComponents unregister, before ~JobSystem

    ENetRole role() const { return m_role; }
    // This process's stable clientId: server-minted per connection, delivered in the Welcome. 0 on the server / before the Welcome - which is what makes
    // ownerClientId == localClientId() false everywhere except on the actual owner (plain aligned read, safe from job workers).
    uint32 localClientId() const { return m_localClientId; }
    oc::string getStatusText() const; // "SERVER 2 peers | out 12.4 KB/s" - empty when role None or the stats tweak is off

    // ---- The frame (main thread) ----

    void receive(double deltaSec); // before script/physics/world updates
    void send(double deltaSec);    // after world.update

    // ---- Ownership (server) ----

    // Hand a networked entity (tree) to a client - its claims then drive it, validated by the plausibility gate. Call right after spawning (same frame,
    // before send() announces).
    void setOwner(Entity& root, uint32 clientId);
    // Mark the server's OWN player body as a primary: it joins the proximity transfer as a source under clientId 0 and is never handed to a client.
    void setServerPrimary(Entity& entity, bool primary);
    // Contact-driven ownership STEAL: the last player to collide with an object owns it, never another client's PRIMARY. Wired as the primaries'
    // PhysicsComponent onContact hook; main thread (physics.dispatchContactEvents).
    void stealOwnershipOnContact(Entity& object, uint32 byClientId);
    // Fired on the main thread when a client completes the handshake / disconnects. joined runs AFTER the Welcome and world replay are queued, so anything
    // spawned inside arrives in the same session stream.
    void setOnClientJoined(oc::function<void(uint32 clientId)> callback) { m_onClientJoined = oc::move(callback); }
    void setOnClientLeft(oc::function<void(uint32 clientId)> callback) { m_onClientLeft = oc::move(callback); }
    // Client: fired inside receive() when the connection to the server drops, BEFORE the automatic reconnect attempt. Do not shut the host down from inside.
    void setOnServerLost(oc::function<void()> callback) { m_onServerLost = oc::move(callback); }

    // ---- Events ----

    // Fires the named script event locally AND across the network (server -> all ready clients, client -> server, which relays to the other clients). Role
    // None = local fire only. Callable from job workers: the network half queues under a mutex and send() drains it. `sender` (a script's `self`) travels
    // as a netId; the payload is copied.
    void fireNetworkEvent(oc::string_view name, oc::span<const uint8> data = {}, Entity* sender = nullptr);
    // Inside an event handler only: who fired it (0 = the server or a local fire, else the originating client - server-stamped) and its payload (a view into
    // the receive buffer, valid for the dispatch).
    uint32 currentEventSender() const { return m_currentEventSender; }
    oc::span<const uint8> currentEventData() const { return m_currentEventData; }
    // Server: authorizes events arriving FROM clients (false drops it: neither fired nor relayed). `sender` is non-null ONLY when `clientId` really owns it.
    // No filter = everything allowed, so install one before any event grants state.
    using EventFilterFn = NetworkManagerState::EventFilterFn;
    void setEventFilter(EventFilterFn callback) { m_eventFilter = oc::move(callback); }
    // C++ listener for every dispatched network event (locally fired AND received), synchronous inside the dispatch. A locally fired event dispatches on
    // the firing thread (worker script thunks included) - keep it cheap and thread-tolerant.
    void setOnGameEvent(oc::function<void(oc::string_view name)> callback) { m_onGameEvent = oc::move(callback); }
};

export namespace Globals
{
// Destructs after every entity holder (their NetworkComponents unregister through the still-open host) and before jobSystem - see InitSeg.h.
OC_INIT_SEG(OC_SEG_NETWORK_MANAGER)
    NetworkManager networkManager;
}
