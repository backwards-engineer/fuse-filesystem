#pragma once
#include "crypto.hpp"
#include "store.hpp"
#include <cerrno>
#include <sqlite3.h>

namespace localfs {
inline void sql_check(sqlite3 *db, int code) {
    if (code == SQLITE_OK || code == SQLITE_ROW || code == SQLITE_DONE)
        return;
    int error = EIO;
    switch (code & 255) {
    case SQLITE_FULL:
        error = ENOSPC;
        break;
    case SQLITE_NOMEM:
        error = ENOMEM;
        break;
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        error = EBUSY;
        break;
    case SQLITE_READONLY:
        error = EROFS;
        break;
    }
    throw Error(error, std::string("SQLite: ") + sqlite3_errmsg(db));
}
inline void execute(sqlite3 *db, const char *sql) {
    sql_check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
class Statement {
    sqlite3 *db_;
    sqlite3_stmt *stmt_ = nullptr;

  public:
    Statement(sqlite3 *db, const char *sql) : db_(db) {
        sql_check(db_, sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr));
    }
    ~Statement() {
        sqlite3_finalize(stmt_);
    }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;
    void bind(int index, Id value) {
        sql_check(db_, sqlite3_bind_int64(stmt_, index, value));
    }
    void bind(int index, std::string_view value) {
        sql_check(db_, sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                                         SQLITE_TRANSIENT));
    }
    void bind(int index, const Bytes &value) {
        sql_check(db_, sqlite3_bind_blob(stmt_, index, value.data(), static_cast<int>(value.size()),
                                         SQLITE_TRANSIENT));
    }
    bool row() {
        int rc = sqlite3_step(stmt_);
        sql_check(db_, rc);
        return rc == SQLITE_ROW;
    }
    void done() {
        if (row())
            throw Error(EIO, "Unexpected SQL result");
    }
    Id number(int column) const {
        return sqlite3_column_int64(stmt_, column);
    }
    std::string text(int column) const {
        const auto *value = sqlite3_column_text(stmt_, column);
        return value ? std::string(reinterpret_cast<const char *>(value), sqlite3_column_bytes(stmt_, column))
                     : std::string();
    }
    Bytes bytes(int column) const {
        const auto *value = static_cast<const unsigned char *>(sqlite3_column_blob(stmt_, column));
        int length = sqlite3_column_bytes(stmt_, column);
        return length ? Bytes(value, value + length) : Bytes();
    }
};
class Transaction {
    sqlite3 *db_;
    bool active_ = true;
    const std::function<void(std::string_view)> &hook_;

  public:
    Transaction(sqlite3 *db, const std::function<void(std::string_view)> &hook) : db_(db), hook_(hook) {
        execute(db_, "BEGIN IMMEDIATE");
    }
    ~Transaction() {
        if (active_)
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    void commit() {
        if (hook_)
            hook_("before_commit");
        execute(db_, "COMMIT");
        active_ = false;
        if (hook_)
            hook_("after_commit");
    }
};
} // namespace localfs
