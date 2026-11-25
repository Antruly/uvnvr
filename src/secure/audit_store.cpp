#include "secure/audit_store.h"

#include <db/uvcpp_db.h>

#include <utility>

namespace uvnvr::secure {
namespace {

constexpr const char* kSelectColumns =
    "id, ts, category, level, COALESCE(actor, '') AS actor, COALESCE(ip, '') AS ip, "
    "COALESCE(user_agent, '') AS user_agent, COALESCE(object_type, '') AS object_type, "
    "COALESCE(object_id, '') AS object_id, action, result, "
    "COALESCE(error_code, '') AS error_code, COALESCE(detail, '{}') AS detail, "
    "COALESCE(request_id, '') AS request_id, COALESCE(prev_hash, '') AS prev_hash, "
    "COALESCE(entry_hash, '') AS entry_hash";

std::string DatabaseUrl(const std::string& path) {
  if (path.rfind("sqlite://", 0) == 0)
    return path;
  return "sqlite://" + path;
}

std::string Cell(const uvcpp::uvcpp_db_table& table, size_t row, const char* column) {
  return table.at(row, column).to_text();
}

bool HasChainColumns(uvcpp::uvcpp_db_client* db, std::string* error) {
  uvcpp::uvcpp_db_table columns;
  const auto status = db->query("PRAGMA table_info(audit_log)", &columns);
  if (!uvcpp::uvcpp_db_ok(status)) {
    if (error != nullptr)
      *error = "cannot inspect audit_log: " + db->last_error();
    return false;
  }
  if (columns.empty()) {
    if (error != nullptr)
      *error = "the database has no audit_log table";
    return false;
  }
  bool prev = false;
  bool entry = false;
  for (size_t row = 0; row < columns.rows().size(); ++row) {
    const std::string name = Cell(columns, row, "name");
    if (name == "prev_hash")
      prev = true;
    if (name == "entry_hash")
      entry = true;
  }
  if (!prev || !entry) {
    if (error != nullptr)
      *error = "audit_log predates the hash chain (prev_hash/entry_hash columns are missing)";
    return false;
  }
  return true;
}

bool ReadMeta(uvcpp::uvcpp_db_client* db, const char* key, std::string* value,
              std::string* error) {
  uvcpp::uvcpp_db_params params;
  params.add(key);
  uvcpp::uvcpp_db_table table;
  const auto status =
      db->query("SELECT value FROM system_meta WHERE key = ?", params, &table);
  if (!uvcpp::uvcpp_db_ok(status)) {
    if (error != nullptr)
      *error = "cannot read system_meta: " + db->last_error();
    return false;
  }
  *value = table.empty() ? std::string() : Cell(table, 0, "value");
  return true;
}

} // namespace

bool ReadAuditChainFromDatabase(const std::string& db_path, AuditChainSnapshot* snapshot,
                                std::string* error) {
  if (snapshot == nullptr) {
    if (error != nullptr)
      *error = "audit chain snapshot output is null";
    return false;
  }
  uvcpp::uvcpp_db_client db;
  const auto opened = db.open(DatabaseUrl(db_path));
  if (!uvcpp::uvcpp_db_ok(opened)) {
    if (error != nullptr)
      *error = "cannot open " + db_path + ": " + db.last_error();
    return false;
  }
  if (!HasChainColumns(&db, error))
    return false;

  uvcpp::uvcpp_db_table table;
  const auto queried =
      db.query(std::string("SELECT ") + kSelectColumns + " FROM audit_log ORDER BY id", &table);
  if (!uvcpp::uvcpp_db_ok(queried)) {
    if (error != nullptr)
      *error = "cannot read audit_log: " + db.last_error();
    return false;
  }

  AuditChainSnapshot result;
  result.records.reserve(table.rows().size());
  for (size_t row = 0; row < table.rows().size(); ++row) {
    AuditRecord record;
    record.id = table.at(row, "id").to_int64();
    record.ts = table.at(row, "ts").to_int64();
    record.category = Cell(table, row, "category");
    record.level = Cell(table, row, "level");
    record.actor = Cell(table, row, "actor");
    record.ip = Cell(table, row, "ip");
    record.user_agent = Cell(table, row, "user_agent");
    record.object_type = Cell(table, row, "object_type");
    record.object_id = Cell(table, row, "object_id");
    record.action = Cell(table, row, "action");
    record.result = Cell(table, row, "result");
    record.error_code = Cell(table, row, "error_code");
    record.detail = Cell(table, row, "detail");
    record.request_id = Cell(table, row, "request_id");
    record.prev_hash = Cell(table, row, "prev_hash");
    record.entry_hash = Cell(table, row, "entry_hash");
    result.records.push_back(std::move(record));
  }

  if (!ReadMeta(&db, "audit_chain_anchor", &result.anchor, error))
    return false;
  if (!ReadMeta(&db, "audit_chain_head", &result.head, error))
    return false;

  *snapshot = std::move(result);
  return true;
}

bool VerifyAuditDatabase(const std::string& db_path, AuditVerification* verification,
                         std::string* error) {
  if (verification == nullptr) {
    if (error != nullptr)
      *error = "audit verification output is null";
    return false;
  }
  AuditChainSnapshot snapshot;
  if (!ReadAuditChainFromDatabase(db_path, &snapshot, error))
    return false;
  *verification = VerifyAuditChain(snapshot.records, snapshot.anchor, snapshot.head);
  return true;
}

} // namespace uvnvr::secure
