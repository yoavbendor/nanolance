// De-risk gate: prove boost::describe -> std adapter compiles and yields the right names/count.
#include "soatins/describe.hpp"

#include <boost/describe.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <type_traits>

namespace {

struct Toy {
    std::uint32_t a;
    std::uint16_t b;
    std::uint8_t c;
};
BOOST_DESCRIBE_STRUCT(Toy, (), (a, b, c))

void require(bool ok, const char* msg) {
    if (!ok) {
        std::fprintf(stderr, "smoke failed: %s\n", msg);
        std::exit(1);
    }
}

}  // namespace

int main() {
    require(soatins::nt_field_count<Toy> == 3, "field count");
    require(std::strcmp(soatins::nt_names<Toy>[0], "a") == 0, "name 0");
    require(std::strcmp(soatins::nt_names<Toy>[1], "b") == 0, "name 1");
    require(std::strcmp(soatins::nt_names<Toy>[2], "c") == 0, "name 2");

    Toy t{0xAABBCCDD, 0x1122, 0x7F};
    require(t.*std::get<0>(soatins::nt_members<Toy>) == 0xAABBCCDD, "member 0 ptr");
    require((std::is_same_v<soatins::nt_pointee_t<soatins::nt_member_ptr<Toy, 1>>, std::uint16_t>),
            "pointee type");
    std::puts("nanotins reflect smoke ok");
    return 0;
}
