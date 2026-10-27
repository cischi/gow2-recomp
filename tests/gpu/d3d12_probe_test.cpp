// gpu::FindHardwareAdapter (runtime/gpu/d3d12_probe.h): runs even without a
// GPU, does not throw and does not crash; if it finds an adapter it gives its name.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "gpu/d3d12_probe.h"

TEST_CASE("d3d12_probe: FindHardwareAdapter does not throw or crash", "[d3d12_probe]") {
  std::optional<std::string> adapter;
  REQUIRE_NOTHROW(adapter = gpu::FindHardwareAdapter());
  if (adapter) {
    INFO("adapter: " << *adapter);
    CHECK(!adapter->empty());
    // same answer to a second call (the DLLs are reloaded)
    CHECK(gpu::FindHardwareAdapter() == adapter);
  } else {
    WARN("no hardware D3D12 adapter");
  }
}
