// Thin read-only wrapper over DuckDB's C API for querying LeRobot's parquet
// files. One ParquetDB == one in-memory DuckDB connection; query() runs SQL
// (typically `SELECT ... FROM '<abs path>.parquet' WHERE ...`) and hands back
// the result as columns of doubles / strings. List columns are not returned
// directly — flatten them in SQL (`col[1] AS c0, col[2] AS c1, …`).
#pragma once

#include <string>
#include <vector>

namespace mp {

class ParquetDB {
public:
    ParquetDB();
    ~ParquetDB();
    ParquetDB(const ParquetDB&) = delete;
    ParquetDB& operator=(const ParquetDB&) = delete;

    bool ok() const { return conn_ != nullptr; }

    struct Column {
        std::string name;
        std::vector<double> num;                  // per row; NaN when null / not numeric
        std::vector<std::string> str;             // per row; "" when null / not a string
        std::vector<std::vector<uint8_t>> blob;   // per row; for BLOB columns
    };
    struct Table {
        size_t rows = 0;
        std::vector<Column> cols;
        const Column* col(const std::string& name) const;
    };

    // Runs `sql`; fills `out` and returns true on success. Logs to stderr and
    // returns false on error.
    bool query(const std::string& sql, Table& out);

private:
    void* db_ = nullptr;   // duckdb_database
    void* conn_ = nullptr; // duckdb_connection
};

// Escape a filesystem path for use inside a SQL string literal
// (`FROM '<sql_path(p)>'`). Doubles single quotes; leaves slashes as-is
// (DuckDB accepts both separators on Windows).
std::string sql_path(const std::string& path);

} // namespace mp
