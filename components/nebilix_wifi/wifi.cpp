#include "nebilix/wifi.hpp"

#include <atomic>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "mdns.h"

namespace nebilix::wifi {
namespace {

constexpr char kLogTag[] = "nebilix.wifi";
constexpr char kNvsNamespace[] = "nebilix_wifi";
constexpr char kSsidKey[] = "ssid";
constexpr char kPasswordKey[] = "password";
constexpr char kProvisioningPassword[] = "nebilixos";

constexpr std::uint16_t kMaximumVisibleNetworks = 20;

constexpr char kProvisioningPageStart[] = R"html(<!doctype html>
<html lang="it"><head><meta charset="utf-8"><meta name="viewport"
content="width=device-width,initial-scale=1"><title>NebilixOS Wi-Fi</title>
<style>body{font-family:system-ui;background:#10151c;color:#eaf2ff;margin:0;
display:grid;place-items:center;min-height:100vh}.box{width:min(88%,360px);
background:#18212c;padding:28px;border-radius:14px}pre{color:#59c3ff;font-size:11px;
line-height:1.15;overflow:hidden}.meta{font-size:12px;color:#9fb0c3;line-height:1.5}
label{display:block;margin-top:16px}input{box-sizing:border-box;width:100%;
padding:11px;margin-top:6px;border:1px solid #405064;border-radius:7px;
background:#0e141b;color:white}button,.refresh{box-sizing:border-box;display:block;width:100%;margin-top:12px;padding:12px;
border:0;border-radius:7px;background:#168ad1;color:white;font-weight:700}
.refresh{background:#34465a;text-align:center;text-decoration:none}.status{min-height:1.3em;color:#9fb0c3;font-size:12px}</style>
</head><body><main class="box"><pre aria-label="NebilixOS">
 _   _      _     _ _ _       ___  ____
| \ | | ___| |__ (_) (_)_  __/ _ \/ ___|
|  \| |/ _ \ '_ \| | | \ \/ / | | \___ \
| |\  |  __/ |_) | | | |>  &lt;| |_| |___) |
|_| \_|\___|_.__/|_|_/_/ /_\\___/|____/
</pre><p class="meta">Embedded Script Operating System<br>
Sviluppato da Costa Fabio<br>Copyright &copy; 2026 Costa Fabio<br>
Apache License 2.0</p><p>Configura la rete Wi-Fi del dispositivo.</p>
<form method="post" action="/configure"><label>Nome rete
(SSID)<input id="ssid" name="ssid" list="networks" maxlength="32" required
autocomplete="off" placeholder="Seleziona o scrivi il nome della rete"></label>
<datalist id="networks">
)html";

constexpr char kProvisioningPageEnd[] = R"html(</datalist>
<p class="status">Seleziona una rete rilevata oppure scrivi manualmente un SSID nascosto.</p>
<a class="refresh" href="/scan">Aggiorna elenco reti</a><label>Password del router
<input name="password" type="password" maxlength="63"></label>
<button type="submit">Salva e connetti al router</button></form></main></body></html>)html";

std::atomic<State> current_state{State::stopped};
std::atomic<std::uint32_t> current_address{0};
std::uint32_t retry_count = 0;
httpd_handle_t web_server = nullptr;
bool local_services_started = false;
wifi_ap_record_t visible_networks[kMaximumVisibleNetworks]{};
std::uint16_t visible_network_count = 0;

esp_err_t start_local_services();

esp_err_t scan_visible_networks() {
  wifi_scan_config_t scan_config{};
  scan_config.show_hidden = false;
  visible_network_count = kMaximumVisibleNetworks;
  const esp_err_t result = esp_wifi_scan_start(&scan_config, true);
  if (result != ESP_OK) {
    visible_network_count = 0;
    return result;
  }
  return esp_wifi_scan_get_ap_records(&visible_network_count, visible_networks);
}

bool decode_form_value(char* value) {
  char* source = value;
  char* destination = value;
  while (*source != '\0') {
    if (*source == '+') {
      *destination++ = ' ';
      ++source;
    } else if (*source == '%' && std::isxdigit(source[1]) &&
               std::isxdigit(source[2])) {
      unsigned int byte = 0;
      if (std::sscanf(source + 1, "%2x", &byte) != 1) {
        return false;
      }
      *destination++ = static_cast<char>(byte);
      source += 3;
    } else {
      *destination++ = *source++;
    }
  }
  *destination = '\0';
  return true;
}

esp_err_t load_credentials(char* ssid, std::size_t ssid_size, char* password,
                           std::size_t password_size) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
  if (result != ESP_OK) {
    return result;
  }
  result = nvs_get_str(handle, kSsidKey, ssid, &ssid_size);
  if (result == ESP_OK) {
    result = nvs_get_str(handle, kPasswordKey, password, &password_size);
  }
  nvs_close(handle);
  return result;
}

esp_err_t save_credentials(const char* ssid, const char* password) {
  nvs_handle_t handle;
  ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &handle), kLogTag,
                      "Unable to open Wi-Fi storage");
  esp_err_t result = nvs_set_str(handle, kSsidKey, ssid);
  if (result == ESP_OK) {
    result = nvs_set_str(handle, kPasswordKey, password);
  }
  if (result == ESP_OK) {
    result = nvs_commit(handle);
  }
  nvs_close(handle);
  return result;
}

void restart_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(1200));
  esp_restart();
}

const char* security_name(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return "aperta";
    case WIFI_AUTH_WEP:
      return "WEP";
    case WIFI_AUTH_WPA_PSK:
      return "WPA";
    case WIFI_AUTH_WPA2_PSK:
      return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
      return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
      return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return "WPA2/WPA3";
    default:
      return "protetta";
  }
}

esp_err_t send_html_escaped(httpd_req_t* request, const std::uint8_t* value) {
  for (std::size_t index = 0; index < 32 && value[index] != 0; ++index) {
    const char* escaped = nullptr;
    switch (value[index]) {
      case '&': escaped = "&amp;"; break;
      case '<': escaped = "&lt;"; break;
      case '>': escaped = "&gt;"; break;
      case '\"': escaped = "&quot;"; break;
      case '\'': escaped = "&#39;"; break;
      default: break;
    }
    if (escaped != nullptr) {
      ESP_RETURN_ON_ERROR(
          httpd_resp_send_chunk(request, escaped, HTTPD_RESP_USE_STRLEN),
          kLogTag, "Unable to send escaped SSID");
    } else {
      const char character = static_cast<char>(value[index]);
      ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, &character, 1), kLogTag,
                          "Unable to send SSID");
    }
  }
  return ESP_OK;
}

esp_err_t page_handler(httpd_req_t* request) {
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(request, "Pragma", "no-cache");

  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(
                          request, kProvisioningPageStart, HTTPD_RESP_USE_STRLEN),
                      kLogTag, "Unable to send provisioning page");
  for (std::uint16_t index = 0; index < visible_network_count; ++index) {
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "<option value=\"", 15),
                        kLogTag, "Unable to start network option");
    ESP_RETURN_ON_ERROR(send_html_escaped(request, visible_networks[index].ssid), kLogTag,
                        "Unable to send network name");
    char details[96]{};
    std::snprintf(details, sizeof(details),
                  "\" label=\"%d dBm - %s\"></option>", visible_networks[index].rssi,
                  security_name(visible_networks[index].authmode));
    ESP_RETURN_ON_ERROR(
        httpd_resp_send_chunk(request, details, HTTPD_RESP_USE_STRLEN), kLogTag,
        "Unable to finish network option");
  }
  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(
                          request, kProvisioningPageEnd, HTTPD_RESP_USE_STRLEN),
                      kLogTag, "Unable to finish provisioning page");
  return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t refresh_handler(httpd_req_t* request) {
  const esp_err_t result = scan_visible_networks();
  if (result != ESP_OK) {
    ESP_LOGW(kLogTag, "Wi-Fi refresh failed: %s", esp_err_to_name(result));
  }
  httpd_resp_set_status(request, "303 See Other");
  httpd_resp_set_hdr(request, "Location", "/");
  return httpd_resp_send(request, nullptr, 0);
}

esp_err_t status_handler(httpd_req_t* request) {
  const auto ip = address();
  esp_chip_info_t chip{};
  esp_chip_info(&chip);
  char page[1800]{};
  std::snprintf(page, sizeof(page), R"html(<!doctype html><html lang="it"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta http-equiv="refresh" content="10"><title>NebilixOS</title><style>
body{font-family:system-ui;background:#10151c;color:#eaf2ff;margin:0;display:grid;
place-items:center;min-height:100vh}.box{width:min(88%%,520px);background:#18212c;
padding:28px;border-radius:14px}h1{color:#59c3ff}.grid{display:grid;grid-template-columns:1fr 1fr;
gap:10px}.item{background:#0e141b;padding:14px;border-radius:8px}.value{font-weight:700}
.meta{color:#9fb0c3;font-size:12px}</style></head><body><main class="box">
<h1>NebilixOS-ESP32</h1><p>Embedded Script Operating System</p><div class="grid">
<div class="item">Versione<br><span class="value">0.1.0 Core</span></div>
<div class="item">Indirizzo<br><span class="value">%u.%u.%u.%u</span></div>
<div class="item">CPU<br><span class="value">%u core</span></div>
<div class="item">Heap libero<br><span class="value">%lu byte</span></div>
</div><p class="meta">nebilixos.local &middot; Costa Fabio &middot; Copyright &copy; 2026<br>
Apache License 2.0</p></main></body></html>)html",
                IP2STR(&ip), static_cast<unsigned>(chip.cores),
                static_cast<unsigned long>(esp_get_free_heap_size()));
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t start_local_services() {
  if (local_services_started) return ESP_OK;
  ESP_RETURN_ON_ERROR(mdns_init(), kLogTag, "Unable to initialize mDNS");
  ESP_RETURN_ON_ERROR(mdns_hostname_set("nebilixos"), kLogTag,
                      "Unable to set mDNS hostname");
  ESP_RETURN_ON_ERROR(mdns_instance_name_set("NebilixOS ESP32"), kLogTag,
                      "Unable to set mDNS instance");
  ESP_RETURN_ON_ERROR(mdns_service_add("NebilixOS Web", "_http", "_tcp", 80,
                                       nullptr, 0),
                      kLogTag, "Unable to advertise HTTP service");
  ESP_RETURN_ON_ERROR(mdns_service_add("NebilixOS Secure Console", "_https",
                                       "_tcp", 443, nullptr, 0),
                      kLogTag, "Unable to advertise HTTPS service");

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 4096;
  ESP_RETURN_ON_ERROR(httpd_start(&web_server, &config), kLogTag,
                      "Unable to start status server");
  const httpd_uri_t status{.uri = "/", .method = HTTP_GET,
                           .handler = status_handler, .user_ctx = nullptr};
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(web_server, &status), kLogTag,
                      "Unable to register status page");
  local_services_started = true;
  ESP_LOGI(kLogTag, "Local services ready at http://nebilixos.local");
  return ESP_OK;
}

esp_err_t send_json_string(httpd_req_t* request, const std::uint8_t* value) {
  const char quote[] = "\"";
  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, quote, 1), kLogTag,
                      "Unable to start JSON string");
  char escaped[7]{};
  for (std::size_t index = 0; index < 32 && value[index] != 0; ++index) {
    const unsigned char byte = value[index];
    if (byte == '\"' || byte == '\\') {
      escaped[0] = '\\';
      escaped[1] = static_cast<char>(byte);
      ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, escaped, 2), kLogTag,
                          "Unable to send JSON escape");
    } else if (byte < 0x20) {
      std::snprintf(escaped, sizeof(escaped), "\\u%04x", byte);
      ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, escaped, 6), kLogTag,
                          "Unable to send JSON escape");
    } else {
      const char character = static_cast<char>(byte);
      ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, &character, 1), kLogTag,
                          "Unable to send JSON character");
    }
  }
  return httpd_resp_send_chunk(request, quote, 1);
}

esp_err_t networks_handler(httpd_req_t* request) {
  httpd_resp_set_type(request, "application/json; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");

  wifi_scan_config_t scan_config{};
  scan_config.show_hidden = false;
  esp_err_t result = esp_wifi_scan_start(&scan_config, true);
  if (result != ESP_OK) {
    ESP_LOGW(kLogTag, "Wi-Fi scan failed: %s", esp_err_to_name(result));
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Scansione Wi-Fi non disponibile");
  }

  std::uint16_t count = kMaximumVisibleNetworks;
  wifi_ap_record_t records[kMaximumVisibleNetworks]{};
  ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&count, records), kLogTag,
                      "Unable to read scanned networks");

  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "[", 1), kLogTag,
                      "Unable to start networks response");
  for (std::uint16_t index = 0; index < count; ++index) {
    if (index > 0) {
      ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, ",", 1), kLogTag,
                          "Unable to separate networks");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "{\"ssid\":", 8), kLogTag,
                        "Unable to send network");
    ESP_RETURN_ON_ERROR(send_json_string(request, records[index].ssid), kLogTag,
                        "Unable to send SSID");
    char details[64]{};
    std::snprintf(details, sizeof(details),
                  ",\"rssi\":%d,\"security\":\"%s\"}", records[index].rssi,
                  security_name(records[index].authmode));
    ESP_RETURN_ON_ERROR(
        httpd_resp_send_chunk(request, details, HTTPD_RESP_USE_STRLEN), kLogTag,
        "Unable to send network details");
  }
  ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "]", 1), kLogTag,
                      "Unable to finish networks response");
  return httpd_resp_send_chunk(request, nullptr, 0);
}

esp_err_t configure_handler(httpd_req_t* request) {
  if (request->content_len <= 0 || request->content_len >= 192) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "Dati di configurazione non validi");
  }

  char body[192]{};
  int received = 0;
  while (received < request->content_len) {
    const int count = httpd_req_recv(request, body + received,
                                     request->content_len - received);
    if (count <= 0) {
      return ESP_FAIL;
    }
    received += count;
  }
  body[received] = '\0';

  char ssid[33]{};
  char password[64]{};
  if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) != ESP_OK ||
      httpd_query_key_value(body, "password", password, sizeof(password)) !=
          ESP_OK ||
      !decode_form_value(ssid) || !decode_form_value(password) ||
      std::strlen(ssid) == 0 ||
      (std::strlen(password) > 0 && std::strlen(password) < 8)) {
    return httpd_resp_send_err(
        request, HTTPD_400_BAD_REQUEST,
        "SSID o password non validi (password: almeno 8 caratteri)");
  }

  if (save_credentials(ssid, password) != ESP_OK) {
    return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Impossibile salvare la configurazione");
  }

  constexpr char response[] =
      "<!doctype html><meta charset=utf-8><title>NebilixOS</title>"
      "<h1>Configurazione salvata</h1><p>NebilixOS si riavvia e tenta la "
      "connessione al router.</p>";
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  const esp_err_t result =
      httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
  xTaskCreate(restart_task, "nbx_wifi_restart", 2048, nullptr, 3, nullptr);
  return result;
}

esp_err_t start_web_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 4096;
  ESP_RETURN_ON_ERROR(httpd_start(&web_server, &config), kLogTag,
                      "Unable to start provisioning server");

  const httpd_uri_t page{.uri = "/",
                         .method = HTTP_GET,
                         .handler = page_handler,
                         .user_ctx = nullptr};
  const httpd_uri_t configure{.uri = "/configure",
                              .method = HTTP_POST,
                              .handler = configure_handler,
                              .user_ctx = nullptr};
  const httpd_uri_t networks{.uri = "/networks",
                             .method = HTTP_GET,
                             .handler = networks_handler,
                             .user_ctx = nullptr};
  const httpd_uri_t refresh{.uri = "/scan",
                            .method = HTTP_GET,
                            .handler = refresh_handler,
                            .user_ctx = nullptr};
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(web_server, &page), kLogTag,
                      "Unable to register provisioning page");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(web_server, &networks), kLogTag,
                      "Unable to register network scan");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(web_server, &refresh), kLogTag,
                      "Unable to register network refresh");
  return httpd_register_uri_handler(web_server, &configure);
}

esp_err_t start_provisioning() {
  std::uint8_t mac[6]{};
  ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_WIFI_STA), kLogTag,
                      "Unable to read device address");

  char access_point_name[33]{};
  char access_point_password[16]{};
  std::snprintf(access_point_name, sizeof(access_point_name),
                "NebilixOS-%02X%02X%02X", mac[3], mac[4], mac[5]);
  std::strncpy(access_point_password, kProvisioningPassword,
               sizeof(access_point_password) - 1);

  wifi_config_t config{};
  std::strncpy(reinterpret_cast<char*>(config.ap.ssid), access_point_name,
               sizeof(config.ap.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(config.ap.password),
               access_point_password, sizeof(config.ap.password) - 1);
  config.ap.ssid_len = std::strlen(access_point_name);
  config.ap.channel = 1;
  config.ap.max_connection = 2;
  config.ap.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), kLogTag,
                      "Unable to select provisioning mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &config), kLogTag,
                      "Unable to configure provisioning access point");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kLogTag,
                      "Unable to start provisioning access point");
  const esp_err_t scan_result = scan_visible_networks();
  if (scan_result != ESP_OK) {
    ESP_LOGW(kLogTag, "Initial Wi-Fi scan failed: %s",
             esp_err_to_name(scan_result));
  }
  ESP_RETURN_ON_ERROR(start_web_server(), kLogTag,
                      "Unable to start provisioning website");

  current_state.store(State::provisioning);
  ESP_LOGW(kLogTag, "Wi-Fi configuration required");
  ESP_LOGW(kLogTag, "Connect to: %s", access_point_name);
  ESP_LOGW(kLogTag, "Password:   %s", access_point_password);
  ESP_LOGW(kLogTag, "Open:       http://192.168.4.1");
  return ESP_OK;
}

void event_handler(void*, esp_event_base_t event_base, std::int32_t event_id,
                   void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    current_state.store(State::connecting);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    current_address.store(0);
    if (retry_count++ < CONFIG_NEBILIX_WIFI_MAX_RETRIES) {
      current_state.store(State::connecting);
      ESP_LOGW(kLogTag, "Connection lost, retry %" PRIu32 "/%d", retry_count,
               CONFIG_NEBILIX_WIFI_MAX_RETRIES);
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
    } else {
      current_state.store(State::disconnected);
      ESP_LOGE(kLogTag, "Unable to connect to the configured router");
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const auto* event = static_cast<ip_event_got_ip_t*>(event_data);
    retry_count = 0;
    current_address.store(event->ip_info.ip.addr);
    current_state.store(State::connected);
    ESP_LOGI(kLogTag, "Connected to router, address=" IPSTR,
             IP2STR(&event->ip_info.ip));
    ESP_ERROR_CHECK_WITHOUT_ABORT(start_local_services());
  }
}

esp_err_t initialize_platform() {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_RETURN_ON_ERROR(nvs_flash_erase(), kLogTag, "Failed to erase NVS");
    result = nvs_flash_init();
  }
  ESP_RETURN_ON_ERROR(result, kLogTag, "Failed to initialize NVS");
  ESP_RETURN_ON_ERROR(esp_netif_init(), kLogTag,
                      "Failed to initialize network stack");
  result = esp_event_loop_create_default();
  if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
    return result;
  }
  wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  return esp_wifi_init(&init_config);
}

}  // namespace

esp_err_t start() {
  State expected = State::stopped;
  if (!current_state.compare_exchange_strong(expected, State::connecting)) {
    return ESP_ERR_INVALID_STATE;
  }
  ESP_RETURN_ON_ERROR(initialize_platform(), kLogTag,
                      "Unable to initialize Wi-Fi platform");

  char ssid[33]{};
  char password[64]{};
  if (load_credentials(ssid, sizeof(ssid), password, sizeof(password)) !=
          ESP_OK ||
      std::strlen(ssid) == 0) {
    esp_netif_create_default_wifi_ap();
    return start_provisioning();
  }

  esp_netif_create_default_wifi_sta();
  ESP_RETURN_ON_ERROR(esp_event_handler_register(
                          WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, nullptr),
                      kLogTag, "Failed to register Wi-Fi event handler");
  ESP_RETURN_ON_ERROR(esp_event_handler_register(
                          IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr),
                      kLogTag, "Failed to register IP event handler");

  wifi_config_t config{};
  std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid,
               sizeof(config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char*>(config.sta.password), password,
               sizeof(config.sta.password) - 1);
  config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  config.sta.pmf_cfg.capable = true;
  config.sta.pmf_cfg.required = false;

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kLogTag,
                      "Failed to select station mode");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), kLogTag,
                      "Failed to configure Wi-Fi station");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kLogTag, "Failed to start Wi-Fi");
  ESP_LOGI(kLogTag, "Connecting to saved network '%s'", ssid);
  return ESP_OK;
}

State state() { return current_state.load(); }

esp_err_t reset_credentials() {
  nvs_handle_t handle;
  ESP_RETURN_ON_ERROR(nvs_open(kNvsNamespace, NVS_READWRITE, &handle), kLogTag,
                      "Unable to open Wi-Fi storage");
  const esp_err_t result = nvs_erase_all(handle);
  if (result == ESP_OK) nvs_commit(handle);
  nvs_close(handle);
  return result;
}

esp_ip4_addr_t address() {
  esp_ip4_addr_t result{};
  result.addr = current_address.load();
  return result;
}

}  // namespace nebilix::wifi
