#pragma once
#include "Application/Settings/DisplaySettings.h"

void ariaCompanionInitialize(u32 gameCode, const DisplaySettings& displaySettings);
extern "C" void aria_companion_vblank();
