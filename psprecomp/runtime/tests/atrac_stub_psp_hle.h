#pragma once
#include <cstdint>
#include "recomp.h"
using HleFunc = void(*)(uint8_t*, recomp_context*);
void psp_hle_register(const char* nid_name, HleFunc fn);
