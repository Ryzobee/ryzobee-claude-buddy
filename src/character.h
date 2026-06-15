#pragma once
#include <stdint.h>
#include <LovyanGFX.hpp>

struct Palette {
  uint16_t body, bg, text, textDim, ink;
};

// Call after board init and sprite allocation. Mounts LittleFS, reads
// /characters/<name>/manifest.json, parses colors, caches GIF paths.
bool characterInit(const char* name);
bool characterLoaded();
void characterSetTarget(lgfx::LGFX_Device* lcd, lgfx::LGFX_Sprite* sprite);

// 0..6: sleep, idle, busy, attention, celebrate, dizzy, heart.
// Closes current GIF, opens the one for this state. No-op if same state.
void characterSetState(uint8_t state);

// Advances timing; if it's time for the next frame, decodes it into the
// sprite. Call every loop iteration. Does nothing if not loaded.
void characterTick();
void characterInvalidate();
void characterClose();

// Peek mode renders the GIF at half scale, centered in the info-panel
// header strip; off renders full-size centered in the upper home area.
void characterSetPeek(bool peek);
void characterRenderTo(lgfx::LGFX_Device* tgt, int cx, int cy);

const Palette& characterPalette();
