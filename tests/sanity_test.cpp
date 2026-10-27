#include <catch2/catch_test_macros.hpp>

TEST_CASE("toolchain") {
    REQUIRE(sizeof(void*) == 8);
}
