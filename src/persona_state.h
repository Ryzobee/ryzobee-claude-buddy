#pragma once
#include <stdint.h>

enum PersonaState : uint8_t {
  P_SLEEP,
  P_IDLE,
  P_BUSY,
  P_ATTENTION,
  P_CELEBRATE,
  P_DIZZY,
  P_HEART
};
