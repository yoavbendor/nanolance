// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

// nlance2table — dump a nanolance-written Lance dataset to CSV or NDJSON (full table or first N rows).
//
// Deliberately scoped to what *nanolance* writes (not arbitrary Lance encodings/compressions), reusing
// the same reader the parity test uses: nano_lance::lance_table_read_dataset -> Arrow batches, then a
// generic ArrowArrayView walk emits text. fixed_size_binary/binary render as lowercase hex (the tool is
// semantic-free: it does not know a column is a MAC or an IP). Nested struct columns flatten to dotted
// names in CSV and nested objects in NDJSON. Nulls are an empty CSV field / JSON null.

#include "nanolance/lance_table_reader.hpp"
#include "nanolance/version.hpp"

#include <CLI/CLI.hpp>
#include <nanoarrow/nanoarrow.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

enum class Format { Csv, Ndjson };

// ---- value formatting (shared by CSV and NDJSON) -------------------------------------------------

std::string to_hex(ArrowBufferView b) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(static_cast<std::size_t>(b.size_bytes) * 2);
    for (int64_t i = 0; i < b.size_bytes; ++i) {
        const auto byte = static_cast<unsigned char>(b.data.as_char[i]);
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 0x0F]);
    }
    return out;
}

/// Render a decimal cell. The value's precision and scale live only in the SCHEMA -- an
/// ArrowArrayView carries neither -- so this is the one cell type that needs it. Without it the
/// switch below fell through to its default and printed an empty cell, which for a decimal column
/// meant a CSV of blanks with no error: exactly the silent loss this tool exists to avoid.
std::string decimal_text(const ArrowArrayView* col, int64_t row, const ArrowSchema* schema) {
    ArrowSchemaView view;
    if (schema == nullptr || ArrowSchemaViewInit(&view, schema, nullptr) != NANOARROW_OK) {
        return {};
    }
    ArrowDecimal decimal;
    ArrowDecimalInit(&decimal, view.decimal_bitwidth, view.decimal_precision, view.decimal_scale);
    ArrowArrayViewGetDecimalUnsafe(col, row, &decimal);
    ArrowBuffer buffer;
    ArrowBufferInit(&buffer);
    std::string out;
    if (ArrowDecimalAppendStringToBuffer(&decimal, &buffer) == NANOARROW_OK) {
        out.assign(reinterpret_cast<const char*>(buffer.data), static_cast<std::size_t>(buffer.size_bytes));
    }
    ArrowBufferReset(&buffer);
    return out;
}

// Render one scalar cell to its raw text (no CSV/JSON quoting applied here).
//
// `schema` may be null; only decimal cells need it, and every caller that can supply it does.
// Temporal columns (timestamp / date / time) print as their raw integer in the column's own unit:
// that is lossless, which matters more here than being pretty, and the unit is in the dataset's
// schema (`nlance-info`).
std::string scalar_text(const ArrowArrayView* col, int64_t row, const ArrowSchema* schema = nullptr) {
    switch (col->storage_type) {
        case NANOARROW_TYPE_BOOL:
            return ArrowArrayViewGetIntUnsafe(col, row) ? "true" : "false";
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
            return std::to_string(ArrowArrayViewGetIntUnsafe(col, row));
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
            return std::to_string(ArrowArrayViewGetUIntUnsafe(col, row));
        case NANOARROW_TYPE_HALF_FLOAT:
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%g", ArrowArrayViewGetDoubleUnsafe(col, row));
            return buf;
        }
        case NANOARROW_TYPE_STRING:
        case NANOARROW_TYPE_LARGE_STRING: {
            ArrowStringView s = ArrowArrayViewGetStringUnsafe(col, row);
            return std::string(s.data, static_cast<std::size_t>(s.size_bytes));
        }
        case NANOARROW_TYPE_BINARY:
        case NANOARROW_TYPE_LARGE_BINARY:
        case NANOARROW_TYPE_FIXED_SIZE_BINARY:
            return to_hex(ArrowArrayViewGetBytesUnsafe(col, row));
        case NANOARROW_TYPE_DECIMAL128:
        case NANOARROW_TYPE_DECIMAL256:
            return decimal_text(col, row, schema);
        default:
            return "";  // unsupported storage type -> empty
    }
}

bool is_struct(const ArrowArrayView* col) { return col->storage_type == NANOARROW_TYPE_STRUCT; }

// ---- CSV ------------------------------------------------------------------------------------------

void csv_escape(std::string& out, const std::string& field) {
    const bool needs_quote = field.find_first_of(",\"\n\r") != std::string::npos;
    if (!needs_quote) {
        out += field;
        return;
    }
    out.push_back('"');
    for (char c : field) {
        if (c == '"') {
            out.push_back('"');  // RFC 4180: double the quote
        }
        out.push_back(c);
    }
    out.push_back('"');
}

// Flatten a (possibly nested-struct) schema into dotted leaf column names.
void csv_header_names(const ArrowSchema* schema, const std::string& prefix, std::vector<std::string>& out) {
    if (schema->n_children > 0 && std::string(schema->format) == "+s") {  // struct
        for (int64_t i = 0; i < schema->n_children; ++i) {
            const ArrowSchema* child = schema->children[i];
            const std::string name = prefix.empty() ? child->name : prefix + "." + child->name;
            csv_header_names(child, name, out);
        }
    } else {
        out.push_back(prefix);
    }
}

// Append one row's leaf cells (depth-first, matching csv_header_names order).
void csv_row_cells(const ArrowArrayView* col, int64_t row, const ArrowSchema* schema,
                   std::vector<std::string>& cells) {
    if (is_struct(col)) {
        const bool null = ArrowArrayViewIsNull(col, row);
        for (int64_t i = 0; i < col->n_children; ++i) {
            if (null) {
                cells.emplace_back();  // whole struct null -> empty leaves
            } else {
                csv_row_cells(col->children[i], row, schema->children[i], cells);
            }
        }
    } else {
        cells.push_back(ArrowArrayViewIsNull(col, row) ? std::string() : scalar_text(col, row, schema));
    }
}

// ---- NDJSON ---------------------------------------------------------------------------------------

void json_escape_string(std::string& out, const std::string& s) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// Numeric storage types render unquoted in JSON; everything else (string/binary/bool-as-text) is quoted.
bool json_is_numeric(ArrowType t) {
    switch (t) {
        case NANOARROW_TYPE_INT8:
        case NANOARROW_TYPE_INT16:
        case NANOARROW_TYPE_INT32:
        case NANOARROW_TYPE_INT64:
        case NANOARROW_TYPE_UINT8:
        case NANOARROW_TYPE_UINT16:
        case NANOARROW_TYPE_UINT32:
        case NANOARROW_TYPE_UINT64:
        case NANOARROW_TYPE_HALF_FLOAT:
        case NANOARROW_TYPE_FLOAT:
        case NANOARROW_TYPE_DOUBLE:
            return true;
        default:
            return false;
    }
}

void json_value(const ArrowArrayView* col, int64_t row, const ArrowSchema* schema, std::string& out) {
    if (ArrowArrayViewIsNull(col, row)) {
        out += "null";
        return;
    }
    if (is_struct(col)) {
        out.push_back('{');
        for (int64_t i = 0; i < col->n_children; ++i) {
            if (i) {
                out.push_back(',');
            }
            json_escape_string(out, schema->children[i]->name);
            out.push_back(':');
            json_value(col->children[i], row, schema->children[i], out);
        }
        out.push_back('}');
        return;
    }
    const std::string text = scalar_text(col, row, schema);
    if (col->storage_type == NANOARROW_TYPE_BOOL) {
        out += text;  // true/false literal
    } else if (json_is_numeric(col->storage_type)) {
        out += text;
    } else {
        json_escape_string(out, text);
    }
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Dump a nanolance-written Lance dataset to CSV or NDJSON"};
    std::string dataset_path;
    std::string output_path;
    std::string format_str = "csv";
    int64_t limit = -1;  // -1 = all rows
    app.add_option("dataset", dataset_path, "Input .lance dataset directory")->required();
    app.add_option("-f,--format", format_str, "Output format: csv | ndjson")->default_val("csv");
    app.add_option("-n,--limit", limit,
                   "Print only the first N rows (print cap; the whole dataset is still read)");
    app.add_option("-o,--output", output_path, "Write to FILE instead of stdout");
    app.set_version_flag("--version", std::string(nanolance::library_version()));
    CLI11_PARSE(app, argc, argv);

    Format format;
    if (format_str == "csv") {
        format = Format::Csv;
    } else if (format_str == "ndjson" || format_str == "jsonl") {
        format = Format::Ndjson;
    } else {
        std::fprintf(stderr, "nlance2table: unknown --format '%s' (expected csv | ndjson)\n", format_str.c_str());
        return 2;
    }

    ArrowSchema schema{};
    std::vector<ArrowArray> batches;
    std::string err;
    if (!nano_lance::lance_table_read_dataset(dataset_path, schema, batches, err)) {
        std::fprintf(stderr, "nlance2table: read failed: %s\n", err.c_str());
        return 1;
    }

    std::ofstream file;
    std::ostream* os = &std::cout;
    if (!output_path.empty()) {
        file.open(output_path, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "nlance2table: cannot open output '%s'\n", output_path.c_str());
            return 1;
        }
        os = &file;
    }

    // CSV header (once), from the flattened schema.
    std::vector<std::string> header;
    if (format == Format::Csv) {
        csv_header_names(&schema, "", header);
        std::string line;
        for (std::size_t i = 0; i < header.size(); ++i) {
            if (i) {
                line.push_back(',');
            }
            csv_escape(line, header[i]);
        }
        line.push_back('\n');
        *os << line;
    }

    int64_t printed = 0;
    int rc = 0;
    for (auto& batch : batches) {
        if (limit >= 0 && printed >= limit) {
            break;
        }
        ArrowArrayView view{};
        ArrowError ae{};
        if (ArrowArrayViewInitFromSchema(&view, &schema, &ae) != NANOARROW_OK ||
            ArrowArrayViewSetArray(&view, &batch, &ae) != NANOARROW_OK) {
            std::fprintf(stderr, "nlance2table: array view init failed: %s\n", ArrowErrorMessage(&ae));
            ArrowArrayViewReset(&view);
            rc = 1;
            break;
        }
        const int64_t n = view.length;
        for (int64_t row = 0; row < n; ++row) {
            if (limit >= 0 && printed >= limit) {
                break;
            }
            std::string line;
            if (format == Format::Csv) {
                std::vector<std::string> cells;
                cells.reserve(header.size());
                for (int64_t c = 0; c < view.n_children; ++c) {
                    csv_row_cells(view.children[c], row, schema.children[c], cells);
                }
                for (std::size_t i = 0; i < cells.size(); ++i) {
                    if (i) {
                        line.push_back(',');
                    }
                    csv_escape(line, cells[i]);
                }
            } else {
                line.push_back('{');
                for (int64_t c = 0; c < view.n_children; ++c) {
                    if (c) {
                        line.push_back(',');
                    }
                    json_escape_string(line, schema.children[c]->name);
                    line.push_back(':');
                    json_value(view.children[c], row, schema.children[c], line);
                }
                line.push_back('}');
            }
            line.push_back('\n');
            *os << line;
            ++printed;
        }
        ArrowArrayViewReset(&view);
    }

    if (schema.release) {
        schema.release(&schema);
    }
    for (auto& b : batches) {
        if (b.release) {
            b.release(&b);
        }
    }
    return rc;
}
