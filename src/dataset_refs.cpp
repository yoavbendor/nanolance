// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include <nanolance/dataset_refs.hpp>

#include <nanolance/manifest_reader.hpp>

#include "fts_json.hpp"
#include "lance_minimal.pb.hpp"

#include <algorithm>
#include <atomic>
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
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return static_cast<bool>(in) || in.eof();
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
bool resolve_tag_target(const fs::path& dataset_path, std::uint64_t version, std::uint64_t& resolved,
                        std::uint64_t& manifest_size, std::string& error) {
    resolved = version;
    if (resolved == 0) {
        std::string ignored;
        resolved = highest_manifest_version(dataset_path, ignored);
    }
    fs::path path;
    if (resolved == 0 || !manifest_file_of(dataset_path, resolved, path, manifest_size)) {
        error = "Version not found error: version main:" + (version == 0 ? std::string("latest") : std::to_string(version)) +
                " does not exist";
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

void reference(const pb::Manifest& manifest, ReferencedFiles& files) {
    for (const auto& fragment : manifest.fragments) {
        for (const auto& file : fragment.files) {
            files.data_paths.insert("data/" + file.path);
        }
        if (fragment.deletion_file.present) {
            files.delete_paths.insert("_deletions/" + std::to_string(fragment.id) + "-" +
                                      std::to_string(fragment.deletion_file.read_version) + "-" +
                                      std::to_string(fragment.deletion_file.id) +
                                      (fragment.deletion_file.file_type == 1U ? ".bin" : ".arrow"));
        }
    }
    if (!manifest.transaction_file.empty()) {
        files.tx_paths.insert("_transactions/" + manifest.transaction_file);
    }
    for (const auto& index : manifest.indices) {
        files.index_uuids.insert(pb::uuid_string(index.uuid));
    }
}

constexpr std::int64_t kUnverifiedThresholdNs = 7LL * 24 * 3600 * 1000000000LL;

bool starts_with(const std::string& s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}
bool ends_with(const std::string& s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
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
    std::error_code ec;
    const auto dir = tags_dir(dataset_path);
    if (!fs::exists(dir, ec)) {
        return true;
    }
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        const auto name = entry.path().filename().string();
        if (!entry.is_regular_file(ec) || !ends_with(name, ".json") || starts_with(name, ".")) {
            continue;
        }
        TagInfo tag;
        if (!read_tag(dataset_path, name.substr(0, name.size() - 5), tag, error)) {
            return false;
        }
        out.push_back(std::move(tag));
    }
    if (ec) {
        error = "failed to list tags: " + ec.message();
        return false;
    }
    std::sort(out.begin(), out.end(), [](const TagInfo& a, const TagInfo& b) { return a.name < b.name; });
    return true;
}

bool get_tag(const fs::path& dataset_path, const std::string& name, TagInfo& out, std::string& error) {
    error.clear();
    return check_valid_tag(name, error) && read_tag(dataset_path, name, out, error);
}

bool create_tag(const fs::path& dataset_path, const std::string& name, std::uint64_t version, std::string& error) {
    error.clear();
    TagInfo tag;
    if (!check_valid_tag(name, error) ||
        !resolve_tag_target(dataset_path, version, tag.version, tag.manifest_size, error)) {
        return false;
    }
    tag.name = name;
    tag.created_at = tag.updated_at = rfc3339(now_ns());
    bool existed = false;
    if (!write_file(tags_dir(dataset_path) / (name + ".json"), tag_json(tag), false, existed, error)) {
        if (existed) {
            error = "Ref conflict error: tag " + name + " already exists";
        }
        return false;
    }
    return true;
}

bool update_tag(const fs::path& dataset_path, const std::string& name, std::uint64_t version, std::string& error) {
    error.clear();
    TagInfo tag;
    if (!check_valid_tag(name, error) || !read_tag(dataset_path, name, tag, error) ||
        !resolve_tag_target(dataset_path, version, tag.version, tag.manifest_size, error)) {
        return false;
    }
    tag.branch.reset();
    tag.updated_at = rfc3339(now_ns());
    bool existed = false;
    return write_file(tags_dir(dataset_path) / (name + ".json"), tag_json(tag), true, existed, error);
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

    std::vector<TagInfo> tags;
    if (!list_tags(dataset_path, tags, error)) {
        return false;
    }
    std::set<std::uint64_t> tagged;
    for (const auto& tag : tags) {
        if (!tag.branch) {
            tagged.insert(tag.version);
        }
    }

    // Every manifest: the ones kept say which files are in use; the ones removed vouch for theirs.
    ReferencedFiles referenced;
    ReferencedFiles verified;
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
        bool should_clean = true;
        const std::int64_t ts =
            manifest.has_timestamp ? manifest.timestamp_seconds * 1000000000LL + manifest.timestamp_nanos : 0;
        if (policy.before_timestamp_ns) {
            should_clean = should_clean && ts < *policy.before_timestamp_ns;
        }
        if (policy.before_version) {
            should_clean = should_clean && version < *policy.before_version;
        }
        if (policy.versions) {
            should_clean = should_clean && policy.versions->count(version) != 0;
        }
        const bool is_latest = read_version <= version;
        const bool is_tagged = tagged.count(version) != 0;
        if (is_tagged && !is_latest && should_clean) {
            tagged_old.insert(version);
        }
        const bool keep = is_latest || !should_clean || is_tagged;
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
    std::optional<std::int64_t> cutoff_ns = earliest_kept_ns;
    if (earliest_kept_ns && latest_removed_ns && *latest_removed_ns > *earliest_kept_ns) {
        cutoff_ns.reset();
    }
    std::vector<CleanupCandidate> files;
    for (const char* dir : {"_versions", "_transactions", "data", "_indices", "_deletions"}) {
        const bool all = std::string_view(dir) == "_indices" || !cutoff_ns;
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
            if (!all && modified_ns > *cutoff_ns) {
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
        qualifies = load_manifest_file(entry.path(), manifest, read_error);
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
