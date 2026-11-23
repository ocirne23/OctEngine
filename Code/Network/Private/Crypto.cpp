module;

#ifdef _MSC_VER
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#else
#include <cstring>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#endif

module Network;

import Core;
import Core.Log;
import :Crypto;

#ifndef _MSC_VER
// Linux: the same primitives through OpenSSL 3. NetKeyPair::handle owns an EVP_PKEY*, NetAeadKey::handle a heap copy of
// the 16-byte AES key. The KDF mirrors BCryptDeriveKey(BCRYPT_KDF_HASH, SHA-256, KDF_SECRET_APPEND = context):
// SHA-256(Z || context), the first 16 bytes the AES-128-GCM key.
struct AeadKeyBytes
{
    uint8 bytes[16];
};

void NetAeadKey::destroy()
{
    if (handle == 0)
        return;
    AeadKeyBytes* key = reinterpret_cast<AeadKeyBytes*>(handle);
    OPENSSL_cleanse(key->bytes, sizeof(key->bytes));
    delete key;
    handle = 0;
}

void NetKeyPair::destroy()
{
    if (handle == 0)
        return;
    EVP_PKEY_free(reinterpret_cast<EVP_PKEY*>(handle));
    handle = 0;
}

bool cryptoRandom(oc::span<uint8> out)
{
    return RAND_bytes(out.data(), int(out.size())) == 1;
}

uint64 cryptoStatelessMac(oc::span<const uint8> secret, oc::span<const uint8> data)
{
    uint8 digest[32] = {};
    unsigned int length = 0;
    HMAC(EVP_sha256(), secret.data(), int(secret.size()), data.data(), data.size(), digest, &length);
    uint64 mac;
    memcpy(&mac, digest, sizeof(mac));
    return mac;
}

bool cryptoGenerateKeyPair(NetKeyPair& out)
{
    out.destroy();
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    if (!key)
    {
        Log::error("Network: ECDH keypair generation failed");
        return false;
    }
    uint8 point[1 + NetCryptoPublicKeySize];
    size_t written = 0;
    if (!EVP_PKEY_get_octet_string_param(key, OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, point, sizeof(point), &written)
        || written != sizeof(point) || point[0] != 0x04)
    {
        EVP_PKEY_free(key);
        return false;
    }
    memcpy(out.publicKey, point + 1, NetCryptoPublicKeySize); // X || Y, as the CNG blob carries them
    out.handle = (uint64)key;
    return true;
}

bool cryptoDeriveAead(const NetKeyPair& local, oc::span<const uint8> peerPublicKey, oc::span<const uint8> context, NetAeadKey& out)
{
    out.destroy();
    if (!local.isValid() || peerPublicKey.size() != NetCryptoPublicKeySize)
        return false;

    uint8 point[1 + NetCryptoPublicKeySize];
    point[0] = 0x04;
    memcpy(point + 1, peerPublicKey.data(), NetCryptoPublicKeySize);
    char groupName[] = "prime256v1";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, groupName, 0),
        OSSL_PARAM_construct_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point)),
        OSSL_PARAM_construct_end(),
    };
    EVP_PKEY* peerKey = nullptr;
    EVP_PKEY_CTX* importCtx = EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr);
    const bool imported = importCtx && EVP_PKEY_fromdata_init(importCtx) == 1
        && EVP_PKEY_fromdata(importCtx, &peerKey, EVP_PKEY_PUBLIC_KEY, params) == 1;
    EVP_PKEY_CTX_free(importCtx);
    if (!imported)
        return false;

    uint8 secret[32] = {};
    size_t secretSize = sizeof(secret);
    EVP_PKEY_CTX* deriveCtx = EVP_PKEY_CTX_new(reinterpret_cast<EVP_PKEY*>(local.handle), nullptr);
    // EVP_PKEY_derive_set_peer validates the peer point (on the curve), as BCryptImportKeyPair does.
    const bool agreed = deriveCtx && EVP_PKEY_derive_init(deriveCtx) == 1 && EVP_PKEY_derive_set_peer(deriveCtx, peerKey) == 1
        && EVP_PKEY_derive(deriveCtx, secret, &secretSize) == 1 && secretSize == sizeof(secret);
    EVP_PKEY_CTX_free(deriveCtx);
    EVP_PKEY_free(peerKey);
    if (!agreed)
        return false;

    uint8 derived[32] = {};
    unsigned int derivedSize = 0;
    EVP_MD_CTX* md = EVP_MD_CTX_new();
    const bool derivedOk = md && EVP_DigestInit_ex(md, EVP_sha256(), nullptr) == 1
        && EVP_DigestUpdate(md, secret, sizeof(secret)) == 1
        && EVP_DigestUpdate(md, context.data(), context.size()) == 1
        && EVP_DigestFinal_ex(md, derived, &derivedSize) == 1;
    EVP_MD_CTX_free(md);
    OPENSSL_cleanse(secret, sizeof(secret));
    if (!derivedOk)
        return false;

    AeadKeyBytes* key = new AeadKeyBytes;
    memcpy(key->bytes, derived, sizeof(key->bytes));
    OPENSSL_cleanse(derived, sizeof(derived));
    out.handle = (uint64)key;
    return true;
}

static bool aesGcm(bool encrypt, const NetAeadKey& key, const uint8 nonce[12], oc::span<const uint8> aad,
                   const uint8* in, size_t size, uint8* out, uint8* tag)
{
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    const uint8* keyBytes = reinterpret_cast<const AeadKeyBytes*>(key.handle)->bytes;
    int length = 0;
    bool ok = EVP_CipherInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr, encrypt ? 1 : 0) == 1
        && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, NetCryptoNonceSize, nullptr) == 1
        && EVP_CipherInit_ex(ctx, nullptr, nullptr, keyBytes, nonce, encrypt ? 1 : 0) == 1
        && (aad.empty() || EVP_CipherUpdate(ctx, nullptr, &length, aad.data(), int(aad.size())) == 1)
        && (size == 0 || EVP_CipherUpdate(ctx, out, &length, in, int(size)) == 1);
    if (ok && !encrypt)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, NetCryptoTagSize, tag) == 1;
    ok = ok && EVP_CipherFinal_ex(ctx, out + size, &length) == 1; // decrypt: fails on a bad tag
    if (ok && encrypt)
        ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, NetCryptoTagSize, tag) == 1;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}

bool cryptoSeal(const NetAeadKey& key, const uint8 nonce[12], oc::span<const uint8> aad, oc::span<const uint8> plain, oc::span<uint8> out)
{
    if (!key.isValid() || out.size() != plain.size() + NetCryptoTagSize)
        return false;
    return aesGcm(true, key, nonce, aad, plain.data(), plain.size(), out.data(), out.data() + plain.size());
}

bool cryptoOpen(const NetAeadKey& key, const uint8 nonce[12], oc::span<const uint8> aad, oc::span<const uint8> cipherAndTag, oc::span<uint8> outPlain)
{
    if (!key.isValid() || cipherAndTag.size() < NetCryptoTagSize || outPlain.size() != cipherAndTag.size() - NetCryptoTagSize)
        return false;
    uint8 tag[NetCryptoTagSize];
    memcpy(tag, cipherAndTag.data() + outPlain.size(), NetCryptoTagSize);
    return aesGcm(false, key, nonce, aad, cipherAndTag.data(), outPlain.size(), outPlain.data(), tag);
}
#else

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
#endif
