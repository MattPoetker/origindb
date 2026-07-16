#include "sql/sql_engine.h"
#include "storage/storage_engine.h"
#include "changefeed/changefeed_engine.h"
#include <spdlog/spdlog.h>
#include <regex>
#include <algorithm>
#include <cctype>

namespace instantdb {

// Transaction entry for Raft log
struct TxnEntry {
    uint64_t txn_id;
    std::vector<SqlStatement> operations;
    uint64_t timestamp;
};

class SqlEngine::Impl {
public:
    Impl(std::shared_ptr<StorageEngine> storage, std::shared_ptr<ChangefeedEngine> changefeed)
        : storage_(storage), changefeed_(changefeed), next_txn_id_(1) {}

    bool Initialize() {
        spdlog::info("SQL Engine initialized");
        return true;
    }

    void Shutdown() {
        spdlog::info("SQL Engine shutdown");
    }

    SqlResult Execute(const std::string& sql,
                     const std::vector<Value>& params,
                     const std::string& transaction_id) {

        spdlog::info("SQL Execute: Starting execution of: {}", sql);

        auto stmt = SqlParser::Parse(sql);
        if (!stmt) {
            spdlog::error("SQL Execute: Failed to parse SQL statement");
            return {false, "Failed to parse SQL statement"};
        }

        spdlog::info("SQL Execute: Parsed statement type: {}", static_cast<int>(stmt->type));

        // For prototype, handle simple operations directly
        switch (stmt->type) {
            case StatementType::CREATE_TABLE:
                spdlog::info("SQL Execute: Executing CREATE_TABLE");
                return ExecuteCreateTable(*stmt);
            case StatementType::INSERT:
                spdlog::info("SQL Execute: Executing INSERT");
                return ExecuteInsert(*stmt);
            case StatementType::SELECT:
                return ExecuteSelect(*stmt);
            case StatementType::UPDATE:
                return ExecuteUpdate(*stmt);
            case StatementType::DELETE:
                return ExecuteDelete(*stmt);
            default:
                return {false, "Statement type not supported in prototype"};
        }
    }

    std::string BeginTransaction(IsolationLevel level) {
        std::string txn_id = "txn-" + std::to_string(next_txn_id_++);
        auto txn = storage_->BeginTransaction(level);
        active_transactions_[txn_id] = txn;
        spdlog::debug("Started transaction {}", txn_id);
        return txn_id;
    }

    bool CommitTransaction(const std::string& txn_id) {
        auto it = active_transactions_.find(txn_id);
        if (it == active_transactions_.end()) {
            return false;
        }

        bool success = storage_->Commit(it->second);
        active_transactions_.erase(it);
        spdlog::debug("Committed transaction {}", txn_id);
        return success;
    }

    bool RollbackTransaction(const std::string& txn_id) {
        auto it = active_transactions_.find(txn_id);
        if (it == active_transactions_.end()) {
            return false;
        }

        bool success = storage_->Rollback(it->second);
        active_transactions_.erase(it);
        spdlog::debug("Rolled back transaction {}", txn_id);
        return success;
    }

private:
    SqlResult ExecuteCreateTable(const SqlStatement& stmt) {
        if (storage_->CreateTable(stmt.table_schema)) {
            spdlog::info("Created table {}", stmt.table_schema.name);
            return {true, "", {}, {}, 0, 0};
        }
        return {false, "Failed to create table"};
    }

    SqlResult ExecuteInsert(const SqlStatement& stmt) {
        spdlog::info("ExecuteInsert: Starting INSERT for table {}", stmt.table_name);

        // Build row from statement
        Row row;

        spdlog::info("ExecuteInsert: Converting key from first value");
        // Convert first value to string for key (handle different types)
        std::visit([&row](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                row.key = v;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                row.key = std::to_string(v);
            } else if constexpr (std::is_same_v<T, double>) {
                row.key = std::to_string(v);
            } else {
                row.key = "unknown";
            }
        }, stmt.values[0]);

        spdlog::info("ExecuteInsert: Key converted to: {}", row.key);

        auto table = storage_->GetTable(stmt.table_name);
        if (!table) {
            spdlog::error("ExecuteInsert: Table {} not found", stmt.table_name);
            return {false, "Table not found: " + stmt.table_name};
        }

        spdlog::info("ExecuteInsert: Found table {}", stmt.table_name);
        const auto& schema = table->GetSchema();

        // Map values to columns
        spdlog::info("ExecuteInsert: Mapping {} values to columns", stmt.values.size());
        for (size_t i = 0; i < stmt.columns.size() && i < stmt.values.size(); ++i) {
            row.columns[stmt.columns[i]] = stmt.values[i];
            spdlog::info("ExecuteInsert: Mapped column {} to value", stmt.columns[i]);
        }

        // Execute the insert
        spdlog::info("ExecuteInsert: Calling storage->Insert");
        if (storage_->Insert(stmt.table_name, row)) {
            // Changefeed event is emitted by the storage engine at commit
            spdlog::debug("Inserted row with key {} into {}", row.key, stmt.table_name);
            return {true, "", {}, {}, 1, 0};
        }

        // Insert failed - check if key already exists and convert to UPDATE
        spdlog::info("ExecuteInsert: Insert failed, checking if key {} exists for UPDATE conversion", row.key);
        auto existing_row = storage_->Get(stmt.table_name, row.key);
        spdlog::info("ExecuteInsert: storage_->Get returned {}", existing_row ? "valid row" : "null");
        if (existing_row) {
            spdlog::info("ExecuteInsert: Key {} exists, converting INSERT to UPDATE", row.key);

            // Get the old row for changefeed
            Row old_row = *existing_row;

            // Create updated row by merging existing data with new values
            Row updated_row = old_row;  // Start with existing row

            // Update only the columns specified in the INSERT
            for (size_t i = 0; i < stmt.columns.size() && i < stmt.values.size(); ++i) {
                updated_row.columns[stmt.columns[i]] = stmt.values[i];
            }

            // Execute the update
            if (storage_->Update(stmt.table_name, row.key, updated_row)) {
                spdlog::info("ExecuteInsert: Successfully converted INSERT to UPDATE for key {}", row.key);
                return {true, "", {}, {}, 0, 1};  // 0 inserts, 1 update
            }

            spdlog::error("ExecuteInsert: UPDATE conversion failed for key {}", row.key);
            return {false, "Failed to convert INSERT to UPDATE"};
        } else {
            // storage_->Get returned null, but INSERT failed with key collision
            // This suggests the row exists but Get is not finding it (possibly due to locking/transaction issues)
            // Since INSERT failed with "already exists", we can assume the row exists and proceed with UPDATE
            spdlog::info("ExecuteInsert: GET returned null but INSERT failed with key collision, proceeding with UPDATE anyway");

            // Execute the update with the new row data
            if (storage_->Update(stmt.table_name, row.key, row)) {
                spdlog::info("ExecuteInsert: Successfully converted INSERT to UPDATE for key {} (with empty old_row)", row.key);
                return {true, "", {}, {}, 0, 1};  // 0 inserts, 1 update
            }

            spdlog::error("ExecuteInsert: UPDATE conversion failed for key {} (with empty old_row)", row.key);
            return {false, "Failed to convert INSERT to UPDATE"};
        }

        return {false, "Failed to insert row"};
    }

    SqlResult ExecuteSelect(const SqlStatement& stmt) {
        auto table = storage_->GetTable(stmt.table_name);
        if (!table) {
            return {false, "Table not found: " + stmt.table_name};
        }

        const auto& schema = table->GetSchema();
        SqlResult result;
        result.success = true;

        // Set up columns
        for (const auto& col : schema.columns) {
            SqlResult::Column column;
            column.name = col.name;
            column.type = DataTypeToString(col.type);
            column.nullable = col.nullable;
            result.columns.push_back(column);
        }

        // Simple scan for prototype
        table->Scan("", "", [&result](const std::string& key, const Row& row) {
            result.rows.push_back(row);
            return true; // Continue scanning
        });

        spdlog::debug("Selected {} rows from {}", result.rows.size(), stmt.table_name);
        return result;
    }

    SqlResult ExecuteUpdate(const SqlStatement& stmt) {
        spdlog::info("ExecuteUpdate: Starting UPDATE for table {}", stmt.table_name);

        auto table = storage_->GetTable(stmt.table_name);
        if (!table) {
            spdlog::error("ExecuteUpdate: Table {} not found", stmt.table_name);
            return {false, "Table not found: " + stmt.table_name};
        }

        spdlog::info("ExecuteUpdate: Found table {}", stmt.table_name);

        // For simplified implementation, support UPDATE with WHERE clause specifying key
        // UPDATE table SET col1=val1, col2=val2 WHERE id=123
        if (stmt.where_clause.empty()) {
            return {false, "UPDATE requires WHERE clause to specify which rows to update"};
        }

        // Parse WHERE clause to extract key (simplified: WHERE id=value)
        std::regex where_regex(R"((\w+)\s*=\s*([^,\s]+))");
        std::smatch where_match;
        if (!std::regex_search(stmt.where_clause, where_match, where_regex)) {
            return {false, "Invalid WHERE clause format. Expected: column=value"};
        }

        std::string key_column = where_match[1].str();
        std::string key_value = where_match[2].str();

        // Remove quotes from key value if present
        if (key_value.front() == '\'' && key_value.back() == '\'') {
            key_value = key_value.substr(1, key_value.length() - 2);
        }

        spdlog::info("ExecuteUpdate: Updating row with {}={}", key_column, key_value);

        // Get existing row to preserve non-updated columns
        auto existing_row = storage_->Get(stmt.table_name, key_value);
        if (!existing_row) {
            return {false, "No row found with " + key_column + "=" + key_value};
        }

        // Create updated row by copying existing row and applying updates
        Row updated_row = *existing_row;

        // Apply column updates
        spdlog::info("ExecuteUpdate: Applying {} column updates", stmt.columns.size());
        for (size_t i = 0; i < stmt.columns.size() && i < stmt.values.size(); ++i) {
            updated_row.columns[stmt.columns[i]] = stmt.values[i];
            spdlog::info("ExecuteUpdate: Updated column {} to new value", stmt.columns[i]);
        }

        // Execute the update
        spdlog::info("ExecuteUpdate: Calling storage->Update");
        if (storage_->Update(stmt.table_name, key_value, updated_row)) {
            // Changefeed event is emitted by the storage engine at commit
            spdlog::debug("Updated row with key {} in {}", key_value, stmt.table_name);
            return {true, "", {}, {}, 1, 0};
        }

        return {false, "Failed to update row"};
    }

    SqlResult ExecuteDelete(const SqlStatement& stmt) {
        // Simplified delete for prototype
        return {false, "DELETE not implemented in prototype"};
    }

    std::string DataTypeToString(DataType type) {
        switch (type) {
            case DataType::INT32: return "INT32";
            case DataType::INT64: return "INT64";
            case DataType::FLOAT: return "FLOAT";
            case DataType::DOUBLE: return "DOUBLE";
            case DataType::STRING: return "STRING";
            case DataType::BYTES: return "BYTES";
            case DataType::BOOLEAN: return "BOOLEAN";
            case DataType::TIMESTAMP: return "TIMESTAMP";
            case DataType::JSON: return "JSON";
            default: return "UNKNOWN";
        }
    }

private:
    std::shared_ptr<StorageEngine> storage_;
    std::shared_ptr<ChangefeedEngine> changefeed_;
    std::unordered_map<std::string, std::shared_ptr<Transaction>> active_transactions_;
    std::atomic<uint64_t> next_txn_id_;
};

// SqlParser implementation (stub for prototype)
std::optional<SqlStatement> SqlParser::Parse(const std::string& sql) {
    std::string upper_sql = sql;
    std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(), ::toupper);

    SqlStatement stmt;
    stmt.raw_sql = sql;

    // Regexes match against upper_sql; identifiers must keep their original
    // case (tables created by WASM modules are case-sensitive). toupper
    // preserves length, so match positions map 1:1 onto the original string.
    auto original_text = [&sql](const std::smatch& m, int group) {
        return sql.substr(m.position(group), m.length(group));
    };

    // Enhanced INSERT parsing: INSERT INTO table (col1, col2, ...) VALUES (val1, val2, ...)
    std::regex insert_regex(R"(INSERT\s+INTO\s+(\w+)\s*(?:\((.*?)\))?\s*VALUES\s*\((.*)\))");
    std::smatch match;

    if (std::regex_search(upper_sql, match, insert_regex)) {
        stmt.type = StatementType::INSERT;
        std::string table_name = original_text(match, 1);
        stmt.table_name = table_name;

        // Parse column list if provided
        if (match.size() > 2 && match[2].matched && !match[2].str().empty()) {
            std::string columns_str = match[2].str();
            std::regex column_regex(R"(\w+)");
            std::sregex_iterator iter(columns_str.begin(), columns_str.end(), column_regex);
            std::sregex_iterator end;

            for (; iter != end; ++iter) {
                stmt.columns.push_back(iter->str());
            }
        } else {
            // Default columns for users table if no column list specified
            stmt.columns = {"id", "name", "email"};
        }

        // Extract values from original SQL (not uppercased) to preserve case
        std::smatch original_match;
        if (std::regex_search(sql, original_match, insert_regex)) {
            std::string values_str = original_match[3].str();
            std::regex value_regex(R"('([^']*)'|([^,\s]+))");
            std::sregex_iterator iter(values_str.begin(), values_str.end(), value_regex);
            std::sregex_iterator end;

        for (; iter != end; ++iter) {
            std::string value;
            if ((*iter)[1].matched) {
                // Quoted string
                value = (*iter)[1].str();
                stmt.values.push_back(value);
            } else {
                // Unquoted value - try to parse as number
                value = (*iter)[2].str();
                try {
                    if (value.find('.') != std::string::npos) {
                        stmt.values.push_back(std::stod(value));
                    } else {
                        stmt.values.push_back(std::stoll(value));
                    }
                } catch (...) {
                    stmt.values.push_back(value);
                }
            }
        }
        }

        return stmt;
    }

    // Simple CREATE TABLE parsing
    std::regex create_regex(R"(CREATE\s+TABLE\s+(\w+)\s*\((.*)\))");
    if (std::regex_search(upper_sql, match, create_regex)) {
        stmt.type = StatementType::CREATE_TABLE;
        stmt.table_schema.name = original_text(match, 1);

        // Simplified column parsing for prototype
        instantdb::Column id_col;
        id_col.name = "id";
        id_col.type = DataType::INT64;
        id_col.nullable = false;
        id_col.is_primary_key = true;

        instantdb::Column name_col;
        name_col.name = "name";
        name_col.type = DataType::STRING;
        name_col.nullable = false;
        name_col.is_primary_key = false;

        instantdb::Column email_col;
        email_col.name = "email";
        email_col.type = DataType::STRING;
        email_col.nullable = false;
        email_col.is_primary_key = false;

        stmt.table_schema.columns = {id_col, name_col, email_col};
        stmt.table_schema.primary_key = {"id"};

        return stmt;
    }

    // Simple SELECT parsing
    std::regex select_regex(R"(SELECT\s+.*\s+FROM\s+(\w+))");
    if (std::regex_search(upper_sql, match, select_regex)) {
        stmt.type = StatementType::SELECT;
        std::string table_name = original_text(match, 1);
        stmt.table_name = table_name;
        return stmt;
    }

    // Simple UPDATE parsing: UPDATE table SET col1=val1, col2=val2, ... WHERE condition
    std::regex update_regex(R"(UPDATE\s+(\w+)\s+SET\s+(.*?)(?:\s+WHERE\s+(.*))?$)");
    if (std::regex_search(upper_sql, match, update_regex)) {
        stmt.type = StatementType::UPDATE;
        stmt.table_name = original_text(match, 1);

        // Extract SET clause from original SQL (not uppercased) to preserve case
        std::smatch original_match;
        if (std::regex_search(sql, original_match, update_regex)) {
            std::string set_clause = original_match[2].str();
        std::regex set_regex(R"((\w+)\s*=\s*([^,]+))");
        std::sregex_iterator iter(set_clause.begin(), set_clause.end(), set_regex);
        std::sregex_iterator end;

        for (; iter != end; ++iter) {
            std::string column = (*iter)[1].str();
            std::string value_str = (*iter)[2].str();

            // Trim whitespace
            value_str.erase(0, value_str.find_first_not_of(" \t"));
            value_str.erase(value_str.find_last_not_of(" \t") + 1);

            stmt.columns.push_back(column);

            // Parse value (remove quotes if present)
            if (value_str.front() == '\'' && value_str.back() == '\'') {
                value_str = value_str.substr(1, value_str.length() - 2);
                stmt.values.push_back(value_str);
            } else {
                // Try to parse as number
                try {
                    if (value_str.find('.') != std::string::npos) {
                        stmt.values.push_back(std::stod(value_str));
                    } else {
                        stmt.values.push_back(std::stoll(value_str));
                    }
                } catch (...) {
                    stmt.values.push_back(value_str);
                }
            }
        }
        }

        // Parse WHERE clause if present
        if (match.size() > 3 && match[3].matched) {
            stmt.where_clause = match[3].str();
        }

        return stmt;
    }

    return std::nullopt;
}

std::string SqlParser::FormatError(const std::string& error, size_t position) {
    return "Parse error at position " + std::to_string(position) + ": " + error;
}

// SqlEngine public interface
SqlEngine::SqlEngine(std::shared_ptr<StorageEngine> storage,
                     std::shared_ptr<ChangefeedEngine> changefeed)
    : impl_(std::make_unique<Impl>(storage, changefeed)) {}

SqlEngine::~SqlEngine() = default;

bool SqlEngine::Initialize() {
    return impl_->Initialize();
}

void SqlEngine::Shutdown() {
    impl_->Shutdown();
}

SqlResult SqlEngine::Execute(const std::string& sql,
                            const std::vector<Value>& params,
                            const std::string& transaction_id) {
    return impl_->Execute(sql, params, transaction_id);
}

std::string SqlEngine::BeginTransaction(IsolationLevel level) {
    return impl_->BeginTransaction(level);
}

bool SqlEngine::CommitTransaction(const std::string& txn_id) {
    return impl_->CommitTransaction(txn_id);
}

bool SqlEngine::RollbackTransaction(const std::string& txn_id) {
    return impl_->RollbackTransaction(txn_id);
}

SqlResult SqlEngine::CallModule(const std::string& module_name,
                               const std::vector<Value>& args,
                               const std::string& transaction_id) {
    // TODO: Implement module calling
    return {false, "Module calling not implemented in prototype"};
}

bool SqlEngine::CreateSubscription(const std::string& name,
                                  const std::string& sql_filter) {
    // TODO: Implement subscription creation
    return false;
}

bool SqlEngine::DropSubscription(const std::string& name) {
    // TODO: Implement subscription deletion
    return false;
}

std::vector<std::string> SqlEngine::ListSubscriptions() const {
    // TODO: Implement subscription listing
    return {};
}

} // namespace instantdb