#pragma once
#include "Application/Settings/DisplaySettings.h"

void ariaCompanionInitialize(const DisplaySettings& displaySettings);
extern "C" void aria_companion_vblank();
