#pragma once

// Chinese commercial cryptography primitives backed by OpenSSL 3:
// SM3 (GB/T 32905), SM2 signatures and SM4-CBC (GB/T 32907).
//
// The module is intentionally free of project state: every function takes
// explicit keys and buffers so it can be reused by the GB/T 28181 signalling
// and GB 35114 media paths without coupling to the server.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::gm {

using Bytes = std::vector<unsigned char>;

inline constexpr std::size_t kSm3DigestSize = 32;
inline constexpr std::size_t kSm4KeySize = 16;
inline constexpr std::size_t kSm4IvSize = 16;
inline constexpr std::size_t kSm4BlockSize = 16;

// The GB/T 32918 default SM2 user ID. Passing no explicit ID uses this value,
// which is also what `openssl dgst -sm3 -sign` uses.
inline constexpr std::string_view kDefaultSm2UserId = "1234567812345678";

// SM3 digest. Returns an empty vector when the digest operation fails.
Bytes Sm3(std::string_view data);
Bytes Sm3(const void* data, std::size_t size);

// Lower-case hexadecimal SM3 digest.
std::string Sm3Hex(std::string_view data);

std::string ToHex(const Bytes& bytes);
bool FromHex(std::string_view hex, Bytes* bytes);

// SM4-CBC with PKCS#7 padding. `key` and `iv` must both be 16 bytes.
bool Sm4CbcEncrypt(const Bytes& key, const Bytes& iv, const Bytes& plaintext, Bytes* ciphertext,
                   std::string* error = nullptr);
bool Sm4CbcDecrypt(const Bytes& key, const Bytes& iv, const Bytes& ciphertext, Bytes* plaintext,
                   std::string* error = nullptr);

// SM4-CTR (GB/T 32907 counter mode). Encryption and decryption are the same
// operation, the output has exactly the same length as the input, and `offset`
// selects the keystream position in bytes. The offset form is what lets the
// recording path decrypt an arbitrary block of a large file without keeping a
// cipher context alive between reads. The 16-byte counter block is treated as
// a big-endian integer and advanced by offset / 16 blocks.
bool Sm4CtrCrypt(const Bytes& key, const Bytes& iv, std::uint64_t offset, const Bytes& input,
                 Bytes* output, std::string* error = nullptr);

// SM2 key pair encoded as PEM: the private key as PKCS#8 ("PRIVATE KEY") and the
// public key as SubjectPublicKeyInfo ("PUBLIC KEY").
struct Sm2KeyPair {
  std::string private_key_pem;
  std::string public_key_pem;
};

bool GenerateSm2KeyPair(Sm2KeyPair* key_pair, std::string* error = nullptr);

// SM2 signature over `message` with SM3. The signature is DER-encoded
// (ECDSA-Sig-Value) and interoperates with `openssl dgst -sm3`.
bool Sm2Sign(std::string_view private_key_pem, std::string_view message, Bytes* signature,
             std::string* error = nullptr);

// Same, with an explicit SM2 user ID (ZA). An empty ID is valid and is used as
// given; use the overload above for the GB/T 32918 default ID.
bool Sm2Sign(std::string_view private_key_pem, std::string_view message, std::string_view user_id,
             Bytes* signature, std::string* error = nullptr);

bool Sm2Verify(std::string_view public_key_pem, std::string_view message, const Bytes& signature,
               std::string* error = nullptr);
bool Sm2Verify(std::string_view public_key_pem, std::string_view message, std::string_view user_id,
               const Bytes& signature, std::string* error = nullptr);

// SM2 public-key encryption (GB/T 32918.4) with the SM3 KDF. `public_key_pem`
// is a SubjectPublicKeyInfo ("PUBLIC KEY"); `private_key_pem` is PKCS#8. The
// ciphertext is the ASN.1 DER structure produced by OpenSSL 3 (C1 coordinates,
// C3 digest, C2 masked plaintext) and interoperates with
// `openssl pkeyutl -encrypt/-decrypt`. This is the primitive used for VKEK
// distribution in GB 35114.
bool Sm2Encrypt(std::string_view public_key_pem, const Bytes& plaintext, Bytes* ciphertext,
                std::string* error = nullptr);
bool Sm2Decrypt(std::string_view private_key_pem, const Bytes& ciphertext, Bytes* plaintext,
                std::string* error = nullptr);

// Verifies with the public key of an X.509 certificate, in PEM or DER form.
bool Sm2VerifyCertificate(std::string_view certificate_pem, std::string_view message,
                          const Bytes& signature, std::string* error = nullptr);
bool Sm2VerifyCertificateDer(const Bytes& certificate_der, std::string_view message,
                             const Bytes& signature, std::string* error = nullptr);

} // namespace uvnvr::gm
