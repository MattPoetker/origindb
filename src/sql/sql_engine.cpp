#include "sql/sql_engine.h"
#include "storage/storage_engine.h"
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
    Impl(std::shared_ptr<StorageEngine> storage)
        : storage_(storage), next_txn_id_(1) {}

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
            // Emit change event for prototype
            EmitChangeEvent(stmt.table_name, "INSERT", row.key, row);

            spdlog::debug("Inserted row with key {} into {}", row.key, stmt.table_name);
            return {true, "", {}, {}, 1, 0};
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
        // Simplified update for prototype
        return {false, "UPDATE not implemented in prototype"};
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

    void EmitChangeEvent(const std::string& table, const std::string& op,
                        const std::string& key, const Row& row) {
        // TODO: Connect to changefeed engine when implemented
        spdlog::debug("Change event: {} on table {} key {}",
                     op, table, key);
    }

private:
    std::shared_ptr<StorageEngine> storage_;
    std::unordered_map<std::string, std::shared_ptr<Transaction>> active_transactions_;
    std::atomic<uint64_t> next_txn_id_;
};

// SqlParser implementation (stub for prototype)
std::optional<SqlStatement> SqlParser::Parse(const std::string& sql) {
    std::string upper_sql = sql;
    std::transform(upper_sql.begin(), upper_sql.end(), upper_sql.begin(), ::toupper);

    SqlStatement stmt;
    stmt.raw_sql = sql;

    // Simple INSERT parsing: INSERT INTO table VALUES (val1, val2, ...)
    std::regex insert_regex(R"(INSERT\s+INTO\s+(\w+)\s+VALUES\s*\((.*)\))");
    std::smatch match;

    if (std::regex_search(upper_sql, match, insert_regex)) {
        stmt.type = StatementType::INSERT;
        std::string table_name = match[1].str();
        std::transform(table_name.begin(), table_name.end(), table_name.begin(), ::tolower);
        stmt.table_name = table_name;

        // Parse values (simplified)
        std::string values_str = match[2].str();
        std::regex value_regex(R"([^,\s]+)");
        std::sregex_iterator iter(values_str.begin(), values_str.end(), value_regex);
        std::sregex_iterator end;

        for (; iter != end; ++iter) {
            std::string value = iter->str();
            // Remove quotes if present
            if (value.front() == '\'' && value.back() == '\'') {
                value = value.substr(1, value.length() - 2);
                stmt.values.push_back(value);
            } else {
                // Try to parse as number
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

        // For prototype, assume columns match values in order
        // In a real implementation, we'd parse the column list or use schema
        stmt.columns = {"id", "name"}; // Hardcoded for users table

        return stmt;
    }

    // Simple CREATE TABLE parsing
    std::regex create_regex(R"(CREATE\s+TABLE\s+(\w+)\s*\((.*)\))");
    if (std::regex_search(upper_sql, match, create_regex)) {
        stmt.type = StatementType::CREATE_TABLE;
        stmt.table_schema.name = match[1].str();

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

        stmt.table_schema.columns = {id_col, name_col};
        stmt.table_schema.primary_key = {"id"};

        return stmt;
    }

    // Simple SELECT parsing
    std::regex select_regex(R"(SELECT\s+.*\s+FROM\s+(\w+))");
    if (std::regex_search(upper_sql, match, select_regex)) {
        stmt.type = StatementType::SELECT;
        std::string table_name = match[1].str();
        std::transform(table_name.begin(), table_name.end(), table_name.begin(), ::tolower);
        stmt.table_name = table_name;
        return stmt;
    }

    return std::nullopt;
}

std::string SqlParser::FormatError(const std::string& error, size_t position) {
    return "Parse error at position " + std::to_string(position) + ": " + error;
}

// SqlEngine public interface
SqlEngine::SqlEngine(std::shared_ptr<StorageEngine> storage)
    : impl_(std::make_unique<Impl>(storage)) {}

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