/*
 * Local ringtone generator for incoming calls.
 *
 * The board drives one speaker, shared with the call audio, so the ringtone is
 * synthesised in software at the same 8 kHz mono format the amplifier expects
 * and written with audio_write_pcm(). While it rings the output volume is
 * switched to the configured ring volume; stopping restores the call volume,
 * which keeps the two levels independent on a single speaker.
 *
 * Patterns are pure tones plus silence, approximating the usual PSTN cadences,
 * so no audio assets are needed in flash.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define RINGTONE_COUNT           5
#define RINGTONE_DEFAULT         0
#define RINGTONE_DEFAULT_VOLUME  70

/* Dialpad feedback: a short DTMF pair at its own level (0 = silent). */
#define KEY_TONE_DEFAULT_VOLUME  60
#define KEY_TONE_MS              90

typedef enum {
    RINGTONE_CLASSIC = 0, /* 440+480 Hz, 2 s on / 4 s off */
    RINGTONE_DOUBLE,      /* 400+450 Hz, 0.4 on / 0.2 off / 0.4 on / 2 s off */
    RINGTONE_BELL,        /* 1050+1400 Hz warble, 0.7 s on / 1.8 s off */
    RINGTONE_CHIME,       /* rising three-note chime, then silence */
    RINGTONE_PULSE,       /* three short 900 Hz beeps */
} ringtone_id_t;

/* Called before every 20 ms frame; returning false stops a looped ringtone
 * without touching the output path (the call audio may have taken it over). */
typedef bool (*ringtone_gate_fn)(void);

/* True while another consumer (the call audio) is using the speaker, so a tone
 * must not disable the output path when it ends. */
typedef bool (*ringtone_busy_fn)(void);

esp_err_t ringtone_init(void);

const char *ringtone_name(int id);

/* Loops the pattern until ringtone_stop(). Volume is the ring level, 0-100. */
esp_err_t ringtone_start(int id, uint8_t volume);

/* stop_output: true when the ringtone enabled the speaker and nothing else
 * uses it; false when the call audio already owns the output path. */
void ringtone_stop(bool stop_output);

/* Plays the pattern once, for the ring-tone picker. */
esp_err_t ringtone_preview(int id, uint8_t volume);

/* Plays the DTMF pair for a dialpad key ('0'-'9', '*', '#'). A ring or a
 * preview already using the speaker wins; a new key restarts the tone. */
esp_err_t ringtone_play_key(char key, uint8_t volume);

bool ringtone_is_playing(void);

/* True while a looped ringtone is running, as opposed to a one-shot preview. */
bool ringtone_is_ringing(void);

/* Installs the predicate used for looped ringtones (usually "the call is
 * still ringing"). */
void ringtone_set_gate(ringtone_gate_fn gate);

void ringtone_set_busy_fn(ringtone_busy_fn busy);
