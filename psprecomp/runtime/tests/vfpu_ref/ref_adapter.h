// PPSSPP-free interface to the reference VFPU interpreter (see ref_adapter.cpp).
#pragma once
#include <cstdint>
#include <cstring>
struct RefState {
	float v[128];        // physical layout vfpu[mtx*16 + col*4 + row], same as recomp_context::vfpu
	uint32_t ctrl[16];
	uint32_t r[32];
};
void ref_init();
void ref_set_accurate_dot(int on);
unsigned char *ref_mem();     // 64 KiB guest memory at 0x08000000
int ref_fn_count();
const char *ref_fn_name(int i);
int ref_fn_find(const char *name);
void ref_run(int fn, uint32_t op, RefState *st);
