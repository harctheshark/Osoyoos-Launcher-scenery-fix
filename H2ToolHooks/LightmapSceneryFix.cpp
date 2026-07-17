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
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------------------------------------------
// Runtime diagnostic dump (env-gated: OSOYOOS_H2V_LMDUMP=1). Inert in production. For each scenery-gated occluder
// insert it appends the raw field values so we can (a) find the TRUE per-section position compression bound and
// (b) correlate sections to placed objects by their placement translation. Writes to a fixed scratchpad path.
// ---------------------------------------------------------------------------------------------------------------
namespace {
	int g_dumpEnabled = -1; // -1 = unchecked
	const char* kDumpPath = "C:\\Users\\hurri\\AppData\\Local\\Temp\\claude\\G--SteamLibrary-steamapps-common-H2EKMine\\cb4a0d95-25a2-4866-85bf-5caf1da6bf1a\\scratchpad\\h2v_lmdump.txt";

	bool dump_on()
	{
		if (g_dumpEnabled < 0)
		{
			char v[2] = {};
			g_dumpEnabled = (GetEnvironmentVariableA("OSOYOOS_H2V_LMDUMP", v, sizeof(v)) != 0) ? 1 : 0;
		}
		return g_dumpEnabled == 1;
	}

	inline bool ptr_ok(uint32_t p) { return p >= 0x400000 && p < 0x40000000; }

	void diag_dump(uint32_t ra, uint32_t section, uint32_t a4, uint32_t P)
	{
		if (!dump_on()) return;
		if (!ptr_ok(section)) return;
		FILE* f = nullptr;
		if (fopen_s(&f, kDumpPath, "a") != 0 || !f) return;

		// canonical parent snapshot: full 104-byte P as dwords + follow every plausible pointer (hunt the
		// compression_info desc; oracle = the vc=266 section whose true bound is -2.05,2.05,-0.9,0.9,-0.1,1.7)
		if (ptr_ok(P) && !IsBadReadPtr(reinterpret_cast<void*>(P), 104)) {
			uint32_t offGeom = *reinterpret_cast<uint32_t*>(P + 0x38);
			fprintf(f, "P=%08X pair=%s\n", P, (offGeom == section) ? "OK" : "STALE");
			uint32_t* pd = reinterpret_cast<uint32_t*>(P);
			for (int base = 0; base < 104; base += 32) {
				fprintf(f, "  Px+%02X:", base);
				for (int k = 0; k < 8 && base + k * 4 < 104; k++) fprintf(f, " %08X", pd[(base / 4) + k]);
				fprintf(f, "\n");
			}
			if (offGeom == section) {
				for (int off = 0; off < 104; off += 4) {
					uint32_t v = pd[off / 4];
					if (ptr_ok(v) && v != section && !IsBadReadPtr(reinterpret_cast<void*>(v), 40)) {
						float* c = reinterpret_cast<float*>(v);
						fprintf(f, "  *P+%02X (%08X)-> f:", off, v);
						for (int k = 0; k < 10; k++) fprintf(f, " %.4g", c[k]);
						fprintf(f, "\n");
					}
				}
			}
		}
		else fprintf(f, "P=%08X (invalid/none)\n", P);

		int    pc = *reinterpret_cast<int*>(section + 0x00);
		uint32_t parts = *reinterpret_cast<uint32_t*>(section + 0x04);
		int    vc = *reinterpret_cast<int*>(section + 0x24);
		uint32_t vbuf = *reinterpret_cast<uint32_t*>(section + 0x28);
		fprintf(f, "SEC=%08X ra=%08X parts=%d vcount=%d partsHeap=%08X vbuf=%08X\n", section, ra, pc, vc, parts, vbuf);

		// placement transform (52 bytes = 13 floats: [0]=scale, [1..9]=R, [10..12]=T). a4 is a STACK ptr.
		if (a4 != 0) {
			float* m = reinterpret_cast<float*>(a4);
			fprintf(f, "  xform scale=%.4f  T=(%.3f, %.3f, %.3f)\n", m[0], m[10], m[11], m[12]);
		}

		// raw vertex min/max across all verts + first vert (confirm [-1,1] normalization & catch already-decompressed)
		if (ptr_ok(vbuf) && vc > 0) {
			float mn[3] = { 1e30f,1e30f,1e30f }, mx[3] = { -1e30f,-1e30f,-1e30f };
			for (int i = 0; i < vc; i++) {
				float* v = reinterpret_cast<float*>(vbuf + 196 * i);
				for (int c = 0; c < 3; c++) { if (v[c] < mn[c]) mn[c] = v[c]; if (v[c] > mx[c]) mx[c] = v[c]; }
			}
			float* v0 = reinterpret_cast<float*>(vbuf);
			fprintf(f, "  vert0=(%.4f, %.4f, %.4f)  vmin=(%.3f,%.3f,%.3f) vmax=(%.3f,%.3f,%.3f)\n",
				v0[0], v0[1], v0[2], mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
		}

		// dump every part fully (72 bytes = 18 floats, idx0..17); fix currently reads idx10..15 (+0x28)
		if (ptr_ok(parts)) {
			for (int p = 0; p < pc && p < 8; p++) {
				float* pf = reinterpret_cast<float*>(parts + 72 * p);
				fprintf(f, "  part%d f0-17:", p);
				for (int k = 0; k < 18; k++) fprintf(f, " %.4g", pf[k]);
				fprintf(f, "\n");
			}
		}

		// section struct as HEX dwords over a wide window (find pointers / int fields)
		uint32_t* sd = reinterpret_cast<uint32_t*>(section);
		for (int base = 0; base < 0xC0; base += 0x20) {
			fprintf(f, "  Sx+%02X:", base);
			for (int k = 0; k < 8; k++) fprintf(f, " %08X", sd[(base / 4) + k]);
			fprintf(f, "\n");
		}
		// follow any tag-heap pointer in the first 0x40 of the section, dump 8 floats there (hunt compression block)
		for (int off = 0; off < 0x40; off += 4) {
			uint32_t p = sd[off / 4];
			if (ptr_ok(p) && !IsBadReadPtr(reinterpret_cast<void*>(p), 32)) {
				float* pf = reinterpret_cast<float*>(p);
				fprintf(f, "  *S+%02X (%08X)-> f:", off, p);
				for (int k = 0; k < 8; k++) fprintf(f, " %.3g", pf[k]);
				fprintf(f, "\n");
			}
		}
		fprintf(f, "\n");
		fclose(f);
	}
	// ---- tag-file bounds database (h2v_scenery_bounds.db) ----
	// One line per unique section fingerprint: 14 words + 6 bound floats + ambiguous flag. Generated offline
	// from every *.render_model under the tag root (Tools/generate_scenery_bounds_db.py). The compression
	// bound is MODEL-WIDE (all sections of a model share it - verified across all 2084 tags), so any
	// unambiguously-resolved section also resolves its sibling sections (same 104-byte element array).
	struct BoundsRec { uint16_t w[14]; float b[6]; uint8_t ambig; uint32_t striptot; };
	BoundsRec* g_db = nullptr;
	int  g_dbN = -1;                                // -1 = not loaded yet
	uint32_t g_lastResolvedP = 0;                   // sibling cache (unambiguous DB hits only)
	float    g_lastResolvedB[6];

	void load_bounds_db(const char* toolName)
	{
		if (g_dbN >= 0) return;
		g_dbN = 0;
		const char* candidates[3] = { nullptr, "tags\\h2v_scenery_bounds.db", "h2v_scenery_bounds.db" };
		char envPath[MAX_PATH] = {};
		if (GetEnvironmentVariableA("OSOYOOS_H2V_BOUNDS_DB", envPath, sizeof(envPath))) candidates[0] = envPath;
		FILE* f = nullptr;
		const char* used = nullptr;
		for (int i = 0; i < 3; i++)
		{
			if (!candidates[i]) continue;
			if (fopen_s(&f, candidates[i], "r") == 0 && f) { used = candidates[i]; break; }
			f = nullptr;
		}
		if (!f)
		{
			DebugPrintf("%s scenery fix: h2v_scenery_bounds.db NOT FOUND (cwd/tags/env) - compressed sections with"
				" empty bounds (e.g. concrete chunks) will not be corrected", toolName);
			return;
		}
		int cap = 8192;
		g_db = static_cast<BoundsRec*>(malloc(cap * sizeof(BoundsRec)));
		char line[512];
		while (g_db && g_dbN < cap && fgets(line, sizeof(line), f))
		{
			BoundsRec r; int wtmp[14]; int atmp; int sttmp = 0;
			int got = sscanf_s(line, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %f %f %f %f %f %f %d %d",
				&wtmp[0], &wtmp[1], &wtmp[2], &wtmp[3], &wtmp[4], &wtmp[5], &wtmp[6], &wtmp[7], &wtmp[8],
				&wtmp[9], &wtmp[10], &wtmp[11], &wtmp[12], &wtmp[13],
				&r.b[0], &r.b[1], &r.b[2], &r.b[3], &r.b[4], &r.b[5], &atmp, &sttmp);
			if (got < 21) continue;
			for (int k = 0; k < 14; k++) r.w[k] = static_cast<uint16_t>(wtmp[k]);
			r.ambig = static_cast<uint8_t>(atmp);
			r.striptot = static_cast<uint32_t>(got >= 22 ? sttmp : 0);
			g_db[g_dbN++] = r;
		}
		fclose(f);
		DebugPrintf("%s scenery fix: loaded %d bounds entries from %s", toolName, g_dbN, used);
	}

	// Full-fingerprint lookup (Vista): prefer the row that also matches the total strip index count
	// (distinguishes same-topology sections from different models); fall back to words-only.
	const BoundsRec* db_lookup(const uint16_t* w, uint32_t striptot)
	{
		const BoundsRec* wordsOnly = nullptr;
		for (int i = 0; i < g_dbN; i++)
		{
			if (memcmp(g_db[i].w, w, 14 * sizeof(uint16_t)) != 0) continue;
			if (striptot && g_db[i].striptot == striptot) return &g_db[i];
			if (!wordsOnly) wordsOnly = &g_db[i];
		}
		return wordsOnly;
	}

	// MCC-side reduced-key lookup: the MCC tool exposes no tag fingerprint words, but (vertex count,
	// part count, total strip index count) are all reachable at the hook AND derivable from the tag files.
	// On multiple matches with differing bounds, prefer the largest volume (same policy as ambiguous keys).
	const BoundsRec* mcc_db_lookup(int vcount, int parts, uint32_t striptot)
	{
		if (striptot == 0) return nullptr;
		const BoundsRec* best = nullptr; float bestVol = -1.0f;
		for (int i = 0; i < g_dbN; i++)
		{
			const BoundsRec* r = &g_db[i];
			if (r->w[2] != vcount || r->w[4] != parts || r->striptot != striptot) continue;
			float vol = (r->b[1] - r->b[0]) * (r->b[3] - r->b[2]) * (r->b[5] - r->b[4]);
			if (vol > bestVol) { best = r; bestVol = vol; }
		}
		return best;
	}

	// ---- placement SCALE respect (shared by both tools) ----
	// Both scenery importers build the occluder transform with scale hardcoded to 1.0; the occluder vertex
	// decode (Vista sub_592C90 / MCC sub_5AFB7F) computes R*(m0*v)+T with a real uniform-scale slot m0 =
	// transform[0]. Our hook runs BEFORE the tool copies the transform, so we write the placement's true
	// scale into transform[0] per placement (the shared vertex buffer is never touched). Scale is recovered
	// by matching the transform translation T against the scenario scenery datums (block +128 count / +132
	// ptr, stride 96, pos @+8, scale @+32 with 0 == 1.0). A failed match changes nothing (safe no-op).
	// Env OSOYOOS_H2V_SCALEMODE: unset/"1" = apply true scale; "skip" = collapse (m0=0, no shadow); "off".
	int g_scaleMode = -2;                            // -2 unchecked, 0 off, 1 apply, 2 skip-shadow
	uint32_t g_scaleLogged[128]; int g_scaleLoggedN = 0;

	int scale_mode()
	{
		if (g_scaleMode == -2)
		{
			char v[8] = {};
			GetEnvironmentVariableA("OSOYOOS_H2V_SCALEMODE", v, sizeof(v));
			if (v[0] == 'o' || v[0] == 'O' || v[0] == '0') g_scaleMode = 0;        // off
			else if (v[0] == 's' || v[0] == 'S') g_scaleMode = 2;                  // skip
			else g_scaleMode = 1;                                                  // apply (default)
		}
		return g_scaleMode;
	}

	// returns the placement scale for translation T within a scenario's scenery datum block, or -1.0f if no
	// datum matched. scenarioBase = Vista sub_57C000() / MCC *(0xC78CF4).
	float find_placement_scale(uint32_t scenarioBase, const float* T)
	{
		if (scenarioBase < 0x400000 || scenarioBase >= 0x40000000) return -1.0f;
		int cnt = *reinterpret_cast<int*>(scenarioBase + 128);
		uint32_t base = *reinterpret_cast<uint32_t*>(scenarioBase + 132);
		if (cnt <= 0 || cnt > 100000 || base < 0x400000 || base >= 0x40000000) return -1.0f;
		if (IsBadReadPtr(reinterpret_cast<void*>(base), cnt * 96)) return -1.0f;
		for (int i = 0; i < cnt; i++)
		{
			float* d = reinterpret_cast<float*>(base + 96 * i);
			float dx = d[2] - T[0], dy = d[3] - T[1], dz = d[4] - T[2];   // datum pos @ +8/+12/+16
			if (dx > -0.01f && dx < 0.01f && dy > -0.01f && dy < 0.01f && dz > -0.01f && dz < 0.01f)
			{
				float s = d[8];                                            // datum scale @ +32 (0 == default 1.0)
				return (s <= 0.0001f) ? 1.0f : s;
			}
		}
		return -1.0f;
	}

	// write the placement scale into the occluder transform's m0 slot (xf[0]). xf = the 52-byte transform
	// (m0 @[0], R @[1..9], T @[10..12]) BEFORE the tool copies it. toolName tags the console line.
	void apply_placement_scale(uint32_t xfAddr, uint32_t scenarioBase, const char* toolName)
	{
		if (!xfAddr || scale_mode() == 0) return;
		float* xf = reinterpret_cast<float*>(xfAddr);
		float* T = xf + 10;
		float s = find_placement_scale(scenarioBase, T);
		if (dump_on() && s < 0.0f) {
			FILE* f = nullptr;
			if (fopen_s(&f, kDumpPath, "a") == 0 && f) { fprintf(f, "SCALEMISS %s T=(%.3f, %.3f, %.3f)\n", toolName, T[0], T[1], T[2]); fclose(f); }
		}
		if (s < 0.0f) return;                                              // no datum matched - leave untouched
		if (s > 0.995f && s < 1.005f) return;                             // unscaled
		if (s < 0.01f || s > 100.0f) return;                             // garbage guard
		xf[0] = (scale_mode() == 2) ? 0.0f : s;                          // m0: true scale, or collapse (no shadow)
		uint32_t key = *reinterpret_cast<uint32_t*>(&s) ^ *reinterpret_cast<uint32_t*>(T) ^ *reinterpret_cast<uint32_t*>(T + 1);
		for (int i = 0; i < g_scaleLoggedN; i++) if (g_scaleLogged[i] == key) return;
		if (g_scaleLoggedN < 128) g_scaleLogged[g_scaleLoggedN++] = key;
		DebugPrintf("%s scenery scale: placement (%.2f, %.2f, %.2f) scale=%.3f -> %s",
			toolName, T[0], T[1], T[2], s, (scale_mode() == 2) ? "shadow suppressed" : "occluder scaled");
	}
}

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
// within sub_49CF55 inherits; every other caller (BSP / instanced / rmdl) clears it. a4 = the transform matrix
// sub_49CF55 is about to `qmemcpy(this, a4, 0x34)` and hand to sub_5AFB7F (out = R*(m0*v)+T). We run before
// that copy, so we write the placement scale into a4[0] for scenery placements only (same lever as Vista).
extern "C" void __cdecl osoyoos_lm_scenery_gate(uint32_t ra, uint32_t a4)
{
	if (ra >= kSub_498962 && ra < kSub_498962End) g_sceneryActive = 1;
	else if (ra >= kSub_49CF55 && ra < kSub_49CF55End) { /* inherit */ }
	else g_sceneryActive = 0;
	if (g_sceneryActive && ra >= kSub_498962 && ra < kSub_498962End)  // scenery placement (not recursion): scale it
		apply_placement_scale(a4, *reinterpret_cast<uint32_t*>(0xC78CF4), "MCC");   // MCC scenario = *(g_scenario)
}

// env-gated (OSOYOOS_H2V_LMDUMP) MCC-side struct dump, once per vbuf: hunt the tag fingerprint words
// around the mesh/section structs so the MCC branch can use the bounds DB like the Vista branch does.
static void mcc_diag_dump(uint32_t a2ptr, uint32_t v7, uint32_t sec, uint32_t vb, int vcount);

// decompress the normalized vertex buffer in place. a2ptr = &mesh; mesh v7=*a2ptr; v7+0=part count,
// v7+36=vcount, v7+40=vbuf (196B/vert, pos@+0), v7+48=total strip index count, v7+4=section.
// Scenery-gated + normalized-only, once per vbuf.
//
// BOUND SOURCE (v4): the runtime value at section+48 is only PART[0]'s model-space AABB - NOT the tag's
// compression bound (verts are normalized MODEL-WIDE; multi-section models like the crashed pelican and any
// 'Geometry Postprocessed' model - whose boxes are all zero, e.g. concrete chunks - bake wrong with it).
// Primary source = the tag-file bounds DB, matched by the MCC-reachable key (vcount, part count, total strip
// indices); section+48 stays as the fallback, including the legacy collapse-on-zero for DB misses.
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
	mcc_diag_dump(a2ptr, v7, sec, vb, vcount);                        // once per vbuf, inert without env var
	for (long i = 0; i < g_doneVbufN && i < 1024; i++) if (g_doneVbuf[i] == vb) return; // already decompressed
	if (g_doneVbufN < 1024) g_doneVbuf[g_doneVbufN++] = vb;

	load_bounds_db("MCC");
	int parts = *reinterpret_cast<int*>(v7 + 0);
	uint32_t striptot = *reinterpret_cast<uint32_t*>(v7 + 48);
	const float* bb = nullptr;
	if (parts >= 1 && parts <= 128)
	{
		const BoundsRec* r = mcc_db_lookup(vcount, parts, striptot);
		if (r) bb = r->b;                                             // true model-wide compression bound
	}
	bool fromDb = bb != nullptr;
	if (!bb) bb = reinterpret_cast<float*>(sec + 48);                 // legacy fallback: part[0] AABB
	if (dump_on()) {                                                  // resolution trace (env-gated)
		FILE* f = nullptr;
		if (fopen_s(&f, kDumpPath, "a") == 0 && f) {
			fprintf(f, "MCCRES vb=%08X vc=%d parts=%d strips=%u src=%s bb=%.4f %.4f %.4f %.4f %.4f %.4f\n",
				vb, vcount, parts, striptot, fromDb ? "DB" : "sec+48", bb[0], bb[1], bb[2], bb[3], bb[4], bb[5]);
			fclose(f);
		}
	}
	float hx = (bb[1] - bb[0]) * 0.5f, hy = (bb[3] - bb[2]) * 0.5f, hz = (bb[5] - bb[4]) * 0.5f;
	float cx = (bb[1] + bb[0]) * 0.5f, cy = (bb[3] + bb[2]) * 0.5f, cz = (bb[5] + bb[4]) * 0.5f;
	// Legacy fallback semantics kept for DB misses: an all-~0 bbox decompresses to a point (zero-area tris
	// the tracer ignores -> no phantom 2x2x2 shadow). Only reject a genuinely GARBAGE bbox.
	if (hx < 0.0f || hy < 0.0f || hz < 0.0f || hx > 50.0f || hy > 50.0f || hz > 50.0f) return;
	for (int i = 0; i < vcount; i++) {
		float* v = reinterpret_cast<float*>(vb + 196 * i);
		v[0] = hx * v[0] + cx; v[1] = hy * v[1] + cy; v[2] = hz * v[2] + cz;
	}
}

static void mcc_diag_dump(uint32_t a2ptr, uint32_t v7, uint32_t sec, uint32_t vb, int vcount)
{
	if (!dump_on()) return;
	static uint32_t seen[256]; static int seenN = 0;
	for (int i = 0; i < seenN; i++) if (seen[i] == vb) return;
	if (seenN < 256) seen[seenN++] = vb;
	FILE* f = nullptr;
	if (fopen_s(&f, kDumpPath, "a") != 0 || !f) return;
	fprintf(f, "MCC a2ptr=%08X v7=%08X sec=%08X vbuf=%08X vcount=%d\n", a2ptr, v7, sec, vb, vcount);
	uint32_t* ctx = reinterpret_cast<uint32_t*>(a2ptr);
	fprintf(f, "  ctx:"); for (int k = 0; k < 8; k++) fprintf(f, " %08X", ctx[k]); fprintf(f, "\n");
	uint32_t* m = reinterpret_cast<uint32_t*>(v7);
	fprintf(f, "  v7 :"); for (int k = 0; k < 16; k++) fprintf(f, " %08X", m[k]); fprintf(f, "\n");
	uint32_t* s = reinterpret_cast<uint32_t*>(sec);
	for (int base = 0; base < 0x70; base += 0x20) {
		fprintf(f, "  sec+%02X:", base);
		for (int k = 0; k < 8; k++) fprintf(f, " %08X", s[(base / 4) + k]);
		fprintf(f, "\n");
	}
	uint16_t* sw = reinterpret_cast<uint16_t*>(sec);
	fprintf(f, "  secW:"); for (int k = 0; k < 28; k++) fprintf(f, " %u", sw[k]); fprintf(f, "\n");
	float* bb = reinterpret_cast<float*>(sec + 48);
	fprintf(f, "  bbox+48: %.4f %.4f %.4f %.4f %.4f %.4f\n", bb[0], bb[1], bb[2], bb[3], bb[4], bb[5]);
	// follow tag-heap-ish pointers in sec[0..0x3C] and v7[0..0x3C]
	for (int off = 0; off < 0x40; off += 4) {
		uint32_t pv = s[off / 4];
		if (ptr_ok(pv) && !IsBadReadPtr(reinterpret_cast<void*>(pv), 56)) {
			uint16_t* pw = reinterpret_cast<uint16_t*>(pv); float* pf = reinterpret_cast<float*>(pv);
			fprintf(f, "  *sec+%02X (%08X) W:", off, pv);
			for (int k = 0; k < 14; k++) fprintf(f, " %u", pw[k]);
			fprintf(f, " | f:"); for (int k = 0; k < 6; k++) fprintf(f, " %.3g", pf[k]);
			fprintf(f, "\n");
		}
	}
	for (int off = 0; off < 0x40; off += 4) {
		uint32_t pv = m[off / 4];
		if (pv != sec && pv != vb && ptr_ok(pv) && !IsBadReadPtr(reinterpret_cast<void*>(pv), 56)) {
			uint16_t* pw = reinterpret_cast<uint16_t*>(pv); float* pf = reinterpret_cast<float*>(pv);
			fprintf(f, "  *v7+%02X (%08X) W:", off, pv);
			for (int k = 0; k < 14; k++) fprintf(f, " %u", pw[k]);
			fprintf(f, " | f:"); for (int k = 0; k < 6; k++) fprintf(f, " %.3g", pf[k]);
			fprintf(f, "\n");
		}
	}
	// vert range
	float mn[3] = { 1e30f,1e30f,1e30f }, mx[3] = { -1e30f,-1e30f,-1e30f };
	for (int i = 0; i < vcount; i++) {
		float* v = reinterpret_cast<float*>(vb + 196 * i);
		for (int c = 0; c < 3; c++) { if (v[c] < mn[c]) mn[c] = v[c]; if (v[c] > mx[c]) mx[c] = v[c]; }
	}
	fprintf(f, "  vmin=(%.3f,%.3f,%.3f) vmax=(%.3f,%.3f,%.3f)\n\n", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
	fclose(f);
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

	// Two-arg variant: __cdecl handler(arg0, arg1). Pushes [esp+push1_disp] then [esp+push0_disp] (right-to-left),
	// so push1_disp addresses arg1 and push0_disp (measured AFTER the first push) addresses arg0.
	void* build_stub2(uint8_t push1_disp, uint8_t push0_disp, void* handler, const uint8_t* stolen, size_t stolen_len, uint32_t back_addr)
	{
		uint8_t buf[64]; size_t n = 0;
		buf[n++] = 0x60;                                                        // pushad
		buf[n++] = 0x9C;                                                        // pushfd
		buf[n++] = 0xFF; buf[n++] = 0x74; buf[n++] = 0x24; buf[n++] = push1_disp; // push dword [esp+push1_disp] (arg1)
		buf[n++] = 0xFF; buf[n++] = 0x74; buf[n++] = 0x24; buf[n++] = push0_disp; // push dword [esp+push0_disp] (arg0)
		buf[n++] = 0xE8; size_t call_rel = n; n += 4;                           // call handler
		buf[n++] = 0x83; buf[n++] = 0xC4; buf[n++] = 0x08;                      // add esp,8
		buf[n++] = 0x9D;                                                        // popfd
		buf[n++] = 0x61;                                                        // popad
		for (size_t i = 0; i < stolen_len; i++) buf[n++] = stolen[i];           // replay stolen prologue
		buf[n++] = 0x68; *reinterpret_cast<uint32_t*>(buf + n) = back_addr; n += 4; // push back_addr
		buf[n++] = 0xC3;                                                        // ret
		uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!mem) return nullptr;
		memcpy(mem, buf, n);
		int32_t rel = static_cast<int32_t>(reinterpret_cast<size_t>(handler) - (reinterpret_cast<size_t>(mem) + call_rel + 4));
		memcpy(mem + call_rel, &rel, 4);
		return mem;
	}
}

// MCC tool_fast.exe path. Returns true if applied. Safe no-op (returns false) on any binary that doesn't
// match the expected stock tool_fast.exe prologues (e.g. the H2V tool), so it can never corrupt an unexpected target.
static bool apply_scenery_fix_mcc()
{
	if (reinterpret_cast<uint32_t>(GetModuleHandleW(nullptr)) != kBase)
	{
		DebugPrintf("MCC scenery fix: unexpected image base, skipping (need no-ASLR 0x400000)");
		return false;
	}

	const uint8_t stolen_cf55[8] = { 0x55, 0x8D, 0x6C, 0x24, 0xA0, 0x83, 0xEC, 0x60 }; // push ebp/lea ebp,[esp-60h]/sub esp,60h
	const uint8_t stolen_da6a[8] = { 0x83, 0xEC, 0x3C, 0xA1, 0xFC, 0x7B, 0xBC, 0x00 }; // sub esp,3Ch/mov eax,[00BC7BFC]

	if (memcmp(reinterpret_cast<void*>(kSub_49CF55), stolen_cf55, sizeof(stolen_cf55)) != 0 ||
		memcmp(reinterpret_cast<void*>(kSub_49DA6A), stolen_da6a, sizeof(stolen_da6a)) != 0)
	{
		DebugPrintf("MCC scenery fix: tool prologue mismatch, skipping (not the expected stock tool_fast.exe)");
		return false;
	}

	// gate is __thiscall sub_49CF55(this, a2, a3, a4, ...): after pushad(0x20)+pushfd(0x4), orig [esp]=ra is at
	// [esp+0x24] and orig [esp+0xC]=a4 (transform) is at [esp+0x30]. handler(ra, a4): push a4 (@0x30) then ra
	// (@0x28 after that push). da6a reads a2 at orig [esp+4] -> [esp+0x28].
	void* cf55_stub = build_stub2(0x30, 0x28, reinterpret_cast<void*>(&osoyoos_lm_scenery_gate), stolen_cf55, sizeof(stolen_cf55), kBack_49CF5D);
	void* da6a_stub = build_stub(0x28, reinterpret_cast<void*>(&osoyoos_lm_decompress), stolen_da6a, sizeof(stolen_da6a), kBack_49DA72);
	if (!cf55_stub || !da6a_stub)
	{
		DebugPrintf("MCC scenery fix: failed to allocate detour stubs");
		return false;
	}

	WriteJmp(static_cast<size_t>(kSub_49CF55), reinterpret_cast<size_t>(cf55_stub));
	NopFill(static_cast<size_t>(kSub_49CF55) + 5, 3);
	WriteJmp(static_cast<size_t>(kSub_49DA6A), reinterpret_cast<size_t>(da6a_stub));
	NopFill(static_cast<size_t>(kSub_49DA6A) + 5, 3);

	DebugPrintf("MCC scenery fix applied (scenery gate @0x%X, decompress @0x%X, scale respect + bounds DB)", kSub_49CF55, kSub_49DA6A);
	return true;
}

/*
 H2V / 2007 H2Tool.exe (== H2Tool_codez.exe) scenery/dynamic-object bake fix.

 Same root cause as MCC (the lightmapper feeds the object's compression-normalized [-1,1] render_model
 verts into the occluder with a placement-only transform, never applying the section compression bbox),
 but a different, older MSVC build so the addresses/struct-layout differ. RUNTIME-VERIFIED against a
 lighttest2 scenery bake (H2Tool_codez.exe, ImageBase 0x400000, no ASLR):

   sub_49DB40  scenery import (per-object), range [0x49DB40, 0x49E744)     -> caller-based scenery gate
   sub_4A7420  occluder-insert(this, SECTION @a2, a3, xform @a4, ...)       -> the one hook point
       section+0x00 = part count;  section+0x04 = parts ptr;  part[i] = *(section+4) + 72*i (72-byte parts)
       section+0x24 = vertex count;  section+0x28 = vbuf (196 bytes/vert, position @ +0, normalized [-1,1])
       compression bound = part[i]+0x30 = 6 floats (xmin,xmax,ymin,ymax,zmin,zmax), PER PART
       a4 xform = m0=1.0 + identity rotation + T=placement pos (NO bbox/scale) -> confirms the bug

 A single naked JMP-detour on sub_4A7420: on entry [esp]=return-address, [esp+4]=section. The stub does
 pushad+pushfd (0x24 bytes) so the return address lands at [esp+0x24] and the section at [esp+0x28], then
 calls the handler which scenery-gates on the return address and decompresses the section vbuf in place,
 once per vbuf (world = h*vNorm + c). Clang-cl has no __asm, so the stub is raw machine code.

 v2 FIX (runtime-decoded against 03a_oldmombasa / earthcity_2, 2026-07-15): the section's ONE shared vertex
 buffer is normalized to the SECTION position bound = the min/max UNION of every part's [+0x30] box (part[0]
 alone already equals that union in practice, but unioning is robust). v1 read the wrong window (part[0]+0x28,
 8 bytes early) which used a constant-0 field for x and shifted the true x-bound into y and y into z -> axis
 scramble (crate "rotated"), x squish+offset (concrete "shadows too far"), under-scaled x on multi-part
 objects (guntower). Some simple greeble sections are stored uncompressed (all-zero [+0x30]); those must be
 left as-is (their verts are already true [-1,1] local), so a near-zero unioned extent skips decompression.
*/
namespace {
	constexpr uint32_t kH2V_Occluder = 0x4A7420;   // occluder-insert (hook point)
	constexpr uint32_t kH2V_OccBack  = 0x4A7427;   // 0x4A7420 + 7 (after the stolen 2 pushes)
	constexpr uint32_t kH2V_ScenLo   = 0x49DB40;   // sub_49DB40 scenery import (gate lo)
	constexpr uint32_t kH2V_ScenHi   = 0x49E744;   // sub_49DB40 end (gate hi)
	// section struct offsets
	constexpr uint32_t kH2V_SecPartCount = 0x00;
	constexpr uint32_t kH2V_SecPartsPtr = 0x04;
	constexpr uint32_t kH2V_SecVCount   = 0x24;
	constexpr uint32_t kH2V_SecVBuf     = 0x28;
	constexpr uint32_t kH2V_PartStride  = 72;      // 72-byte part struct
	constexpr uint32_t kH2V_PartBBox    = 0x30;    // part[i] + 0x30 = per-part model-space AABB (xmin,xmax,ymin,ymax,zmin,zmax)
	constexpr int      kH2V_Stride      = 196;
	// stock H2Tool.exe / H2Tool_codez.exe prologue at 0x4A7420: push -1 ; push 7E0630h ; mov eax,fs:0 ...
	const uint8_t kH2V_Stolen[7] = { 0x6A, 0xFF, 0x68, 0x30, 0x06, 0x7E, 0x00 };

	// Section-PARENT stash sites inside sub_49DB40. At each scenery call site the parent struct P
	// (the 104-byte render_model section block element, which owns the canonical geometry_compression_info)
	// sits in EAX for exactly one instruction:  call sub_532970 ; -> eax=P ; add eax,34h ; add esp,0Ch ; ...
	// A 6-byte detour on the two `add eax,34h / add esp,0Ch` pairs stashes EAX before the section (a2) is
	// derived from P and sub_4A7420 is invoked - the pairing is structural (same iteration, same thread).
	constexpr uint32_t kH2V_StashA     = 0x49DF50; // call site 1 (call @0x49DFAB)
	constexpr uint32_t kH2V_StashABack = 0x49DF56;
	constexpr uint32_t kH2V_StashB     = 0x49E163; // call site 2 (call @0x49E1BE)
	constexpr uint32_t kH2V_StashBBack = 0x49E169;
	const uint8_t kH2V_StashStolen[6] = { 0x83, 0xC0, 0x34, 0x83, 0xC4, 0x0C }; // add eax,34h / add esp,0Ch
	// parent struct P: a 104-byte render_model SECTION tag-block element. Its first 28 bytes are 14 words
	// mirroring the tag fields exactly (verified vs Guerilla): [0]=global classification, [1]=pad,
	// [2]=total vertex count, [3]=total tri count, [4]=part count, [5]=shadow tris, [6]=shadow parts,
	// [7]=opaque points, [8]=opaque verts, [9]=opaque parts, [10]=nodes/vertex bytes, [11]=shadow rigid tris,
	// [12]=classification, [13]=GEOMETRY COMPRESSION FLAGS (bit0 = position compressed, bit1 = texcoord).
	// P+0x34 = geometry tag_block {count, elements_ptr, def_ptr}: elements_ptr @P+0x38 == the a2 section
	// (heap base is 0 in this tool - block pointers are direct). The compression_info block at runtime is
	// STRIPPED (count 0) even when the tag file has it, and 'Geometry Postprocessed' models also ship
	// all-zero per-part boxes -> the bounds must come from the tag-file DB (see h2v_scenery_bounds.db).
	constexpr uint32_t kH2V_P_Flags     = 0x1A;    // flags16 word (bit0 = position compressed)
	constexpr uint32_t kH2V_P_GeomCount = 0x34;
	constexpr uint32_t kH2V_P_GeomPtr   = 0x38;    // == a2 section (pairing check)
	constexpr uint32_t kH2V_P_Stride    = 104;     // section elements are 104 bytes apart (sibling detection)

	long g_h2vDoneN = 0;
	uint32_t g_h2vDone[4096];
	volatile uint32_t g_h2vPendingP = 0;           // parent P stashed by the mid-fn hooks, consumed per occluder call

}

// ra = return address (caller of sub_4A7420); section = the render section (a2); a4 = placement transform.
extern "C" void __cdecl osoyoos_h2v_occluder(uint32_t ra, uint32_t section, uint32_t a4)
{
	if (!(ra >= kH2V_ScenLo && ra < kH2V_ScenHi)) return;             // scenery import only
	uint32_t P = g_h2vPendingP; g_h2vPendingP = 0;                    // consume the parent stashed at this call site
	diag_dump(ra, section, a4, P);                                    // env-gated (OSOYOOS_H2V_LMDUMP), inert otherwise
	// per-placement scale respect (runs before the tool copies the transform; every call, not gated by the
	// once-per-vbuf decompress below). Vista scenario base = sub_57C000().
	apply_placement_scale(a4, (reinterpret_cast<uint32_t(__cdecl*)()>(0x57C000))(), "H2V");
	if (section < 0x400000 || section >= 0x40000000) return;
	int pc = *reinterpret_cast<int*>(section + kH2V_SecPartCount);
	if (pc <= 0 || pc > 4096) return;
	uint32_t parts = *reinterpret_cast<uint32_t*>(section + kH2V_SecPartsPtr);
	if (parts < 0x400000 || parts >= 0x40000000) return;
	uint32_t vbuf = *reinterpret_cast<uint32_t*>(section + kH2V_SecVBuf);
	if (vbuf < 0x400000 || vbuf >= 0x40000000) return;
	int vcount = *reinterpret_cast<int*>(section + kH2V_SecVCount);
	if (vcount <= 0 || vcount > 200000) return;
	float* v0 = reinterpret_cast<float*>(vbuf);
	if (v0[0] < -1.5f || v0[0] > 1.5f || v0[1] < -1.5f || v0[1] > 1.5f || v0[2] < -1.5f || v0[2] > 1.5f) return; // normalized only

	// --- bound resolution ladder ---
	// The section's vertices are normalized against the MODEL-WIDE compression bound from the tag's
	// compression_info (NOT the section's own extent - multi-section models like the crashed pelican prove
	// this). That block is stripped from tool memory, so the primary source is the offline tag-file DB;
	// unresolved sections of the same model reuse a sibling's bound (bounds are model-wide); the parts-union
	// remains a last-resort approximation (exact only for tight single-section models).
	load_bounds_db("H2V");
	bool pairOK = false;
	uint16_t words[14] = {};
	if (P >= 0x400000 && P < 0x40000000 &&
		*reinterpret_cast<uint32_t*>(P + kH2V_P_GeomPtr) == section &&
		*reinterpret_cast<int*>(P + kH2V_P_GeomCount) >= 1)
	{
		pairOK = true;
		memcpy(words, reinterpret_cast<void*>(P), sizeof(words));
		if (!(words[13] & 1)) return;                                  // position NOT compressed -> verts already local
	}

	bool haveBound = false;
	float X0 = 0, X1 = 0, Y0 = 0, Y1 = 0, Z0 = 0, Z1 = 0;
	const BoundsRec* amb = nullptr;
	if (pairOK)
	{
		uint32_t striptot = 0;                                        // total strip indices = sum of part[+0x08]
		for (int p = 0; p < pc; p++) striptot += *reinterpret_cast<uint16_t*>(parts + kH2V_PartStride * p + 0x08);
		const BoundsRec* r = db_lookup(words, striptot);
		if (r && !r->ambig)
		{
			X0 = r->b[0]; X1 = r->b[1]; Y0 = r->b[2]; Y1 = r->b[3]; Z0 = r->b[4]; Z1 = r->b[5];
			haveBound = true;
			g_lastResolvedP = P;                                       // sibling cache (model-wide bounds)
			for (int k = 0; k < 6; k++) g_lastResolvedB[k] = r->b[k];
		}
		else
		{
			amb = r;                                                   // ambiguous hit kept as 3rd choice
			// sibling: another section of the SAME model (same 104-byte element array) already resolved
			if (!haveBound && g_lastResolvedP)
			{
				uint32_t d = (P > g_lastResolvedP) ? (P - g_lastResolvedP) : (g_lastResolvedP - P);
				if (d % kH2V_P_Stride == 0 && d <= kH2V_P_Stride * 64)
				{
					X0 = g_lastResolvedB[0]; X1 = g_lastResolvedB[1]; Y0 = g_lastResolvedB[2];
					Y1 = g_lastResolvedB[3]; Z0 = g_lastResolvedB[4]; Z1 = g_lastResolvedB[5];
					haveBound = true;
				}
			}
			if (!haveBound && amb)
			{
				X0 = amb->b[0]; X1 = amb->b[1]; Y0 = amb->b[2]; Y1 = amb->b[3]; Z0 = amb->b[4]; Z1 = amb->b[5];
				haveBound = true;
			}
		}
	}

	// last resort: min/max union of every part's [+0x30] AABB (approximation; exact for tight 1-section models)
	if (!haveBound)
	{
		X0 = 1e30f; X1 = -1e30f; Y0 = 1e30f; Y1 = -1e30f; Z0 = 1e30f; Z1 = -1e30f;
		for (int p = 0; p < pc; p++)
		{
			float* b = reinterpret_cast<float*>(parts + kH2V_PartStride * p + kH2V_PartBBox);
			if (b[0] < X0) X0 = b[0];  if (b[1] > X1) X1 = b[1];
			if (b[2] < Y0) Y0 = b[2];  if (b[3] > Y1) Y1 = b[3];
			if (b[4] < Z0) Z0 = b[4];  if (b[5] > Z1) Z1 = b[5];
		}
	}
	float ex = X1 - X0, ey = Y1 - Y0, ez = Z1 - Z0;
	if (ex < 0.0f || ey < 0.0f || ez < 0.0f) return;                  // corrupt
	if (ex > 2000.0f || ey > 2000.0f || ez > 2000.0f) return;         // garbage-large
	if (ex + ey + ez < 0.01f) return;                                 // no usable bound -> leave verts untouched

	float hx = (X1 - X0) * 0.5f, hy = (Y1 - Y0) * 0.5f, hz = (Z1 - Z0) * 0.5f;
	float cx = (X1 + X0) * 0.5f, cy = (Y1 + Y0) * 0.5f, cz = (Z1 + Z0) * 0.5f;

	for (long i = 0; i < g_h2vDoneN && i < 4096; i++) if (g_h2vDone[i] == vbuf) return; // once per vbuf
	if (g_h2vDoneN < 4096) g_h2vDone[g_h2vDoneN++] = vbuf;
	for (int i = 0; i < vcount; i++)
	{
		float* v = reinterpret_cast<float*>(vbuf + kH2V_Stride * i);
		v[0] = hx * v[0] + cx; v[1] = hy * v[1] + cy; v[2] = hz * v[2] + cz;
	}
}

// H2V path. Returns true if applied. Safe no-op on any binary whose 0x4A7420 prologue doesn't match.
static bool apply_scenery_fix_h2v()
{
	if (reinterpret_cast<uint32_t>(GetModuleHandleW(nullptr)) != 0x400000) return false;
	if (memcmp(reinterpret_cast<void*>(kH2V_Occluder), kH2V_Stolen, sizeof(kH2V_Stolen)) != 0)
	{
		DebugPrintf("lightmap fix (H2V): prologue mismatch @0x%X, skipping (not stock H2Tool.exe)", kH2V_Occluder);
		return false;
	}

	// pushad(0x20)+pushfd(0x4)=0x24 -> orig [esp]=ra at +0x24, section (orig+4) at +0x28, a4 (orig+0xC) at +0x30.
	// __cdecl handler(ra, section, a4): push right-to-left (a4, section, ra), recomputing displacements per push.
	// push [esp+0x30]=a4; push [esp+0x2C]=section; push [esp+0x2C]=ra; call; add esp,0Ch; popfd; popad;
	// <replay 7 stolen bytes>; push kH2V_OccBack; ret
	uint8_t buf[80]; size_t n = 0;
	buf[n++] = 0x60;                                                       // pushad
	buf[n++] = 0x9C;                                                       // pushfd
	buf[n++] = 0xFF; buf[n++] = 0x74; buf[n++] = 0x24; buf[n++] = 0x30;    // push dword [esp+0x30]  (a4)
	buf[n++] = 0xFF; buf[n++] = 0x74; buf[n++] = 0x24; buf[n++] = 0x2C;    // push dword [esp+0x2C]  (section)
	buf[n++] = 0xFF; buf[n++] = 0x74; buf[n++] = 0x24; buf[n++] = 0x2C;    // push dword [esp+0x2C]  (ra)
	buf[n++] = 0xE8; size_t call_rel = n; n += 4;                         // call handler (rel32 patched below)
	buf[n++] = 0x83; buf[n++] = 0xC4; buf[n++] = 0x0C;                    // add esp,0Ch
	buf[n++] = 0x9D;                                                       // popfd
	buf[n++] = 0x61;                                                       // popad
	for (size_t i = 0; i < sizeof(kH2V_Stolen); i++) buf[n++] = kH2V_Stolen[i]; // replay stolen prologue
	buf[n++] = 0x68; *reinterpret_cast<uint32_t*>(buf + n) = kH2V_OccBack; n += 4; // push back_addr
	buf[n++] = 0xC3;                                                       // ret

	uint8_t* mem = static_cast<uint8_t*>(VirtualAlloc(nullptr, n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if (!mem) { DebugPrintf("lightmap fix (H2V): stub alloc failed"); return false; }
	memcpy(mem, buf, n);
	int32_t rel = static_cast<int32_t>(reinterpret_cast<size_t>(&osoyoos_h2v_occluder) - (reinterpret_cast<size_t>(mem) + call_rel + 4));
	memcpy(mem + call_rel, &rel, 4);

	WriteJmp(static_cast<size_t>(kH2V_Occluder), reinterpret_cast<size_t>(mem));   // 5-byte JMP
	NopFill(static_cast<size_t>(kH2V_Occluder) + 5, sizeof(kH2V_Stolen) - 5);      // NOP the remaining stolen bytes

	// Parent-stash detours (canonical compression_info source). Optional: on a byte mismatch the main fix
	// still works via the parts-union fallback, so these fail soft.
	int stashes = 0;
	const struct { uint32_t at, back; } kStash[2] = { { kH2V_StashA, kH2V_StashABack }, { kH2V_StashB, kH2V_StashBBack } };
	for (int s = 0; s < 2; s++)
	{
		if (memcmp(reinterpret_cast<void*>(kStash[s].at), kH2V_StashStolen, sizeof(kH2V_StashStolen)) != 0)
		{
			DebugPrintf("H2V scenery fix: stash site @0x%X byte mismatch, skipping (parts-union fallback stays active)", kStash[s].at);
			continue;
		}
		// mov [g_h2vPendingP], eax ; add eax,34h ; add esp,0Ch ; push back ; ret
		uint8_t sbuf[24]; size_t sn = 0;
		sbuf[sn++] = 0xA3; *reinterpret_cast<uint32_t*>(sbuf + sn) = reinterpret_cast<uint32_t>(&g_h2vPendingP); sn += 4;
		for (size_t i = 0; i < sizeof(kH2V_StashStolen); i++) sbuf[sn++] = kH2V_StashStolen[i];
		sbuf[sn++] = 0x68; *reinterpret_cast<uint32_t*>(sbuf + sn) = kStash[s].back; sn += 4;
		sbuf[sn++] = 0xC3;
		uint8_t* smem = static_cast<uint8_t*>(VirtualAlloc(nullptr, sn, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!smem) { DebugPrintf("H2V scenery fix: stash stub alloc failed @0x%X", kStash[s].at); continue; }
		memcpy(smem, sbuf, sn);
		WriteJmp(static_cast<size_t>(kStash[s].at), reinterpret_cast<size_t>(smem));  // 5-byte JMP
		NopFill(static_cast<size_t>(kStash[s].at) + 5, sizeof(kH2V_StashStolen) - 5); // NOP the 6th stolen byte
		stashes++;
	}

	DebugPrintf("H2V scenery fix applied (occluder hook @0x%X, %d/2 parent-stash hooks)", kH2V_Occluder, stashes);
	return true;
}

// Public entry: dispatch to whichever tool we landed in. Both self-check their prologues and no-op on a
// mismatch, so this is safe on any binary.
bool apply_lightmap_scenery_fix()
{
	DebugPrintf("Applying lightmap scenery/dynamic-object bake fix");
	if (reinterpret_cast<uint32_t>(GetModuleHandleW(nullptr)) != kBase)
	{
		DebugPrintf("lightmap fix: unexpected image base, skipping (need no-ASLR 0x400000)");
		return false;
	}
	if (apply_scenery_fix_mcc()) return true;   // MCC tool_fast.exe
	if (apply_scenery_fix_h2v()) return true;   // H2V / 2007 H2Tool.exe (+ H2Tool_codez.exe)
	DebugPrintf("lightmap scenery fix: no known tool target matched (neither MCC nor H2V prologue)");
	return false;
}
