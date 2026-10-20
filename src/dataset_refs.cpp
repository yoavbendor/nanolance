// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/work_stats.hpp"
#include <nanolance/dataset_refs.hpp>
#include "nanolance/byte_source.hpp"

#include <nanolance/base_paths.hpp>
#include <nanolance/manifest_reader.hpp>
#include <nanolance/manifest_writer.hpp>

#include "fts_json.hpp"
#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace nano_lance {
namespace {

namespace fs = std::filesystem;
namespace json = fts::json;

// ── small helpers ──────────────────────────────────────────────────────────────────────────────────

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// `ns` since the epoch as RFC 3339 in UTC, with nanoseconds ("2026-10-08T16:00:17.502516072Z").
std::string rfc3339(std::int64_t ns) {
    using namespace std::chrono;
    const sys_time<nanoseconds> t{nanoseconds{ns}};
    const auto day = floor<days>(t);
    const year_month_day ymd{day};
    const hh_mm_ss<nanoseconds> tod{t - day};
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02d.%09lldZ", static_cast<int>(ymd.year()),
                  static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
                  static_cast<int>(tod.hours().count()), static_cast<int>(tod.minutes().count()),
                  static_cast<int>(tod.seconds().count()), static_cast<long long>(tod.subseconds().count()));
    return buf;
}

void json_string(std::ostringstream& out, const std::string& s) {
    out << '"';
    for (const char c : s) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out << esc;
                } else {
                    out << c;
                }
        }
    }
    out << '"';
}

bool read_text(const fs::path& path, std::string& out) {
    std::vector<std::uint8_t> bytes;
    std::string io_error;
    if (!io::read_all(path.string(), bytes, io_error)) {
        return false;
    }
    out.assign(bytes.begin(), bytes.end());
    work_stats::count_read(out.size());
    return true;
}

/// Write `bytes` to `path` through a temp file: replacing whatever is there (`replace`), or failing
/// when it exists (`!replace`, Lance's put_if_absent).
bool write_file(const fs::path& path, const std::string& bytes, bool replace, bool& existed, std::string& error) {
    existed = false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    static std::atomic<std::uint64_t> counter{0};
    const auto temp = path.parent_path() / ("." + path.filename().string() + "." +
                                            std::to_string(std::random_device{}()) + "-" +
                                            std::to_string(counter.fetch_add(1U)) + ".tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        out.close();
        work_stats::count_written_file(temp);
        if (!out) {
            fs::remove(temp, ec);
            error = "failed to write " + path.string();
            return false;
        }
    }
    if (replace) {
        fs::rename(temp, path, ec);
        if (ec) {
            fs::remove(temp, ec);
            error = "failed to write " + path.string() + ": " + ec.message();
            return false;
        }
        return true;
    }
    fs::create_hard_link(temp, path, ec);
    std::error_code ignored;
    if (!ec) {
        fs::remove(temp, ignored);
        return true;
    }
    if (fs::exists(path, ignored)) {
        fs::remove(temp, ignored);
        existed = true;
        return false;
    }
    fs::rename(temp, path, ec);  // no hard links here
    if (ec) {
        fs::remove(temp, ignored);
        error = "failed to write " + path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

/// The `_versions` file of `version`, and its size; false when there is none.
bool manifest_file_of(const fs::path& dataset_path, std::uint64_t version, fs::path& path, std::uint64_t& size) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dataset_path / "_versions", ec)) {
        std::uint64_t v = 0;
        if (entry.is_regular_file(ec) && parse_manifest_version(entry.path().filename().string(), v) && v == version) {
            path = entry.path();
            size = static_cast<std::uint64_t>(entry.file_size(ec));
            return !ec;
        }
    }
    return false;
}

// ── tags ───────────────────────────────────────────────────────────────────────────────────────────

fs::path tags_dir(const fs::path& dataset_path) { return dataset_path / "_refs" / "tags"; }

/// Lance's check_valid_tag.
bool check_valid_tag(const std::string& s, std::string& error) {
    const auto fail = [&](const char* why) {
        error = std::string("Ref is invalid: ") + why;
        return false;
    };
    if (s.empty()) {
        return fail("Ref cannot be empty");
    }
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) != 0 || u >= 0x80 || c == '.' || c == '-' || c == '_')) {
            return fail("Ref characters must be either alphanumeric, '.', '-' or '_'");
        }
    }
    if (s.front() == '.') {
        return fail("Ref cannot begin with a dot");
    }
    if (s.back() == '.') {
        return fail("Ref cannot end with a dot");
    }
    if (s.size() >= 5 && s.compare(s.size() - 5, 5, ".lock") == 0) {
        return fail("Ref cannot end with .lock");
    }
    if (s.find("..") != std::string::npos) {
        return fail("Ref cannot have two consecutive dots");
    }
    return true;
}

/// The tag file as Lance writes it (serde, camelCase, pretty-printed).
std::string tag_json(const TagInfo& tag) {
    std::ostringstream out;
    out << "{\n  \"branch\": ";
    if (tag.branch) {
        json_string(out, *tag.branch);
    } else {
        out << "null";
    }
    out << ",\n  \"version\": " << tag.version;
    if (!tag.created_at.empty()) {
        out << ",\n  \"createdAt\": ";
        json_string(out, tag.created_at);
    }
    if (!tag.updated_at.empty()) {
        out << ",\n  \"updatedAt\": ";
        json_string(out, tag.updated_at);
    }
    out << ",\n  \"manifestSize\": " << tag.manifest_size << ",\n  \"metadata\": {";
    bool first = true;
    for (const auto& [k, v] : tag.metadata) {
        out << (first ? "\n    " : ",\n    ");
        json_string(out, k);
        out << ": ";
        json_string(out, v);
        first = false;
    }
    out << (tag.metadata.empty() ? "}" : "\n  }") << "\n}";
    return out.str();
}

bool read_tag(const fs::path& dataset_path, const std::string& name, TagInfo& out, std::string& error) {
    std::string text;
    if (!read_text(tags_dir(dataset_path) / (name + ".json"), text)) {
        error = "Ref not found error: tag " + name + " does not exist";
        return false;
    }
    json::Value root;
    json::Reader reader(text);
    const auto* version = reader.parse(root) && root.kind == json::Value::Object ? root.get("version") : nullptr;
    if (version == nullptr || version->kind != json::Value::Number) {
        error = "tag " + name + " is not a valid tag file";
        return false;
    }
    out = TagInfo{};
    out.name = name;
    out.version = static_cast<std::uint64_t>(version->n);
    if (const auto* b = root.get("branch"); b != nullptr && b->kind == json::Value::String) {
        out.branch = b->s;
    }
    if (const auto* c = root.get("createdAt"); c != nullptr && c->kind == json::Value::String) {
        out.created_at = c->s;
    }
    if (const auto* u = root.get("updatedAt"); u != nullptr && u->kind == json::Value::String) {
        out.updated_at = u->s;
    }
    if (const auto* m = root.get("manifestSize"); m != nullptr && m->kind == json::Value::Number) {
        out.manifest_size = static_cast<std::uint64_t>(m->n);
    }
    if (const auto* md = root.get("metadata"); md != nullptr && md->kind == json::Value::Object) {
        for (const auto& [k, v] : md->fields) {
            if (v.kind == json::Value::String) {
                out.metadata[k] = v.s;
            }
        }
    }
    return true;
}

/// `version` (0: the latest) and its manifest's size, or Lance's "version not found".
bool resolve_tag_target(const fs::path& location, const std::string& branch, std::uint64_t version,
                        std::uint64_t& resolved, std::uint64_t& manifest_size, std::string& error) {
    resolved = version;
    if (resolved == 0) {
        std::string ignored;
        resolved = highest_manifest_version(location, ignored);
    }
    fs::path path;
    if (resolved == 0 || !manifest_file_of(location, resolved, path, manifest_size)) {
        error = "Version not found error: version " + (branch.empty() ? std::string("main") : branch) + ":" +
                (version == 0 ? std::string("latest") : std::to_string(version)) + " does not exist";
        return false;
    }
    return true;
}

// ── cleanup ────────────────────────────────────────────────────────────────────────────────────────

struct ReferencedFiles {
    std::unordered_set<std::string> data_paths;    // "data/<file>"
    std::unordered_set<std::string> delete_paths;  // "_deletions/<file>"
    std::unordered_set<std::string> tx_paths;      // "_transactions/<file>"
    std::unordered_set<std::string> index_uuids;
};

/// A fragment's deletion file, relative to its dataset (`_deletions/<fragment>-<read version>-<id>`).
std::string deletion_file_name(const pb::DataFragment& fragment) {
    return "_deletions/" + std::to_string(fragment.id) + "-" + std::to_string(fragment.deletion_file.read_version) +
           "-" + std::to_string(fragment.deletion_file.id) + (fragment.deletion_file.file_type == 1U ? ".bin" : ".arrow");
}

void reference(const pb::Manifest& manifest, ReferencedFiles& files) {
    for (const auto& fragment : manifest.fragments) {
        for (const auto& file : fragment.files) {
            files.data_paths.insert("data/" + file.path);
        }
        if (fragment.deletion_file.present) {
            files.delete_paths.insert(deletion_file_name(fragment));
        }
    }
    if (!manifest.transaction_file.empty()) {
        files.tx_paths.insert("_transactions/" + manifest.transaction_file);
    }
    for (const auto& index : manifest.indices) {
        files.index_uuids.insert(pb::uuid_string(index.uuid));
    }
}

bool starts_with(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

constexpr std::int64_t kUnverifiedThresholdNs = 7LL * 24 * 3600 * 1000000000LL;

std::int64_t manifest_timestamp_ns(const pb::Manifest& manifest) {
    return manifest.has_timestamp ? manifest.timestamp_seconds * 1000000000LL + manifest.timestamp_nanos : 0;
}

// ── branches, as cleanup sees them ─────────────────────────────────────────────────────────────────

/// The root a dataset directory sits in, and the branch the directory holds ("" for main). A branch
/// lives at `<root>/tree/<branch>`, as pylance lays it out; its tags, branch references and the
/// branches it forks from are the root's.
struct BranchScope {
    fs::path root;
    std::string branch;
};

std::string canonical_text(const fs::path& path) {
    std::error_code ec;
    const auto canonical = fs::weakly_canonical(path, ec);
    return (ec ? path.lexically_normal() : canonical).generic_string();
}

bool branch_scope_of(const fs::path& dataset_path, const std::string& branch, BranchScope& out, std::string& error) {
    out.branch = branch;
    out.root = dataset_path;
    if (branch.empty()) {
        return true;
    }
    std::string text = dataset_path.lexically_normal().generic_string();
    while (text.size() > 1 && text.back() == '/') {
        text.pop_back();
    }
    const std::string suffix = "/tree/" + branch;
    if (text.size() <= suffix.size() || text.compare(text.size() - suffix.size(), suffix.size(), suffix) != 0) {
        error = "branch " + branch + " is not at " + dataset_path.string() + ", where a branch lives: <root>/tree/" +
                branch;
        return false;
    }
    out.root = text.substr(0, text.size() - suffix.size());
    return true;
}

/// A branch forked from this lineage: its name, its directory, and the version of this lineage it
/// forks from (Lance's collect_referenced_versions: a branch whose identifier extends this one's).
struct ForkedBranch {
    std::string name;
    fs::path dir;
    std::uint64_t version = 0;
};

bool forked_branches(const BranchScope& scope, std::vector<ForkedBranch>& out, std::string& error) {
    std::vector<BranchInfo> branches;
    if (!list_branches(scope.root, branches, error)) {
        return false;
    }
    std::vector<std::pair<std::uint64_t, std::string>> own;
    if (!scope.branch.empty()) {
        const auto self = std::find_if(branches.begin(), branches.end(),
                                       [&](const BranchInfo& b) { return b.name == scope.branch; });
        if (self == branches.end()) {
            error = "branch " + scope.branch + " has no reference file under " + (scope.root / "_refs").string();
            return false;
        }
        own = self->identifier;
    }
    for (const auto& b : branches) {
        if (b.name == scope.branch || b.identifier.size() <= own.size() ||
            !std::equal(own.begin(), own.end(), b.identifier.begin())) {
            continue;
        }
        const auto version = b.identifier[own.size()].first;
        if (version != 0) {
            out.push_back({b.name, scope.root / "tree" / b.name, version});
        }
    }
    return true;
}

/// Whether `base_id` names a base path that is the dataset root at `dir_text` (canonical).
bool names_dataset_dir(const std::vector<BasePath>& bases, const std::optional<std::uint32_t>& base_id,
                       const std::string& dir_text) {
    if (!base_id) {
        return false;  // absent: the branch's own directory, not this one
    }
    for (const auto& base : bases) {
        if (base.id == *base_id) {
            // pylance records the root as it was opened: a relative path cannot be resolved from here
            // (it was relative to that process's directory), so it is taken to name this one -- keeping
            // files another directory's branch uses costs space; removing this one's loses data.
            if (base.is_dataset_root && !fs::path(base.path).is_absolute()) {
                return true;
            }
            return base.is_dataset_root && canonical_text(base.path) == dir_text;
        }
    }
    return false;
}

/// What a fork's branch still uses from this dataset directory (Lance's retain_branch_lineage_files):
/// the fork version itself is kept, and the files, deletions and indices it names here.
struct ForkProtection {
    std::set<std::uint64_t> versions;
    ReferencedFiles files;
};

/// Scan every manifest of the branch `fork` and record what names files in `dataset_path`. A manifest
/// that names one protects the version the branch forks from.
bool protect_fork(const fs::path& dataset_path, const ForkedBranch& fork, ForkProtection& out, std::string& error) {
    const std::string own = canonical_text(dataset_path);
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(fork.dir / "_versions", ec)) {
        const auto name = entry.path().filename().string();
        std::uint64_t version = 0;
        const bool attached = parse_manifest_version(name, version);
        const bool detached = !attached && starts_with(name, "d") && ends_with(name, ".manifest");
        if (!entry.is_regular_file(ec) || (!attached && !detached)) {
            continue;
        }
        pb::Manifest manifest;
        if (!load_manifest_file(entry.path(), manifest, error)) {
            error = "cannot clean up: a branch " + fork.name + " manifest could not be read: " + error;
            return false;
        }
        std::vector<BasePath> bases;
        if (!read_base_paths(manifest, bases, error)) {
            return false;
        }
        bool names_here = false;
        for (const auto& fragment : manifest.fragments) {
            for (const auto& file : fragment.files) {
                std::optional<std::uint32_t> base;
                if (!data_file_base_id(file, base, error)) {
                    return false;
                }
                if (names_dataset_dir(bases, base, own)) {
                    out.files.data_paths.insert("data/" + file.path);
                    names_here = true;
                }
            }
            if (fragment.deletion_file.present) {
                std::optional<std::uint32_t> base;
                if (!deletion_file_base_id(fragment.deletion_file, base, error)) {
                    return false;
                }
                if (names_dataset_dir(bases, base, own)) {
                    out.files.delete_paths.insert(deletion_file_name(fragment));
                    names_here = true;
                }
            }
        }
        for (const auto& index : manifest.indices) {
            std::optional<std::uint32_t> base;
            if (!index_base_id(index, base, error)) {
                return false;
            }
            if (names_dataset_dir(bases, base, own)) {
                out.files.index_uuids.insert(pb::uuid_string(index.uuid));
                names_here = true;
            }
        }
        if (names_here) {
            out.versions.insert(fork.version);
        }
    }
    if (ec) {
        error = "failed to list " + (fork.dir / "_versions").string() + ": " + ec.message();
        return false;
    }
    return true;
}


struct File {
    std::string relative;
    std::uint64_t size = 0;
    bool maybe_in_progress = false;
};

/// What Lance's cleanup_file_if_not_referenced decides for one file: its kind, or empty to keep it.
std::string removable_kind(const File& file, const ReferencedFiles& referenced, const ReferencedFiles& verified,
                           bool& unverified) {
    const auto& p = file.relative;
    unverified = true;
    // `verdict(in_working_set, in_old_version)`: kept, removed by age, or removed as an old version's.
    const auto verdict = [&](bool kept, bool known_old, const char* kind) -> std::string {
        if (kept) {
            return {};
        }
        if (!file.maybe_in_progress) {
            return kind;
        }
        if (known_old) {
            unverified = false;
            return kind;
        }
        return {};
    };
    if (starts_with(p, "_versions/")) {
        // A temp manifest a failed commit left: Lance's `_versions/.tmp*`, or nanolance's `<name>.<id>.tmp`.
        const auto name = p.substr(10);
        if ((starts_with(name, ".tmp") || ends_with(name, ".tmp")) && !file.maybe_in_progress) {
            return "temporary_manifest";
        }
        return {};
    }
    if (starts_with(p, "_indices/")) {
        const auto slash = p.find('/', 9);
        if (slash == std::string::npos) {
            return {};
        }
        const auto uuid = p.substr(9, slash - 9);
        return verdict(referenced.index_uuids.count(uuid) != 0, verified.index_uuids.count(uuid) != 0, "index");
    }
    if (ends_with(p, ".lance")) {
        if (!starts_with(p, "data/")) {
            return {};
        }
        return verdict(referenced.data_paths.count(p) != 0, verified.data_paths.count(p) != 0, "data");
    }
    if (ends_with(p, ".blob")) {
        // Blob v2 sidecars, data/<data file stem>/<blob>.blob: kept with their data file.
        const auto first = p.find('/');
        const auto second = p.find('/', first + 1);
        if (!starts_with(p, "data/") || second == std::string::npos || p.find('/', second + 1) != std::string::npos) {
            return {};
        }
        const auto parent = "data/" + p.substr(first + 1, second - first - 1) + ".lance";
        return verdict(referenced.data_paths.count(parent) != 0, verified.data_paths.count(parent) != 0, "data");
    }
    if (ends_with(p, ".arrow") || ends_with(p, ".bin")) {
        if (!starts_with(p, "_deletions/")) {
            return {};
        }
        return verdict(referenced.delete_paths.count(p) != 0, verified.delete_paths.count(p) != 0, "deletion");
    }
    if (ends_with(p, ".txn")) {
        if (!starts_with(p, "_transactions/")) {
            return {};
        }
        const bool known = verified.tx_paths.count(p) != 0;
        if (referenced.tx_paths.count(p) != 0) {
            return {};
        }
        if (!file.maybe_in_progress || known) {
            unverified = !known;
            return "transaction";
        }
        return {};
    }
    return {};
}

void record(CleanupResult& out, const CleanupCandidate& file, std::size_t max_candidates) {
    auto& s = out.stats;
    s.bytes_removed += file.size_bytes;
    if (file.kind == "manifest") {
        ++s.old_versions;
    } else if (file.kind == "data") {
        ++s.data_files_removed;
    } else if (file.kind == "transaction") {
        ++s.transaction_files_removed;
    } else if (file.kind == "index") {
        ++s.index_files_removed;
    } else if (file.kind == "deletion") {
        ++s.deletion_files_removed;
    }
    if (max_candidates > 0) {
        if (out.candidates.size() < max_candidates) {
            out.candidates.push_back(file);
        } else {
            out.candidates_truncated = true;
        }
    }
}

std::string tags_named(const std::vector<TagInfo>& tags, const std::set<std::uint64_t>& versions) {
    std::string text = "{";
    bool first = true;
    for (const auto& tag : tags) {
        if (!tag.branch && versions.count(tag.version) != 0) {
            text += (first ? "\"" : ", \"") + tag.name + "\": " + std::to_string(tag.version);
            first = false;
        }
    }
    return text + "}";
}

}  // namespace

// ── tags ───────────────────────────────────────────────────────────────────────────────────────────

bool list_tags(const fs::path& dataset_path, std::vector<TagInfo>& out, std::string& error) {
    error.clear();
    out.clear();
    std::vector<io::DirEntry> entries;
    if (!io::list_dir(tags_dir(dataset_path).string(), entries, error)) {
        return false;
    }
    for (const auto& entry : entries) {
        const auto& name = entry.name;
        if (entry.is_dir || !ends_with(name, ".json") || starts_with(name, ".")) {
            continue;
        }
        TagInfo tag;
        if (!read_tag(dataset_path, name.substr(0, name.size() - 5), tag, error)) {
            return false;
        }
        out.push_back(std::move(tag));
    }
    std::sort(out.begin(), out.end(), [](const TagInfo& a, const TagInfo& b) { return a.name < b.name; });
    return true;
}

/// A branch file's name without `.json`, as Lance names it: its path segments percent-encoded
/// (`team/x` is `team%2Fx.json`). Undecodable escapes are kept as written.
std::string decode_branch_file_name(const std::string& encoded) {
    std::string out;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size() && std::isxdigit(static_cast<unsigned char>(encoded[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(encoded[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(encoded.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(encoded[i]);
        }
    }
    return out;
}

bool list_branches(const fs::path& dataset_path, std::vector<BranchInfo>& out, std::string& error) {
    error.clear();
    out.clear();
    const fs::path dir = dataset_path / "_refs" / "branches";
    std::vector<io::DirEntry> entries;
    if (!io::list_dir(dir.string(), entries, error)) {
        return false;
    }
    for (const auto& entry : entries) {
        const auto& file = entry.name;
        if (entry.is_dir || !ends_with(file, ".json") || starts_with(file, ".")) {
            continue;
        }
        const std::string name = decode_branch_file_name(file.substr(0, file.size() - 5));
        std::string text;
        if (!read_text(dir / file, text)) {
            error = "failed to read branch " + name;
            return false;
        }
        json::Value root;
        json::Reader reader(text);
        if (!reader.parse(root) || root.kind != json::Value::Object) {
            error = "branch " + name + " is not a valid branch file";
            return false;
        }
        BranchInfo branch;
        branch.name = name;
        const auto* parent_version = root.get("parentVersion");
        if (parent_version == nullptr || parent_version->kind != json::Value::Number) {
            error = "branch " + name + " is not a valid branch file";
            return false;
        }
        branch.parent_version = static_cast<std::uint64_t>(parent_version->n);
        if (const auto* p = root.get("parentBranch"); p != nullptr && p->kind == json::Value::String) {
            branch.parent_branch = p->s;
        }
        if (const auto* id = root.get("identifier"); id != nullptr && id->kind == json::Value::Object) {
            if (const auto* mapping = id->get("version_mapping");
                mapping != nullptr && mapping->kind == json::Value::Array) {
                for (const auto& pair : mapping->items) {
                    if (pair.kind != json::Value::Array || pair.items.size() != 2U ||
                        pair.items[0].kind != json::Value::Number || pair.items[1].kind != json::Value::String) {
                        error = "branch " + name + " has a malformed identifier";
                        return false;
                    }
                    branch.identifier.emplace_back(static_cast<std::uint64_t>(pair.items[0].n), pair.items[1].s);
                }
            }
        }
        if (const auto* c = root.get("createAt"); c != nullptr && c->kind == json::Value::Number) {
            branch.create_at = static_cast<std::uint64_t>(c->n);
        }
        if (const auto* m = root.get("manifestSize"); m != nullptr && m->kind == json::Value::Number) {
            branch.manifest_size = static_cast<std::uint64_t>(m->n);
        }
        if (const auto* md = root.get("metadata"); md != nullptr && md->kind == json::Value::Object) {
            for (const auto& [k, v] : md->fields) {
                if (v.kind == json::Value::String) {
                    branch.metadata[k] = v.s;
                }
            }
        }
        out.push_back(std::move(branch));
    }
    std::sort(out.begin(), out.end(), [](const BranchInfo& a, const BranchInfo& b) { return a.name < b.name; });
    return true;
}

fs::path branch_location(const fs::path& root, const std::string& branch) {
    return branch.empty() ? root : root / "tree" / branch;
}

namespace {

/// Lance's check_valid_branch: the names a branch may have (`main` is the main branch's own).
bool check_valid_branch(const std::string& name, std::string& error) {
    const auto fail = [&](const std::string& why) {
        error = "Ref is invalid: " + why;
        return false;
    };
    if (name.empty()) {
        return fail("Branch name cannot be empty");
    }
    if (name.front() == '/' || name.back() == '/') {
        return fail("Branch name cannot start or end with a '/'");
    }
    if (name.find("//") != std::string::npos) {
        return fail("Branch name cannot contain consecutive '/'");
    }
    if (name.find("..") != std::string::npos || name.find('\\') != std::string::npos) {
        return fail("Branch name cannot contain '..' or '\\'");
    }
    std::size_t start = 0;
    while (start <= name.size()) {
        const auto end = std::min(name.find('/', start), name.size());
        const auto segment = name.substr(start, end - start);
        if (segment.empty()) {
            return fail("Branch name cannot have empty segments between '/'");
        }
        for (const char c : segment) {
            if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '-' && c != '_') {
                return fail("Branch segment '" + segment +
                            "' contains invalid characters. Only alphanumeric, '.', '-', '_' are allowed.");
            }
        }
        start = end + 1;
    }
    if (ends_with(name, ".lock")) {
        return fail("Branch name cannot end with '.lock'");
    }
    if (name == "main") {
        return fail("Branch name cannot be 'main'");
    }
    return true;
}

/// A branch's reference file: pylance's name for it, with the `/` of a name percent-encoded (`team%2Fx`).
fs::path branch_ref_path(const fs::path& root, const std::string& name) {
    std::string file;
    for (const char c : name) {
        if (c == '/') {
            file += "%2F";
        } else {
            file.push_back(c);
        }
    }
    return root / "_refs" / "branches" / (file + ".json");
}

/// The branch's reference as Lance writes it (serde_json, pretty), its key order too.
std::string branch_json(const BranchInfo& branch) {
    std::ostringstream out;
    out << "{\n  \"parentBranch\": ";
    if (branch.parent_branch) {
        json_string(out, *branch.parent_branch);
    } else {
        out << "null";
    }
    out << ",\n  \"identifier\": {\n    \"version_mapping\": [";
    bool first = true;
    for (const auto& [version, id] : branch.identifier) {
        out << (first ? "\n      [\n        " : ",\n      [\n        ") << version << ",\n        ";
        json_string(out, id);
        out << "\n      ]";
        first = false;
    }
    out << (branch.identifier.empty() ? "]" : "\n    ]") << "\n  },\n  \"parentVersion\": " << branch.parent_version
        << ",\n  \"createAt\": " << branch.create_at << ",\n  \"manifestSize\": " << branch.manifest_size
        << ",\n  \"metadata\": {";
    first = true;
    for (const auto& [k, v] : branch.metadata) {
        out << (first ? "\n    " : ",\n    ");
        json_string(out, k);
        out << ": ";
        json_string(out, v);
        first = false;
    }
    out << (branch.metadata.empty() ? "}" : "\n  }") << "\n}";
    return out.str();
}

/// A version id as Lance makes one: a random (version 4) UUID, simple form (32 lowercase hex digits).
std::string new_branch_id() {
    static std::mt19937_64 engine{std::random_device{}()};
    std::array<std::uint8_t, 16> bytes{};
    for (auto& b : bytes) {
        b = static_cast<std::uint8_t>(engine() & 0xFFU);
    }
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3FU) | 0x80U);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) {
        out.push_back(hex[b >> 4U]);
        out.push_back(hex[b & 0x0FU]);
    }
    return out;
}

const BranchInfo* find_branch(const std::vector<BranchInfo>& branches, const std::string& name) {
    for (const auto& b : branches) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

void put_varint_bytes(std::vector<std::uint8_t>& out, std::uint64_t v) {
    while (v >= 0x80U) {
        out.push_back(static_cast<std::uint8_t>((v & 0x7FU) | 0x80U));
        v >>= 7U;
    }
    out.push_back(static_cast<std::uint8_t>(v));
}

void put_key_bytes(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t wire) {
    put_varint_bytes(out, (static_cast<std::uint64_t>(field) << 3U) | wire);
}

void put_string_bytes(std::vector<std::uint8_t>& out, std::uint32_t field, const std::string& s) {
    put_key_bytes(out, field, 2);
    put_varint_bytes(out, s.size());
    out.insert(out.end(), s.begin(), s.end());
}

/// The Clone operation a clone's first version records (lance.table.Transaction.Clone): the clone of
/// `source_branch`'s version `version` at `source_dir`. A shallow clone sets is_shallow; a branch sets
/// branch_name; a clone of a dataset (not a branch) leaves it out.
std::vector<std::uint8_t> clone_operation(const std::string& source_branch, std::uint64_t version,
                                          const std::string& source_dir, const std::string& branch_name,
                                          bool shallow) {
    std::vector<std::uint8_t> out;
    if (shallow) {
        put_key_bytes(out, 1, 0);
        put_varint_bytes(out, 1);  // is_shallow
    }
    if (!source_branch.empty()) {
        put_string_bytes(out, 2, source_branch);  // ref_name: absent for main
    }
    put_key_bytes(out, 3, 0);
    put_varint_bytes(out, version);  // ref_version
    put_string_bytes(out, 4, source_dir);
    if (!branch_name.empty()) {
        put_string_bytes(out, 5, branch_name);
    }
    return out;
}

/// The shallow form of the source's manifest at `version`, as a branch or a shallow clone starts from it: the
/// files and deletions the source's version names keep their base; those with no base (the source's own
/// directory) point at a new base path, the source's directory as a dataset root, numbered after the
/// source's. The source's base paths are kept. The caller sets the branch field, the Clone operation and
/// the version.
bool shallow_manifest(const fs::path& source_dir, const std::string& source_branch, std::uint64_t version,
                      pb::Manifest& manifest, std::uint64_t& manifest_size, std::string& error) {
    fs::path manifest_path;
    if (version == 0 || !manifest_file_of(source_dir, version, manifest_path, manifest_size)) {
        error = "Version not found error: Manifest file " + (source_dir / "_versions").string() + " for version " +
                std::to_string(version) + " does not exist";
        return false;
    }
    if (!load_manifest_file(manifest_path, manifest, error)) {
        return false;
    }
    std::vector<BasePath> bases;
    if (!read_base_paths(manifest, bases, error)) {
        return false;
    }
    std::uint32_t new_base = 0;
    for (const auto& base : bases) {
        new_base = std::max(new_base, base.id + 1U);
    }
    for (auto& fragment : manifest.fragments) {
        for (auto& file : fragment.files) {
            std::optional<std::uint32_t> base;
            if (!data_file_base_id(file, base, error)) {
                return false;
            }
            if (!base) {
                set_data_file_base_id(file, new_base);
            }
        }
        if (fragment.deletion_file.present) {
            std::optional<std::uint32_t> base;
            if (!deletion_file_base_id(fragment.deletion_file, base, error)) {
                return false;
            }
            if (!base) {
                set_deletion_file_base_id(fragment.deletion_file, new_base);
            }
        }
    }
    for (auto& index : manifest.indices) {
        std::optional<std::uint32_t> base;
        if (!index_base_id(index, base, error)) {
            return false;
        }
        if (!base) {
            set_index_base_id(index, new_base);
        }
    }
    BasePath inherited;
    inherited.id = new_base;
    inherited.name = source_branch;
    inherited.has_name = !source_branch.empty();
    inherited.is_dataset_root = true;
    inherited.path = source_dir.string();
    bases.push_back(inherited);
    write_base_paths(manifest, bases);
    manifest.reader_feature_flags |= pb::kFlagMultipleBasePaths;
    manifest.writer_feature_flags |= pb::kFlagMultipleBasePaths;
    return true;
}

/// Make a clone's manifest the first version of its dataset: `version`, the Clone operation read at that
/// version, a timestamp now, and nanolance as the writer.
void finish_clone(pb::Manifest& manifest, std::uint64_t version, std::vector<std::uint8_t> operation) {
    // The first version of a clone is the source's version, not the one after the latest: a branch starts at
    // its fork (Lance's CommitBuilder with the Clone transaction read at that version).
    manifest.version = version;
    manifest.transaction_file.clear();
    manifest.tag.clear();
    manifest.explicit_operation_field = 113;  // Transaction.clone
    manifest.explicit_operation = std::move(operation);
    manifest.explicit_read_version = version;
    manifest.has_timestamp = true;
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    manifest.timestamp_seconds = static_cast<std::int64_t>(nanos / 1000000000LL);
    manifest.timestamp_nanos = static_cast<std::int32_t>(nanos % 1000000000LL);
    manifest.writer_library = "nanolance";
    manifest.writer_version = nanolance_writer_version();
}

/// The directories a deep clone creates in its target, and whether each already existed.
struct CloneDirs {
    static constexpr std::array<const char*, 5> names{"_versions", "_transactions", "data", "_deletions", "_indices"};
    std::array<bool, 5> existed{};
};

CloneDirs remember_clone_dirs(const fs::path& target) {
    CloneDirs dirs;
    std::error_code ec;
    for (std::size_t i = 0; i < CloneDirs::names.size(); ++i) {
        dirs.existed[i] = fs::exists(target / CloneDirs::names[i], ec);
    }
    return dirs;
}

/// Remove what a failed clone created in `target` (the target itself when this clone created it).
void undo_clone(const fs::path& target, bool target_existed, const CloneDirs& dirs) {
    std::error_code ec;
    if (!target_existed) {
        fs::remove_all(target, ec);
        return;
    }
    for (std::size_t i = 0; i < CloneDirs::names.size(); ++i) {
        if (!dirs.existed[i]) {
            fs::remove_all(target / CloneDirs::names[i], ec);
        }
    }
}

/// Whether `target` already holds a dataset (a manifest in its `_versions`).
bool holds_dataset(const fs::path& target) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(target / "_versions", ec)) {
        std::uint64_t v = 0;
        if (entry.is_regular_file(ec) && parse_manifest_version(entry.path().filename().string(), v)) {
            return true;
        }
    }
    return false;
}

/// The deep form of the source's version: its data, deletion and index files copied into `target`, and no
/// base path to the source (a base is what an absent base_id, or base 0 of a branch, names in the source).
bool deep_manifest(const fs::path& source_dir, std::uint64_t version, const fs::path& target,
                   pb::Manifest& manifest, std::string& error) {
    fs::path manifest_path;
    std::uint64_t manifest_size = 0;
    if (version == 0 || !manifest_file_of(source_dir, version, manifest_path, manifest_size)) {
        error = "Version not found error: Manifest file " + (source_dir / "_versions").string() + " for version " +
                std::to_string(version) + " does not exist";
        return false;
    }
    if (!load_manifest_file(manifest_path, manifest, error)) {
        return false;
    }
    std::vector<BasePath> bases;
    if (!read_base_paths(manifest, bases, error)) {
        return false;
    }
    // Copy first, with the source's bases still in the manifest: every file is resolved against them.
    std::error_code ec;
    for (const auto& fragment : manifest.fragments) {
        for (const auto& file : fragment.files) {
            fs::path dir;
            if (!data_file_dir(source_dir, bases, file, dir, error)) {
                return false;
            }
            fs::create_directories(target / "data", ec);
            fs::copy_file(dir / file.path, target / "data" / file.path, fs::copy_options::none, ec);
            if (ec) {
                error = "failed to copy data file " + file.path + ": " + ec.message();
                return false;
            }
        }
        if (fragment.deletion_file.present) {
            fs::path dir;
            if (!deletion_file_dir(source_dir, bases, fragment.deletion_file, dir, error)) {
                return false;
            }
            const auto name = fs::path(deletion_file_name(fragment)).filename();
            fs::create_directories(target / "_deletions", ec);
            fs::copy_file(dir / name, target / "_deletions" / name, fs::copy_options::none, ec);
            if (ec) {
                error = "failed to copy deletion file " + name.string() + ": " + ec.message();
                return false;
            }
        }
    }
    for (const auto& index : manifest.indices) {
        fs::path dir;
        if (!index_dir_of(source_dir, manifest, index, dir, error)) {
            return false;
        }
        const auto dst = target / "_indices" / pb::uuid_string(index.uuid);
        fs::create_directories(dst.parent_path(), ec);
        fs::copy(dir, dst, fs::copy_options::recursive, ec);
        if (ec) {
            error = "failed to copy index " + pb::uuid_string(index.uuid) + ": " + ec.message();
            return false;
        }
    }
    // Then the manifest names the copies: no base on any file, index or deletion, and no base path.
    for (auto& fragment : manifest.fragments) {
        for (auto& file : fragment.files) {
            set_data_file_base_id(file, std::nullopt);
        }
        if (fragment.deletion_file.present) {
            set_deletion_file_base_id(fragment.deletion_file, std::nullopt);
        }
    }
    for (auto& index : manifest.indices) {
        set_index_base_id(index, std::nullopt);
    }
    write_base_paths(manifest, {});
    manifest.reader_feature_flags &= ~pb::kFlagMultipleBasePaths;
    manifest.writer_feature_flags &= ~pb::kFlagMultipleBasePaths;
    return true;
}

bool clone_version(const fs::path& root, const std::string& source_branch, std::uint64_t version,
                   const fs::path& target, bool deep, std::string& error) {
    error.clear();
    const auto source_dir = branch_location(root, source_branch);
    fs::path source_manifest;
    std::uint64_t source_size = 0;
    if (version == 0 || !manifest_file_of(source_dir, version, source_manifest, source_size)) {
        // pylance's words for a version that is not there (an OSError there, as for a missing tag).
        error = "Dataset at path " + (source_dir / "_versions" / (std::to_string(version) + ".manifest")).string() +
                " was not found";
        return false;
    }
    if (holds_dataset(target)) {
        error = "Dataset already exists: " + target.string();
        return false;
    }
    std::error_code ec;
    const bool target_existed = fs::exists(target, ec);
    const auto dirs = remember_clone_dirs(target);
    pb::Manifest manifest;
    std::uint64_t manifest_size = 0;
    const bool built = deep ? deep_manifest(source_dir, version, target, manifest, error)
                            : shallow_manifest(source_dir, source_branch, version, manifest, manifest_size, error);
    if (!built) {
        undo_clone(target, target_existed, dirs);
        return false;
    }
    write_manifest_branch(manifest, "");  // a clone is a dataset of its own, not a branch
    finish_clone(manifest, version, clone_operation(source_branch, version, source_dir.string(), "", !deep));
    if (!publish_manifest(target, manifest, error)) {
        undo_clone(target, target_existed, dirs);
        return false;
    }
    return true;
}

}  // namespace

bool shallow_clone(const fs::path& root, const std::string& source_branch, std::uint64_t version,
                   const fs::path& target, std::string& error) {
    return clone_version(root, source_branch, version, target, false, error);
}

bool deep_clone(const fs::path& root, const std::string& source_branch, std::uint64_t version,
                const fs::path& target, std::string& error) {
    return clone_version(root, source_branch, version, target, true, error);
}

bool create_branch(const fs::path& root, const std::string& name, const std::string& source_branch,
                   std::uint64_t version, std::string& error) {
    error.clear();
    if (!check_valid_branch(name, error)) {
        return false;
    }
    std::vector<BranchInfo> branches;
    if (!list_branches(root, branches, error)) {
        return false;
    }
    std::error_code ec;
    if (find_branch(branches, name) != nullptr || fs::exists(branch_ref_path(root, name), ec)) {
        error = "Ref conflict error: branch " + name + " already exists";
        return false;
    }
    std::vector<std::pair<std::uint64_t, std::string>> identifier;
    if (!source_branch.empty()) {
        const auto* parent = find_branch(branches, source_branch);
        if (parent == nullptr) {
            error = "Ref not found error: parent branch " + source_branch + " does not exist";
            return false;
        }
        identifier = parent->identifier;
    }
    const auto source_dir = branch_location(root, source_branch);
    pb::Manifest manifest;
    std::uint64_t manifest_size = 0;
    if (!shallow_manifest(source_dir, source_branch, version, manifest, manifest_size, error)) {
        return false;
    }
    write_manifest_branch(manifest, name);
    finish_clone(manifest, version, clone_operation(source_branch, version, source_dir.string(), name, true));
    if (!publish_manifest(branch_location(root, name), manifest, error)) {
        return false;
    }

    BranchInfo branch;
    branch.name = name;
    if (!source_branch.empty()) {
        branch.parent_branch = source_branch;
    }
    branch.identifier = identifier;
    branch.identifier.emplace_back(version, new_branch_id());
    branch.parent_version = version;
    branch.create_at = static_cast<std::uint64_t>(now_ns() / 1000000000LL);
    branch.manifest_size = manifest_size;
    bool existed = false;
    if (!write_file(branch_ref_path(root, name), branch_json(branch), false, existed, error)) {
        if (existed) {
            error = "Ref conflict error: branch " + name + " already exists";
        }
        return false;
    }
    return true;
}

namespace {

/// Lance's "[(\"name\", version), ...]" for the (name, version) pairs a refusal lists.
std::string pairs_text(const std::vector<std::pair<std::string, std::uint64_t>>& pairs) {
    std::string out = "[";
    bool first = true;
    for (const auto& [name, version] : pairs) {
        out += (first ? "(\"" : ", (\"") + name + "\", " + std::to_string(version) + ")";
        first = false;
    }
    return out + "]";
}

/// Remove `dir` and then the directories above it that are left empty, up to (not including) `stop`.
void remove_branch_dirs(const fs::path& dir, const fs::path& stop) {
    std::error_code ec;
    fs::remove_all(dir, ec);
    for (auto up = dir.parent_path(); up.generic_string().size() > stop.generic_string().size(); up = up.parent_path()) {
        if (!fs::is_directory(up, ec) || !fs::is_empty(up, ec) || ec) {
            break;
        }
        fs::remove(up, ec);
    }
}

}  // namespace

bool delete_branch(const fs::path& root, const std::string& name, std::string& error) {
    error.clear();
    if (name == "main") {
        error = "Ref is invalid: Branch name cannot be 'main'";
        return false;
    }
    std::vector<BranchInfo> branches;
    if (!list_branches(root, branches, error)) {
        return false;
    }
    const auto* self = find_branch(branches, name);
    if (self == nullptr) {
        error = "Ref not found error: Branch " + name + " does not exist";
        return false;
    }
    // Branches forked from this one (deepest first, as Lance lists them), then the tags naming it.
    std::vector<const BranchInfo*> children;
    for (const auto& b : branches) {
        if (b.name != name && b.identifier.size() > self->identifier.size() &&
            std::equal(self->identifier.begin(), self->identifier.end(), b.identifier.begin())) {
            children.push_back(&b);
        }
    }
    if (!children.empty()) {
        std::sort(children.begin(), children.end(),
                  [](const BranchInfo* a, const BranchInfo* b) { return a->identifier > b->identifier; });
        std::vector<std::pair<std::string, std::uint64_t>> referenced;
        for (const auto* child : children) {
            referenced.emplace_back(child->name, child->identifier[self->identifier.size()].first);
        }
        error = "Ref conflict error: Branch " + name + " is referenced by " + pairs_text(referenced) +
                " versions, can not delete";
        return false;
    }
    std::vector<TagInfo> tags;
    if (!list_tags(root, tags, error)) {
        return false;
    }
    std::vector<std::pair<std::string, std::uint64_t>> tagged;
    for (const auto& tag : tags) {
        if (tag.branch && *tag.branch == name) {
            tagged.emplace_back(tag.name, tag.version);
        }
    }
    if (!tagged.empty()) {
        error = "Ref conflict error: Branch " + name + " is referenced by tags " + pairs_text(tagged) +
                ", can not delete";
        return false;
    }
    std::error_code ec;
    fs::remove(branch_ref_path(root, name), ec);
    if (ec) {
        error = "failed to delete the reference of branch " + name + ": " + ec.message();
        return false;
    }
    remove_branch_dirs(branch_location(root, name), root / "tree");
    return true;
}

bool replace_branch_metadata(const fs::path& root, const std::string& name,
                             const std::map<std::string, std::string>& metadata, std::string& error) {
    error.clear();
    if (!check_valid_branch(name, error)) {
        return false;
    }
    std::vector<BranchInfo> branches;
    if (!list_branches(root, branches, error)) {
        return false;
    }
    const auto* found = find_branch(branches, name);
    if (found == nullptr) {
        error = "Ref not found error: branch " + name + " does not exist";
        return false;
    }
    BranchInfo branch = *found;
    branch.metadata = metadata;
    bool existed = false;
    return write_file(branch_ref_path(root, name), branch_json(branch), true, existed, error);
}

bool get_tag(const fs::path& dataset_path, const std::string& name, TagInfo& out, std::string& error) {
    error.clear();
    return check_valid_tag(name, error) && read_tag(dataset_path, name, out, error);
}

bool create_tag(const fs::path& root, const std::string& name, const std::string& branch, std::uint64_t version,
                std::string& error) {
    error.clear();
    TagInfo tag;
    if (!check_valid_tag(name, error) ||
        !resolve_tag_target(branch_location(root, branch), branch, version, tag.version, tag.manifest_size, error)) {
        return false;
    }
    if (!branch.empty()) {
        tag.branch = branch;
    }
    tag.name = name;
    tag.created_at = tag.updated_at = rfc3339(now_ns());
    bool existed = false;
    if (!write_file(tags_dir(root) / (name + ".json"), tag_json(tag), false, existed, error)) {
        if (existed) {
            error = "Ref conflict error: tag " + name + " already exists";
        }
        return false;
    }
    return true;
}

bool update_tag(const fs::path& root, const std::string& name, const std::string& branch, std::uint64_t version,
                std::string& error) {
    error.clear();
    TagInfo tag;
    if (!check_valid_tag(name, error) || !read_tag(root, name, tag, error) ||
        !resolve_tag_target(branch_location(root, branch), branch, version, tag.version, tag.manifest_size, error)) {
        return false;
    }
    // The tag now names the reference's branch, as Lance's update does.
    tag.branch.reset();
    if (!branch.empty()) {
        tag.branch = branch;
    }
    tag.updated_at = rfc3339(now_ns());
    bool existed = false;
    return write_file(tags_dir(root) / (name + ".json"), tag_json(tag), true, existed, error);
}

bool delete_tag(const fs::path& dataset_path, const std::string& name, std::string& error) {
    error.clear();
    if (!check_valid_tag(name, error)) {
        return false;
    }
    std::error_code ec;
    if (!fs::remove(tags_dir(dataset_path) / (name + ".json"), ec)) {
        error = ec ? "failed to delete tag " + name + ": " + ec.message()
                   : "Ref not found error: tag " + name + " does not exist";
        return false;
    }
    return true;
}

bool replace_tag_metadata(const fs::path& dataset_path, const std::string& name,
                          const std::map<std::string, std::string>& metadata, std::string& error) {
    error.clear();
    TagInfo tag;
    if (!check_valid_tag(name, error) || !read_tag(dataset_path, name, tag, error)) {
        return false;
    }
    tag.metadata = metadata;
    bool existed = false;
    return write_file(tags_dir(dataset_path) / (name + ".json"), tag_json(tag), true, existed, error);
}

// ── cleanup ────────────────────────────────────────────────────────────────────────────────────────

bool cleanup_retain_versions(const fs::path& dataset_path, std::uint64_t n, CleanupPolicy& policy,
                             std::string& error) {
    error.clear();
    if (n == 0) {
        error = "Invalid user input: retain_versions must be greater than 0, got 0";
        return false;
    }
    const auto versions = list_manifest_versions(dataset_path, error);
    if (versions.empty()) {
        if (error.empty()) {
            error = "no manifest found in " + (dataset_path / "_versions").string();
        }
        return false;
    }
    policy.before_version = versions.size() <= n ? versions.front() : versions[versions.size() - n];
    return true;
}

bool cleanup_old_versions(const fs::path& dataset_path, std::uint64_t read_version, const CleanupPolicy& policy,
                          bool execute, std::size_t max_candidates, CleanupResult& out, std::string& error) {
    error.clear();
    out = CleanupResult{};
    if (policy.delete_rate_limit && *policy.delete_rate_limit == 0) {
        error = "Cleanup error: delete_rate_limit must be greater than 0, got 0";
        return false;
    }
    if (policy.versions && policy.versions->empty()) {
        error = "Invalid user input: versions must not be empty when specified";
        return false;
    }
    if (read_version == 0) {
        read_version = highest_manifest_version(dataset_path, error);
        if (read_version == 0) {
            if (error.empty()) {
                error = "no manifest found in " + (dataset_path / "_versions").string();
            }
            return false;
        }
    }
    out.read_version = read_version;

    // The branch this directory holds: its tags are the ones on it, and the root's branches and tags
    // are the ones the other branches of the lineage keep (as Lance's cleanup works on a branch).
    pb::Manifest current;
    std::string branch;
    BranchScope scope;
    if (!load_manifest_version(dataset_path, read_version, current, error) ||
        !read_manifest_branch(current, branch, error) || !branch_scope_of(dataset_path, branch, scope, error)) {
        return false;
    }
    std::vector<TagInfo> tags;
    if (!list_tags(scope.root, tags, error)) {
        return false;
    }
    std::set<std::uint64_t> tagged;
    for (const auto& tag : tags) {
        const bool on_this_branch = scope.branch.empty() ? !tag.branch : (tag.branch && *tag.branch == scope.branch);
        if (on_this_branch) {
            tagged.insert(tag.version);
        }
    }

    // Whether a version with commit time `ts` is one the policy removes.
    const auto selected = [&policy](std::uint64_t version, std::int64_t ts) {
        bool should_clean = true;
        if (policy.before_timestamp_ns) {
            should_clean = should_clean && ts < *policy.before_timestamp_ns;
        }
        if (policy.before_version) {
            should_clean = should_clean && version < *policy.before_version;
        }
        if (policy.versions) {
            should_clean = should_clean && policy.versions->count(version) != 0;
        }
        return should_clean;
    };
    // The versions a branch forks from are kept, with the files a branch's manifests name here. Only a
    // fork the policy would remove needs it (Lance's find_referenced_branches).
    std::vector<ForkedBranch> forks;
    if (!forked_branches(scope, forks, error)) {
        return false;
    }
    ForkProtection protection;
    for (const auto& fork : forks) {
        fs::path fork_manifest_path;
        std::uint64_t fork_manifest_size = 0;
        if (!manifest_file_of(dataset_path, fork.version, fork_manifest_path, fork_manifest_size)) {
            continue;  // no such version here: nothing for the branch to hold
        }
        pb::Manifest fork_manifest;
        if (!load_manifest_file(fork_manifest_path, fork_manifest, error)) {
            return false;
        }
        if (selected(fork.version, manifest_timestamp_ns(fork_manifest)) &&
            !protect_fork(dataset_path, fork, protection, error)) {
            return false;
        }
    }

    // Every manifest: the ones kept say which files are in use; the ones removed vouch for theirs.
    ReferencedFiles referenced;
    ReferencedFiles verified;
    referenced.data_paths = protection.files.data_paths;
    referenced.delete_paths = protection.files.delete_paths;
    referenced.index_uuids = protection.files.index_uuids;
    std::set<std::uint64_t> tagged_old;
    std::vector<CleanupCandidate> old_manifests;
    std::optional<std::int64_t> earliest_kept_ns;  // commit times, for Lance's listing cutoff
    std::optional<std::int64_t> latest_removed_ns;
    const auto versions_dir = dataset_path / "_versions";
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(versions_dir, ec)) {
        const auto name = entry.path().filename().string();
        std::uint64_t version = 0;
        const bool attached = parse_manifest_version(name, version);
        const bool detached = !attached && starts_with(name, "d") && ends_with(name, ".manifest");
        if (!entry.is_regular_file(ec) || (!attached && !detached)) {
            continue;
        }
        pb::Manifest manifest;
        std::string read_error;
        if (!load_manifest_file(entry.path(), manifest, read_error)) {
            std::error_code gone;
            if (attached && version < read_version && !fs::exists(entry.path(), gone)) {
                continue;  // another cleanup removed it after the listing
            }
            error = "cannot clean up: " + entry.path().string() + ": " + read_error;
            return false;
        }
        if (!manifest.index_section_error.empty()) {
            error = "cannot clean up: the indices of " + entry.path().string() +
                    " could not be read: " + manifest.index_section_error;
            return false;
        }
        if (detached) {
            reference(manifest, referenced);  // never removed here; its files stay
            continue;
        }
        const std::int64_t ts = manifest_timestamp_ns(manifest);
        const bool should_clean = selected(version, ts);
        const bool is_latest = read_version <= version;
        const bool is_tagged = tagged.count(version) != 0;
        const bool is_forked = protection.versions.count(version) != 0;
        if (is_tagged && !is_latest && should_clean) {
            tagged_old.insert(version);
        }
        const bool keep = is_latest || !should_clean || is_tagged || is_forked;
        reference(manifest, keep ? referenced : verified);
        auto& bound = keep ? earliest_kept_ns : latest_removed_ns;
        if (!bound || (keep ? ts < *bound : ts > *bound)) {
            bound = ts;
        }
        if (!keep) {
            old_manifests.push_back({fs::relative(entry.path(), dataset_path, ec).generic_string(), "manifest", false,
                                     static_cast<std::uint64_t>(entry.file_size(ec))});
        }
    }
    if (ec) {
        error = "failed to list " + versions_dir.string() + ": " + ec.message();
        return false;
    }
    if (policy.error_if_tagged_old_versions && !tagged_old.empty()) {
        error = "Cleanup error: " + std::to_string(tagged_old.size()) +
                " tagged version(s) have been marked for cleanup. Either set `error_if_tagged_old_versions=false` "
                "or delete the following tag(s) to enable cleanup: " +
                tags_named(tags, tagged_old);
        return false;
    }

    // The files the kept versions do not use, under the directories Lance manages.
    for (const auto& candidate : old_manifests) {
        record(out, candidate, max_candidates);
    }
    // Lance lists only files no newer than the earliest version kept (a newer file may belong to a
    // write in progress), unless a removed version is newer than a kept one (a tagged old version):
    // then everything, so the removed version's files are not orphaned. Index files: always all.
    // A flag and a value rather than a copied std::optional: GCC's -O3 flow analysis flags the copy as
    // maybe-uninitialized (-Werror in CI).
    const bool has_cutoff = earliest_kept_ns.has_value() &&
                            !(latest_removed_ns.has_value() && *latest_removed_ns > *earliest_kept_ns);
    const std::int64_t cutoff_ns = has_cutoff ? *earliest_kept_ns : 0;
    std::vector<CleanupCandidate> files;
    for (const char* dir : {"_versions", "_transactions", "data", "_indices", "_deletions"}) {
        const bool all = std::string_view(dir) == "_indices" || !has_cutoff;
        std::error_code walk_ec;
        if (!fs::exists(dataset_path / dir, walk_ec)) {
            continue;
        }
        const auto file_now = fs::file_time_type::clock::now();
        for (auto it = fs::recursive_directory_iterator(dataset_path / dir, walk_ec);
             !walk_ec && it != fs::recursive_directory_iterator(); it.increment(walk_ec)) {
            std::error_code entry_ec;
            if (!it->is_regular_file(entry_ec)) {
                continue;
            }
            File file;
            file.relative = fs::relative(it->path(), dataset_path, entry_ec).generic_string();
            file.size = static_cast<std::uint64_t>(it->file_size(entry_ec));
            const auto modified = it->last_write_time(entry_ec);
            const auto age = std::chrono::duration_cast<std::chrono::nanoseconds>(file_now - modified).count();
            // The modification time itself, on the clock commit times are on (not now minus an age:
            // two clocks read at different moments put a file written just after the earliest kept
            // version before it whenever the walk is slow).
            const auto modified_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         fs::file_time_type::clock::to_sys(modified).time_since_epoch())
                                         .count();
            if (!all && modified_ns > cutoff_ns) {
                continue;  // modified after the earliest kept version
            }
            file.maybe_in_progress = !policy.delete_unverified && age < kUnverifiedThresholdNs;
            bool unverified = false;
            const auto kind = removable_kind(file, referenced, verified, unverified);
            if (!kind.empty() && !entry_ec) {
                files.push_back({file.relative, kind, unverified, file.size});
            }
        }
        if (walk_ec) {
            error = "failed to list " + (dataset_path / dir).string() + ": " + walk_ec.message();
            return false;
        }
    }
    for (const auto& candidate : files) {
        record(out, candidate, max_candidates);
    }
    if (!execute) {
        return true;
    }

    // Old manifests first: a cleanup cut short then leaves only unreferenced files (which a later
    // cleanup removes), never a version whose files are gone.
    const auto pause = policy.delete_rate_limit
                           ? std::chrono::nanoseconds((1000000000ULL + *policy.delete_rate_limit - 1) /
                                                      *policy.delete_rate_limit)
                           : std::chrono::nanoseconds(0);
    auto next = std::chrono::steady_clock::now();
    std::set<fs::path> index_dirs;
    const auto remove = [&](const CleanupCandidate& file) {
        if (pause.count() > 0) {
            std::this_thread::sleep_until(next);
            next = std::max(next + pause, std::chrono::steady_clock::now());
        }
        std::error_code remove_ec;
        fs::remove(dataset_path / file.path, remove_ec);
        if (remove_ec) {
            error = "failed to remove " + (dataset_path / file.path).string() + ": " + remove_ec.message();
            return false;
        }
        if (file.kind == "index") {
            for (auto dir = (dataset_path / file.path).parent_path(); dir != dataset_path / "_indices" &&
                                                                     starts_with(dir.generic_string(),
                                                                                 (dataset_path / "_indices").generic_string());
                 dir = dir.parent_path()) {
                index_dirs.insert(dir);
            }
        }
        return true;
    };
    for (const auto& candidate : old_manifests) {
        if (!remove(candidate)) {
            return false;
        }
    }
    for (const auto& candidate : files) {
        if (!remove(candidate)) {
            return false;
        }
    }
    // Index directories left empty, deepest first.
    for (auto it = index_dirs.rbegin(); it != index_dirs.rend(); ++it) {
        std::error_code dir_ec;
        if (fs::is_empty(*it, dir_ec) && !dir_ec) {
            fs::remove(*it, dir_ec);
        }
    }
    return true;
}

bool parse_duration(const std::string& text, std::int64_t& nanoseconds) {
    // humantime's units, longest spelling first within each.
    static const std::vector<std::pair<std::string, double>> kUnits = {
        {"nanoseconds", 1}, {"nanosecond", 1}, {"nsec", 1}, {"ns", 1},
        {"microseconds", 1e3}, {"microsecond", 1e3}, {"usec", 1e3}, {"us", 1e3}, {"\xC2\xB5s", 1e3},
        {"milliseconds", 1e6}, {"millisecond", 1e6}, {"msec", 1e6}, {"ms", 1e6},
        {"seconds", 1e9}, {"second", 1e9}, {"secs", 1e9}, {"sec", 1e9}, {"s", 1e9},
        {"minutes", 60e9}, {"minute", 60e9}, {"mins", 60e9}, {"min", 60e9}, {"m", 60e9},
        {"hours", 3600e9}, {"hour", 3600e9}, {"hrs", 3600e9}, {"hr", 3600e9}, {"h", 3600e9},
        {"days", 86400e9}, {"day", 86400e9}, {"d", 86400e9},
        {"weeks", 604800e9}, {"week", 604800e9}, {"w", 604800e9},
        {"months", 2630016e9}, {"month", 2630016e9}, {"M", 2630016e9},
        {"years", 31557600e9}, {"year", 31557600e9}, {"y", 31557600e9},
    };
    double total = 0;
    std::size_t i = 0;
    bool any = false;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') {
            ++i;
        }
        if (i == text.size()) {
            break;
        }
        const auto start = i;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])) != 0) {
            ++i;
        }
        if (i == start) {
            return false;
        }
        const double value = std::stod(text.substr(start, i - start));
        while (i < text.size() && text[i] == ' ') {
            ++i;
        }
        const auto unit_start = i;
        while (i < text.size() && text[i] != ' ' && std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
            ++i;
        }
        const auto unit = text.substr(unit_start, i - unit_start);
        const auto match = std::find_if(kUnits.begin(), kUnits.end(), [&](const auto& u) { return u.first == unit; });
        if (match == kUnits.end()) {
            return false;
        }
        total += value * match->second;
        any = true;
    }
    if (!any || total > 9.2e18) {
        return false;
    }
    nanoseconds = static_cast<std::int64_t>(total);
    return true;
}

void run_auto_cleanup(const fs::path& dataset_path, std::uint64_t committed_version,
                      const std::map<std::string, std::string>& config) {
    // Lance's build_cleanup_policy: nothing unless an interval is set and this version is on it.
    const auto interval_it = config.find("lance.auto_cleanup.interval");
    if (interval_it == config.end() || committed_version < 2) {
        return;
    }
    std::uint64_t interval = 0;
    try {
        interval = std::stoull(interval_it->second);
    } catch (...) {
        return;
    }
    if (interval != 0 && committed_version % interval != 0) {
        return;
    }
    CleanupPolicy policy;
    std::string error;
    if (const auto it = config.find("lance.auto_cleanup.older_than"); it != config.end()) {
        std::int64_t ns = 0;
        if (!parse_duration(it->second, ns)) {
            return;
        }
        policy.before_timestamp_ns = now_ns() - ns;
    }
    if (const auto it = config.find("lance.auto_cleanup.retain_versions"); it != config.end()) {
        try {
            if (!cleanup_retain_versions(dataset_path, std::stoull(it->second), policy, error)) {
                return;
            }
        } catch (...) {
            return;
        }
    }
    if (const auto it = config.find("lance.auto_cleanup.delete_rate_limit"); it != config.end()) {
        try {
            policy.delete_rate_limit = std::stoull(it->second);
        } catch (...) {
            return;
        }
    }
    // As of the version the commit was made from (Lance runs the hook with the dataset before it).
    CleanupResult result;
    (void)cleanup_old_versions(dataset_path, committed_version - 1, policy, true, 0, result, error);
}

// ── drop ───────────────────────────────────────────────────────────────────────────────────────────

bool drop_dataset(const fs::path& dataset_path, bool ignore_not_found, std::string& error) {
    error.clear();
    std::error_code ec;
    if (!fs::exists(dataset_path, ec)) {
        if (ignore_not_found) {
            return true;
        }
        error = "Dataset at path " + dataset_path.string() + " was not found";
        return false;
    }
    // Positive evidence only: a manifest that reads, or a namespace marker; or nothing there at all.
    bool qualifies = fs::exists(dataset_path / ".lance-reserved", ec) ||
                     fs::exists(dataset_path / ".lance-deregistered", ec) ||
                     (fs::is_directory(dataset_path, ec) && fs::is_empty(dataset_path, ec));
    for (const auto& entry : fs::directory_iterator(dataset_path / "_versions", ec)) {
        if (qualifies) {
            break;
        }
        const auto name = entry.path().filename().string();
        std::uint64_t version = 0;
        if (!entry.is_regular_file(ec) ||
            !(parse_manifest_version(name, version) || (starts_with(name, "d") && ends_with(name, ".manifest")))) {
            continue;
        }
        pb::Manifest manifest;
        std::string read_error;
        qualifies = load_manifest_file_unchecked(entry.path(), manifest, read_error);
    }
    if (!qualifies) {
        error = "Invalid user input: Refusing to drop '" + dataset_path.generic_string() +
                "': no readable Lance manifest was found under '_versions', so this is not a dataset root. "
                "Check that the path points at a dataset and not at a parent directory, and check the logs for "
                "manifests that could not be read. A path holding only data files, or only manifests that cannot "
                "be read, needs an explicit storage-level delete instead: such leftovers neither block re-creating "
                "the dataset nor survive cleanup.";
        return false;
    }
    fs::remove_all(dataset_path, ec);
    if (ec) {
        error = "failed to drop " + dataset_path.string() + ": " + ec.message();
        return false;
    }
    return true;
}

}  // namespace nano_lance
