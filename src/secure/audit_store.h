#pragma once

// Database glue for the audit hash chain. Kept separate from audit_chain.h so
// the hashing rules stay testable without a database, and so the CLI and the
// server read the chain the same way.

#include "secure/audit_chain.h"

#include <string>
#include <vector>

namespace uvnvr::secure {

// The audit rows and the two system_meta anchors that bound the chain.
struct AuditChainSnapshot {
  std::vector<AuditRecord> records;
  std::string anchor; // prev_hash of the first retained chained entry
  std::string head;   // entry_hash of the newest chained entry
};

// Opens `db_path` (a plain filesystem path or a sqlite:// URL) read-only in
// practice and loads the audit chain. Returns false only for I/O/schema
// errors; a broken chain is reported by the verification result.
bool ReadAuditChainFromDatabase(const std::string& db_path, AuditChainSnapshot* snapshot,
                                std::string* error);

// Reads and verifies the chain in one call. `verification.ok` is the chain
// verdict; the return value is false only when the database could not be read.
bool VerifyAuditDatabase(const std::string& db_path, AuditVerification* verification,
                         std::string* error);

} // namespace uvnvr::secure
