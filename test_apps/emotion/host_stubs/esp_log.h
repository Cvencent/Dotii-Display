#pragma once

/* Host builds drop firmware logging; the tests assert on behaviour instead. */
#define ESP_LOGE(tag, format, ...) ((void)(tag), (void)0)
#define ESP_LOGW(tag, format, ...) ((void)(tag), (void)0)
#define ESP_LOGI(tag, format, ...) ((void)(tag), (void)0)
#define ESP_LOGD(tag, format, ...) ((void)(tag), (void)0)