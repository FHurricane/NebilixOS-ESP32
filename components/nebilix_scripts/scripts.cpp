#include "nebilix/scripts.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace nebilix::scripts {
namespace {
constexpr char kTag[] = "nebilix.scripts";
constexpr char kRoot[] = "/scripts";
constexpr std::size_t kMaxId = 31;
constexpr int kMaxRunning = 2;
constexpr int kMaxLines = 256;
constexpr int kMaxRepeatDepth = 4;
constexpr int kMaxPinsPerScript = 8;

enum class PinMode : std::uint8_t { input, output };

struct PinRequirement {
  char alias[16]{};
  PinMode mode{PinMode::input};
};

struct Binding {
  char alias[16]{};
  int pin{-1};
};

struct Manifest {
  char id[32]{};
  char name[48]{};
  char version[16]{"1.0.0"};
  bool autostart{false};
  bool allowed_gpio[GPIO_NUM_MAX]{};  // Compatibility with NBX v1 fixed pins.
  PinRequirement requirements[kMaxPinsPerScript]{};
  int requirement_count{};
};

struct Slot {
  TaskHandle_t task{};
  volatile bool stopping{};
  char id[32]{};
  Binding bindings[kMaxPinsPerScript]{};
  int binding_count{};
};

Slot slots[kMaxRunning]{};
int pin_owner[GPIO_NUM_MAX]{};
portMUX_TYPE slots_lock = portMUX_INITIALIZER_UNLOCKED;

char* trim(char* text) {
  while (*text && std::isspace(static_cast<unsigned char>(*text))) ++text;
  char* end = text + std::strlen(text);
  while (end > text && std::isspace(static_cast<unsigned char>(end[-1]))) --end;
  *end = '\0';
  return text;
}

bool valid_id(const char* id) {
  if (!id || !*id || std::strlen(id) > kMaxId) return false;
  for (const char* p = id; *p; ++p) {
    if (!(std::islower(static_cast<unsigned char>(*p)) ||
          std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '_')) return false;
  }
  return true;
}

bool valid_alias(const char* alias) {
  if (!alias || !*alias || std::strlen(alias) > 15 || !std::islower(static_cast<unsigned char>(*alias)))
    return false;
  for (const char* p = alias; *p; ++p) {
    if (!(std::islower(static_cast<unsigned char>(*p)) ||
          std::isdigit(static_cast<unsigned char>(*p)) || *p == '_')) return false;
  }
  return true;
}

bool safe_metadata(const char* value) {
  if (!value || !*value) return false;
  for (const char* p = value; *p; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (!(std::isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.')) return false;
  }
  return true;
}

bool usable_pin(int pin) {
  return pin >= 0 && pin < GPIO_NUM_MAX && GPIO_IS_VALID_GPIO(pin) &&
         pin != 1 && pin != 3 && pin != 20 && pin != 24 &&
         pin != 28 && pin != 29 && pin != 30 && pin != 31 &&
         pin != 37 && pin != 38 && !(pin >= 6 && pin <= 11);
}
bool supports_output(int pin) { return usable_pin(pin) && GPIO_IS_VALID_OUTPUT_GPIO(pin); }
bool strapping_pin(int pin) { return pin == 0 || pin == 2 || pin == 5 || pin == 12 || pin == 15; }

void source_path(const char* id, char* path, std::size_t size) {
  std::snprintf(path, size, "%s/%s.nbx", kRoot, id);
}

void bindings_path(const char* id, char* path, std::size_t size) {
  std::snprintf(path, size, "%s/%s.pins", kRoot, id);
}

const PinRequirement* find_requirement(const Manifest& manifest, const char* alias) {
  for (int i = 0; i < manifest.requirement_count; ++i)
    if (std::strcmp(manifest.requirements[i].alias, alias) == 0) return &manifest.requirements[i];
  return nullptr;
}

int binding_pin(const Binding* bindings, int count, const char* alias) {
  for (int i = 0; i < count; ++i) if (std::strcmp(bindings[i].alias, alias) == 0) return bindings[i].pin;
  return -1;
}

bool parse_pin_mode(const char* text, PinMode& mode) {
  if (std::strcmp(text, "input") == 0) { mode = PinMode::input; return true; }
  if (std::strcmp(text, "output") == 0) { mode = PinMode::output; return true; }
  return false;
}

bool numeric_reference(const char* reference, int& pin) {
  if (!reference || !*reference) return false;
  char* end = nullptr;
  const long value = std::strtol(reference, &end, 10);
  if (!end || *end) return false;
  pin = static_cast<int>(value);
  return true;
}

esp_err_t validate_reference(const Manifest& manifest, const char* reference, PinMode operation,
                             int line, char* error, std::size_t error_size) {
  int pin = -1;
  if (numeric_reference(reference, pin)) {
    if (!usable_pin(pin) || !manifest.allowed_gpio[pin] ||
        (operation == PinMode::output && !supports_output(pin))) {
      std::snprintf(error, error_size, "Riga %d: GPIO fissa non consentita", line);
      return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
  }
  const PinRequirement* requirement = find_requirement(manifest, reference);
  if (!requirement || (operation == PinMode::output && requirement->mode != PinMode::output)) {
    std::snprintf(error, error_size, "Riga %d: pin logico '%s' non dichiarato", line, reference);
    return ESP_ERR_INVALID_ARG;
  }
  return ESP_OK;
}

esp_err_t parse_manifest(char* source, Manifest& manifest, char* error, std::size_t error_size) {
  int repeat_depth = 0;
  int line_number = 0;
  bool has_header = false;
  char* save = nullptr;
  for (char* raw = strtok_r(source, "\n", &save); raw; raw = strtok_r(nullptr, "\n", &save)) {
    ++line_number;
    if (line_number > kMaxLines) {
      std::snprintf(error, error_size, "Troppe righe (max %d)", kMaxLines);
      return ESP_ERR_INVALID_SIZE;
    }
    char* line = trim(raw);
    if (!*line) continue;
    if (*line == '#') {
      line = trim(line + 1);
      if (std::strcmp(line, "nebilix-script:1") == 0) has_header = true;
      else if (std::strncmp(line, "id=", 3) == 0)
        std::snprintf(manifest.id, sizeof(manifest.id), "%s", line + 3);
      else if (std::strncmp(line, "name=", 5) == 0)
        std::snprintf(manifest.name, sizeof(manifest.name), "%s", line + 5);
      else if (std::strncmp(line, "version=", 8) == 0)
        std::snprintf(manifest.version, sizeof(manifest.version), "%s", line + 8);
      else if (std::strncmp(line, "autostart=", 10) == 0)
        manifest.autostart = std::strcmp(line + 10, "true") == 0 || std::strcmp(line + 10, "1") == 0;
      else if (std::strncmp(line, "pin.", 4) == 0) {
        char* equals = std::strchr(line + 4, '=');
        if (!equals || manifest.requirement_count >= kMaxPinsPerScript) {
          std::snprintf(error, error_size, "Dichiarazione pin non valida"); return ESP_ERR_INVALID_ARG;
        }
        *equals = '\0';
        const char* alias = line + 4;
        PinMode mode{};
        if (!valid_alias(alias) || !parse_pin_mode(equals + 1, mode) || find_requirement(manifest, alias)) {
          std::snprintf(error, error_size, "Pin logico '%s' non valido", alias); return ESP_ERR_INVALID_ARG;
        }
        auto& requirement = manifest.requirements[manifest.requirement_count++];
        std::snprintf(requirement.alias, sizeof(requirement.alias), "%s", alias);
        requirement.mode = mode;
      } else if (std::strncmp(line, "gpio=", 5) == 0) {
        char* pin_save = nullptr;
        for (char* item = strtok_r(line + 5, ",", &pin_save); item;
             item = strtok_r(nullptr, ",", &pin_save)) {
          const int pin = std::atoi(trim(item));
          if (!usable_pin(pin)) {
            std::snprintf(error, error_size, "GPIO %d non consentita", pin); return ESP_ERR_INVALID_ARG;
          }
          manifest.allowed_gpio[pin] = true;
        }
      }
      continue;
    }

    char reference[16]{}, value[16]{};
    int number = 0;
    if (std::sscanf(line, "gpio.mode %15s %15s", reference, value) == 2) {
      PinMode mode{};
      if (!parse_pin_mode(value, mode) ||
          validate_reference(manifest, reference, mode, line_number, error, error_size) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    } else if (std::sscanf(line, "gpio.write %15s %15s", reference, value) == 2) {
      if ((std::strcmp(value, "high") && std::strcmp(value, "low")) ||
          validate_reference(manifest, reference, PinMode::output, line_number, error, error_size) != ESP_OK)
        return ESP_ERR_INVALID_ARG;
    } else if (std::sscanf(line, "sleep %d", &number) == 1) {
      if (number < 1 || number > 60000) {
        std::snprintf(error, error_size, "Riga %d: sleep 1..60000 ms", line_number); return ESP_ERR_INVALID_ARG;
      }
    } else if (std::sscanf(line, "repeat %d", &number) == 1) {
      if (number < 0 || number > 100000 || ++repeat_depth > kMaxRepeatDepth) {
        std::snprintf(error, error_size, "Riga %d: repeat non valido", line_number); return ESP_ERR_INVALID_ARG;
      }
    } else if (std::strcmp(line, "end") == 0) {
      if (--repeat_depth < 0) {
        std::snprintf(error, error_size, "Riga %d: end senza repeat", line_number); return ESP_ERR_INVALID_ARG;
      }
    } else if (std::strncmp(line, "log ", 4) != 0) {
      std::snprintf(error, error_size, "Riga %d: istruzione sconosciuta", line_number); return ESP_ERR_INVALID_ARG;
    }
  }
  if (!has_header || !valid_id(manifest.id) || repeat_depth != 0 ||
      (*manifest.name && !safe_metadata(manifest.name)) || !safe_metadata(manifest.version)) {
    std::snprintf(error, error_size, "Manifest o blocchi repeat non validi"); return ESP_ERR_INVALID_ARG;
  }
  if (!*manifest.name) std::snprintf(manifest.name, sizeof(manifest.name), "%s", manifest.id);
  return ESP_OK;
}

esp_err_t load_source(const char* id, char*& source, std::size_t& length) {
  char path[64]; source_path(id, path, sizeof(path));
  FILE* file = std::fopen(path, "rb");
  if (!file) return ESP_ERR_NOT_FOUND;
  std::fseek(file, 0, SEEK_END); length = std::ftell(file); std::rewind(file);
  if (!length || length > kMaxScriptBytes) { std::fclose(file); return ESP_ERR_INVALID_SIZE; }
  source = static_cast<char*>(std::malloc(length + 1));
  if (!source) { std::fclose(file); return ESP_ERR_NO_MEM; }
  const auto read = std::fread(source, 1, length, file); std::fclose(file); source[read] = '\0';
  if (read != length) { std::free(source); source = nullptr; return ESP_FAIL; }
  return ESP_OK;
}

esp_err_t read_manifest(const char* id, Manifest& manifest) {
  char* source = nullptr; std::size_t length = 0;
  ESP_RETURN_ON_ERROR(load_source(id, source, length), kTag, "load script");
  char error[96]{}; const esp_err_t result = parse_manifest(source, manifest, error, sizeof(error));
  std::free(source); return result;
}

esp_err_t load_bindings(const char* id, const Manifest& manifest, Binding* bindings,
                        int& count, bool require_all) {
  count = 0;
  if (manifest.requirement_count == 0) return ESP_OK;
  char path[64]; bindings_path(id, path, sizeof(path));
  FILE* file = std::fopen(path, "r");
  if (!file) return require_all ? ESP_ERR_NOT_FOUND : ESP_OK;
  char line[48]{};
  while (std::fgets(line, sizeof(line), file) && count < kMaxPinsPerScript) {
    char alias[16]{}; int pin = -1;
    if (std::sscanf(line, "%15[a-z0-9_]=%d", alias, &pin) != 2) continue;
    const PinRequirement* requirement = find_requirement(manifest, alias);
    if (!requirement || !usable_pin(pin) ||
        (requirement->mode == PinMode::output && !supports_output(pin)) ||
        binding_pin(bindings, count, alias) >= 0) continue;
    bool duplicate_pin = false;
    for (int i = 0; i < count; ++i) if (bindings[i].pin == pin) duplicate_pin = true;
    if (duplicate_pin) continue;
    std::snprintf(bindings[count].alias, sizeof(bindings[count].alias), "%s", alias);
    bindings[count++].pin = pin;
  }
  std::fclose(file);
  if (require_all) {
    for (int i = 0; i < manifest.requirement_count; ++i)
      if (binding_pin(bindings, count, manifest.requirements[i].alias) < 0) return ESP_ERR_INVALID_STATE;
  }
  return ESP_OK;
}

esp_err_t save_bindings(const char* id, const Binding* bindings, int count) {
  char path[64], temporary[68], backup[68]; bindings_path(id, path, sizeof(path));
  std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  std::snprintf(backup, sizeof(backup), "%s.bak", path);
  FILE* file = std::fopen(temporary, "w");
  if (!file) return ESP_FAIL;
  bool ok = true;
  for (int i = 0; i < count; ++i)
    if (std::fprintf(file, "%s=%d\n", bindings[i].alias, bindings[i].pin) < 0) ok = false;
  std::fflush(file); fsync(fileno(file)); std::fclose(file);
  if (!ok) { unlink(temporary); return ESP_FAIL; }
  unlink(backup);
  const bool had_previous = std::rename(path, backup) == 0;
  if (std::rename(temporary, path) != 0) {
    if (had_previous) std::rename(backup, path);
    unlink(temporary);
    return ESP_FAIL;
  }
  unlink(backup);
  return ESP_OK;
}

Slot* find_slot(const char* id) {
  for (auto& slot : slots) if (*slot.id && std::strcmp(slot.id, id) == 0) return &slot;
  return nullptr;
}

int resolve_pin(const Slot* slot, const char* reference) {
  int pin = -1;
  return numeric_reference(reference, pin) ? pin : binding_pin(slot->bindings, slot->binding_count, reference);
}

void release_slot(Slot* slot) {
  taskENTER_CRITICAL(&slots_lock);
  const int index = static_cast<int>(slot - slots);
  for (int i = 0; i < slot->binding_count; ++i)
    if (slot->bindings[i].pin >= 0 && pin_owner[slot->bindings[i].pin] == index)
      pin_owner[slot->bindings[i].pin] = -1;
  slot->task = nullptr; slot->stopping = false; slot->id[0] = '\0'; slot->binding_count = 0;
  taskEXIT_CRITICAL(&slots_lock);
}

void execute_task(void* context) {
  auto* slot = static_cast<Slot*>(context);
  char* source = nullptr; std::size_t length = 0;
  if (load_source(slot->id, source, length) != ESP_OK) goto done;
  {
    char* lines[kMaxLines]{}; int line_count = 0; char* save = nullptr;
    for (char* raw = strtok_r(source, "\n", &save); raw && line_count < kMaxLines;
         raw = strtok_r(nullptr, "\n", &save)) {
      char* line = trim(raw); if (*line && *line != '#') lines[line_count++] = line;
    }
    struct Repeat { int start; int remaining; } repeats[kMaxRepeatDepth]{};
    int depth = 0;
    for (int pc = 0; pc < line_count && !slot->stopping; ++pc) {
      char reference[16]{}, value[16]{}; int count = 0;
      if (std::sscanf(lines[pc], "gpio.mode %15s %15s", reference, value) == 2) {
        const int pin = resolve_pin(slot, reference);
        gpio_set_direction(static_cast<gpio_num_t>(pin),
            std::strcmp(value, "output") == 0 ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT);
      } else if (std::sscanf(lines[pc], "gpio.write %15s %15s", reference, value) == 2) {
        gpio_set_level(static_cast<gpio_num_t>(resolve_pin(slot, reference)),
                       std::strcmp(value, "high") == 0);
      } else if (std::sscanf(lines[pc], "sleep %d", &count) == 1) {
        for (int left = count; left > 0 && !slot->stopping;) {
          const int slice = left > 100 ? 100 : left; vTaskDelay(pdMS_TO_TICKS(slice)); left -= slice;
        }
      } else if (std::sscanf(lines[pc], "repeat %d", &count) == 1) {
        repeats[depth++] = {.start = pc + 1, .remaining = count};
      } else if (std::strcmp(lines[pc], "end") == 0 && depth > 0) {
        auto& repeat = repeats[depth - 1];
        if (repeat.remaining == 0 || --repeat.remaining > 0) pc = repeat.start - 1; else --depth;
      } else if (std::strncmp(lines[pc], "log ", 4) == 0) {
        ESP_LOGI(kTag, "[%s] %s", slot->id, lines[pc] + 4);
      }
      vTaskDelay(1);
    }
  }
  std::free(source);
done:
  for (int i = 0; i < slot->binding_count; ++i) gpio_reset_pin(static_cast<gpio_num_t>(slot->bindings[i].pin));
  ESP_LOGI(kTag, "Script '%s' stopped", slot->id); release_slot(slot); vTaskDelete(nullptr);
}

bool parse_bindings_json(const char* json, const Manifest& manifest, Binding* bindings,
                         int& count, char* error, std::size_t error_size) {
  count = 0; const char* cursor = json;
  while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (*cursor++ != '{') return false;
  while (true) {
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    if (*cursor == '}') { ++cursor; break; }
    if (*cursor++ != '"' || count >= kMaxPinsPerScript) return false;
    char alias[16]{}; int length = 0;
    while (*cursor && *cursor != '"' && length < 15) alias[length++] = *cursor++;
    if (*cursor++ != '"') return false;
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    if (*cursor++ != ':') return false;
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    char* end = nullptr; const long value = std::strtol(cursor, &end, 10);
    if (end == cursor) return false;
    cursor = end;
    const PinRequirement* requirement = find_requirement(manifest, alias);
    const int pin = static_cast<int>(value);
    if (!requirement || !usable_pin(pin) ||
        (requirement->mode == PinMode::output && !supports_output(pin)) ||
        binding_pin(bindings, count, alias) >= 0) {
      std::snprintf(error, error_size, "Binding %s=%d non valido", alias, pin); return false;
    }
    for (int i = 0; i < count; ++i) if (bindings[i].pin == pin) {
      std::snprintf(error, error_size, "GPIO %d assegnata due volte", pin); return false;
    }
    std::snprintf(bindings[count].alias, sizeof(bindings[count].alias), "%s", alias);
    bindings[count++].pin = pin;
    while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
    if (*cursor == ',') { ++cursor; continue; }
    if (*cursor == '}') { ++cursor; break; }
    return false;
  }
  while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
  if (*cursor) return false;
  for (int i = 0; i < manifest.requirement_count; ++i)
    if (binding_pin(bindings, count, manifest.requirements[i].alias) < 0) {
      std::snprintf(error, error_size, "Manca il pin %s", manifest.requirements[i].alias); return false;
    }
  return true;
}
}  // namespace

esp_err_t start() {
  for (int& owner : pin_owner) owner = -1;
  const esp_vfs_spiffs_conf_t config{.base_path = kRoot, .partition_label = "scripts",
                                     .max_files = 10, .format_if_mount_failed = true};
  ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&config), kTag, "mount scripts partition");
  std::size_t total = 0, used = 0;
  ESP_RETURN_ON_ERROR(esp_spiffs_info("scripts", &total, &used), kTag, "read scripts storage");
  ESP_LOGI(kTag, "Script storage ready: %u/%u bytes used", static_cast<unsigned>(used),
           static_cast<unsigned>(total));
  DIR* directory = opendir(kRoot);
  if (directory) {
    for (dirent* entry = readdir(directory); entry; entry = readdir(directory)) {
      const char* suffix = std::strstr(entry->d_name, ".nbx");
      if (!suffix || suffix[4]) continue;
      char id[32]{}; const std::size_t length = suffix - entry->d_name;
      if (length >= sizeof(id)) continue;
      std::memcpy(id, entry->d_name, length);
      Manifest manifest{};
      if (read_manifest(id, manifest) == ESP_OK && manifest.autostart)
        ESP_LOGI(kTag, "Autostart '%s': %s", id, esp_err_to_name(run(id)));
    }
    closedir(directory);
  }
  return ESP_OK;
}

esp_err_t install(const char* id, const char* source, std::size_t length,
                  char* message, std::size_t message_size) {
  if (!valid_id(id) || !source || !length || length > kMaxScriptBytes) return ESP_ERR_INVALID_ARG;
  if (find_slot(id)) return ESP_ERR_INVALID_STATE;
  char* copy = static_cast<char*>(std::malloc(length + 1));
  if (!copy) return ESP_ERR_NO_MEM;
  std::memcpy(copy, source, length); copy[length] = '\0';
  Manifest manifest{}; char error[128]{};
  esp_err_t result = parse_manifest(copy, manifest, error, sizeof(error)); std::free(copy);
  if (result != ESP_OK || std::strcmp(id, manifest.id) != 0) {
    std::snprintf(message, message_size, "%s", result == ESP_OK ? "ID manifest diverso dall'URL" : error);
    return result == ESP_OK ? ESP_ERR_INVALID_ARG : result;
  }
  char path[64], temporary[68], backup[68]; source_path(id, path, sizeof(path));
  std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  std::snprintf(backup, sizeof(backup), "%s.bak", path);
  FILE* file = std::fopen(temporary, "wb"); if (!file) return ESP_FAIL;
  const bool written = std::fwrite(source, 1, length, file) == length;
  std::fflush(file); fsync(fileno(file)); std::fclose(file);
  if (!written) { unlink(temporary); return ESP_FAIL; }
  unlink(backup);
  const bool had_previous = std::rename(path, backup) == 0;
  if (std::rename(temporary, path) != 0) {
    if (had_previous) std::rename(backup, path);
    unlink(temporary);
    return ESP_FAIL;
  }
  unlink(backup);
  char pin_path[64]; bindings_path(id, pin_path, sizeof(pin_path)); unlink(pin_path);
  std::snprintf(message, message_size, "Script %s %s installato; configura %d pin",
                manifest.name, manifest.version, manifest.requirement_count);
  return ESP_OK;
}

esp_err_t remove(const char* id) {
  if (!valid_id(id) || find_slot(id)) return ESP_ERR_INVALID_STATE;
  char path[64]; source_path(id, path, sizeof(path));
  if (unlink(path) != 0) return ESP_ERR_NOT_FOUND;
  bindings_path(id, path, sizeof(path)); unlink(path); return ESP_OK;
}

esp_err_t run(const char* id) {
  if (!valid_id(id) || find_slot(id)) return ESP_ERR_INVALID_STATE;
  Manifest manifest{}; ESP_RETURN_ON_ERROR(read_manifest(id, manifest), kTag, "validate script");
  Binding bindings[kMaxPinsPerScript]{}; int binding_count = 0;
  ESP_RETURN_ON_ERROR(load_bindings(id, manifest, bindings, binding_count, true), kTag, "load pin bindings");

  Slot* free_slot = nullptr; int slot_index = -1;
  taskENTER_CRITICAL(&slots_lock);
  for (int i = 0; i < kMaxRunning; ++i) if (!slots[i].task && !*slots[i].id) {
    free_slot = &slots[i]; slot_index = i; break;
  }
  if (free_slot) {
    for (int i = 0; i < binding_count; ++i) if (pin_owner[bindings[i].pin] >= 0) free_slot = nullptr;
  }
  if (free_slot) {
    std::snprintf(free_slot->id, sizeof(free_slot->id), "%s", id);
    free_slot->binding_count = binding_count; free_slot->stopping = false;
    for (int i = 0; i < binding_count; ++i) {
      free_slot->bindings[i] = bindings[i]; pin_owner[bindings[i].pin] = slot_index;
    }
  }
  taskEXIT_CRITICAL(&slots_lock);
  if (!free_slot) return ESP_ERR_INVALID_STATE;
  if (xTaskCreate(execute_task, "nbx_script", 4096, free_slot, 3, &free_slot->task) != pdPASS) {
    release_slot(free_slot); return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(kTag, "Script '%s' started with %d pin bindings", id, binding_count); return ESP_OK;
}

esp_err_t stop(const char* id) {
  Slot* slot = find_slot(id); if (!slot) return ESP_ERR_NOT_FOUND; slot->stopping = true; return ESP_OK;
}

esp_err_t info(const char* id, char* output, std::size_t size) {
  Manifest manifest{}; ESP_RETURN_ON_ERROR(read_manifest(id, manifest), kTag, "read manifest");
  std::snprintf(output, size, "id=%s name=%s version=%s running=%s pins=%d autostart=%s",
      manifest.id, manifest.name, manifest.version, find_slot(id) ? "yes" : "no",
      manifest.requirement_count, manifest.autostart ? "yes" : "no"); return ESP_OK;
}

esp_err_t list_json(char* output, std::size_t size) {
  if (!output || size < 3) return ESP_ERR_INVALID_ARG;
  std::size_t used = std::snprintf(output, size, "{\"scripts\":[");
  DIR* directory = opendir(kRoot); if (!directory) return ESP_FAIL; bool first = true;
  for (dirent* entry = readdir(directory); entry; entry = readdir(directory)) {
    const char* suffix = std::strstr(entry->d_name, ".nbx"); if (!suffix || suffix[4]) continue;
    char id[32]{}; const std::size_t len = suffix - entry->d_name;
    if (len >= sizeof(id)) continue;
    std::memcpy(id, entry->d_name, len);
    Manifest manifest{}; if (read_manifest(id, manifest) != ESP_OK) continue;
    Binding bindings[kMaxPinsPerScript]{}; int count = 0;
    const bool configured = load_bindings(id, manifest, bindings, count, true) == ESP_OK;
    const int added = std::snprintf(output + used, size - used,
        "%s{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"running\":%s,\"configured\":%s,\"pinCount\":%d}",
        first ? "" : ",", manifest.id, manifest.name, manifest.version,
        find_slot(id) ? "true" : "false", configured ? "true" : "false", manifest.requirement_count);
    if (added < 0 || static_cast<std::size_t>(added) >= size - used) { closedir(directory); return ESP_ERR_INVALID_SIZE; }
    used += added; first = false;
  }
  closedir(directory); if (used + 3 > size) return ESP_ERR_INVALID_SIZE;
  std::snprintf(output + used, size - used, "]}"); return ESP_OK;
}

esp_err_t pins_json(char* output, std::size_t size) {
  if (!output || size < 3) return ESP_ERR_INVALID_ARG;
  std::size_t used = std::snprintf(output, size, "{\"pins\":["); bool first = true;
  for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
    if (!usable_pin(pin)) continue;
    const int owner = pin_owner[pin];
    const char* owner_id = owner >= 0 && owner < kMaxRunning ? slots[owner].id : "";
    const int added = std::snprintf(output + used, size - used,
        "%s{\"gpio\":%d,\"input\":true,\"output\":%s,\"strapping\":%s,\"owner\":%s%s%s}",
        first ? "" : ",", pin, supports_output(pin) ? "true" : "false",
        strapping_pin(pin) ? "true" : "false", *owner_id ? "\"" : "null",
        *owner_id ? owner_id : "", *owner_id ? "\"" : "");
    if (added < 0 || static_cast<std::size_t>(added) >= size - used) return ESP_ERR_INVALID_SIZE;
    used += added; first = false;
  }
  if (used + 3 > size) return ESP_ERR_INVALID_SIZE;
  std::snprintf(output + used, size - used, "]}"); return ESP_OK;
}

esp_err_t bindings_json(const char* id, char* output, std::size_t size) {
  Manifest manifest{}; ESP_RETURN_ON_ERROR(read_manifest(id, manifest), kTag, "read manifest");
  Binding bindings[kMaxPinsPerScript]{}; int count = 0;
  load_bindings(id, manifest, bindings, count, false);
  std::size_t used = std::snprintf(output, size, "{\"id\":\"%s\",\"running\":%s,\"requirements\":[",
                                   id, find_slot(id) ? "true" : "false");
  for (int i = 0; i < manifest.requirement_count; ++i) {
    const int pin = binding_pin(bindings, count, manifest.requirements[i].alias);
    char gpio[12]{};
    if (pin < 0) std::snprintf(gpio, sizeof(gpio), "null");
    else std::snprintf(gpio, sizeof(gpio), "%d", pin);
    const int added = std::snprintf(output + used, size - used,
        "%s{\"alias\":\"%s\",\"mode\":\"%s\",\"gpio\":%s}", i ? "," : "",
        manifest.requirements[i].alias,
        manifest.requirements[i].mode == PinMode::output ? "output" : "input",
        gpio);
    if (added < 0 || static_cast<std::size_t>(added) >= size - used) return ESP_ERR_INVALID_SIZE;
    used += added;
  }
  if (used + 3 > size) return ESP_ERR_INVALID_SIZE;
  std::snprintf(output + used, size - used, "]}"); return ESP_OK;
}

esp_err_t set_bindings_json(const char* id, const char* json, char* message, std::size_t message_size) {
  if (!valid_id(id) || !json || find_slot(id)) return ESP_ERR_INVALID_STATE;
  Manifest manifest{}; ESP_RETURN_ON_ERROR(read_manifest(id, manifest), kTag, "read manifest");
  Binding bindings[kMaxPinsPerScript]{}; int count = 0;
  if (!parse_bindings_json(json, manifest, bindings, count, message, message_size)) {
    if (!*message) std::snprintf(message, message_size, "JSON binding non valido");
    return ESP_ERR_INVALID_ARG;
  }
  ESP_RETURN_ON_ERROR(save_bindings(id, bindings, count), kTag, "save bindings");
  std::snprintf(message, message_size, "%d pin assegnati a %s", count, id); return ESP_OK;
}

esp_err_t assign_pin(const char* id, const char* alias, int pin) {
  if (!valid_id(id) || !valid_alias(alias) || find_slot(id)) return ESP_ERR_INVALID_ARG;
  Manifest manifest{}; ESP_RETURN_ON_ERROR(read_manifest(id, manifest), kTag, "read manifest");
  const PinRequirement* requirement = find_requirement(manifest, alias);
  if (!requirement || !usable_pin(pin) || (requirement->mode == PinMode::output && !supports_output(pin)))
    return ESP_ERR_INVALID_ARG;
  Binding bindings[kMaxPinsPerScript]{}; int count = 0; load_bindings(id, manifest, bindings, count, false);
  int index = -1;
  for (int i = 0; i < count; ++i) {
    if (bindings[i].pin == pin && std::strcmp(bindings[i].alias, alias)) return ESP_ERR_INVALID_STATE;
    if (std::strcmp(bindings[i].alias, alias) == 0) index = i;
  }
  if (index < 0) { if (count >= kMaxPinsPerScript) return ESP_ERR_NO_MEM; index = count++; }
  std::snprintf(bindings[index].alias, sizeof(bindings[index].alias), "%s", alias);
  bindings[index].pin = pin; return save_bindings(id, bindings, count);
}

esp_err_t command(const char* input, char* output, std::size_t size) {
  if (!input || !output) return ESP_ERR_INVALID_ARG;
  if (std::strcmp(input, "script list") == 0) return list_json(output, size);
  if (std::strcmp(input, "pin list") == 0 || std::strcmp(input, "pin status") == 0) {
    std::snprintf(output, size,
        "GPIO: 0,2,4-5,12-19,21-23,25-27,32-36,39; 34-36/39 input; 1/3 console; 0,2,5,12,15 boot");
    return ESP_OK;
  }
  char id[32]{}, alias[16]{}; int pin = -1;
  if (std::sscanf(input, "pin assign %31s %15s %d", id, alias, &pin) == 3) {
    const esp_err_t result = assign_pin(id, alias, pin);
    if (result == ESP_OK) std::snprintf(output, size, "OK: %s.%s = GPIO%d", id, alias, pin);
    else std::snprintf(output, size, "Errore %s", esp_err_to_name(result));
    return result;
  }
  if (std::sscanf(input, "script bindings %31s", id) == 1) return bindings_json(id, output, size);
  char verb[16]{};
  if (std::sscanf(input, "script %15s %31s", verb, id) != 2) {
    std::snprintf(output, size, "Uso: script list|info|bindings|start|stop|remove <id> | pin list|assign <id> <alias> <gpio>");
    return ESP_ERR_INVALID_ARG;
  }
  esp_err_t result = ESP_ERR_INVALID_ARG;
  if (std::strcmp(verb, "info") == 0) return info(id, output, size);
  if (std::strcmp(verb, "start") == 0) result = run(id);
  else if (std::strcmp(verb, "stop") == 0) result = stop(id);
  else if (std::strcmp(verb, "remove") == 0) result = remove(id);
  std::snprintf(output, size, result == ESP_OK ? "OK: script %s %s" : "Errore %s: %s",
                result == ESP_OK ? verb : esp_err_to_name(result), id); return result;
}
}  // namespace nebilix::scripts
