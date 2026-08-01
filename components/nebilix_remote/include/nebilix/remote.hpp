#pragma once
#include "esp_err.h"
namespace nebilix::remote {
esp_err_t start();
const char* administrator_token();
}
