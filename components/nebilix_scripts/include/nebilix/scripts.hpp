#pragma once

#include <cstddef>

#include "esp_err.h"

namespace nebilix::scripts {

constexpr std::size_t kMaxScriptBytes = 16 * 1024;

esp_err_t start();
esp_err_t install(const char* id, const char* source, std::size_t length,
                  char* message, std::size_t message_size);
esp_err_t remove(const char* id);
esp_err_t run(const char* id);
esp_err_t stop(const char* id);
esp_err_t info(const char* id, char* output, std::size_t size);
esp_err_t list_json(char* output, std::size_t size);
esp_err_t pins_json(char* output, std::size_t size);
esp_err_t bindings_json(const char* id, char* output, std::size_t size);
esp_err_t set_bindings_json(const char* id, const char* json,
                            char* message, std::size_t message_size);
esp_err_t assign_pin(const char* id, const char* alias, int pin);
esp_err_t command(const char* input, char* output, std::size_t size);

}  // namespace nebilix::scripts
