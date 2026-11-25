#pragma once

// Tamper-evident audit trail built from SM3 hashes.
//
// Every audit row carries the hash of the preceding row (`prev_hash`) and its
// own hash (`entry_hash`). `entry_hash = SM3(prev_hash || canonical fields)`,
// so changing any field of an entry, or re-ordering/removing entries, makes the
// chain stop verifying. The database keeps the first retained entry's
// `prev_hash` as the anchor and the newest hash as the head; the head check
// also catches truncation of the tail.
//
// The types here are storage-agnostic so the same verification code is used by
// the unit tests, the server's append path and the CLI (`--verify-audit`).
// Database glue lives in audit_store.h.

#include "crypto/sm.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace uvnvr::secure {

inline constexpr std::string_view kAuditChainVersion = "uvnvr-audit-chain-v1";
// The first chained entry in a fresh database starts from an empty hash.
inline constexpr std::string_view kAuditGenesisHash = "";

struct AuditRecord {
  int64_t id = 0;
  int64_t ts = 0;
  std::string category;
  std::string level;
  std::string actor;
  std::string ip;
  std::string user_agent;
  std::string object_type;
  std::string object_id;
  std::string action;
  std::string result;
  std::string error_code;
  std::string detail;
  std::string request_id;
  // Filled by the chain builder / read back from the database.
  std::string prev_hash;
  std::string entry_hash;
};

// Deterministic, unambiguous encoding of one record. Field values are length
// prefixed, so moving a byte from one field to the next cannot produce the same
// byte stream. It deliberately excludes `id`: the row id is assigned by the
// database after the hash is computed.
std::string CanonicalAuditMessage(const AuditRecord& record, std::string_view prev_hash);

// Lower-case hex SM3 of `CanonicalAuditMessage`.
std::string ComputeAuditEntryHash(const AuditRecord& record, std::string_view prev_hash);

// Convenience: fills `record.prev_hash` and `record.entry_hash`. `previous` is
// the hash of the preceding chained entry (empty for the first entry).
void AppendToAuditChain(const std::string& previous, AuditRecord* record);

struct AuditVerification {
  bool ok = false;
  std::size_t checked = 0;          // chained entries verified
  std::size_t skipped_legacy = 0;   // leading rows written without a chain
  std::size_t broken_index = 0;     // index into the supplied vector
  int64_t broken_id = 0;            // audit_log.id at the break, when known
  std::string expected_hash;        // hash the record should have carried
  std::string actual_hash;          // hash the record actually carried
  std::string message;
};

// Verifies a chain. `anchor` is the hash the first chained entry must point at
// (empty for a chain that starts at the beginning of the retained rows).
// When `expected_head` is non-empty the newest hash is compared against it,
// which detects truncation of the tail.
//
// Leading rows with empty `entry_hash` are legacy entries written before the
// chain was enabled; they are skipped. An unhashed row appearing after the
// chain started is reported as a break, because it means rows were injected or
// the chain was disabled midway.
AuditVerification VerifyAuditChain(const std::vector<AuditRecord>& records,
                                   std::string_view anchor = {},
                                   std::string_view expected_head = {});

} // namespace uvnvr::secure
