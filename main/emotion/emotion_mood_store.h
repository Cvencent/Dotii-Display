#pragma once

#include <stdbool.h>

#include "emotion_mood.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the two learned preference weights from NVS. Conversation text and
   audio are never stored. Missing keys are normal on first boot. */
bool emotion_mood_store_load(emotion_mood_t *mood);
bool emotion_mood_store_save(const emotion_mood_t *mood);

#ifdef __cplusplus
}
#endif