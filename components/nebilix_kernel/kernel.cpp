#include "nebilix/kernel.hpp"
#include "nebilix/wifi.hpp"

#include <atomic>
#include <cinttypes>
#include <cstdio>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace nebilix::kernel {
namespace {

constexpr char kLogTag[] = "nebilix.kernel";
constexpr char kVersion[] = "0.1.0";
constexpr TickType_t kSupervisorPeriod = pdMS_TO_TICKS(5000);
constexpr std::uint32_t kSupervisorStackSize = 3072;
constexpr UBaseType_t kSupervisorPriority = 2;

std::atomic<State> current_state{State::stopped};
SystemInfo current_system_info{};
std::int64_t boot_time_us = 0;

constexpr char kBanner[] = R"banner(
 _   _      _     _ _ _       ___  ____
| \ | | ___| |__ (_) (_)_  __/ _ \/ ___|
|  \| |/ _ \ '_ \| | | \ \/ / | | \___ \
| |\  |  __/ |_) | | | |>  <| |_| |___) |
|_| \_|\___|_.__/|_|_/_/ /_\\___/|____/

NebilixOS 0.1.0 - Embedded Script Operating System
Developed by Costa Fabio
Copyright (c) 2026 Costa Fabio
Licensed under the Apache License 2.0
)banner";

void supervisor_task(void*) {
  while (true) {
    vTaskDelay(kSupervisorPeriod);
    current_system_info.free_heap_bytes = esp_get_free_heap_size();

    ESP_LOGI(kLogTag,
             "uptime=%" PRIu64 " ms, free_heap=%" PRIu32
             " bytes, supervisor_stack=%u words",
             uptime_ms(), current_system_info.free_heap_bytes,
             uxTaskGetStackHighWaterMark(nullptr));
  }
}

esp_err_t detect_hardware() {
  esp_chip_info_t chip{};
  esp_chip_info(&chip);

  std::uint32_t flash_size = 0;
  const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_size);
  if (flash_result != ESP_OK) {
    return flash_result;
  }

  current_system_info = {
      .chip_revision = static_cast<std::uint16_t>(chip.revision),
      .core_count = chip.cores,
      .flash_size_bytes = flash_size,
      .free_heap_bytes = esp_get_free_heap_size(),
      .has_wifi = (chip.features & CHIP_FEATURE_WIFI_BGN) != 0,
      .has_bluetooth =
          (chip.features & (CHIP_FEATURE_BT | CHIP_FEATURE_BLE)) != 0,
  };
  return ESP_OK;
}

}  // namespace

esp_err_t start() {
  State expected = State::stopped;
  if (!current_state.compare_exchange_strong(expected, State::booting)) {
    return ESP_ERR_INVALID_STATE;
  }

  boot_time_us = esp_timer_get_time();
  const esp_err_t hardware_result = detect_hardware();
  if (hardware_result != ESP_OK) {
    current_state.store(State::fault);
    return hardware_result;
  }

  print_banner();
  ESP_LOGI(kLogTag, "NebilixOS kernel %s booting", kVersion);
  ESP_LOGI(kLogTag,
           "ESP32 revision %u.%u, cores=%u, flash=%" PRIu32
           " bytes, Wi-Fi=%s, Bluetooth=%s",
           current_system_info.chip_revision / 100,
           current_system_info.chip_revision % 100,
           current_system_info.core_count, current_system_info.flash_size_bytes,
           current_system_info.has_wifi ? "yes" : "no",
           current_system_info.has_bluetooth ? "yes" : "no");

  const BaseType_t task_result = xTaskCreatePinnedToCore(
      supervisor_task, "nbx_supervisor", kSupervisorStackSize, nullptr,
      kSupervisorPriority, nullptr, 0);
  if (task_result != pdPASS) {
    current_state.store(State::fault);
    return ESP_ERR_NO_MEM;
  }

  current_state.store(State::running);
  ESP_LOGI(kLogTag, "Kernel state: RUNNING");

  const esp_err_t wifi_result = wifi::start();
  if (wifi_result != ESP_OK && wifi_result != ESP_ERR_NOT_SUPPORTED) {
    ESP_LOGE(kLogTag, "Wi-Fi service failed to start: %s",
             esp_err_to_name(wifi_result));
    return wifi_result;
  }
  return ESP_OK;
}

void print_banner() { printf("%s\n", kBanner); }

State state() { return current_state.load(); }

SystemInfo system_info() { return current_system_info; }

std::uint64_t uptime_ms() {
  if (boot_time_us == 0) {
    return 0;
  }
  return static_cast<std::uint64_t>((esp_timer_get_time() - boot_time_us) / 1000);
}

}  // namespace nebilix::kernel
