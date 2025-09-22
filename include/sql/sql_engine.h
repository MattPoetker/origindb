#pragma once

#include <memory>
#include <string>
#include <vector>
#include <optional>
#include <variant>

#include "storage/table.h"
#include "storage/storage_engine.h"

namespace instantdb {

class StorageEngine;
class ModuleStore;
class RaftNode;
class Transaction;
class ChangefeedEngine;

// SQL statement types
enum class StatementType {
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE_TABLE,
    DROP_TABLE,
    CREATE_INDEX,
    DROP_INDEX,
    CREATE_MODULE,
    DROP_MODULE,
    CALL_MODULE,
    CREATE_SUBSCRIPTION,
    DROP_SUBSCRIPTION,
    BEGIN,
    COMMIT,
    ROLLBACK
};

// Parsed SQL statement
struct SqlStatement {
    StatementType type;
    std::string table_name;
    std::vector<std::string> columns;
    std::vector<Value> values;
    std::string where_clause;  // Simplified for now
    std::string module_name;
    std::vector<Value> module_args;
    TableSchema table_schema;
    IndexSchema index_schema;
    std::string raw_sql;
};

// SQL execution result
struct SqlResult {
    struct Column {
        std::string name;
        std::string type;
        bool nullable;
    };

    bool success = false;
    std::string error;
    std::vector<Column> columns;
    std::vector<Row> rows;
    uint64_t rows_affected = 0;
    uint64_t execution_time_ms = 0;
    bool needs_redirect = false;
    std::string leader_address;
};

// SQL Engine
class SqlEngine {
public:
    SqlEngine(std::shared_ptr<StorageEngine> storage,
              std::shared_ptr<ChangefeedEngine> changefeed = nullptr);
    ~SqlEngine();

    bool Initialize();
    void Shutdown();

    // SQL execution
    SqlResult Execute(const std::string& sql,
                     const std::vector<Value>& params = {},
                     const std::string& transaction_id = "");

    // Transaction management
    std::string BeginTransaction(IsolationLevel level = IsolationLevel::SNAPSHOT);
    bool CommitTransaction(const std::string& txn_id);
    bool RollbackTransaction(const std::string& txn_id);

    // Module invocation
    SqlResult CallModule(const std::string& module_name,
                        const std::vector<Value>& args,
                        const std::string& transaction_id = "");

    // Subscription management
    bool CreateSubscription(const std::string& name,
                           const std::string& sql_filter);
    bool DropSubscription(const std::string& name);
    std::vector<std::string> ListSubscriptions() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// SQL Parser
class SqlParser {
public:
    static std::optional<SqlStatement> Parse(const std::string& sql);
    static std::string FormatError(const std::string& error, size_t position);
};

// Query planner
class QueryPlanner {
public:
    enum class PlanType {
        FULL_SCAN,
        INDEX_SCAN,
        PRIMARY_KEY_LOOKUP,
        MODULE_INVOCATION
    };

    struct QueryPlan {
        PlanType type;
        std::string table_name;
        std::string index_name;
        std::vector<std::string> projected_columns;
        std::string filter_expression;
        size_t estimated_cost;
    };

    static QueryPlan CreatePlan(const SqlStatement& stmt,
                                const TableSchema& schema);
};

// Query executor
class QueryExecutor {
public:
    QueryExecutor(std::shared_ptr<StorageEngine> storage,
                  std::shared_ptr<Transaction> txn);

    SqlResult Execute(const QueryPlanner::QueryPlan& plan);

private:
    std::shared_ptr<StorageEngine> storage_;
    std::shared_ptr<Transaction> txn_;
};

} // namespace instantdb