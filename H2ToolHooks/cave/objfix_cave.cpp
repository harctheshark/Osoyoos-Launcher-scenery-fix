/*
 Self-contained relocatable CODE CAVE for the H2 lightmapper scenery fix (object-decompress + zero-bbox + scale).
 Built as an x86 DLL linked at base 0x17B0000, then mapped into stock tool_fast.exe by the launcher via
 VirtualAllocEx + WriteProcessMemory (see H2ToolLightmapFixInjector.cs). The tool's hook sites are overwritten with
 `jmp allocBase + <stub RVA>` and jump INTO the stubs below; the stubs call the C handlers, replay the stolen
 prologue, and jump back into the tool.

 Three hooks on stock tool_fast.exe (no-ASLR, ImageBase 0x400000, MD5 3A889D370A7BE537AF47FF8035ACD201):
   sub_498962 (scenery import, per object)  -> stub_scale  : capture this placement's scale (@placement+32)
   sub_49CF55 (occluder insert)             -> stub_gate   : scenery-only gate (track caller)
   sub_49DA6A (add vertex)                  -> stub_decomp : decompress vbuf in place + set occluder matrix m0=scale

 Export the three stubs so the build/extract step can read their RVAs and feed them to the C# CavePayload.Jumps.
 The cave is MAPPED (never LoadLibrary'd), so no CRT init runs: /GS- avoids an uninitialized stack cookie, and the
 handlers avoid the CRT (own SEH-free bounds checks; no __try — every deref is guarded by explicit range tests).
*/

typedef unsigned int   u32;
typedef unsigned short u16;

// FP marker the compiler references when floating-point is used; normally supplied by the CRT (excluded here).
extern "C" int _fltused = 0x9875;

// ---- tool addresses (ImageBase 0x400000) ----
#define kSub_498962    0x498962u
#define kSub_498962End 0x49946Bu
#define kSub_49CF55    0x49CF55u
#define kSub_49CF55End 0x49D326u
#define kScenario      0x00C78CF4u   // dword_C78CF4 = scenario objects block

// ---- cave state (relocated data; the injector rebases pointers, these are plain values) ----
static volatile long g_sceneryActive = 0;
static volatile long g_doneVbufN = 0;
static u32  g_doneVbuf[1024];
static float g_curScale = 1.0f;
// range check: is p a plausible in-bounds pointer we can read?
static inline bool ok(u32 p) { return p >= 0x10000u && p < 0xFFFF0000u; }

// sub_498962(a1=ecx=placement index): read the placement scale @+32 (0 -> 1.0). No CRT / no __try; fully guarded.
extern "C" void __cdecl h_scale(u32 a1)
{
    g_curScale = 1.0f;
    u32 scn = *(u32*)kScenario;
    if (!ok(scn)) return;
    int pcount = *(int*)(scn + 128);
    if ((int)a1 < 0 || (int)a1 >= pcount) return;
    u32 pbase = *(u32*)(scn + 132);
    if (pbase == 0xFFFFFFFFu || !ok(pbase)) return;
    float s = *(float*)(pbase + 96u * a1 + 32u);
    g_curScale = (s > 0.0001f && s < 1000.0f) ? s : 1.0f;
}

// sub_49CF55 caller gate: scenery only (called from sub_498962); recursion within sub_49CF55 inherits.
extern "C" void __cdecl h_gate(u32 ra)
{
    if (ra >= kSub_498962 && ra < kSub_498962End) g_sceneryActive = 1;
    else if (ra >= kSub_49CF55 && ra < kSub_49CF55End) { /* inherit */ }
    else g_sceneryActive = 0;
}

// sub_49DA6A(a1=matrix, a2ptr=&mesh): (1) set the occluder matrix scale m0 (a1[0]) to THIS placement's scale, and
// (2) decompress the shared vbuf once to plain model-local (world_local = h*vNorm + c), incl. zero/degenerate bbox.
// The scale lives ONLY in the occluder matrix, NOT the verts. The object's own lighting reads the occluder-
// transformed positions (R*(m0*local)+T), so scaling m0 scales BOTH the cast shadow AND the lit surface, per
// placement -> the object lights correctly, is not self-occluded to black, and each instance of a model can carry
// its own scale (TRUE PER-INSTANCE). Proven in tool_logging/tlog2 (scale-5 railing comes out lit + 5x-shadowed).
// NOTE: baking scale into the verts instead (h,c *= scale) works too but is PER-MODEL (shared vbuf) and was the old
// behavior; the earlier per-instance attempt was black because it scaled m0 WITHOUT the plain decompress below.
extern "C" void __cdecl h_decomp(u32 a1, u32 a2ptr)
{
    if (!g_sceneryActive) return;
    // per-instance: scale the occluder matrix m0 for this placement (runs for EVERY placement, before the once-per-
    // vbuf guard below, so instance #2+ of a model still gets its own occluder scale).
    if (g_curScale != 1.0f && ok(a1)) { float* M = (float*)a1; if (M[0] > 0.9f && M[0] < 1.1f) M[0] = g_curScale; }
    if (!ok(a2ptr)) return;
    u32 v7 = *(u32*)a2ptr; if (!ok(v7)) return;
    u32 sec = *(u32*)(v7 + 4); if (sec < 0x400000u || sec >= 0x40000000u) return;
    u32 vb = *(u32*)(v7 + 40); if (vb < 0x400000u || vb >= 0x40000000u) return;
    int vcount = *(int*)(v7 + 36); if (vcount <= 0 || vcount > 200000) return;
    float* v0 = (float*)vb;
    if (v0[0] < -1.5f || v0[0] > 1.5f || v0[1] < -1.5f || v0[1] > 1.5f || v0[2] < -1.5f || v0[2] > 1.5f) return; // normalized only
    float* bb = (float*)(sec + 48);
    float hx = (bb[1] - bb[0]) * 0.5f, hy = (bb[3] - bb[2]) * 0.5f, hz = (bb[5] - bb[4]) * 0.5f;
    float cx = (bb[1] + bb[0]) * 0.5f, cy = (bb[3] + bb[2]) * 0.5f, cz = (bb[5] + bb[4]) * 0.5f;
    // zero/degenerate bbox: decompress anyway (collapses to a point -> no 2x2x2 phantom). Only reject garbage.
    if (hx < 0.0f || hy < 0.0f || hz < 0.0f || hx > 50.0f || hy > 50.0f || hz > 50.0f) return;
    long n = g_doneVbufN;
    for (long i = 0; i < n && i < 1024; i++) if (g_doneVbuf[i] == vb) return; // once per vbuf
    if (n < 1024) { g_doneVbuf[n] = vb; g_doneVbufN = n + 1; }
    // PLAIN decompress to model-local (NO scale baked in -- scale lives in the occluder matrix m0 above).
    for (int i = 0; i < vcount; i++) {
        float* v = (float*)(vb + 196 * i);
        v[0] = hx * v[0] + cx; v[1] = hy * v[1] + cy; v[2] = hz * v[2] + cz;
    }
}

// ================= detour stubs (baked into the image; C# jumps here) =================
// Each: save regs -> pass tool arg(s) to the handler -> restore -> replay the stolen 8-byte prologue -> push
// return-into-tool address -> ret (a balanced jmp). __declspec(naked) = raw asm, no prologue/epilogue.

extern "C" __declspec(dllexport) __declspec(naked) void stub_scale()   // <- tool sub_498962
{
    __asm {
        pushad
        pushfd
        mov eax, [esp+28]           // saved ECX = a1 (placement index)
        push eax
        call h_scale
        add esp, 4
        popfd
        popad
        push ebp                    // stolen: push ebp / lea ebp,[esp-74h] / sub esp,74h
        lea ebp, [esp-74h]
        sub esp, 74h
        push 49896Ah                // resume sub_498962 + 8
        ret
    }
}

extern "C" __declspec(dllexport) __declspec(naked) void stub_gate()    // <- tool sub_49CF55
{
    __asm {
        pushad
        pushfd
        mov eax, [esp+36]           // return address (orig [esp] + 32 pushad + 4 pushfd)
        push eax
        call h_gate
        add esp, 4
        popfd
        popad
        push ebp                    // stolen: push ebp / lea ebp,[esp-60h] / sub esp,60h
        lea ebp, [esp-60h]
        sub esp, 60h
        push 49CF5Dh                // resume sub_49CF55 + 8
        ret
    }
}

extern "C" __declspec(dllexport) __declspec(naked) void stub_decomp()  // <- tool sub_49DA6A
{
    __asm {
        pushad
        pushfd
        mov eax, [esp+40]           // a2 = orig [esp+4] + 36
        mov edx, [esp+28]           // saved ECX = this = a1 (matrix)
        push eax                    // a2ptr
        push edx                    // a1
        call h_decomp
        add esp, 8
        popfd
        popad
        sub esp, 3Ch                // stolen: sub esp,3Ch / mov eax,[00BC7BFC]
        mov eax, ds:[0BC7BFCh]
        push 49DA72h                // resume sub_49DA6A + 8
        ret
    }
}

// Minimal entry (never actually called — cave is mapped, not loaded). Keeps the linker happy for /LD.
extern "C" int __stdcall _DllMainCRTStartup(void*, unsigned, void*) { return 1; }
