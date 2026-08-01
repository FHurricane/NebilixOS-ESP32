#include "nebilix/remote.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_https_server.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "nebilix/kernel.hpp"
#include "nebilix/scripts.hpp"
#include "nebilix/wifi.hpp"
#include "nvs.h"

namespace nebilix::remote {
namespace {
constexpr char kTag[] = "nebilix.remote";
constexpr char kNamespace[] = "nebilix_remote";
constexpr char kTokenKey[] = "admin_token";
constexpr char kCertificateKey[] = "tls_cert";
constexpr char kPrivateKeyKey[] = "tls_key";
char admin_token[33]{};
char tls_certificate[1536]{};
char tls_private_key[512]{};
httpd_handle_t server = nullptr;

constexpr char kConsolePage[] = R"html(<!doctype html><html lang="it"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<link rel="icon" href="/favicon.ico" sizes="any">
<title>NebilixOS Dashboard</title><style>:root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;
min-height:100vh;background:radial-gradient(circle at 80% 0,#17335a 0,transparent 40%),#081018;
color:#e8f1ff;font-family:system-ui}main{width:min(92%,920px);margin:auto;padding:36px 0}header{display:flex;
justify-content:space-between;align-items:end;gap:20px}h1{margin:0;color:#68c8ff;font-size:clamp(2rem,7vw,4rem)}
.meta,.muted{color:#91a4ba}.status{color:#55d6a0}.cards{display:grid;grid-template-columns:repeat(3,1fr);gap:12px;
margin:24px 0}.card,.panel{background:#111c27cc;border:1px solid #26394c;border-radius:14px;padding:18px;
box-shadow:0 18px 50px #0005}.card b{display:block;margin-top:7px;font-size:1.05rem}.actions{display:flex;
flex-wrap:wrap;gap:8px;margin:14px 0}input,button{padding:12px;border-radius:9px;border:1px solid #405064;
background:#0b151f;color:white}#token{width:100%}button{background:#168ad1;font-weight:700;cursor:pointer}.row{display:flex;
gap:8px}#command{flex:1}pre{height:280px;overflow:auto;background:#03070a;padding:16px;border-radius:10px;
white-space:pre-wrap}.pin-script{border-top:1px solid #26394c;padding:14px 0}.pin-script:first-child{border-top:0}
.pin-title{display:flex;justify-content:space-between;gap:12px;align-items:center}.pin-grid{display:grid;
grid-template-columns:repeat(2,minmax(0,1fr));gap:10px;margin:10px 0}.pin-field{display:grid;gap:5px}.pin-field select{
padding:10px;border-radius:9px;border:1px solid #405064;background:#0b151f;color:white}@media(max-width:650px){header{display:block}.cards{grid-template-columns:1fr}.pin-grid{grid-template-columns:1fr}.row{align-items:stretch;
flex-direction:column}}</style></head><body><main><header><div><p class="status">● NebilixOS online</p>
<h1>NebilixOS</h1><p class="muted">Embedded Script Operating System</p></div><p class="meta">Core Edition 0.1.0</p></header>
<section class="cards"><div class="card">Sistema<b>ESP32</b></div><div class="card">Accesso locale<b>nebilixos.local</b></div>
<div class="card">Canale remoto<b>HTTPS + WebSocket</b></div></section><section class="panel"><h2>Mini terminale</h2>
<p class="muted">Inserisci il token amministrativo recuperabile dalla shell USB con <code>remote token</code>.</p>
<div class="row"><input id="token" type="password" autocomplete="current-password" placeholder="Token amministratore">
<button id="unlock">Sblocca terminale</button></div><p id="auth" class="muted">Terminale bloccato</p>
<div class="actions"><button data-command="info">Informazioni</button><button data-command="uptime">Uptime</button>
<button data-command="wifi status">Stato Wi-Fi</button><button data-command="help">Aiuto</button></div>
<div class="row"><input id="command" autocomplete="off" placeholder="Scrivi un comando"><button id="send">Esegui</button></div>
<pre id="output">Connessione alla console in corso...</pre></section>
<section class="panel" style="margin-top:16px"><h2>Pin Manager</h2><p class="muted">Assegna i pin logici richiesti dagli script ai GPIO fisici. Le GPIO 6-11 sono protette; i pin di boot sono segnalati.</p>
<button id="load-pins">Carica configurazione pin</button><p id="pin-status" class="muted">Sblocca il terminale e carica gli script.</p><div id="pin-manager"></div></section><p class="meta">Sviluppato da Costa Fabio &middot;
Copyright &copy; 2026 &middot; Apache License 2.0</p></main><script>const out=document.querySelector('#output'),
cmd=document.querySelector('#command'),token=document.querySelector('#token'),auth=document.querySelector('#auth');let unlocked=false;
const ws=new WebSocket('wss://'+location.host+'/ws');
ws.onopen=()=>out.textContent='Console connessa. Inserisci il token amministrativo.\n';ws.onmessage=e=>{out.textContent+=e.data+'\n';
if(e.data==='AUTH_OK'){unlocked=true;auth.textContent='Terminale sbloccato';auth.className='status'}
if(e.data==='Autenticazione fallita'){unlocked=false;auth.textContent='Token non valido';auth.className='muted'}
out.scrollTop=out.scrollHeight};ws.onerror=()=>out.textContent+='Errore di connessione WebSocket.\n';
function sendFrame(value){const key=token.value.trim();if(!key){out.textContent+='Token amministrativo richiesto.\n';return false}
if(ws.readyState!==1){out.textContent+='Console non ancora connessa.\n';return false}ws.send(key+'\n'+value);return true}
function run(value){if(!unlocked){out.textContent+='Prima premi Sblocca terminale.\n';return}if(sendFrame(value))out.textContent+='nebilixos> '+value+'\n'}
function send(){if(cmd.value){run(cmd.value);cmd.value=''}}document.querySelector('#send').onclick=send;
document.querySelector('#unlock').onclick=()=>{unlocked=false;auth.textContent='Verifica token...';sendFrame('__auth__')};
cmd.onkeydown=e=>{if(e.key==='Enter')send()};document.querySelectorAll('[data-command]').forEach(button=>
button.onclick=()=>run(button.dataset.command));
const pinManager=document.querySelector('#pin-manager'),pinStatus=document.querySelector('#pin-status');let availablePins=[];
async function api(path,options={}){const key=token.value.trim();if(!key)throw new Error('Token amministratore richiesto');
const headers={Authorization:'Bearer '+key,...(options.headers||{})};const response=await fetch(path,{...options,headers});
let data={};try{data=await response.json()}catch{}if(!response.ok)throw new Error(data.error||('HTTP '+response.status));return data}
function pinOption(pin,mode){if(mode==='output'&&!pin.output)return null;const option=document.createElement('option');
option.value=String(pin.gpio);option.textContent='GPIO '+pin.gpio+(pin.strapping?' (pin di boot)':'')+(pin.owner?' · occupata da '+pin.owner:'');
option.disabled=Boolean(pin.owner);return option}
async function loadPins(){pinStatus.textContent='Caricamento…';pinManager.replaceChildren();try{const [scriptsData,pinsData]=await Promise.all([api('/api/v1/scripts'),api('/api/v1/pins')]);
availablePins=pinsData.pins||[];for(const script of scriptsData.scripts||[]){const binding=await api('/api/v1/scripts/'+encodeURIComponent(script.id)+'/bindings');
if(!binding.requirements.length)continue;const section=document.createElement('div');section.className='pin-script';const title=document.createElement('div');title.className='pin-title';
const name=document.createElement('strong');name.textContent=script.name+' ('+script.id+')';const state=document.createElement('span');state.className='muted';state.textContent=binding.running?'In esecuzione':(script.configured?'Configurato':'Da configurare');title.append(name,state);
const grid=document.createElement('div');grid.className='pin-grid';for(const requirement of binding.requirements){const label=document.createElement('label');label.className='pin-field';label.textContent=requirement.alias+' · '+requirement.mode;const select=document.createElement('select');select.dataset.alias=requirement.alias;
const empty=document.createElement('option');empty.value='';empty.textContent='Scegli GPIO';select.append(empty);for(const pin of availablePins){const option=pinOption(pin,requirement.mode);if(option){if(pin.gpio===requirement.gpio){option.selected=true;option.disabled=false}select.append(option)}}select.disabled=binding.running;label.append(select);grid.append(label)}
const save=document.createElement('button');save.type='button';save.textContent='Salva assegnazione';save.disabled=binding.running;save.onclick=async()=>{const values={};for(const select of grid.querySelectorAll('select')){if(select.value===''){pinStatus.textContent='Seleziona tutti i GPIO per '+script.name;return}values[select.dataset.alias]=Number(select.value)}
save.disabled=true;try{await api('/api/v1/scripts/'+encodeURIComponent(script.id)+'/bindings',{method:'PUT',headers:{'Content-Type':'application/json'},body:JSON.stringify(values)});pinStatus.textContent='Pin salvati per '+script.name;await loadPins()}catch(error){pinStatus.textContent=error.message;save.disabled=false}};
section.append(title,grid,save);pinManager.append(section)}if(!pinManager.children.length)pinStatus.textContent='Nessuno script richiede GPIO.';else pinStatus.textContent='Configurazione caricata.'}catch(error){pinStatus.textContent='Errore: '+error.message}}
document.querySelector('#load-pins').onclick=loadPins;</script></body></html>)html";

esp_err_t load_or_create_token() {
  nvs_handle_t handle;
  ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag,
                      "Unable to open remote storage");
  std::size_t size = sizeof(admin_token);
  esp_err_t result = nvs_get_str(handle, kTokenKey, admin_token, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    std::uint8_t random[16]{};
    esp_fill_random(random, sizeof(random));
    for (std::size_t i = 0; i < sizeof(random); ++i) {
      std::snprintf(admin_token + i * 2, 3, "%02x", random[i]);
    }
    result = nvs_set_str(handle, kTokenKey, admin_token);
    if (result == ESP_OK) result = nvs_commit(handle);
  }
  nvs_close(handle);
  return result;
}

esp_err_t generate_tls_identity(nvs_handle_t handle) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context random;
  mbedtls_pk_context key;
  mbedtls_x509write_cert certificate;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&random);
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&certificate);
  int result = 0;
  constexpr unsigned char personalization[] = "NebilixOS per-device TLS identity";
  unsigned char serial[16]{};
  char dns_name[] = "nebilixos.local";
  mbedtls_x509_san_list san{};
  if ((result = mbedtls_ctr_drbg_seed(&random, mbedtls_entropy_func, &entropy,
          personalization, sizeof(personalization) - 1)) != 0) goto done;
  if ((result = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0) goto done;
  if ((result = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
          mbedtls_ctr_drbg_random, &random)) != 0) goto done;

  if ((result = mbedtls_ctr_drbg_random(&random, serial, sizeof(serial))) != 0) goto done;
  serial[0] &= 0x7f;
  if (serial[0] == 0) serial[0] = 1;
  mbedtls_x509write_crt_set_version(&certificate, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&certificate, &key);
  mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
  if ((result = mbedtls_x509write_crt_set_subject_name(&certificate,
          "CN=nebilixos.local,O=NebilixOS,OU=Device")) != 0) goto done;
  if ((result = mbedtls_x509write_crt_set_issuer_name(&certificate,
          "CN=nebilixos.local,O=NebilixOS,OU=Device")) != 0) goto done;
  if ((result = mbedtls_x509write_crt_set_serial_raw(&certificate, serial, sizeof(serial))) != 0) goto done;
  if ((result = mbedtls_x509write_crt_set_validity(&certificate,
          "20260101000000", "20451231235959")) != 0) goto done;
  if ((result = mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1)) != 0) goto done;
  if ((result = mbedtls_x509write_crt_set_key_usage(&certificate,
          MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_AGREEMENT)) != 0) goto done;
  san.node.type = MBEDTLS_X509_SAN_DNS_NAME;
  san.node.san.unstructured_name.p = reinterpret_cast<unsigned char*>(dns_name);
  san.node.san.unstructured_name.len = std::strlen(dns_name);
  if ((result = mbedtls_x509write_crt_set_subject_alternative_name(&certificate, &san)) != 0) goto done;
  if ((result = mbedtls_pk_write_key_pem(&key,
          reinterpret_cast<unsigned char*>(tls_private_key), sizeof(tls_private_key))) != 0) goto done;
  if ((result = mbedtls_x509write_crt_pem(&certificate,
          reinterpret_cast<unsigned char*>(tls_certificate), sizeof(tls_certificate),
          mbedtls_ctr_drbg_random, &random)) != 0) goto done;
  if (nvs_set_blob(handle, kPrivateKeyKey, tls_private_key,
                   std::strlen(tls_private_key) + 1) != ESP_OK ||
      nvs_set_blob(handle, kCertificateKey, tls_certificate,
                   std::strlen(tls_certificate) + 1) != ESP_OK ||
      nvs_commit(handle) != ESP_OK) result = -1;
done:
  mbedtls_x509write_crt_free(&certificate);
  mbedtls_pk_free(&key);
  mbedtls_ctr_drbg_free(&random);
  mbedtls_entropy_free(&entropy);
  if (result != 0) {
    tls_certificate[0] = '\0';
    tls_private_key[0] = '\0';
    ESP_LOGE(kTag, "TLS identity generation failed: -0x%04x", -result);
    return ESP_FAIL;
  }
  ESP_LOGI(kTag, "Generated a unique TLS identity for this board");
  return ESP_OK;
}

esp_err_t load_or_create_tls_identity() {
  nvs_handle_t handle;
  ESP_RETURN_ON_ERROR(nvs_open(kNamespace, NVS_READWRITE, &handle), kTag,
                      "Unable to open TLS storage");
  std::size_t certificate_size = sizeof(tls_certificate);
  std::size_t key_size = sizeof(tls_private_key);
  const esp_err_t certificate_result = nvs_get_blob(
      handle, kCertificateKey, tls_certificate, &certificate_size);
  const esp_err_t key_result = nvs_get_blob(
      handle, kPrivateKeyKey, tls_private_key, &key_size);
  esp_err_t result = ESP_OK;
  if (certificate_result != ESP_OK || key_result != ESP_OK ||
      certificate_size < 2 || key_size < 2 ||
      tls_certificate[certificate_size - 1] != '\0' ||
      tls_private_key[key_size - 1] != '\0') {
    result = generate_tls_identity(handle);
  } else {
    ESP_LOGI(kTag, "Loaded the board-specific TLS identity");
  }
  nvs_close(handle);
  return result;
}

void command_response(const char* command, char* output, std::size_t size) {
  if (std::strcmp(command, "help") == 0) {
    std::snprintf(output, size, "Comandi: help, info, uptime, wifi status, script list|info|bindings|start|stop|remove, pin list|assign, reboot");
  } else if (std::strcmp(command, "info") == 0) {
    const auto info = kernel::system_info();
    const auto ip = wifi::address();
    std::snprintf(output, size,
                  "NebilixOS 0.1.0 Core | ESP32 %u core | Flash %" PRIu32
                  " | Heap %lu | IP " IPSTR,
                  info.core_count, info.flash_size_bytes,
                  static_cast<unsigned long>(esp_get_free_heap_size()), IP2STR(&ip));
  } else if (std::strcmp(command, "uptime") == 0) {
    std::snprintf(output, size, "Uptime: %" PRIu64 " ms", kernel::uptime_ms());
  } else if (std::strcmp(command, "wifi status") == 0) {
    const auto ip = wifi::address();
    std::snprintf(output, size, "Wi-Fi state: %u | IP " IPSTR,
                  static_cast<unsigned>(wifi::state()), IP2STR(&ip));
  } else if (std::strncmp(command, "script ", 7) == 0 ||
             std::strncmp(command, "pin ", 4) == 0) {
    if (scripts::command(command, output, size) != ESP_OK && !*output)
      std::snprintf(output, size, "Comando script fallito");
  } else if (std::strcmp(command, "reboot") == 0) {
    std::snprintf(output, size, "Riavvio NebilixOS...");
  } else {
    std::snprintf(output, size, "Comando sconosciuto: %s", command);
  }
}

void reboot_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(600));
  esp_restart();
}

esp_err_t root_handler(httpd_req_t* request) {
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, kConsolePage, HTTPD_RESP_USE_STRLEN);
}

esp_err_t favicon_handler(httpd_req_t* request) {
  extern const unsigned char favicon_start[] asm("_binary_favicon_ico_start");
  extern const unsigned char favicon_end[] asm("_binary_favicon_ico_end");
  httpd_resp_set_type(request, "image/x-icon");
  httpd_resp_set_hdr(request, "Cache-Control", "public, max-age=604800, immutable");
  return httpd_resp_send(request, reinterpret_cast<const char*>(favicon_start),
                         favicon_end - favicon_start);
}

bool authorized(httpd_req_t* request) {
  char authorization[48]{};
  if (httpd_req_get_hdr_value_str(request, "Authorization", authorization,
                                  sizeof(authorization)) != ESP_OK) return false;
  constexpr char prefix[] = "Bearer ";
  return std::strncmp(authorization, prefix, sizeof(prefix) - 1) == 0 &&
         std::strcmp(authorization + sizeof(prefix) - 1, admin_token) == 0;
}

void add_cors_headers(httpd_req_t* request) {
  char origin[96]{};
  if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK) return;
  const char* allowed_origin = nullptr;
  if (std::strcmp(origin, "https://www.costafabio.it") == 0)
    allowed_origin = "https://www.costafabio.it";
  else if (std::strcmp(origin, "https://costafabio.it") == 0)
    allowed_origin = "https://costafabio.it";
  if (!allowed_origin) return;
  httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", allowed_origin);
  httpd_resp_set_hdr(request, "Access-Control-Allow-Methods", "GET, PUT, POST, DELETE, OPTIONS");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Headers", "Authorization, Content-Type");
  httpd_resp_set_hdr(request, "Access-Control-Allow-Private-Network", "true");
  httpd_resp_set_hdr(request, "Vary", "Origin");
}

esp_err_t api_error(httpd_req_t* request, const char* status, const char* message) {
  add_cors_headers(request);
  httpd_resp_set_status(request, status);
  httpd_resp_set_type(request, "application/json");
  char body[160]{};
  std::snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", message);
  return httpd_resp_sendstr(request, body);
}

bool api_auth(httpd_req_t* request) {
  add_cors_headers(request);
  if (authorized(request)) return true;
  httpd_resp_set_hdr(request, "WWW-Authenticate", "Bearer");
  api_error(request, "401 Unauthorized", "Token non valido");
  return false;
}

esp_err_t api_options_handler(httpd_req_t* request) {
  add_cors_headers(request);
  httpd_resp_set_status(request, "204 No Content");
  return httpd_resp_send(request, nullptr, 0);
}

const char* script_path(httpd_req_t* request) {
  constexpr char prefix[] = "/api/v1/scripts/";
  return std::strncmp(request->uri, prefix, sizeof(prefix) - 1) == 0
             ? request->uri + sizeof(prefix) - 1 : nullptr;
}

esp_err_t scripts_list_handler(httpd_req_t* request) {
  if (!api_auth(request)) return ESP_OK;
  char* body = static_cast<char*>(std::malloc(4096));
  if (!body) return api_error(request, "503 Service Unavailable", "Memoria insufficiente");
  const esp_err_t result = scripts::list_json(body, 4096);
  if (result == ESP_OK) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, body);
  } else {
    api_error(request, "500 Internal Server Error", esp_err_to_name(result));
  }
  std::free(body);
  return ESP_OK;
}

esp_err_t pins_handler(httpd_req_t* request) {
  if (!api_auth(request)) return ESP_OK;
  char* body = static_cast<char*>(std::malloc(4096));
  if (!body) return api_error(request, "503 Service Unavailable", "Memoria insufficiente");
  const esp_err_t result = scripts::pins_json(body, 4096);
  if (result == ESP_OK) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_sendstr(request, body);
  } else {
    api_error(request, "500 Internal Server Error", esp_err_to_name(result));
  }
  std::free(body);
  return ESP_OK;
}

esp_err_t bindings_get_handler(httpd_req_t* request) {
  if (!api_auth(request)) return ESP_OK;
  const char* path = script_path(request);
  char id[32]{}, resource[16]{};
  if (!path || std::sscanf(path, "%31[^/]/%15s", id, resource) != 2 ||
      std::strcmp(resource, "bindings") != 0)
    return api_error(request, "400 Bad Request", "Percorso binding non valido");
  char body[1024]{};
  const esp_err_t result = scripts::bindings_json(id, body, sizeof(body));
  if (result != ESP_OK) return api_error(request, "404 Not Found", esp_err_to_name(result));
  httpd_resp_set_type(request, "application/json");
  return httpd_resp_sendstr(request, body);
}

esp_err_t scripts_install_handler(httpd_req_t* request) {
  if (!api_auth(request)) return ESP_OK;
  const char* id = script_path(request);
  if (!id) return api_error(request, "400 Bad Request", "Percorso non valido");
  char binding_id[32]{}, resource[16]{};
  const bool is_binding = std::sscanf(id, "%31[^/]/%15s", binding_id, resource) == 2 &&
                          std::strcmp(resource, "bindings") == 0;
  const std::size_t maximum = is_binding ? 512 : scripts::kMaxScriptBytes;
  if ((!is_binding && std::strchr(id, '/')) || request->content_len <= 0 ||
      static_cast<std::size_t>(request->content_len) > maximum)
    return api_error(request, "400 Bad Request", "ID o dimensione non validi");
  const std::size_t length = request->content_len;
  char* source = static_cast<char*>(std::malloc(length + 1));
  if (!source) return api_error(request, "503 Service Unavailable", "Memoria insufficiente");
  std::size_t received = 0;
  while (received < length) {
    const int count = httpd_req_recv(request, source + received, length - received);
    if (count <= 0) { std::free(source); return api_error(request, "400 Bad Request", "Upload interrotto"); }
    received += count;
  }
  source[length] = '\0';
  char message[128]{};
  const esp_err_t result = is_binding
      ? scripts::set_bindings_json(binding_id, source, message, sizeof(message))
      : scripts::install(id, source, length, message, sizeof(message));
  std::free(source);
  if (result != ESP_OK) return api_error(request, "422 Unprocessable Entity", *message ? message : esp_err_to_name(result));
  httpd_resp_set_type(request, "application/json");
  char body[180]{};
  std::snprintf(body, sizeof(body), "{\"ok\":true,\"message\":\"%s\"}", message);
  return httpd_resp_sendstr(request, body);
}

esp_err_t scripts_action_handler(httpd_req_t* request) {
  if (!api_auth(request)) return ESP_OK;
  const char* path = script_path(request);
  if (!path) return api_error(request, "400 Bad Request", "Percorso non valido");
  char id[32]{}, action[12]{};
  if (std::sscanf(path, "%31[^/]/%11s", id, action) != 2)
    return api_error(request, "400 Bad Request", "Azione non valida");
  esp_err_t result = ESP_ERR_NOT_SUPPORTED;
  if (std::strcmp(action, "start") == 0) result = scripts::run(id);
  else if (std::strcmp(action, "stop") == 0) result = scripts::stop(id);
  if (result != ESP_OK) return api_error(request, "409 Conflict", esp_err_to_name(result));
  return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t scripts_remove_handler(httpd_req_t* request) {
  if (!api_auth(request)) return ESP_OK;
  const char* id = script_path(request);
  if (!id || std::strchr(id, '/')) return api_error(request, "400 Bad Request", "ID non valido");
  const esp_err_t result = scripts::remove(id);
  if (result != ESP_OK) return api_error(request, "409 Conflict", esp_err_to_name(result));
  return httpd_resp_sendstr(request, "{\"ok\":true}");
}

esp_err_t websocket_handler(httpd_req_t* request) {
  if (request->method == HTTP_GET) return ESP_OK;
  httpd_ws_frame_t frame{};
  frame.type = HTTPD_WS_TYPE_TEXT;
  ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(request, &frame, 0), kTag,
                      "Unable to read WebSocket frame");
  if (frame.len == 0 || frame.len > 192) return ESP_ERR_INVALID_SIZE;
  char payload[193]{};
  frame.payload = reinterpret_cast<std::uint8_t*>(payload);
  ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(request, &frame, frame.len), kTag,
                      "Unable to receive WebSocket payload");
  char* separator = std::strchr(payload, '\n');
  const char* response = "Autenticazione fallita";
  char output[320]{};
  if (separator != nullptr) {
    *separator = '\0';
    const char* command = separator + 1;
    if (std::strcmp(payload, admin_token) == 0) {
      if (std::strcmp(command, "__auth__") == 0) {
        response = "AUTH_OK";
      } else {
        command_response(command, output, sizeof(output));
        response = output;
      }
      if (std::strcmp(command, "reboot") == 0) {
        xTaskCreate(reboot_task, "nbx_remote_reboot", 2048, nullptr, 3, nullptr);
      }
    }
  }
  httpd_ws_frame_t reply{};
  reply.type = HTTPD_WS_TYPE_TEXT;
  reply.payload = reinterpret_cast<std::uint8_t*>(const_cast<char*>(response));
  reply.len = std::strlen(response);
  return httpd_ws_send_frame(request, &reply);
}

esp_err_t start_server() {
  httpd_ssl_config_t config = HTTPD_SSL_CONFIG_DEFAULT();
  config.httpd.max_open_sockets = 2;
  config.httpd.max_uri_handlers = 12;
  config.httpd.stack_size = 10240;
  config.httpd.uri_match_fn = httpd_uri_match_wildcard;
  config.servercert = reinterpret_cast<const std::uint8_t*>(tls_certificate);
  config.servercert_len = std::strlen(tls_certificate) + 1;
  config.prvtkey_pem = reinterpret_cast<const std::uint8_t*>(tls_private_key);
  config.prvtkey_len = std::strlen(tls_private_key) + 1;
  ESP_RETURN_ON_ERROR(httpd_ssl_start(&server, &config), kTag,
                      "Unable to start HTTPS console");
  const httpd_uri_t root{.uri = "/", .method = HTTP_GET,
                         .handler = root_handler, .user_ctx = nullptr};
  const httpd_uri_t favicon{.uri = "/favicon.ico", .method = HTTP_GET,
                            .handler = favicon_handler, .user_ctx = nullptr};
  const httpd_uri_t ws{.uri = "/ws", .method = HTTP_GET,
                       .handler = websocket_handler, .user_ctx = nullptr,
                       .is_websocket = true, .handle_ws_control_frames = false,
                       .supported_subprotocol = nullptr};
  const httpd_uri_t scripts_list{.uri = "/api/v1/scripts", .method = HTTP_GET,
      .handler = scripts_list_handler, .user_ctx = nullptr};
  const httpd_uri_t pins{.uri = "/api/v1/pins", .method = HTTP_GET,
      .handler = pins_handler, .user_ctx = nullptr};
  const httpd_uri_t bindings{.uri = "/api/v1/scripts/*", .method = HTTP_GET,
      .handler = bindings_get_handler, .user_ctx = nullptr};
  const httpd_uri_t scripts_install{.uri = "/api/v1/scripts/*", .method = HTTP_PUT,
      .handler = scripts_install_handler, .user_ctx = nullptr};
  const httpd_uri_t scripts_action{.uri = "/api/v1/scripts/*", .method = HTTP_POST,
      .handler = scripts_action_handler, .user_ctx = nullptr};
  const httpd_uri_t scripts_remove{.uri = "/api/v1/scripts/*", .method = HTTP_DELETE,
      .handler = scripts_remove_handler, .user_ctx = nullptr};
  const httpd_uri_t api_options{.uri = "/api/v1/*", .method = HTTP_OPTIONS,
      .handler = api_options_handler, .user_ctx = nullptr};
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &root), kTag,
                      "Unable to register console page");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &favicon), kTag,
                      "Unable to register favicon");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &ws), kTag, "register websocket");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scripts_list), kTag, "register script list");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &pins), kTag, "register pin list");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &bindings), kTag, "register pin bindings");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scripts_install), kTag, "register script install");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scripts_action), kTag, "register script action");
  ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &scripts_remove), kTag, "register script removal");
  return httpd_register_uri_handler(server, &api_options);
}

void service_task(void*) {
  while (wifi::state() != wifi::State::connected) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (load_or_create_tls_identity() == ESP_OK && start_server() == ESP_OK) {
    ESP_LOGI(kTag, "Secure console: https://nebilixos.local");
    ESP_LOGW(kTag, "Administrator token: %s", admin_token);
  }
  vTaskDelete(nullptr);
}
}  // namespace

esp_err_t start() {
  ESP_RETURN_ON_ERROR(load_or_create_token(), kTag,
                      "Unable to initialize administrator token");
  return xTaskCreate(service_task, "nbx_remote", 12288, nullptr, 4, nullptr) ==
                 pdPASS
             ? ESP_OK
             : ESP_ERR_NO_MEM;
}

const char* administrator_token() { return admin_token; }
}  // namespace nebilix::remote
