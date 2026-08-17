module;

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

module Network;

import Core;
import Core.Log;
import :Crypto;

static bool ntOk(NTSTATUS status) { return status >= 0; }

static BCRYPT_ALG_HANDLE algHmacSha256()
{
    static const BCRYPT_ALG_HANDLE handle = []
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
        return alg;
    }();
    return handle;
}

static BCRYPT_ALG_HANDLE algAesGcm()
{
    static const BCRYPT_ALG_HANDLE handle = []
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        if (!ntOk(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)))
            return (BCRYPT_ALG_HANDLE)nullptr;
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);
        return alg;
    }();
    return handle;
}

static BCRYPT_ALG_HANDLE algEcdhP256()
{
    static const BCRYPT_ALG_HANDLE handle = []
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDH_P256_ALGORITHM, nullptr, 0);
        return alg;
    }();
    return handle;
}

void NetAeadKey::destroy()
{
    if (handle == 0)
        return;
    BCryptDestroyKey((BCRYPT_KEY_HANDLE)handle);
    handle = 0;
}

void NetKeyPair::destroy()
{
    if (handle == 0)
        return;
    BCryptDestroyKey((BCRYPT_KEY_HANDLE)handle);
    handle = 0;
}

bool cryptoRandom(oc::span<uint8> out)
{
    return ntOk(BCryptGenRandom(nullptr, out.data(), (ULONG)out.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

uint64 cryptoStatelessMac(oc::span<const uint8> secret, oc::span<const uint8> data)
{
    uint8 digest[32] = {};
    BCryptHash(algHmacSha256(), (PUCHAR)secret.data(), (ULONG)secret.size(), (PUCHAR)data.data(), (ULONG)data.size(), digest, sizeof(digest));
    uint64 mac;
    memcpy(&mac, digest, sizeof(mac));
    return mac;
}

bool cryptoGenerateKeyPair(NetKeyPair& out)
{
    out.destroy();
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!ntOk(BCryptGenerateKeyPair(algEcdhP256(), &key, 256, 0)) || !ntOk(BCryptFinalizeKeyPair(key, 0)))
    {
        Log::error("Network: ECDH keypair generation failed");
        return false;
    }
    uint8 blob[sizeof(BCRYPT_ECCKEY_BLOB) + NetCryptoPublicKeySize];
    ULONG written = 0;
    if (!ntOk(BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof(blob), &written, 0)) || written != sizeof(blob))
    {
        BCryptDestroyKey(key);
        return false;
    }
    memcpy(out.publicKey, blob + sizeof(BCRYPT_ECCKEY_BLOB), NetCryptoPublicKeySize);
    out.handle = (uint64)key;
    return true;
}

bool cryptoDeriveAead(const NetKeyPair& local, oc::span<const uint8> peerPublicKey, oc::span<const uint8> context, NetAeadKey& out)
{
    out.destroy();
    if (!local.isValid() || peerPublicKey.size() != NetCryptoPublicKeySize)
        return false;

    uint8 blob[sizeof(BCRYPT_ECCKEY_BLOB) + NetCryptoPublicKeySize];
    BCRYPT_ECCKEY_BLOB header{ BCRYPT_ECDH_PUBLIC_P256_MAGIC, 32 };
    memcpy(blob, &header, sizeof(header));
    memcpy(blob + sizeof(header), peerPublicKey.data(), NetCryptoPublicKeySize);

    BCRYPT_KEY_HANDLE peerKey = nullptr; // import validates the point is on the curve
    if (!ntOk(BCryptImportKeyPair(algEcdhP256(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey, blob, sizeof(blob), 0)))
        return false;

    BCRYPT_SECRET_HANDLE secret = nullptr;
    const bool agreed = ntOk(BCryptSecretAgreement((BCRYPT_KEY_HANDLE)local.handle, peerKey, &secret, 0));
    BCryptDestroyKey(peerKey);
    if (!agreed)
        return false;

    BCryptBuffer kdfParams[2] = {
        { sizeof(BCRYPT_SHA256_ALGORITHM), KDF_HASH_ALGORITHM, (PVOID)BCRYPT_SHA256_ALGORITHM },
        { (ULONG)context.size(), KDF_SECRET_APPEND, (PVOID)context.data() },
    };
    BCryptBufferDesc kdfDesc{ BCRYPTBUFFER_VERSION, 2, kdfParams };
    uint8 derived[32] = {};
    ULONG written = 0;
    const bool derivedOk = ntOk(BCryptDeriveKey(secret, BCRYPT_KDF_HASH, &kdfDesc, derived, sizeof(derived), &written, 0));
    BCryptDestroySecret(secret);
    if (!derivedOk)
        return false;

    BCRYPT_KEY_HANDLE aesKey = nullptr;
    const bool keyOk = ntOk(BCryptGenerateSymmetricKey(algAesGcm(), &aesKey, nullptr, 0, derived, 16, 0));
    SecureZeroMemory(derived, sizeof(derived));
    if (!keyOk)
        return false;
    out.handle = (uint64)aesKey;
    return true;
}

bool cryptoSeal(const NetAeadKey& key, const uint8 nonce[12], oc::span<const uint8> aad, oc::span<const uint8> plain, oc::span<uint8> out)
{
    if (!key.isValid() || out.size() != plain.size() + NetCryptoTagSize)
        return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce;
    info.cbNonce = NetCryptoNonceSize;
    info.pbAuthData = (PUCHAR)aad.data();
    info.cbAuthData = (ULONG)aad.size();
    info.pbTag = out.data() + plain.size();
    info.cbTag = NetCryptoTagSize;
    ULONG written = 0;
    return ntOk(BCryptEncrypt((BCRYPT_KEY_HANDLE)key.handle, (PUCHAR)plain.data(), (ULONG)plain.size(), &info,
        nullptr, 0, out.data(), (ULONG)plain.size(), &written, 0)) && written == plain.size();
}

bool cryptoOpen(const NetAeadKey& key, const uint8 nonce[12], oc::span<const uint8> aad, oc::span<const uint8> cipherAndTag, oc::span<uint8> outPlain)
{
    if (!key.isValid() || cipherAndTag.size() < NetCryptoTagSize || outPlain.size() != cipherAndTag.size() - NetCryptoTagSize)
        return false;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = (PUCHAR)nonce;
    info.cbNonce = NetCryptoNonceSize;
    info.pbAuthData = (PUCHAR)aad.data();
    info.cbAuthData = (ULONG)aad.size();
    info.pbTag = (PUCHAR)cipherAndTag.data() + outPlain.size();
    info.cbTag = NetCryptoTagSize;
    ULONG written = 0;
    return ntOk(BCryptDecrypt((BCRYPT_KEY_HANDLE)key.handle, (PUCHAR)cipherAndTag.data(), (ULONG)outPlain.size(), &info,
        nullptr, 0, outPlain.data(), (ULONG)outPlain.size(), &written, 0)) && written == outPlain.size();
}
