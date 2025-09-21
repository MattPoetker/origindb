#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace instantdb {

enum class WALEntryType {
    CREATE_TABLE,
    DROP_TABLE,
    INSERT,
    UPDATE,
    DELETE,
    CREATE_INDEX,
    DROP_INDEX,
    BEGIN_TXN,
    COMMIT_TXN,
    ROLLBACK_TXN
};

struct WALEntry {
    WALEntryType type;
    uint64_t sequence = 0;
    uint64_t transaction_id = 0;
    uint64_t timestamp = 0;
    std::string table_name;
    std::string key;
    std::vector<uint8_t> data;
};

class WAL {
public:
    virtual ~WAL() = default;

    virtual bool Initialize() = 0;
    virtual bool Append(const WALEntry& entry) = 0;
    virtual std::vector<WALEntry> ReadAll() = 0;
    virtual bool Truncate(uint64_t sequence) = 0;
    virtual void Flush() = 0;
    virtual uint64_t GetLastSequence() const = 0;
    virtual uint64_t GetSize() const = 0;
};

} // namespace instantdb