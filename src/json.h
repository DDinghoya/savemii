#ifndef _JSON_H
#define _JSON_H

#include <cstdio>
#include <string>
#include <jansson.h>

#include "savemng.h"

std::string getSlotDate(uint32_t highID, uint32_t lowID, uint8_t slot);

auto setSlotDate(uint32_t highID, uint32_t lowID, uint8_t slot, std::string date) -> bool;

#endif