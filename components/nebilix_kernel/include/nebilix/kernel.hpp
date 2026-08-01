#pragma once

#include <cstdint>

#include "esp_err.h"

namespace nebilix::kernel {

enum class State : std::uint8_t {
  stopped,
  booting,
  running,
  fault,
};

struct SystemInfo {
  std::uint16_t chip_revision;
  std::uint8_t core_count;
  std::uint32_t flash_size_bytes;
  std::uint32_t free_heap_bytes;
  bool has_wifi;
  bool has_bluetooth;
};

esp_err_t start();
void print_banner();
State state();
SystemInfo system_info();
std::uint64_t uptime_ms();

}  // namespace nebilix::kernel
