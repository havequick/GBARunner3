#pragma once
#include "Application/Settings/DisplaySettings.h"

void ariaCompanionInitialize(const DisplaySettings& displaySettings);
extern "C" int aria_companion_vblank();
