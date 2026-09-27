/**
 *  @file       sql.cpp
 *  @brief      SQLite 文档载荷：替换 meta.jsonl，与 USearch 图分离事务边界。
 */

#include "api.hpp"

#include <cstdio>
#include <cstring>

#include <sqlite3.h>

namespace api {

Base::Base(Base&& other) noexcept : db(other.db) { other.db = nullptr; }

Base& Base::operator=(Base&& other) noexcept {
    if (this == &other)
        return *this;
    close();
    db = other.db;
    other.db = nullptr;
    return *this;
}

Base::~Base() { close(); }

void Base::close() {
    if (db) {
        sqlite3_close(static_cast<sqlite3*>(db));
        db = nullptr;
    }
}

static error_t execSql(sqlite3* db, char const* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        if (err)
            sqlite3_free(err);
        return "sqlite exec failed";
    }
    return {};
}

expected_gt<Base> Base::open(fs::path const& path) {
    expected_gt<Base> out;
    try {
        if (path.has_parent_path())
            fs::create_directories(path.parent_path());
    } catch (...) {
        return out.failed("cannot create sqlite parent");
    }
    Base b;
    sqlite3* raw = nullptr;
    int rc = sqlite3_open(path.string().c_str(), &raw);
    if (rc != SQLITE_OK) {
        if (raw)
            sqlite3_close(raw);
        return out.failed("sqlite open failed");
    }
    b.db = raw;
    if (error_t e = execSql(raw, "PRAGMA journal_mode=WAL;"); e)
        return out.failed(e.release());
    char const* ddl =
        "CREATE TABLE IF NOT EXISTS docs ("
        "  id TEXT PRIMARY KEY NOT NULL,"
        "  text TEXT NOT NULL DEFAULT '',"
        "  meta TEXT NOT NULL DEFAULT '{}',"
        "  key INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS notes ("
        "  id TEXT PRIMARY KEY NOT NULL,"
        "  path TEXT NOT NULL"
        ");";
    if (error_t e = execSql(raw, ddl); e)
        return out.failed(e.release());
    out.result = std::move(b);
    return out;
}

error_t Base::begin() {
    if (!db)
        return "sqlite closed";
    return execSql(static_cast<sqlite3*>(db), "BEGIN IMMEDIATE;");
}

error_t Base::commit() {
    if (!db)
        return "sqlite closed";
    return execSql(static_cast<sqlite3*>(db), "COMMIT;");
}

error_t Base::rollback() {
    if (!db)
        return "sqlite closed";
    return execSql(static_cast<sqlite3*>(db), "ROLLBACK;");
}

expected_gt<Doc> Base::get(std::string const& id) {
    expected_gt<Doc> out;
    if (!db)
        return out.failed("sqlite closed");
    sqlite3* raw = static_cast<sqlite3*>(db);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(raw, "SELECT id, text, meta FROM docs WHERE id=?1;", -1, &stmt, nullptr) !=
        SQLITE_OK)
        return out.failed("sqlite prepare failed");
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return out.failed("doc not found");
    }
    Doc doc;
    doc.id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
    doc.text = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
    char const* meta = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 2));
    try {
        doc.meta = meta ? json::parse(meta) : json::object();
    } catch (...) {
        doc.meta = json::object();
    }
    sqlite3_finalize(stmt);
    out.result = std::move(doc);
    return out;
}

error_t Base::put(Doc const& doc, std::uint64_t key) {
    if (!db)
        return "sqlite closed";
    sqlite3* raw = static_cast<sqlite3*>(db);
    sqlite3_stmt* stmt = nullptr;
    char const* sql =
        "INSERT INTO docs(id, text, meta, key) VALUES(?1,?2,?3,?4) "
        "ON CONFLICT(id) DO UPDATE SET text=excluded.text, meta=excluded.meta, key=excluded.key;";
    if (sqlite3_prepare_v2(raw, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return "sqlite prepare failed";
    std::string meta = doc.meta.dump();
    sqlite3_bind_text(stmt, 1, doc.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, doc.text.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, meta.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, static_cast<sqlite3_int64>(key));
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return "sqlite put failed";
    return {};
}

error_t Base::del(std::string const& id) {
    if (!db)
        return "sqlite closed";
    sqlite3* raw = static_cast<sqlite3*>(db);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(raw, "DELETE FROM docs WHERE id=?1;", -1, &stmt, nullptr) != SQLITE_OK)
        return "sqlite prepare failed";
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return "sqlite del failed";
    return {};
}

expected_gt<std::vector<Doc>> Base::list() {
    expected_gt<std::vector<Doc>> out;
    if (!db)
        return out.failed("sqlite closed");
    sqlite3* raw = static_cast<sqlite3*>(db);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(raw, "SELECT id, text, meta FROM docs;", -1, &stmt, nullptr) != SQLITE_OK)
        return out.failed("sqlite prepare failed");
    std::vector<Doc> rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Doc doc;
        doc.id = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        doc.text = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
        char const* meta = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 2));
        try {
            doc.meta = meta ? json::parse(meta) : json::object();
        } catch (...) {
            doc.meta = json::object();
        }
        rows.push_back(std::move(doc));
    }
    sqlite3_finalize(stmt);
    out.result = std::move(rows);
    return out;
}

} // namespace api
