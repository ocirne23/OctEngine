export module Network:Crypto;

import Core;

// Module-internal wrappers over Windows CNG (bcrypt): system RNG, HMAC-SHA256 for the stateless
// connect challenge, ephemeral ECDH P-256 key agreement, and AES-128-GCM packet sealing.
// Nothing here is exported — the public surface is NetHostConfig::encrypt.

constexpr uint32 NetCryptoPublicKeySize = 64; // P-256 uncompressed X||Y
constexpr uint32 NetCryptoTagSize = 16;       // GCM auth tag appended per packet
constexpr uint32 NetCryptoNonceSize = 12;

// AES-GCM session key (BCRYPT_KEY_HANDLE). Movable RAII.
struct NetAeadKey
{
    NetAeadKey() = default;
    NetAeadKey(const NetAeadKey&) = delete;
    NetAeadKey& operator=(const NetAeadKey&) = delete;
    NetAeadKey(NetAeadKey&& other) noexcept : handle(other.handle) { other.handle = 0; }
    NetAeadKey& operator=(NetAeadKey&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            handle = other.handle;
            other.handle = 0;
        }
        return *this;
    }
    ~NetAeadKey() { destroy(); }

    bool isValid() const { return handle != 0; }
    void destroy();

    uint64 handle = 0;
};

// Ephemeral ECDH P-256 keypair (one per NetHost). Movable RAII.
struct NetKeyPair
{
    NetKeyPair() = default;
    NetKeyPair(const NetKeyPair&) = delete;
    NetKeyPair& operator=(const NetKeyPair&) = delete;
    NetKeyPair(NetKeyPair&& other) noexcept : handle(other.handle)
    {
        memcpy(publicKey, other.publicKey, sizeof(publicKey));
        other.handle = 0;
    }
    NetKeyPair& operator=(NetKeyPair&& other) noexcept
    {
        if (this != &other)
        {
            destroy();
            handle = other.handle;
            memcpy(publicKey, other.publicKey, sizeof(publicKey));
            other.handle = 0;
        }
        return *this;
    }
    ~NetKeyPair() { destroy(); }

    bool isValid() const { return handle != 0; }
    void destroy();

    uint64 handle = 0;
    uint8 publicKey[NetCryptoPublicKeySize] = {};
};

bool cryptoRandom(oc::span<uint8> out);

// HMAC-SHA256(secret, data) truncated to 64 bits — the stateless connect-challenge MAC
uint64 cryptoStatelessMac(oc::span<const uint8> secret, oc::span<const uint8> data);

bool cryptoGenerateKeyPair(NetKeyPair& out);

// ECDH agreement + SHA256 KDF over context -> AES-128-GCM key (same on both ends)
bool cryptoDeriveAead(const NetKeyPair& local, oc::span<const uint8> peerPublicKey, oc::span<const uint8> context, NetAeadKey& out);

// out = ciphertext || 16B tag (out.size() == plain.size() + NetCryptoTagSize)
bool cryptoSeal(const NetAeadKey& key, const uint8 nonce[12], oc::span<const uint8> aad, oc::span<const uint8> plain, oc::span<uint8> out);

// cipherAndTag = ciphertext || 16B tag; false on auth failure (forged/corrupt/replayed-wrong)
bool cryptoOpen(const NetAeadKey& key, const uint8 nonce[12], oc::span<const uint8> aad, oc::span<const uint8> cipherAndTag, oc::span<uint8> outPlain);
