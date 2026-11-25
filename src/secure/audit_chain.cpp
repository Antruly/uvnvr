#include "secure/audit_chain.h"

#include <utility>

namespace uvnvr::secure {
namespace {

void AppendField(std::string* out, std::string_view name, std::string_view value) {
  out->append(name);
  out->push_back('=');
  out->append(std::to_string(value.size()));
  out->push_back(':');
  out->append(value);
  out->push_back('\n');
}

void AppendField(std::string* out, std::string_view name, int64_t value) {
  AppendField(out, name, std::to_string(value));
}

} // namespace

std::string CanonicalAuditMessage(const AuditRecord& record, std::string_view prev_hash) {
  std::string message;
  message.reserve(256 + record.detail.size());
  message.append(kAuditChainVersion);
  message.push_back('\n');
  AppendField(&message, "prev_hash", prev_hash);
  AppendField(&message, "ts", record.ts);
  AppendField(&message, "category", record.category);
  AppendField(&message, "level", record.level);
  AppendField(&message, "actor", record.actor);
  AppendField(&message, "ip", record.ip);
  AppendField(&message, "user_agent", record.user_agent);
  AppendField(&message, "object_type", record.object_type);
  AppendField(&message, "object_id", record.object_id);
  AppendField(&message, "action", record.action);
  AppendField(&message, "result", record.result);
  AppendField(&message, "error_code", record.error_code);
  AppendField(&message, "detail", record.detail);
  AppendField(&message, "request_id", record.request_id);
  return message;
}

std::string ComputeAuditEntryHash(const AuditRecord& record, std::string_view prev_hash) {
  return gm::Sm3Hex(CanonicalAuditMessage(record, prev_hash));
}

void AppendToAuditChain(const std::string& previous, AuditRecord* record) {
  if (record == nullptr)
    return;
  record->prev_hash = previous;
  record->entry_hash = ComputeAuditEntryHash(*record, previous);
}

AuditVerification VerifyAuditChain(const std::vector<AuditRecord>& records,
                                   std::string_view anchor, std::string_view expected_head) {
  AuditVerification result;
  std::size_t first = 0;
  while (first < records.size() && records[first].entry_hash.empty())
    ++first;
  result.skipped_legacy = first;

  if (first == records.size()) {
    result.ok = expected_head.empty();
    result.message = records.empty()
                         ? "no audit entries"
                         : (result.ok ? "only legacy entries without a chain"
                                      : "the chain head is missing");
    if (!result.ok) {
      result.broken_index = records.size();
      result.expected_hash = std::string(expected_head);
      result.message = "the audit log ends before the recorded chain head (truncated tail)";
    }
    return result;
  }

  std::string previous(anchor);
  for (std::size_t index = first; index < records.size(); ++index) {
    const AuditRecord& record = records[index];
    result.broken_index = index;
    result.broken_id = record.id;
    if (record.entry_hash.empty()) {
      result.actual_hash.clear();
      result.expected_hash = previous;
      result.message = "unhashed audit entry after the chain started (id " +
                       std::to_string(record.id) + ")";
      return result;
    }
    if (record.prev_hash != previous) {
      result.actual_hash = record.prev_hash;
      result.expected_hash = previous;
      result.message = "audit chain link broken before entry id " + std::to_string(record.id) +
                       ": prev_hash does not match the preceding entry";
      return result;
    }
    const std::string computed = ComputeAuditEntryHash(record, record.prev_hash);
    if (computed != record.entry_hash) {
      result.actual_hash = record.entry_hash;
      result.expected_hash = computed;
      result.message = "audit entry id " + std::to_string(record.id) +
                       " does not match its SM3 hash (content was modified)";
      return result;
    }
    previous = record.entry_hash;
    ++result.checked;
  }

  if (!expected_head.empty() && previous != expected_head) {
    result.broken_index = records.size();
    result.broken_id = records.empty() ? 0 : records.back().id;
    result.actual_hash = previous;
    result.expected_hash = std::string(expected_head);
    result.message = "the audit chain head does not match the recorded head (truncated tail)";
    return result;
  }

  result.ok = true;
  result.broken_index = 0;
  result.broken_id = 0;
  result.message = "audit chain verified";
  return result;
}

} // namespace uvnvr::secure
