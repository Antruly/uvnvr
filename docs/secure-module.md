# Secure module: audit chain, firmware signatures, data-at-rest encryption

`src/secure/` implements the P6.1 security capabilities from
`docs/compliance-gb28181-gb35114.md`:

- an SM3 hash chain over the audit log, with a verifier and a CLI command;
- SM2/SM3 signature verification for firmware packages, gating the upgrade
  endpoint;
- optional SM4 encryption at rest for snapshots and recorded TS segments.

It reuses the P6.1a primitives in `src/crypto/` and is disabled at build time
by default:

```sh
cmake -S . -B build -DUVNVR_ENABLE_SECURE=ON
```

When the option is off, the server and its storage format are unchanged. When
it is on, GM crypto must also be available (`UVNVR_ENABLE_GM_CRYPTO`); the
configuration step forces the secure module off if OpenSSL 3 Crypto is
missing.

## Settings

| Key | Default | Meaning |
| --- | --- | --- |
| `audit.chain` | `1` | Append `prev_hash`/`entry_hash` to new audit rows. |
| `secure.device_cert` | `<data-dir>/device_cert.pem` | PEM certificate used to derive the at-rest key. Provisioned by the operator; the product never self-signs a device identity. |
| `secure.firmware.public_key` | `<data-dir>/firmware_pub.pem` | Signer public key (SPKI PEM) or certificate used by the upgrade entry. |
| `ai.snapshot.encrypt` | `0` | Seal AI/alert snapshots with SM4. |
| `storage.encrypt` | `0` | Encrypt finalized recording segments with SM4-CTR. |

`ai.snapshot.encrypt`, `storage.encrypt` and `audit.chain` are exposed by
`GET/PUT /api/v1/settings` under `secure` and `audit`. The AI configuration
endpoint also accepts `snapshotEncrypt`.

If encryption is enabled but the device certificate is missing, snapshot and
recording writes fail rather than silently falling back to plaintext. A
certificate change makes existing encrypted files unreadable, so back it up
with the data.

Limits worth stating explicitly for a security review:

- The SM3 audit chain is unkeyed. It detects edits, re-links and truncation
  against the stored anchor/head, but a party that can rewrite both the audit
  rows and `system_meta` can recompute the whole chain. Export or sign the head
  with a key the device does not hold when that threat is in scope.
- The at-rest key is derived from the provisioned certificate file, as
  specified for this phase. The certificate must therefore be treated as
  device-identity material (restrictive permissions, backed up with the data);
  it is not a substitute for a hardware-protected secret.
- The snapshot envelope has a plaintext SM3 for corruption detection but no
  MAC, and the recording container has no authentication. Confidentiality is
  provided; authenticity of stored media is not.

## Audit chain

Each new row contains:

- `prev_hash`: `entry_hash` of the preceding chained row (empty for the first);
- `entry_hash`: `SM3("uvnvr-audit-chain-v1" || length-prefixed fields ||
  prev_hash)`.

`system_meta.audit_chain_anchor` records the `prev_hash` of the first retained
row and `system_meta.audit_chain_head` records the newest hash. Verification
checks every link, recomputes every entry hash, and compares the head, so a
modified field, a rewritten link or a truncated tail is reported with the row
id and chain index. Rows written before the chain existed have empty hashes and
are reported as skipped legacy rows.

```sh
uvnvr --verify-audit --data-dir /var/lib/uvnvr
# OK /var/lib/uvnvr/uvnvr.db: 42 chained entries verified
# FAIL ...: audit entry id 17 does not match its SM3 hash (content was modified)
#   broken index: 16  entry id: 17
```

## Firmware packages

Container layout and signing rules live in `src/secure/firmware.h`. The signed
message is the exact JSON header, which contains the payload size and SM3;
verification also recomputes the payload digest. Signing is a release-side
operation with a private key that never reaches the device.

```sh
uvnvr --verify-firmware firmware.pkg firmware_pub.pem
```

HTTP upgrade entry (requires `system.manage` and CSRF):

```
POST /api/v1/system/firmware/verify
Content-Type: application/octet-stream
<body: signed package>

200 {"verified":true,"product":"uvnvr","version":"...","stagedPath":"..."}
400 {"error":"invalid_firmware","message":"..."}
```

A failed verification writes an audit row with result `failure` and does not
touch the staging area. A verified payload is written to
`<data-dir>/firmware/staged.bin`; the actual flash/rollback flow is outside this
module.

## Snapshots and recordings

Snapshot envelope (`SealBlob`):

```
"UVSE" | version u8 | algorithm u8 | reserved u16
salt[16] | iv[16] | SM3(plaintext)[32] | SM4-CTR ciphertext
```

The per-file SM4 key is `SM3(master || salt)[0..16]`, where
`master = SM3("uvnvr-storage-key-v1" || device certificate PEM)[0..16]`. The
reader detects the magic, so plaintext snapshots written before encryption stay
readable. Snapshots are sealed when AI events or alert captures are written and
transparently opened by the snapshot APIs.

Recorded segments are encrypted with SM4-CTR using an IV derived from the key
and the final path (`<name>.ts.enc`). Ciphertext length equals plaintext length,
so playback keeps its existing size/offset arithmetic; the block cache and the
keyframe scan decrypt each range at its plaintext offset. Recording finalization
removes the plaintext `.part` file after the encrypted file is complete.

## Tests

`tests/secure_test.cpp` covers:

- 32-entry chain verification, field tampering at index 7, a rewritten
  `prev_hash` and a truncated tail;
- a real SQLite database: verify, then `UPDATE` row 4 and verify again (reports
  broken index 3 / row id 4);
- valid firmware package accepted; modified payload, modified signature and a
  different key rejected;
- SM4 snapshot seal/open (including wrong key and corrupted blob) and
  random-access decryption of a 300 KB encrypted recording.

Manual evidence helpers:

```sh
secure_test --seal device_cert.pem snapshot.jpg snapshot.jpg.enc
secure_test --open device_cert.pem snapshot.jpg.enc snapshot.out
secure_test --make-firmware release_key.pem payload.bin uvnvr 0.19.1 fw.pkg
```
