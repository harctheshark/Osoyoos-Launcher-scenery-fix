/*
 Copyright (c) num0005. Some rights reserved
 This software is part of the Osoyoos Launcher.
 Released under the MIT License, see LICENSE.md for more information.

 H2 lightmapper scenery / dynamic-object bake fix.
 See Documentation / Lightmapper_Scenery_Object_Bake_Fix.md for the full RE writeup.

 The lightmapper feeds render_model verts to the occluder builder and the per-vertex lighting gather WITHOUT
 applying the section compression bounding box, so dynamic objects are treated as a raw ~2x2x2 normalized cube:
 oversized/offset/blobby-black shadows AND wrong per-vertex colors. Fix = decompress the (shared) normalized
 vertex buffer in place (world = h*vNorm + c), gated to scenery only so BSP / instanced-geo / rmdl are untouched.

 Two hooks on the STOCK tool_fast.exe (no-ASLR, ImageBase 0x400000):
   sub_49CF55 (occluder insert)  -> scenery gate: track caller so only scenery inserts decompress
   sub_49DA6A (add vertex)       -> decompress the vertex buffer in place, once per vbuf

 Clang-cl has no MSVC __asm/naked support, so the detour stubs are built as raw machine code at runtime and the
 hook logic lives in plain C handlers.
*/

#include "platform.h"
#include "patches.h"
#include "Debug.h"
#include <cstdint>

// tool_fast.exe absolute addresses (ImageBase 0x400000, no ASLR)
namespace {
	constexpr uint32_t kBase          = 0x400000;
	constexpr uint32_t kSub_49CF55    = 0x49CF55;   // occluder insert
	constexpr uint32_t kSub_49CF55End = 0x49D326;
	constexpr uint32_t kSub_498962    = 0x498962;   // scenery import (caller of the occluder insert)
	constexpr uint32_t kSub_498962End = 0x49946B;
	constexpr uint32_t kSub_49DA6A    = 0x49DA6A;   // add vertex
	constexpr uint32_t kBack_49CF5D   = 0x49CF5D;   // sub_49CF55 + 8 (after stolen prologue)
	constexpr uint32_t kBack_49DA72   = 0x49DA72;   // sub_49DA6A + 8

	// --- state ---
	long g_sceneryActive = 0;
	long g_doneVbufN = 0;
	uint32_t g_doneVbuf[1024];
}

// scenery gate: set when the occluder insert (sub_49CF55) is called from scenery import (sub_498962); recursion
// within sub_49CF55 inherits; every other caller (BSP / instanced / rmdl) clears it.
extern "C" void __cdecl osoyoos_lm_scenery_gate(uint32_t ra)
{
	if (ra >= kSub_498962 && ra < kSub_498962End) g_sceneryActive = 1;
	else if (ra >= kSub_49CF55 && ra < kSub_49CF55End) { /* inherit */ }
	else g_sceneryActive = 0;
}

// decompress the normalized vertex buffer in place. a2ptr = &mesh; mesh v7=*a2ptr; v7+36=vcount, v7+40=vbuf
// (196B/vert, pos@+0), v7+4=section, section+48 = 6-float compression bbox. Scenery-gated + normalized-only.
extern "C" void __cdecl osoyoos_lm_decompress(uint32_t a2ptr)
{
	if (!g_sceneryActive) return;
	uint32_t v7 = *reinterpret_cast<uint32_t*>(a2ptr);
	uint32_t sec = *reinterpret_cast<uint32_t*>(v7 + 4);
	if (sec < 0x400000 || sec >= 0x40000000) return;
	uint32_t vb = *reinterpret_cast<uint32_t*>(v7 + 40);
	if (vb < 0x400000 || vb >= 0x40000000) return;
	int vcount = *reinterpret_cast<int*>(v7 + 36);
	if (vcount <= 0 || vcount > 200000) return;
	float* v0 = reinterpret_cast<float*>(vb);
	if (v0[0] < -1.5f || v0[0] > 1.5f || v0[1] < -1.5f || v0[1] > 1.5f || v0[2] < -1.5f || v0[2] > 1.5f) return; // normalized only
	float* bb = reinterpret_cast<float*>(sec + 48);
	float hx = (bb[1] - bb[0]) * 0.5f, hy = (bb[3] - bb[2]) * 0.5f, hz = (bb[5] - bb[4]) * 0.5f;
	float cx = (bb[1] + bb[0]) * 0.5f, cy = (bb[3] + bb[2]) * 0.5f, cz = (bb[5] + bb[4]) * 0.5f;
	if (hx <= 0.001f || hy <= 0.001f || hz <= 0.001f || hx > 50.0f || hy > 50.0f || hz > 50.0f) return;
	for (long i = 0; i < g_doneVbufN && i < 1024; i++) if (g_doneVbuf[i] == vb) return; // already decompressed
	if (g_doneVbufN < 1024) g_doneVbuf[g_doneVbufN++] = vb;
	for (int i = 0; i < vcount; i++) {
		float* v = reinterpret_cast<float*>(vb + 196 * i);
		v[0] = hx * v[0] + cx; v[1] = hy * v[1] + cy; v[2] = hz * v[2] + cz;
	}
}

namespace {
	// Build a RWX detour stub: save regs -> call `handler` with one dword arg read from [esp+arg_off] -> restore
	// regs -> replay `stolen` prologue bytes -> jmp back to `back_addr`. `arg_off` is the displacement in the
	// `mov eax,[esp+arg_off]` (already accounts for pushad(32)+pushfd(4)).
	void* build_stub(uint8_t arg_off, void* handler, const uint8_t* stolen, size_t stolen_len, uint32_t back_addr)
	{
		uint8_t buf[64]; size_t n = 0;
		buf[n++] = 0x60;                       // pushad
		buf[n++] = 0x9C;                       // pushfd
		buf[n++] = 0x8B; buf[n++] = 0x44; buf[n++] = 0x24; buf[n++] = arg_off; // mov eax,[esp+arg_off]
		buf[n++] = 0x50;                       // push eax
		buf[n++] = 0xE8; size_t call_rel = n; n += 4;                          // call handler (rel patched below)
		buf[n++] = 0x83; buf[n++] = 0xC4; buf[n++] = 0x04;                     // add esp,4
		buf[n++] = 0x9D;                       // popfd
		buf[n++] = 0x61;                       // popad
		for (size_t i = 0; i < stolen_len; i++) buf[n++] = stolen[i];          // replay stolen prologue
		buf[n++] = 0x68; *reinterpret_cast<uint32_t*>(buf + n) = back_addr; n += 4; // push back_addr
		buf[n++] = 0xC3;                       // ret

		uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!mem) return nullptr;
		memcpy(mem, buf, n);
		int32_t rel = static_cast<int32_t>(reinterpret_cast<size_t>(handler) - (reinterpret_cast<size_t>(mem) + call_rel + 4));
		memcpy(mem + call_rel, &rel, 4);
		return mem;
	}
}

// Returns true if the fix was applied. Safe no-op (returns false) on any binary that doesn't match the expected
// stock tool_fast.exe prologues, so it can never corrupt an unexpected target.
bool apply_lightmap_scenery_fix()
{
	DebugPrintf("Applying lightmap scenery/dynamic-object bake fix");

	if (reinterpret_cast<uint32_t>(GetModuleHandleW(nullptr)) != kBase)
	{
		DebugPrintf("lightmap fix: unexpected image base, skipping (need no-ASLR 0x400000)");
		return false;
	}

	const uint8_t stolen_cf55[8] = { 0x55, 0x8D, 0x6C, 0x24, 0xA0, 0x83, 0xEC, 0x60 }; // push ebp/lea ebp,[esp-60h]/sub esp,60h
	const uint8_t stolen_da6a[8] = { 0x83, 0xEC, 0x3C, 0xA1, 0xFC, 0x7B, 0xBC, 0x00 }; // sub esp,3Ch/mov eax,[00BC7BFC]

	if (memcmp(reinterpret_cast<void*>(kSub_49CF55), stolen_cf55, sizeof(stolen_cf55)) != 0 ||
		memcmp(reinterpret_cast<void*>(kSub_49DA6A), stolen_da6a, sizeof(stolen_da6a)) != 0)
	{
		DebugPrintf("lightmap fix: tool prologue mismatch, skipping (not the expected stock tool_fast.exe)");
		return false;
	}

	// arg offsets: cf55 reads the return address at orig [esp] -> [esp+0x24]; da6a reads a2 at orig [esp+4] -> [esp+0x28]
	void* cf55_stub = build_stub(0x24, reinterpret_cast<void*>(&osoyoos_lm_scenery_gate), stolen_cf55, sizeof(stolen_cf55), kBack_49CF5D);
	void* da6a_stub = build_stub(0x28, reinterpret_cast<void*>(&osoyoos_lm_decompress), stolen_da6a, sizeof(stolen_da6a), kBack_49DA72);
	if (!cf55_stub || !da6a_stub)
	{
		DebugPrintf("lightmap fix: failed to allocate detour stubs");
		return false;
	}

	WriteJmp(static_cast<size_t>(kSub_49CF55), reinterpret_cast<size_t>(cf55_stub));
	NopFill(static_cast<size_t>(kSub_49CF55) + 5, 3);
	WriteJmp(static_cast<size_t>(kSub_49DA6A), reinterpret_cast<size_t>(da6a_stub));
	NopFill(static_cast<size_t>(kSub_49DA6A) + 5, 3);

	DebugPrintf("lightmap scenery fix applied (scenery gate @0x%X, decompress @0x%X)", kSub_49CF55, kSub_49DA6A);
	return true;
}
