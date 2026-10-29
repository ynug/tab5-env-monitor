#pragma once

/* Announcement player: embedded 16 kHz mono WAVs (VOICEVOX:ずんだもん), queued and played in order. */
#include <stdbool.h>
#include "alarm.h"
#include "esp_err.h"

esp_err_t audio_init(void);
void audio_set_volume(int percent);
/* Queue a sound; returns false if the queue is full */
bool audio_play(sound_t s);
bool audio_busy(void);
