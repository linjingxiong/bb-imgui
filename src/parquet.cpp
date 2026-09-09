#include "parquet.h"

#include <cstdio>
#include <limits>

#include <duckdb.h>

namespace mp {

ParquetDB::ParquetDB() {
    duckdb_database db = nullptr;
    duckdb_connection conn = nullptr;
    if (duckdb_open(nullptr, &db) != DuckDBSuccess) {
        std::fprintf(stderr, "parquet: duckdb_open failed\n");
        return;
    }
    if (duckdb_connect(db, &conn) != DuckDBSuccess) {
        std::fprintf(stderr, "parquet: duckdb_connect failed\n");
        duckdb_close(&db);
        return;
    }
    db_ = db;
    conn_ = conn;
}

ParquetDB::~ParquetDB() {
    if (conn_) {
        auto c = (duckdb_connection)conn_;
        duckdb_disconnect(&c);
    }
    if (db_) {
        auto d = (duckdb_database)db_;
        duckdb_close(&d);
    }
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
        std::fprintf(stderr, "parquet: query failed: %s\n  sql: %s\n",
                     duckdb_result_error(&res), sql.c_str());
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
        const bool is_str = ty == DUCKDB_TYPE_VARCHAR || ty == DUCKDB_TYPE_ENUM ||
                            ty == DUCKDB_TYPE_BLOB;
        if (is_str)
            col.str.resize(nrow);
        else
            col.num.assign(nrow, 0.0);

        for (idx_t r = 0; r < nrow; ++r) {
            if (duckdb_value_is_null(&res, c, r)) {
                if (!is_str) col.num[r] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            if (is_str) {
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
