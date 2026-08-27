#pragma once

#include <sqlite3.h>

#include <stdexcept>
#include <string>
#include <string_view>

namespace bang::sql {

class Statement {
public:
    Statement(sqlite3* database, std::string_view sql)
    {
        if (sqlite3_prepare_v2(database, sql.data(), static_cast<int>(sql.size()),
                &statement_, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error(
                std::string("SQLite prepare failed: ") + sqlite3_errmsg(database));
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const { return statement_; }

private:
    sqlite3_stmt* statement_ = nullptr;
};

inline void bindText(sqlite3_stmt* statement, int index, std::string_view value)
{
    sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()),
        SQLITE_TRANSIENT);
}

inline std::string columnText(sqlite3_stmt* statement, int index)
{
    const auto* text = sqlite3_column_text(statement, index);
    return text == nullptr ? std::string {} : reinterpret_cast<const char*>(text);
}

inline void expectRow(sqlite3_stmt* statement)
{
    if (sqlite3_step(statement) != SQLITE_ROW) {
        throw std::runtime_error("SQLite read returned no row");
    }
}

inline void expectDone(sqlite3* database, sqlite3_stmt* statement)
{
    if (sqlite3_step(statement) != SQLITE_DONE) {
        throw std::runtime_error(
            std::string("SQLite write failed: ") + sqlite3_errmsg(database));
    }
}

} // namespace bang::sql
