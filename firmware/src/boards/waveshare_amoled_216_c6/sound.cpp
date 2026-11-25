#include "../../hal/sound_hal.h"
#include "board.h"

#include "../../chime.h"

// C6 AMOLED-2.16: ES8311 codec + speaker. Codec/I2S/playback live in the shared
// chime engine (../../chime.cpp); this file only supplies the board's pins.
// No amp-enable hook: the amp has no MCU-controlled enable on this board, so
// the engine mutes the codec between dings. Wire is already up (board_init()
// ran it).

// Codec volume 75, as on the 1.8 and LCD-1.54 (the S3 2.16 ships 77, tuned by
// ear on its own speaker). Heard at boot and on alerts on this board's speaker
// but not yet tuned (`chime 0.7 <v>`).
#define CHIME_VOLUME 75

void sound_hal_init(void) {
#ifdef CLAWD_C6_SETTINGS
    // Wi-Fi AP + BLE + full 480x480 LVGL animation leaves too little internal
    // RAM for the C6's pre-rendered ding and I2S driver. The travel/settings
    // variant keeps the visual Clawd and reserves that RAM for networking.
    return;
#else
    const ChimeConfig cfg = {
        SND_I2S_MCLK, SND_I2S_BCLK, SND_I2S_WS, SND_I2S_DOUT, SND_I2S_DIN,
        SND_SAMPLE_RATE, SND_ES8311_ADDR, CHIME_VOLUME, nullptr
    };
    chime_init(cfg);
#endif
}

void sound_hal_chime(void) {
#ifndef CLAWD_C6_SETTINGS
    chime_play();
#endif
}
void sound_hal_tune(float gain, int volume) {
#ifndef CLAWD_C6_SETTINGS
    chime_tune(gain, volume);
#endif
}
void sound_hal_tick(void) {
#ifndef CLAWD_C6_SETTINGS
    chime_tick();
#endif
}
