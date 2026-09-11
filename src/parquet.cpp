#include "parquet.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>

#include <duckdb.h>

namespace mp {

// One in-memory DuckDB instance for the whole process; every ParquetDB is just
// a connection to it. A fresh instance per ParquetDB would each spin up its own
// worker pool — dozens of threads and a lot of idle CPU for a few small reads.
namespace {
std::mutex g_db_mx;
duckdb_database g_db = nullptr;
int g_conns = 0;
} // namespace

ParquetDB::ParquetDB() {
    std::lock_guard<std::mutex> lk(g_db_mx);
    if (!g_db) {
        if (duckdb_open(nullptr, &g_db) != DuckDBSuccess) {
            std::fprintf(stderr, "parquet: duckdb_open failed\n");
            g_db = nullptr;
            return;
        }
        duckdb_connection c = nullptr;
        if (duckdb_connect(g_db, &c) == DuckDBSuccess) {
            // Cap the worker pool; cache parquet footers so switching between a
            // dataset's data files doesn't re-parse a 450MB file's metadata.
            for (const char* s : {"SET threads TO 4", "SET parquet_metadata_cache = true"}) {
                duckdb_result r;
                duckdb_query(c, s, &r);
                duckdb_destroy_result(&r);
            }
            duckdb_disconnect(&c);
        }
    }
    if (!g_db) return;
    duckdb_connection conn = nullptr;
    if (duckdb_connect(g_db, &conn) != DuckDBSuccess) {
        std::fprintf(stderr, "parquet: duckdb_connect failed\n");
        return;
    }
    conn_ = conn;
    ++g_conns;
}

ParquetDB::~ParquetDB() {
    if (!conn_) return;
    auto c = (duckdb_connection)conn_;
    duckdb_disconnect(&c);
    std::lock_guard<std::mutex> lk(g_db_mx);
    if (--g_conns == 0 && g_db) {
        duckdb_close(&g_db);
        g_db = nullptr;
    }
}

void ParquetDB::interrupt() {
    if (conn_) duckdb_interrupt((duckdb_connection)conn_);
}

const ParquetDB::Column* ParquetDB::Table::col(const std::string& name) const {
    for (const auto& c : cols)
        if (c.name == name) return &c;
    return nullptr;
}

bool ParquetDB::query(const std::string& sql, Table& out) {
    out = Table{};
    if (!conn_) return false;

    duckdb_result res;
    if (duckdb_query((duckdb_connection)conn_, sql.c_str(), &res) != DuckDBSuccess) {
        const char* err = duckdb_result_error(&res);
        // A deliberate interrupt() from another thread isn't an error worth logging.
        if (!err || !std::strstr(err, "INTERRUPT"))
            std::fprintf(stderr, "parquet: query failed: %s\n  sql: %s\n", err, sql.c_str());
        duckdb_destroy_result(&res);
        return false;
    }

    const idx_t ncol = duckdb_column_count(&res);
    const idx_t nrow = duckdb_row_count(&res);
    out.rows = (size_t)nrow;
    out.cols.resize(ncol);

    for (idx_t c = 0; c < ncol; ++c) {
        Column& col = out.cols[c];
        col.name = duckdb_column_name(&res, c);
        const duckdb_type ty = duckdb_column_type(&res, c);
        const bool is_blob = ty == DUCKDB_TYPE_BLOB;
        const bool is_str = ty == DUCKDB_TYPE_VARCHAR || ty == DUCKDB_TYPE_ENUM;
        if (is_blob)
            col.blob.resize(nrow);
        else if (is_str)
            col.str.resize(nrow);
        else
            col.num.assign(nrow, 0.0);

        for (idx_t r = 0; r < nrow; ++r) {
            if (duckdb_value_is_null(&res, c, r)) {
                if (!is_str && !is_blob) col.num[r] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            if (is_blob) {
                duckdb_blob b = duckdb_value_blob(&res, c, r);
                if (b.data && b.size) {
                    col.blob[r].assign((const uint8_t*)b.data, (const uint8_t*)b.data + b.size);
                    duckdb_free(b.data);
                }
            } else if (is_str) {
                char* s = duckdb_value_varchar(&res, c, r);
                if (s) {
                    col.str[r] = s;
                    duckdb_free(s);
                }
            } else {
                col.num[r] = duckdb_value_double(&res, c, r);
            }
        }
    }

    duckdb_destroy_result(&res);
    return true;
}

std::string sql_path(const std::string& path) {
    std::string out;
    out.reserve(path.size() + 8);
    for (char ch : path) {
        if (ch == '\'') out += "''";
        else out += ch;
    }
    return out;
}

} // namespace mp
