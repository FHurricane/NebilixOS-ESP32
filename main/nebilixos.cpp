#include "esp_err.h"
#include "nebilix/kernel.hpp"
#include "nebilix/shell.hpp"
#include "nebilix/remote.hpp"
#include "nebilix/scripts.hpp"

extern "C" void app_main(void) {
  ESP_ERROR_CHECK(nebilix::kernel::start());
  ESP_ERROR_CHECK(nebilix::scripts::start());
  ESP_ERROR_CHECK(nebilix::remote::start());
  ESP_ERROR_CHECK(nebilix::shell::start());
}
