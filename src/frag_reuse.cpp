// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Yoav Bendor

#include "nanolance/frag_reuse.hpp"

#include "nanolance/byte_source.hpp"
#include "nanolance/roaring_bitmap.hpp"
#include "index_files.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>

namespace nano_lance {
namespace {

constexpr const char* kFragReuseName = "__lance_frag_reuse";
constexpr const char* kFragReuseDetails = "FragmentReuseIndexDetails";

// A protobuf wire reader for the few messages this file decodes (table.proto FragmentReuseIndexDetails).
struct Field {
    std::uint32_t number = 0;
    std::uint32_t wire = 0;
    std::uint64_t value = 0;
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
};

class Wire {
public:
    Wire(const std::uint8_t* data, std::size_t size) : at_(data), end_(data + size) {}

    /// The next field; false at the end. `bad` is set on a malformed message.
    bool next(Field& f) {
        if (at_ >= end_) {
            return false;
        }
        std::uint64_t key = 0;
        if (!varint(key)) {
            return fail();
        }
        f = Field{};
        f.number = static_cast<std::uint32_t>(key >> 3U);
        f.wire = static_cast<std::uint32_t>(key & 7U);
        switch (f.wire) {
            case 0:
                return varint(f.value) || fail();
            case 1:
                return skip(8) || fail();
            case 5:
                return skip(4) || fail();
            case 2: {
                std::uint64_t n = 0;
                if (!varint(n) || n > static_cast<std::uint64_t>(end_ - at_)) {
                    return fail();
                }
                f.data = at_;
                f.size = static_cast<std::size_t>(n);
                at_ += n;
                return true;
            }
            default:
                return fail();
        }
    }
    bool bad() const { return bad_; }

private:
    bool varint(std::uint64_t& v) {
        v = 0;
        for (unsigned shift = 0; shift < 64U && at_ < end_; shift += 7U) {
            const std::uint8_t byte = *at_++;
            v |= static_cast<std::uint64_t>(byte & 0x7FU) << shift;
            if (byte < 0x80U) {
                return true;
            }
        }
        return false;
    }
    bool skip(std::size_t n) {
        if (static_cast<std::size_t>(end_ - at_) < n) {
            return false;
        }
        at_ += n;
        return true;
    }
    bool fail() {
        bad_ = true;
        return false;
    }

    const std::uint8_t* at_;
    const std::uint8_t* end_;
    bool bad_ = false;
};

struct Digest {
    std::uint64_t id = 0;
    std::uint64_t physical = 0;
};

struct RawGroup {
    std::vector<std::uint8_t> changed;  // a RoaringTreemap of the old addresses that were rewritten
    std::vector<Digest> old_frags;
    std::vector<Digest> new_frags;
};

struct RawVersion {
    std::uint64_t dataset_version = 0;
    std::vector<RawGroup> groups;
};

bool read_digest(const Field& f, Digest& d, std::string& error) {
    Wire w(f.data, f.size);
    Field g;
    while (w.next(g)) {
        if (g.number == 1) {
            d.id = g.value;
        } else if (g.number == 2) {
            d.physical = g.value;
        }
    }
    if (w.bad()) {
        error = "malformed fragment digest in the fragment reuse index";
        return false;
    }
    return true;
}

bool read_group(const Field& f, RawGroup& out, std::string& error) {
    Wire w(f.data, f.size);
    Field g;
    while (w.next(g)) {
        if (g.number == 1 && g.wire == 2) {
            out.changed.assign(g.data, g.data + g.size);
        } else if ((g.number == 2 || g.number == 3) && g.wire == 2) {
            Digest d;
            if (!read_digest(g, d, error)) {
                return false;
            }
            (g.number == 2 ? out.old_frags : out.new_frags).push_back(d);
        }
    }
    if (w.bad()) {
        error = "malformed group in the fragment reuse index";
        return false;
    }
    return true;
}

bool read_version(const Field& f, RawVersion& out, std::string& error) {
    Wire w(f.data, f.size);
    Field g;
    while (w.next(g)) {
        if (g.number == 1 && g.wire == 0) {
            out.dataset_version = g.value;
        } else if (g.number == 3 && g.wire == 2) {
            RawGroup group;
            if (!read_group(g, group, error)) {
                return false;
            }
            out.groups.push_back(std::move(group));
        }
    }
    if (w.bad()) {
        error = "malformed version in the fragment reuse index";
        return false;
    }
    return true;
}

/// InlineContent (`repeated Version versions = 1`).
bool read_inline(const std::uint8_t* data, std::size_t size, std::vector<RawVersion>& versions, std::string& error) {
    Wire w(data, size);
    Field f;
    while (w.next(f)) {
        if (f.number == 1 && f.wire == 2) {
            RawVersion v;
            if (!read_version(f, v, error)) {
                return false;
            }
            versions.push_back(std::move(v));
        }
    }
    if (w.bad()) {
        error = "malformed InlineContent in the fragment reuse index";
        return false;
    }
    return true;
}

/// Turns one version into a remap step: the rewritten (surviving) rows of each old fragment, and the
/// new fragments each group wrote, in Lance's rank and position order.
bool build_step(const RawVersion& raw, FragReuse::Step& step, std::string& error) {
    for (const auto& rg : raw.groups) {
        const std::size_t gi = step.groups.size();
        FragReuse::Group group;
        std::uint64_t total_new = 0;
        for (const auto& of : rg.old_frags) {
            group.old_ids.push_back(static_cast<std::uint32_t>(of.id));
        }
        for (const auto& nf : rg.new_frags) {
            group.new_ids.push_back(static_cast<std::uint32_t>(nf.id));
            if (nf.physical == 0U) {
                continue;
            }
            group.new_ranges.push_back({static_cast<std::uint32_t>(nf.id), total_new, nf.physical});
            total_new += nf.physical;
        }
        std::vector<std::uint64_t> changed;
        if (!roaring::decode_treemap(rg.changed.data(), rg.changed.size(), changed, error)) {
            error = "fragment reuse index: " + error;
            return false;
        }
        std::map<std::uint32_t, std::vector<std::uint32_t>> kept;
        for (const auto addr : changed) {
            kept[static_cast<std::uint32_t>(addr >> 32U)].push_back(static_cast<std::uint32_t>(addr));
        }
        std::uint64_t rewritten_before = 0;
        std::set<std::uint64_t> seen;
        for (const auto& of : rg.old_frags) {
            if (!seen.insert(of.id).second) {
                error = "fragment reuse index: old fragment " + std::to_string(of.id) + " listed twice";
                return false;
            }
            FragReuse::OldFragment old;
            old.group = gi;
            old.rows_before = rewritten_before;
            old.physical = of.physical;
            const auto it = kept.find(static_cast<std::uint32_t>(of.id));
            if (it != kept.end()) {
                old.kept = std::move(it->second);
                kept.erase(it);
            }
            if (!old.kept.empty() && old.kept.back() >= of.physical) {
                error = "fragment reuse index: a row beyond old fragment " + std::to_string(of.id);
                return false;
            }
            rewritten_before += old.kept.size();
            if (step.frags.count(static_cast<std::uint32_t>(of.id)) != 0U) {
                error = "fragment reuse index: old fragment " + std::to_string(of.id) + " in two groups";
                return false;
            }
            step.frags.emplace(static_cast<std::uint32_t>(of.id), std::move(old));
        }
        if (!kept.empty()) {
            error = "fragment reuse index: rewritten rows from fragments not in their group";
            return false;
        }
        if (rewritten_before != total_new) {
            error = "fragment reuse index: a group rewrote " + std::to_string(rewritten_before) +
                    " rows into fragments holding " + std::to_string(total_new);
            return false;
        }
        step.groups.push_back(std::move(group));
    }
    return true;
}

}  // namespace

bool FragReuse::from_details(const std::filesystem::path& dataset_path, const pb::IndexMetadata& index,
                             std::shared_ptr<const FragReuse>& out, std::string& error) {
    out.reset();
    const auto& url = index.details_type_url;
    if (url.size() < std::string(kFragReuseDetails).size() ||
        url.compare(url.size() - std::string(kFragReuseDetails).size(), std::string::npos, kFragReuseDetails) != 0) {
        error = "index '" + index.name + "' is not a fragment reuse index";
        return false;
    }
    // FragmentReuseIndexDetails: oneof { InlineContent inline = 1; ExternalFile external = 2; }
    Wire w(index.details_value.data(), index.details_value.size());
    Field f;
    std::vector<std::uint8_t> content;
    bool have = false;
    while (w.next(f)) {
        if (f.number == 1 && f.wire == 2) {
            content.assign(f.data, f.data + f.size);
            have = true;
        } else if (f.number == 2 && f.wire == 2) {
            // ExternalFile: path = 1, offset = 2, size = 3; the file is under _indices/<uuid>/.
            std::string path;
            std::uint64_t offset = 0;
            std::uint64_t size = 0;
            Wire e(f.data, f.size);
            Field x;
            while (e.next(x)) {
                if (x.number == 1 && x.wire == 2) {
                    path.assign(reinterpret_cast<const char*>(x.data), x.size);
                } else if (x.number == 2) {
                    offset = x.value;
                } else if (x.number == 3) {
                    size = x.value;
                }
            }
            std::vector<std::uint8_t> file;
            const auto file_path = dataset_path / "_indices" / pb::uuid_string(index.uuid) / path;
            if (e.bad() || path.empty() || !io::read_all(file_path.string(), file, error)) {
                error = error.empty() ? "fragment reuse index: unreadable external details" : error;
                return false;
            }
            if (offset > file.size() || size > file.size() - offset) {
                error = "fragment reuse index: external details out of range";
                return false;
            }
            content.assign(file.begin() + static_cast<std::ptrdiff_t>(offset),
                           file.begin() + static_cast<std::ptrdiff_t>(offset + size));
            have = true;
        }
    }
    if (w.bad() || !have) {
        error = "fragment reuse index: no content";
        return false;
    }
    std::vector<RawVersion> raw;
    if (!read_inline(content.data(), content.size(), raw, error)) {
        return false;
    }
    auto remap = std::make_shared<FragReuse>();
    for (const auto& v : raw) {
        Step step;
        if (!build_step(v, step, error)) {
            return false;
        }
        remap->steps_.push_back(std::move(step));
    }
    out = std::move(remap);
    return true;
}

bool FragReuse::load(const std::filesystem::path& dataset_path, const pb::Manifest& manifest,
                     std::shared_ptr<const FragReuse>& out, std::string& error) {
    out.reset();
    const pb::IndexMetadata* newest = nullptr;
    for (const auto& index : manifest.indices) {
        if (index.name == kFragReuseName && (newest == nullptr || index.dataset_version >= newest->dataset_version)) {
            newest = &index;
        }
    }
    if (newest == nullptr) {
        return true;
    }
    // Immutable per index (a new FRI gets a new uuid), so cached by dataset and uuid.
    static index_files::IndexCache<FragReuse> cache;
    const auto key = dataset_path.string() + "\n" + pb::uuid_string(newest->uuid);
    if ((out = cache.find(key)) != nullptr) {
        return true;
    }
    std::shared_ptr<const FragReuse> loaded;
    if (!from_details(dataset_path, *newest, loaded, error)) {
        return false;
    }
    cache.put(key, loaded);
    out = std::move(loaded);
    return true;
}

bool FragReuse::resolve(std::uint64_t address, std::uint64_t& out) const {
    std::uint64_t current = address;
    for (const auto& step : steps_) {
        const auto it = step.frags.find(static_cast<std::uint32_t>(current >> 32U));
        if (it == step.frags.end()) {
            continue;  // not rewritten by this version
        }
        const auto& of = it->second;
        const auto offset = static_cast<std::uint32_t>(current);
        if (offset >= of.physical) {
            continue;  // past the fragment's rows: no version rewrote it
        }
        const auto pos = std::lower_bound(of.kept.begin(), of.kept.end(), offset);
        if (pos == of.kept.end() || *pos != offset) {
            return false;  // deleted by the compaction
        }
        const std::uint64_t index = of.rows_before + static_cast<std::uint64_t>(pos - of.kept.begin());
        const auto& ranges = step.groups[of.group].new_ranges;
        const auto r = std::upper_bound(ranges.begin(), ranges.end(), index,
                                        [](std::uint64_t v, const NewRange& range) { return v < range.rows_before; });
        const auto& target = *(r - 1);
        current = (static_cast<std::uint64_t>(target.fragment) << 32U) | (index - target.rows_before);
    }
    out = current;
    return true;
}

void FragReuse::remap_coverage(std::vector<std::uint32_t>& ids) const {
    if (steps_.empty()) {
        return;
    }
    std::set<std::uint32_t> covered(ids.begin(), ids.end());
    for (const auto& step : steps_) {
        for (const auto& group : step.groups) {
            std::size_t removed = 0;
            for (const auto id : group.old_ids) {
                removed += covered.erase(id);
            }
            if (removed == group.old_ids.size() && removed != 0U) {
                covered.insert(group.new_ids.begin(), group.new_ids.end());  // straddling: nothing is added
            }
        }
    }
    ids.assign(covered.begin(), covered.end());
}

bool FragReuse::touches(std::uint32_t fragment) const {
    return std::any_of(steps_.begin(), steps_.end(), [&](const Step& s) { return s.frags.count(fragment) != 0U; });
}

}  // namespace nano_lance
