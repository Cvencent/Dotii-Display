#pragma once
/* Host-test stand-in for ESP-IDF's esp_attr.h. The host build has no PSRAM
   section attribute, so the macro expands to nothing. */
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_ATTR
