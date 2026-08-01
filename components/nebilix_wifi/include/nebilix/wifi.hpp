#pragma once

#include <cstdint>

#include "esp_err.h"
#include "esp_netif_ip_addr.h"

namespace nebilix::wifi {

enum class State : std::uint8_t {
  stopped,
  provisioning,
  connecting,
  connected,
  disconnected,
  fault,
};

esp_err_t start();
esp_err_t reset_credentials();
State state();
esp_ip4_addr_t address();

}  // namespace nebilix::wifi
