#include "nebilix/shell.hpp"

#include <cstdio>
#include <cstring>

#include "esp_console.h"
#include "esp_netif_ip_addr.h"
#include "esp_system.h"
#include "nebilix/kernel.hpp"
#include "nebilix/wifi.hpp"
#include "nebilix/remote.hpp"
#include "nebilix/scripts.hpp"

namespace nebilix::shell {
namespace {
esp_console_repl_t* repl = nullptr;

int info_command(int, char**) {
  const auto info = kernel::system_info();
  const auto ip = wifi::address();
  std::printf("NebilixOS 0.1.0\nESP32 rev %u.%u, core: %u\nFlash: %lu byte\n"
              "Heap libero: %lu byte\nIP: " IPSTR "\n",
              info.chip_revision / 100, info.chip_revision % 100,
              info.core_count, static_cast<unsigned long>(info.flash_size_bytes),
              static_cast<unsigned long>(esp_get_free_heap_size()), IP2STR(&ip));
  return 0;
}
int uptime_command(int, char**) {
  std::printf("Uptime: %llu ms\n",
              static_cast<unsigned long long>(kernel::uptime_ms()));
  return 0;
}
int wifi_command(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "status") == 0) {
    const auto ip = wifi::address();
    std::printf("Wi-Fi state: %u, IP: " IPSTR "\n",
                static_cast<unsigned>(wifi::state()), IP2STR(&ip));
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "reset") == 0) {
    const esp_err_t result = wifi::reset_credentials();
    if (result == ESP_OK) {
      std::puts("Credenziali cancellate. Riavvio...");
      std::fflush(stdout);
      esp_restart();
    }
    std::printf("Errore: %s\n", esp_err_to_name(result));
    return 1;
  }
  std::puts("Uso: wifi status | wifi reset");
  return 1;
}
int reboot_command(int, char**) {
  std::puts("Riavvio NebilixOS...");
  std::fflush(stdout);
  esp_restart();
  return 0;
}
int remote_command(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "token") == 0) {
    std::printf("Token amministratore: %s\n", remote::administrator_token());
    return 0;
  }
  std::puts("Uso: remote token");
  return 1;
}
int script_command(int argc, char** argv) {
  char input[96]{"script"};
  for (int i = 1; i < argc; ++i) {
    std::strncat(input, " ", sizeof(input) - std::strlen(input) - 1);
    std::strncat(input, argv[i], sizeof(input) - std::strlen(input) - 1);
  }
  char output[512]{};
  const esp_err_t result = scripts::command(input, output, sizeof(output));
  std::puts(output);
  return result == ESP_OK ? 0 : 1;
}
int pin_command(int argc, char** argv) {
  char input[96]{"pin"};
  for (int i = 1; i < argc; ++i) {
    std::strncat(input, " ", sizeof(input) - std::strlen(input) - 1);
    std::strncat(input, argv[i], sizeof(input) - std::strlen(input) - 1);
  }
  char output[512]{};
  const esp_err_t result = scripts::command(input, output, sizeof(output));
  std::puts(output);
  return result == ESP_OK ? 0 : 1;
}
esp_err_t add(const char* name, const char* help, esp_console_cmd_func_t fn) {
  const esp_console_cmd_t command{.command = name, .help = help, .hint = nullptr,
                                  .func = fn, .argtable = nullptr,
                                  .func_w_context = nullptr, .context = nullptr};
  return esp_console_cmd_register(&command);
}
}  // namespace

esp_err_t start() {
  esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  config.prompt = "nebilixos> ";
  config.max_cmdline_length = 128;
  config.task_stack_size = 4096;
  const esp_console_dev_uart_config_t uart = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart, &config, &repl));
  ESP_ERROR_CHECK(esp_console_register_help_command());
  ESP_ERROR_CHECK(add("info", "Informazioni di sistema", info_command));
  ESP_ERROR_CHECK(add("uptime", "Tempo trascorso dall'avvio", uptime_command));
  ESP_ERROR_CHECK(add("wifi", "wifi status | wifi reset", wifi_command));
  ESP_ERROR_CHECK(add("reboot", "Riavvia NebilixOS", reboot_command));
  ESP_ERROR_CHECK(add("remote", "remote token", remote_command));
  ESP_ERROR_CHECK(add("script", "script list | script info|bindings|start|stop|remove <id>", script_command));
  ESP_ERROR_CHECK(add("pin", "pin list | pin assign <script> <alias> <gpio>", pin_command));
  kernel::print_banner();
  return esp_console_start_repl(repl);
}
}  // namespace nebilix::shell
