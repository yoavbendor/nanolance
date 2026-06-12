// M4b: PTPv2 via embed<> spec composition. Two guarantees:
//   1. PtpHeaderSpec reads the 34-byte common header byte-identically to the existing protocols::Gptp
//      overlay (same column names) — so a PTP DAG emits a Gptp-compatible table.
//   2. embed<> splices a sub-spec at an offset with a name prefix: the body Timestamp / PortIdentity /
//      ClockQuality fields land at the right wire offset, and the prefix keeps the body's requesting
//      PortIdentity distinct from the header's source clock_identity (the double-PortIdentity collision).
// Body fields are checked against direct big-endian reads of the raw bytes (the spec's whole job).

#include "nanotins/gptp.hpp"
#include "nanotins/protocol_specs_ptp.hpp"
#include "nanotins/struct_spec.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace nanotins::literals;

namespace {

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                       \
        }                                                                       \
    } while (0)

std::vector<std::uint8_t> bytes(std::size_t n) {
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = static_cast<std::uint8_t>((i * 37u + 11u) & 0xFFu);
    }
    return v;
}

std::uint64_t be_read(const std::uint8_t* p, std::size_t off, std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(p[off + i]);
    }
    return v;
}

// embed<> must flatten to the sum of the parts' field counts (header + body sub-records).
static_assert(nanotins::SyncMsgSpec::field_count ==
              nanotins::PtpHeaderSpec::field_count + nanotins::TimestampSpec::field_count);
static_assert(nanotins::DelayRespMsgSpec::field_count == nanotins::PtpHeaderSpec::field_count +
                                                             nanotins::TimestampSpec::field_count +
                                                             nanotins::PortIdentitySpec::field_count);

}  // namespace

int main() {
    // ---- (1) header parity vs protocols::Gptp overlay ----
    {
        const auto b = bytes(34);
        nanotins::struct_view<nanotins::PtpHeaderSpec> v(b.data());
        const auto* o = reinterpret_cast<const protocols::Gptp*>(b.data());
        const std::uint8_t ts = o->ts_msgtype.word_host();
        CHECK(v("transport_specific"_fld) == ((ts >> 4) & 0xF));
        CHECK(v("message_type"_fld) == (ts & 0xF));
        CHECK(v("version_ptp"_fld) == (o->rsv_version.word_host() & 0xF));
        CHECK(v("message_length"_fld) == o->message_length.host());
        CHECK(v("domain_number"_fld) == o->domain_number);
        CHECK(v("flags"_fld) == o->flags.host());
        CHECK(v("correction_field"_fld) == o->correction_field.host());
        CHECK(std::memcmp(v("clock_identity"_fld).data(), o->clock_identity.data(), 8) == 0);
        CHECK(v("source_port_number"_fld) == o->source_port_number.host());
        CHECK(v("sequence_id"_fld) == o->sequence_id.host());
        CHECK(v("control_field"_fld) == o->control_field);
        CHECK(v("log_message_interval"_fld) == o->log_message_interval);
    }

    // ---- (2) Sync body: header + origin Timestamp @34 (embed offset + prefix) ----
    {
        const auto b = bytes(44);
        nanotins::struct_view<nanotins::SyncMsgSpec> v(b.data());
        // header fields still readable through the composed spec
        CHECK(v("message_type"_fld) == (b[0] & 0xF));
        CHECK(v("sequence_id"_fld) == be_read(b.data(), 30, 2));
        // the embedded, prefixed timestamp at byte 34
        CHECK(v("origin_seconds_msb"_fld) == be_read(b.data(), 34, 2));
        CHECK(v("origin_seconds_lsb"_fld) == be_read(b.data(), 36, 4));
        CHECK(v("origin_nanoseconds"_fld) == be_read(b.data(), 40, 4));
    }

    // ---- (3) Delay_Resp: the double-PortIdentity must stay distinct via the prefix ----
    {
        const auto b = bytes(54);
        nanotins::struct_view<nanotins::DelayRespMsgSpec> v(b.data());
        CHECK(v("receive_seconds_lsb"_fld) == be_read(b.data(), 36, 4));
        // header source clock_identity @20 vs body requesting clock_identity @44 — different bytes
        CHECK(std::memcmp(v("clock_identity"_fld).data(), b.data() + 20, 8) == 0);
        CHECK(std::memcmp(v("requesting_clock_identity"_fld).data(), b.data() + 44, 8) == 0);
        CHECK(std::memcmp(v("clock_identity"_fld).data(), v("requesting_clock_identity"_fld).data(), 8) != 0);
        CHECK(v("requesting_port_number"_fld) == be_read(b.data(), 52, 2));
    }

    // ---- (4) Announce: scalars + embedded ClockQuality + fixed-size grandmaster identity ----
    {
        const auto b = bytes(64);
        nanotins::struct_view<nanotins::AnnounceMsgSpec> v(b.data());
        CHECK(v("origin_seconds_msb"_fld) == be_read(b.data(), 34, 2));
        CHECK(v("current_utc_offset"_fld) == static_cast<std::int16_t>(be_read(b.data(), 44, 2)));
        CHECK(v("grandmaster_priority1"_fld) == b[47]);
        CHECK(v("grandmaster_clock_class"_fld) == b[48]);
        CHECK(v("grandmaster_clock_accuracy"_fld) == b[49]);
        CHECK(v("grandmaster_offset_scaled_log_variance"_fld) == be_read(b.data(), 50, 2));
        CHECK(v("grandmaster_priority2"_fld) == b[52]);
        CHECK(std::memcmp(v("grandmaster_identity"_fld).data(), b.data() + 53, 8) == 0);
        CHECK(v("steps_removed"_fld) == be_read(b.data(), 61, 2));
        CHECK(v("time_source"_fld) == b[63]);
    }

    std::printf("struct_spec_ptp: ok (PtpHeaderSpec == Gptp overlay; embed<> offsets + prefixes correct)\n");
    return 0;
}
