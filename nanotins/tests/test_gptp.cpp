// gPTP extension gate: the worked "add a protocol" example must work end-to-end with nothing but the
// nanotins core — overlay raw gPTP bytes, then drive the SAME be<>/bits<>/SoA/arrow machinery the
// built-in protocols use to produce a nanoarrow table. Proves a user can add a protocol with one struct
// and get overlay + schema + SoA for free. No Lance here: we read the in-memory Arrow array back directly.

#include "nanotins/arrow_glue.hpp"
#include "nanotins/gptp.hpp"

#include <nanoarrow/nanoarrow.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "gptp test failed: %s\n", msg);
        std::exit(1);
    }
}

int child_by_name(const ArrowSchema& s, const char* name) {
    for (std::int64_t i = 0; i < s.n_children; ++i) {
        if (s.children[i]->name && std::strcmp(s.children[i]->name, name) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// A gPTP Sync message (34-byte common header) with known field values.
std::array<std::uint8_t, 34> make_sync() {
    std::array<std::uint8_t, 34> b{};
    b[0] = (1u << 4) | protocols::kPtpMsgSync;  // transportSpecific=1, messageType=0 (Sync)
    b[1] = (0u << 4) | 2u;                       // reserved=0, versionPTP=2
    b[2] = 0x00;                                 // messageLength = 44 (be)
    b[3] = 0x2C;
    b[4] = 0x00;          // domainNumber
    b[5] = 0x00;          // reserved
    b[6] = 0x02;          // flags = 0x0200 (be) -> twoStepFlag
    b[7] = 0x00;
    // correctionField (8) + reserved2 (4) stay zero (b[8..19]).
    const std::array<std::uint8_t, 8> cid = {0xAA, 0xBB, 0xCC, 0xFF, 0xFE, 0x11, 0x22, 0x33};
    for (int i = 0; i < 8; ++i) {
        b[20 + i] = cid[i];  // clockIdentity
    }
    b[28] = 0x00;  // sourcePortNumber = 1 (be)
    b[29] = 0x01;
    b[30] = 0x04;  // sequenceId = 1234 = 0x04D2 (be)
    b[31] = 0xD2;
    b[32] = 0x00;  // controlField (Sync)
    b[33] = 0xFD;  // logMessageInterval = -3 (int8)
    return b;
}

}  // namespace

int main() {
    const auto raw = make_sync();
    const protocols::Bytes wire(raw.data(), raw.size());

    // --- 1. zero-copy overlay: read fields straight off the wire bytes ---
    protocols::Gptp g{};
    require(protocols::decode_gptp(protocols::kEtherTypePtp, wire, g), "decode_gptp should accept PTP");
    require(!protocols::decode_gptp(0x0800, wire, g), "decode_gptp must reject non-PTP ethertype");
    require((g.ts_msgtype.word_host() & 0x0F) == protocols::kPtpMsgSync, "messageType");
    require((g.rsv_version.word_host() & 0x0F) == 2, "versionPTP");
    require(g.message_length.host() == 44, "messageLength");
    require(g.flags.host() == 0x0200, "flags");
    require(g.correction_field.host() == 0, "correctionField");
    require(g.source_port_number.host() == 1, "sourcePortNumber");
    require(g.sequence_id.host() == 1234, "sequenceId");
    require(g.log_message_interval == -3, "logMessageInterval");
    require(std::memcmp(g.clock_identity.data(), raw.data() + 20, 8) == 0, "clockIdentity");

    // --- 2. the killer feature: the SAME struct drives an SoA + a nanoarrow schema/array ---
    nanotins::soa<protocols::Gptp> s;
    s.resize(2);
    s.store(0, g);
    s.store(1, g);

    ArrowSchema schema{};
    ArrowArray batch{};
    std::string err;
    require(nanotins::arrow_schema<protocols::Gptp>(schema, err), ("arrow_schema: " + err).c_str());
    require(nanotins::to_arrow<protocols::Gptp>(s, batch, err), ("to_arrow: " + err).c_str());

    // bits<> sub-fields expand to named columns; reserved* members are absent (not described).
    require(child_by_name(schema, "message_type") >= 0, "column message_type present");
    require(child_by_name(schema, "version_ptp") >= 0, "column version_ptp present");
    require(child_by_name(schema, "reserved1") < 0, "reserved members must not be columns");
    require(child_by_name(schema, "reserved2") < 0, "reserved members must not be columns");

    ArrowArrayView view{};
    ArrowError ae{};
    require(ArrowArrayViewInitFromSchema(&view, &schema, &ae) == NANOARROW_OK, "view init");
    require(ArrowArrayViewSetArray(&view, &batch, &ae) == NANOARROW_OK, "view set");

    const int c_mt = child_by_name(schema, "message_type");
    const int c_ver = child_by_name(schema, "version_ptp");
    const int c_seq = child_by_name(schema, "sequence_id");
    const int c_port = child_by_name(schema, "source_port_number");
    const int c_log = child_by_name(schema, "log_message_interval");
    const int c_cid = child_by_name(schema, "clock_identity");
    require(c_mt >= 0 && c_ver >= 0 && c_seq >= 0 && c_port >= 0 && c_log >= 0 && c_cid >= 0, "all columns");

    for (int row = 0; row < 2; ++row) {
        require(ArrowArrayViewGetUIntUnsafe(view.children[c_mt], row) == protocols::kPtpMsgSync, "col message_type");
        require(ArrowArrayViewGetUIntUnsafe(view.children[c_ver], row) == 2, "col version_ptp");
        require(ArrowArrayViewGetUIntUnsafe(view.children[c_seq], row) == 1234, "col sequence_id");
        require(ArrowArrayViewGetUIntUnsafe(view.children[c_port], row) == 1, "col source_port_number");
        require(ArrowArrayViewGetIntUnsafe(view.children[c_log], row) == -3, "col log_message_interval");
        ArrowBufferView cid = ArrowArrayViewGetBytesUnsafe(view.children[c_cid], row);
        require(cid.size_bytes == 8 && std::memcmp(cid.data.data, raw.data() + 20, 8) == 0, "col clock_identity");
    }

    ArrowArrayViewReset(&view);
    batch.release(&batch);
    schema.release(&schema);
    std::puts("nanotins gptp ok (overlay + bits/be + SoA + nanoarrow schema for a user-added protocol)");
    return 0;
}
