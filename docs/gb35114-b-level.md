# GB 35114 B level: VKEK distribution and SM4 video encryption

`src/gb35114/` implements the B-level video encryption part of
`docs/compliance-gb28181-gb35114.md` (issue #25):

- the platform wraps a random **VKEK** (video key encryption key) with the
  device SM2 certificate or public key;
- the device unwraps it with its SM2 private key and both sides derive the
  per-stream working keys with **HKDF-SM3**;
- each H.264 access unit is sealed with **SM4-CTR** and an **HMAC-SM3** tag
  before PS muxing, so the standard PS/RTP layers carry it unchanged;
- a platform-side CLI decrypts the stream and ffmpeg decodes the picture.

The module is disabled by default and is not linked into the server, so
preview, recording, playback, ONVIF, RTSP and the private protocol are
untouched. All cryptography comes from the existing `src/crypto/` wrappers over
OpenSSL 3 (Apache-2.0); no GPL/LGPL dependency is added.

```sh
cmake -S . -B build -DUVNVR_ENABLE_GB35114=ON -DUVNVR_BUILD_TESTS=ON
cmake --build build -j
./build/tests/gb35114_test
python3 tests/integration/gb35114_sm4.py ./build/gb35114_cli
```

## Where it fits

```
camera / channel  ──►  H.264 ES
                         │
                         ├─ SplitH264AccessUnits()          (src/gb35114/es_crypto.cpp)
                         ├─ SealEsStream(device keys)       SM4-CTR + HMAC-SM3
                         ▼
                    sealed ES ──► GB/T 28181 PS muxer ──► RTP ──► platform
                                                                   │
                                              OpenEsStream(platform keys)
                                                                   ▼
                                                              H.264 ES ──► decoder
```

`src/gb28181/` (SIP, PS, RTP) is a separate work item. The B-level layer is
deliberately transport-agnostic: `SealEsStream()` runs on the ES **before** PS
muxing, and every sealed frame carries its own length, so PES/RTP boundaries do
not have to be preserved for the receiver to decrypt. `gb35114_sm4.py` exercises
that composition with a small MPEG-PS/RTP reference transport; the production
PS/RTP stack from the GB/T 28181 work can replace it without touching the
ciphertext format.

## Key distribution (VKEK)

The platform picks a 16-byte VKEK and a 16-byte salt, then sends this envelope
over the 35114 signalling channel (the SIP stack is part of the 28181 work
item):

| offset | size | field |
| --- | --- | --- |
| 0 | 4 | magic `GVKE` |
| 4 | 1 | version `1` |
| 5 | 1 | algorithm `1` = SM2 `C1||C3||C2` |
| 6 | 2 | header size, big endian (`40`) |
| 8 | 4 | key id, big endian |
| 12 | 16 | HKDF salt (not secret) |
| 28 | 12 | reserved, zero |
| 40 | 4 | wrapped VKEK length, big endian |
| 44 | n | wrapped VKEK |

The wrapped VKEK is SM2 public-key encryption in the GB/T 32918.4 wire form
`C1 || C3 || C2` (C1 = `04 || X || Y`). OpenSSL emits/consumes an ASN.1
SEQUENCE; `gm::Sm2Encrypt`/`gm::Sm2Decrypt` convert at that boundary so callers
only see the standard form.

Both sides derive three per-stream keys (HKDF from RFC 5869, SM3 as the hash):

```
PRK       = HMAC-SM3(salt, VKEK)
info      = label || 0x00 || key_id_be32 || 0x00 || stream_id
video-key = HKDF-Expand(PRK, "GB35114/v1/video-key" || info, 16)
iv-seed   = HKDF-Expand(PRK, "GB35114/v1/iv-seed"  || info, 16)
mac-key   = HKDF-Expand(PRK, "GB35114/v1/mac-key"  || info, 16)
```

Binding the key id and the GB/T 28181 channel id into every derivation keeps
one VKEK exchange from producing the same key on two streams. Re-keying is a
new envelope with a new key id and salt; the old and new keys can overlap
while the platform switches.

## Sealed ES frame

Each access unit becomes one self-delimiting frame:

| offset | size | field |
| --- | --- | --- |
| 0 | 4 | magic `GBS1` |
| 4 | 1 | version `1` |
| 5 | 1 | flags (bit 0 = keyframe) |
| 6 | 2 | header size (`56`, tag included) |
| 8 | 4 | key id |
| 12 | 8 | sequence number |
| 20 | 8 | plaintext length |
| 28 | 8 | random nonce |
| 36 | 4 | reserved, zero |
| 40 | 16 | HMAC-SM3 tag over bytes `[0,40)` and the ciphertext |
| 56 | n | SM4-CTR ciphertext (same length as the plaintext) |

- `IV = SM3("GB35114/iv/v1" || iv-seed || nonce || sequence)[0..16)`.
- SM4-CTR is length preserving; there is no padding and the frame overhead is a
  constant 56 bytes.
- The tag is checked before decryption. A wrong VKEK, a wrong stream id, a
  modified header or a modified ciphertext all fail with
  `sealed frame authentication failed` and produce no output.
- The splitter preserves every byte of the Annex B stream (start codes,
  SPS/PPS, SEI, leading junk) and keeps all slices of one picture in one access
  unit. `SealEsStream`/`OpenEsStream` are byte-exact round trips.

## Key derivation and frame examples

```cpp
#include "gb35114/stream_keys.h"
#include "gb35114/es_crypto.h"

// Platform, once per stream:
gm::Bytes envelope, vkek, salt;
gb35114::CreateStreamKeyEnvelopeForCertificate(device_cert_pem, key_id,
                                          &envelope, &vkek, &salt, &error);
gb35114::VideoKeys platform_keys;
gb35114::DeriveStreamKeys(vkek, key_id, salt, stream_id, &platform_keys, &error);

// Device, on receiving the envelope:
gm::Bytes device_vkek;
gb35114::StreamKeyEnvelope parsed;
gb35114::UnwrapStreamVkek(device_private_key_pem, envelope, &device_vkek, &parsed, &error);
gb35114::VideoKeys device_keys;
gb35114::DeriveStreamKeys(device_vkek, parsed, stream_id, &device_keys, &error);

// Between ES extraction and PS muxing:
gm::Bytes sealed;
gb35114::SealEsStream(device_keys, access_units, &sealed, &frames, &error);

// Platform side:
gb35114::OpenEsStream(platform_keys, sealed, &es, &frames, &error);
```

## CLI (platform reference implementation)

```
gb35114_cli wrap <device-public-key-or-cert.pem> <stream-id> <key-id> \
                 <envelope-out> <platform-key-out>
gb35114_cli encrypt <device-private-key.pem> <envelope-in> <stream-id> \
                    <in.h264> <out.gbes>
gb35114_cli decrypt <platform-key-in> <stream-id> <in.gbes> <out.h264>
gb35114_cli info <in.gbes>
```

`platform-key-out` is a 44-byte `GPK1` file holding the key id, salt and VKEK.
It is written with mode 0600 and is as sensitive as the video itself. The CLI
never prints the VKEK; only the salt and sizes appear on stdout.

Example:

```sh
gb35114_cli wrap device.crt.pem 34020000001320000001 7 env.bin platform.key
gb35114_cli encrypt device.key.pem env.bin 34020000001320000001 sample.h264 sealed.gbes
gb35114_cli info sealed.gbes
gb35114_cli decrypt platform.key 34020000001320000001 sealed.gbes out.h264
ffmpeg -i out.h264 -frames:v 1 frame.png
```

## Tests and acceptance evidence

`tests/gb35114_test.cpp` covers:

- SM2 `C1||C3||C2` encrypt/decrypt, certificate wrapping, wrong key, tampered
  and truncated ciphertext;
- HMAC-SM3 known answer (cross-checked with
  `openssl dgst -sm3 -mac HMAC`);
- envelope parse/unwrap, key-id and stream-id binding, wrong device key;
- access-unit splitting (AUD, SPS/PPS, multi-slice pictures, byte preservation);
- frame seal/open, keyframe/sequence/size metadata, wrong key material, wrong
  MAC key, modified header/ciphertext/tag, truncation and trailing bytes;
- whole-stream seal/open and empty input.

`tests/integration/gb35114_sm4.py` is the end-to-end acceptance check:

1. generates an SM2 device key + certificate and a 20-frame H.264 testsrc2 clip;
2. wraps a VKEK and encrypts the clip;
3. carries the sealed frames through MPEG-PS and RTP and asserts the bytes are
   identical, while ffprobe validates the same PS syntax with plaintext H.264;
4. decrypts with the platform key, asserts SHA-256 equality with the source,
   decodes a PNG and checks `signalstats` (non-blank picture);
5. asserts a wrong VKEK, wrong stream id and modified ciphertext are rejected,
   and that ffmpeg cannot decode the encrypted file directly.

Expected tail of the run:

```
      ps=... bytes rtp=... packets rtp_bytes=...; PS/RTP round trip identical
      reference PS accepted by ffprobe: codec_name=h264 width=320 height=240 nb_read_frames=20
      decrypt ... frames=20 es_bytes=49495
      sha256 match: ...
      decoded ... frame.png ..., YAVG=... YMAX=...
      wrong VKEK rejected: ... sealed frame authentication failed ...
      wrong stream id rejected: ...
      modified ciphertext rejected: ...
      direct ffmpeg decode rejected: ... Invalid data found ...
gb35114_sm4: PASS
```

## Security notes and limits

- **Profile, not yet a conformance claim.** The frame layout, HKDF labels and
  envelope are a UvNVR profile. Bit-exact interoperability with a certified
  35114 platform still needs the formal wire-format text and the joint test in
  #27; the SM2 `C1||C3||C2` form, SM4 and SM3 primitives themselves are
  standard.
- **VKEK confidentiality only.** SM2 wrapping protects the VKEK in transit.
  The envelope is not signed; platform authenticity belongs to the A-level
  certificate/signature exchange and the SIP signalling protection, which are
  separate work items. Do not run B level over an unauthenticated signalling
  channel.
- **No frame replay protection.** The tag binds key id, sequence, nonce and
  ciphertext, but a whole frame can be replayed. RTP sequence/SSRC handling and
  the SIP session provide ordering; add an explicit anti-replay window when the
  standard's B-level profile requires it.
- **Nonce uniqueness.** `SealEsStream` draws an 8-byte CSPRNG nonce per frame,
  so IV reuse under one key is negligible. A caller using `SealEsFrame`
  directly must supply a nonce that never repeats for the same key/sequence.
- **Key material in memory.** The VKEK and derived keys are ordinary
  `std::vector` buffers; zeroisation and hardware key storage (TPM/密码模块)
  are not implemented in this phase.
- **Transport buffering.** The reference PS muxer in the integration test caps
  a PES at 65535 bytes; production PS/RTP must handle larger frames (PES length
  0 or fragmentation) as part of the 28181 work item.
