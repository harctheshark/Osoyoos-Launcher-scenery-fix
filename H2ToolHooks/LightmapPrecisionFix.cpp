/*
 H2 lightmapper "precision fix" for Halo 2 MCC tool_fast.exe -- COMPLETE R7 port.

 This is the full six-hook FP64 precision path (adapted from Fixes/tool_fast_gpt/tool_fast_gpt.cpp, the
 proven R7 standalone DLL) folded into H2ToolHooks so it ships as a feature flag (LIGHTMAP_PRECISION_FIX)
 alongside the scenery + quality patches. Hooks (stock MCC tool_fast, no-ASLR, ImageBase 0x400000):
   0x49C09C  build stored Plucker edge coefficients        ROOT FIX (producer)
   0x4B6D88  interpolate sample position and normal        supporting producer
   0x4A8CEB / 0x4AE1DF  construct biased ray origins       supporting producers
   0x4B2DD4  generate ray Plucker data and traverse grid   supporting traversal (widened FP64 packet)
   0x4B4507  evaluate triangle edges and plane hit         supporting consumer (reads widened FP64 packet)
 The traversal->leaf pair share a private double-precision Plucker packet (WidenedLeafCall magic "GPF5").
 Entry point apply_lightmap_precision_fix() replaces R7's DllMain; every hook is signature-verified so it is
 a safe no-op on any binary that is not stock MCC tool_fast. Logging is disabled (initialize_logger is not
 called, so g_log_path stays empty and append_log early-returns).

 R7 uses MSVC inline __asm / naked trampolines, so this file requires the MSVC toolset (v143) and is built
 /fp:strict without LTCG (see the per-file overrides in H2ToolHooks.vcxproj) to reproduce R7's exact codegen.
*/
#if defined(__clang__)
#error "LightmapPrecisionFix.cpp uses MSVC inline __asm (R7 port); build with the MSVC toolset (v143), not clang-cl."
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>

namespace
{
constexpr std::uintptr_t kImageBase = 0x00400000u;
constexpr std::uintptr_t kTraceAddress = 0x004B2DD4u;
constexpr std::uintptr_t kStaticSurfaceAddress = 0x004B4507u;
constexpr std::uintptr_t kInterpolateSurfaceAddress = 0x004B6D88u;
constexpr std::uintptr_t kFinalizeGatherBiasAddress = 0x004A8CEBu;
constexpr std::uintptr_t kComputeLightBiasAddress = 0x004AE1DFu;
constexpr std::uintptr_t kSurfaceEdgeBuilderAddress = 0x0049C09Cu;
constexpr std::uintptr_t kTransformPointAddress = 0x005AFB7Fu;
constexpr std::uintptr_t kFinalizeAddress = 0x004B430Au;
constexpr std::uintptr_t kGatherAddress = 0x004B1A39u;
constexpr std::uintptr_t kGatherTraceAddress = 0x004B27C5u;
constexpr std::uintptr_t kHitRadianceAddress = 0x004B29A2u;
constexpr std::uintptr_t kHitSkyIndexAddress = 0x004B2A87u;
constexpr std::uintptr_t kSkyRadianceAddress = 0x004B13FAu;
constexpr std::uintptr_t kMainSamplesAddress = 0x00C46928u;
constexpr std::uintptr_t kSecondarySamplesAddress = 0x00C46924u;
constexpr std::uintptr_t kRandomStatePointerAddress = 0x01622084u;
constexpr std::uintptr_t kGatherRayCounterAddress = 0x00C69018u;
constexpr std::uintptr_t kPhotonKnearestAddress = 0x004B883Au;
constexpr std::uintptr_t kPhotonRadiusDivideAddress = 0x004B8875u;
constexpr std::uintptr_t kPhotonRadiusSquaresAddress = 0x004B88C0u;
constexpr std::uintptr_t kPhotonRadiusGrowFourAddress = 0x004B893Cu;
constexpr std::uintptr_t kPhotonRadiusGrowTwoAddress = 0x004B894Fu;
constexpr std::uintptr_t kPhotonSplitDeltaAddress = 0x004B79B3u;
constexpr std::uintptr_t kPhotonPositionDeltaAddress = 0x004B7A46u;
constexpr std::uintptr_t kPhotonDistanceSquaredAddress = 0x004B7A6Fu;
constexpr std::uintptr_t kPhotonInnerEllipsoidAddress = 0x004B7A96u;
constexpr std::uintptr_t kPhotonOuterEllipsoidAddress = 0x004B7AE9u;
constexpr std::uintptr_t kPhotonNormalDotAddress = 0x004B7B5Cu;
constexpr std::uintptr_t kPhotonQueryPositionSlotAddress = 0x00C69038u;
constexpr std::uintptr_t kPhotonQueryNormalSlotAddress = 0x00C6903Cu;
constexpr std::uintptr_t kPhotonInnerRadiusAddress = 0x00C69040u;
constexpr std::uintptr_t kPhotonInnerRadiusSquaredAddress = 0x00C69044u;
constexpr std::uintptr_t kTraceRva = kTraceAddress - kImageBase;
constexpr std::uintptr_t kStaticSurfaceRva = kStaticSurfaceAddress - kImageBase;
constexpr std::uintptr_t kInterpolateSurfaceRva = kInterpolateSurfaceAddress - kImageBase;
constexpr std::uintptr_t kFinalizeGatherBiasRva = kFinalizeGatherBiasAddress - kImageBase;
constexpr std::uintptr_t kComputeLightBiasRva = kComputeLightBiasAddress - kImageBase;
constexpr std::uintptr_t kSurfaceEdgeBuilderRva = kSurfaceEdgeBuilderAddress - kImageBase;
constexpr std::uintptr_t kTransformPointRva = kTransformPointAddress - kImageBase;
constexpr std::uintptr_t kFinalizeRva = kFinalizeAddress - kImageBase;
constexpr std::uintptr_t kGatherRva = kGatherAddress - kImageBase;
constexpr std::uintptr_t kGatherTraceRva = kGatherTraceAddress - kImageBase;
constexpr std::uintptr_t kHitRadianceRva = kHitRadianceAddress - kImageBase;
constexpr std::uintptr_t kHitSkyIndexRva = kHitSkyIndexAddress - kImageBase;
constexpr std::uintptr_t kSkyRadianceRva = kSkyRadianceAddress - kImageBase;
constexpr std::uintptr_t kMainSamplesRva = kMainSamplesAddress - kImageBase;
constexpr std::uintptr_t kSecondarySamplesRva = kSecondarySamplesAddress - kImageBase;
constexpr std::uintptr_t kRandomStatePointerRva = kRandomStatePointerAddress - kImageBase;
constexpr std::uintptr_t kGatherRayCounterRva = kGatherRayCounterAddress - kImageBase;
constexpr std::uintptr_t kPhotonKnearestRva = kPhotonKnearestAddress - kImageBase;
constexpr std::uintptr_t kPhotonRadiusDivideRva = kPhotonRadiusDivideAddress - kImageBase;
constexpr std::uintptr_t kPhotonRadiusSquaresRva = kPhotonRadiusSquaresAddress - kImageBase;
constexpr std::uintptr_t kPhotonRadiusGrowFourRva = kPhotonRadiusGrowFourAddress - kImageBase;
constexpr std::uintptr_t kPhotonRadiusGrowTwoRva = kPhotonRadiusGrowTwoAddress - kImageBase;
constexpr std::uintptr_t kPhotonSplitDeltaRva = kPhotonSplitDeltaAddress - kImageBase;
constexpr std::uintptr_t kPhotonPositionDeltaRva = kPhotonPositionDeltaAddress - kImageBase;
constexpr std::uintptr_t kPhotonDistanceSquaredRva = kPhotonDistanceSquaredAddress - kImageBase;
constexpr std::uintptr_t kPhotonInnerEllipsoidRva = kPhotonInnerEllipsoidAddress - kImageBase;
constexpr std::uintptr_t kPhotonOuterEllipsoidRva = kPhotonOuterEllipsoidAddress - kImageBase;
constexpr std::uintptr_t kPhotonNormalDotRva = kPhotonNormalDotAddress - kImageBase;
constexpr std::uintptr_t kPhotonQueryPositionSlotRva = kPhotonQueryPositionSlotAddress - kImageBase;
constexpr std::uintptr_t kPhotonQueryNormalSlotRva = kPhotonQueryNormalSlotAddress - kImageBase;
constexpr std::uintptr_t kPhotonInnerRadiusRva = kPhotonInnerRadiusAddress - kImageBase;
constexpr std::uintptr_t kPhotonInnerRadiusSquaredRva =
    kPhotonInnerRadiusSquaredAddress - kImageBase;

constexpr std::size_t kHitSize = 0x44;
constexpr double kDirectionEpsilon = 1.0e-6;
constexpr double kHitEpsilon = 1.0e-5;
constexpr double kNormalizeEpsilon = 0.00009999999747378752;
constexpr std::uint32_t kWidenedLeafMagic = 0x35465047u; // "GPF5"
constexpr std::uint32_t kWidenedLeafMagicInverse = ~kWidenedLeafMagic;
constexpr char kBuildId[] = "GPT-FP64-SURFACE-EDGES-20260717-R7-4C87B0";

std::uintptr_t g_exe_base = 0;
char g_exe_path[1024]{};
char g_dll_path[1024]{};
char g_log_path[1024]{};
char g_run_id[160]{};
volatile LONG g_trace_calls = 0;
volatile LONG g_trace_hits = 0;
volatile LONG g_trace_misses = 0;
volatile LONG g_cells_visited = 0;
volatile LONG g_static_tests = 0;
volatile LONG g_static_hits = 0;
volatile LONG g_dynamic_tests = 0;
volatile LONG g_dynamic_hits = 0;
volatile LONG g_aabb_rejects = 0;
volatile LONG g_leaf_entry_calls = 0;
volatile LONG g_leaf_fp64_tests = 0;
volatile LONG g_leaf_fp64_hits = 0;
volatile LONG g_photon_knn_calls = 0;
__declspec(align(8)) volatile LONG64 g_gather_calls = 0;
__declspec(align(8)) volatile LONG64 g_gather_samples = 0;
__declspec(align(8)) volatile LONG64 g_gather_trace_hits = 0;
__declspec(align(8)) volatile LONG64 g_gather_trace_misses = 0;
__declspec(align(8)) volatile LONG64 g_gather_sky_hits = 0;
__declspec(align(8)) volatile LONG64 g_gather_surface_hits = 0;
__declspec(align(8)) volatile LONG64 g_interpolate_surface_calls = 0;
__declspec(align(8)) volatile LONG64 g_surface_edge_builder_calls = 0;
__declspec(align(8)) volatile LONG64 g_surface_edges_built = 0;
__declspec(align(8)) volatile LONG64 g_surface_edge_coefficients_changed = 0;
bool g_hook_installed = false;
bool g_trace_hook_installed = false;
bool g_leaf_hook_installed = false;
bool g_interpolate_hook_installed = false;
bool g_finalize_bias_hook_installed = false;
bool g_compute_bias_hook_installed = false;
bool g_surface_edge_builder_hook_installed = false;

#pragma pack(push, 1)
struct Ray
{
    float origin[3];
    float direction[3];
    float t_min;
    float t_max;
    std::uint8_t suppress_sky;
    std::uint8_t padding[3];
    std::int32_t filter;
};

struct InitialCell
{
    std::uint32_t owner;
    std::int32_t packed_cell;
};

struct HitRecord
{
    std::uint8_t bytes[kHitSize];
};

struct CellEntry
{
    std::uint32_t packed_cell;
    std::uint32_t range_index;
};
#pragma pack(pop)

static_assert(sizeof(Ray) == 0x28, "Ray layout mismatch");
static_assert(sizeof(InitialCell) == 8, "Initial-cell layout mismatch");
static_assert(sizeof(HitRecord) == kHitSize, "Hit-record layout mismatch");
static_assert(sizeof(CellEntry) == 8, "Cell-entry layout mismatch");

LONG counter_value(volatile LONG* value)
{
    return InterlockedCompareExchange(value, 0, 0);
}

LONG64 counter_value64(volatile LONG64* value)
{
    return InterlockedCompareExchange64(value, 0, 0);
}

void initialize_logger(HINSTANCE instance)
{
    if (GetModuleFileNameA(nullptr, g_exe_path, static_cast<DWORD>(sizeof(g_exe_path))) == 0)
        strcpy_s(g_exe_path, "<GetModuleFileName(exe) failed>");
    if (GetModuleFileNameA(instance, g_dll_path, static_cast<DWORD>(sizeof(g_dll_path))) == 0)
        strcpy_s(g_dll_path, "tool_fast_GPT.dll");

    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    const DWORD process_id = GetCurrentProcessId();

    _snprintf_s(
        g_run_id,
        sizeof(g_run_id),
        _TRUNCATE,
        "%04u%02u%02uT%02u%02u%02u.%03uZ-PID%lu-QPC%I64X",
        utc.wYear,
        utc.wMonth,
        utc.wDay,
        utc.wHour,
        utc.wMinute,
        utc.wSecond,
        utc.wMilliseconds,
        process_id,
        static_cast<unsigned long long>(qpc.QuadPart));

    char filename[512]{};
    _snprintf_s(
        filename,
        sizeof(filename),
        _TRUNCATE,
        "GPT_ONLY__FP64_SURFACE_EDGE_BUILDER__20260717_R7__PID_%lu__%s.gptlog",
        process_id,
        g_run_id);

    const char* slash = std::strrchr(g_dll_path, '\\');
    const char* forward_slash = std::strrchr(g_dll_path, '/');
    if (!slash || (forward_slash && forward_slash > slash))
        slash = forward_slash;

    if (slash)
    {
        const int directory_length = static_cast<int>(slash - g_dll_path + 1);
        _snprintf_s(
            g_log_path,
            sizeof(g_log_path),
            _TRUNCATE,
            "%.*s%s",
            directory_length,
            g_dll_path,
            filename);
    }
    else
    {
        strcpy_s(g_log_path, filename);
    }
}

void append_log(const char* event_name, const char* format, ...)
{
    if (g_log_path[0] == '\0')
        return;

    char body[1536]{};
    va_list arguments;
    va_start(arguments, format);
    _vsnprintf_s(body, sizeof(body), _TRUNCATE, format, arguments);
    va_end(arguments);

    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char line[2048]{};
    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%04u-%02u-%02uT%02u:%02u:%02u.%03uZ] GPT_ONLY build=%s run=%s event=%s %s\r\n",
        utc.wYear,
        utc.wMonth,
        utc.wDay,
        utc.wHour,
        utc.wMinute,
        utc.wSecond,
        utc.wMilliseconds,
        kBuildId,
        g_run_id,
        event_name,
        body);

    HANDLE file = CreateFileA(
        g_log_path,
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(file, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

void log_counters(const char* event_name)
{
    append_log(
        event_name,
        "trace_4B2DD4_calls=%ld trace_hits=%ld trace_misses=%ld cells_visited=%ld "
        "leaf_4B4507_entry_calls=%ld leaf_fp64_tests=%ld leaf_fp64_hits=%ld "
        "static_tests=%ld dynamic_tests=%ld aabb_rejects=%ld "
        "interpolate_surface_calls=%I64d trace_hook_installed=%s leaf_hook_installed=%s "
        "interpolate_hook_installed=%s finalize_bias_hook_installed=%s "
        "compute_bias_hook_installed=%s surface_edge_builder_hook_installed=%s "
        "surface_edge_builder_calls=%I64d surface_edges_built=%I64d "
        "surface_edge_coefficients_changed=%I64d all_hooks_installed=%s",
        counter_value(&g_trace_calls),
        counter_value(&g_trace_hits),
        counter_value(&g_trace_misses),
        counter_value(&g_cells_visited),
        counter_value(&g_leaf_entry_calls),
        counter_value(&g_leaf_fp64_tests),
        counter_value(&g_leaf_fp64_hits),
        counter_value(&g_static_tests),
        counter_value(&g_dynamic_tests),
        counter_value(&g_aabb_rejects),
        counter_value64(&g_interpolate_surface_calls),
        g_trace_hook_installed ? "true" : "false",
        g_leaf_hook_installed ? "true" : "false",
        g_interpolate_hook_installed ? "true" : "false",
        g_finalize_bias_hook_installed ? "true" : "false",
        g_compute_bias_hook_installed ? "true" : "false",
        g_surface_edge_builder_hook_installed ? "true" : "false",
        counter_value64(&g_surface_edge_builder_calls),
        counter_value64(&g_surface_edges_built),
        counter_value64(&g_surface_edge_coefficients_changed),
        g_hook_installed ? "true" : "false");
}

std::uintptr_t g_photon_knn_continue = 0;
std::uintptr_t g_photon_radius_divide_continue = 0;
std::uintptr_t g_photon_radius_squares_continue = 0;
std::uintptr_t g_photon_radius_grow_four_continue = 0;
std::uintptr_t g_photon_radius_grow_two_continue = 0;
std::uintptr_t g_photon_split_delta_continue = 0;
std::uintptr_t g_photon_position_delta_continue = 0;
std::uintptr_t g_photon_distance_squared_continue = 0;
std::uintptr_t g_photon_inner_ellipsoid_continue = 0;
std::uintptr_t g_photon_outer_ellipsoid_continue = 0;
std::uintptr_t g_photon_normal_dot_continue = 0;
std::uintptr_t g_photon_query_position_slot = 0;
std::uintptr_t g_photon_query_normal_slot = 0;
std::uintptr_t g_photon_inner_radius_pointer = 0;
std::uintptr_t g_photon_inner_radius_squared_pointer = 0;

extern "C" __declspec(naked) void photon_knn_entry_hook()
{
    __asm
    {
        push ebp
        mov ebp, esp
        and esp, 0FFFFFFF8h
        pushfd
        lock inc dword ptr [g_photon_knn_calls]
        popfd
        jmp dword ptr [g_photon_knn_continue]
    }
}

extern "C" __declspec(naked) void photon_radius_divide_double_hook()
{
    __asm
    {
        cvtss2sd xmm2, xmm2
        cvtss2sd xmm0, xmm0
        divsd xmm2, xmm0
        cvtsd2ss xmm2, xmm2
        movss dword ptr [esp + 10h], xmm2
        jmp dword ptr [g_photon_radius_divide_continue]
    }
}

extern "C" __declspec(naked) void photon_radius_squares_double_hook()
{
    __asm
    {
        movaps xmm0, xmm2
        and dword ptr [esp + 14h], 0
        push edx
        mov edx, dword ptr [g_photon_inner_radius_pointer]
        movss dword ptr [edx], xmm3
        pop edx

        cvtss2sd xmm3, xmm3
        mulsd xmm3, xmm3
        cvtsd2ss xmm3, xmm3
        lea eax, dword ptr [eax + edi * 8]

        cvtss2sd xmm0, xmm0
        mulsd xmm0, xmm0
        cvtsd2ss xmm0, xmm0
        jmp dword ptr [g_photon_radius_squares_continue]
    }
}

extern "C" __declspec(naked) void photon_radius_grow_four_double_hook()
{
    __asm
    {
        movss xmm2, dword ptr [esp + 10h]
        cvtss2sd xmm2, xmm2
        addsd xmm2, xmm2
        addsd xmm2, xmm2
        cvtsd2ss xmm2, xmm2
        push 0FFFFFFFEh
        pop eax
        jmp dword ptr [g_photon_radius_grow_four_continue]
    }
}

extern "C" __declspec(naked) void photon_radius_grow_two_double_hook()
{
    __asm
    {
        movss xmm2, dword ptr [esp + 10h]
        cvtss2sd xmm2, xmm2
        addsd xmm2, xmm2
        cvtsd2ss xmm2, xmm2
        or eax, 0FFFFFFFFh
        jmp dword ptr [g_photon_radius_grow_two_continue]
    }
}

extern "C" __declspec(naked) void photon_split_delta_double_hook()
{
    __asm
    {
        cvtss2sd xmm0, xmm0
        cvtss2sd xmm1, dword ptr [esi + ecx * 4]
        subsd xmm0, xmm1
        cvtsd2ss xmm0, xmm0
        jmp dword ptr [g_photon_split_delta_continue]
    }
}

extern "C" __declspec(naked) void photon_position_delta_double_hook()
{
    __asm
    {
        mov eax, dword ptr [g_photon_query_position_slot]
        mov eax, dword ptr [eax]

        movss xmm3, dword ptr [esi]
        cvtss2sd xmm3, xmm3
        cvtss2sd xmm0, dword ptr [eax]
        subsd xmm3, xmm0
        cvtsd2ss xmm3, xmm3

        movss xmm4, dword ptr [esi + 4]
        cvtss2sd xmm4, xmm4
        cvtss2sd xmm0, dword ptr [eax + 4]
        subsd xmm4, xmm0
        cvtsd2ss xmm4, xmm4

        movss xmm5, dword ptr [esi + 8]
        cvtss2sd xmm5, xmm5
        cvtss2sd xmm0, dword ptr [eax + 8]
        subsd xmm5, xmm0
        cvtsd2ss xmm5, xmm5

        push edx
        mov edx, dword ptr [g_photon_inner_radius_squared_pointer]
        movss xmm6, dword ptr [edx]
        pop edx
        jmp dword ptr [g_photon_position_delta_continue]
    }
}

extern "C" __declspec(naked) void photon_distance_squared_double_hook()
{
    __asm
    {
        cvtss2sd xmm0, xmm3
        mulsd xmm0, xmm0
        cvtss2sd xmm1, xmm4
        mulsd xmm1, xmm1
        addsd xmm0, xmm1
        cvtss2sd xmm1, xmm5
        mulsd xmm1, xmm1
        addsd xmm0, xmm1
        cvtsd2ss xmm2, xmm0
        comiss xmm6, xmm2
        jmp dword ptr [g_photon_distance_squared_continue]
    }
}

extern "C" __declspec(naked) void photon_inner_ellipsoid_double_hook()
{
    __asm
    {
        mov eax, dword ptr [g_photon_query_normal_slot]
        mov eax, dword ptr [eax]
        sub esp, 10h
        movdqu xmmword ptr [esp], xmm7

        cvtss2sd xmm0, xmm3
        cvtss2sd xmm7, dword ptr [eax]
        mulsd xmm0, xmm7
        cvtss2sd xmm1, xmm4
        cvtss2sd xmm7, dword ptr [eax + 4]
        mulsd xmm1, xmm7
        addsd xmm0, xmm1
        cvtss2sd xmm1, xmm5
        cvtss2sd xmm7, dword ptr [eax + 8]
        mulsd xmm1, xmm7
        addsd xmm0, xmm1
        cvtsd2ss xmm1, xmm0

        cvtss2sd xmm0, xmm1
        movapd xmm7, xmm0
        addsd xmm7, xmm7
        addsd xmm7, xmm0
        mulsd xmm7, xmm0
        cvtss2sd xmm0, xmm2
        addsd xmm7, xmm0
        cvtsd2ss xmm0, xmm7

        movdqu xmm7, xmmword ptr [esp]
        add esp, 10h
        jmp dword ptr [g_photon_inner_ellipsoid_continue]
    }
}

extern "C" __declspec(naked) void photon_outer_ellipsoid_double_hook()
{
    __asm
    {
        mov edx, dword ptr [g_photon_query_normal_slot]
        mov edx, dword ptr [edx]
        sub esp, 10h
        movdqu xmmword ptr [esp], xmm7

        cvtss2sd xmm0, xmm3
        cvtss2sd xmm7, dword ptr [edx]
        mulsd xmm0, xmm7
        cvtss2sd xmm1, xmm4
        cvtss2sd xmm7, dword ptr [edx + 4]
        mulsd xmm1, xmm7
        addsd xmm0, xmm1
        cvtss2sd xmm1, xmm5
        cvtss2sd xmm7, dword ptr [edx + 8]
        mulsd xmm1, xmm7
        addsd xmm0, xmm1
        cvtsd2ss xmm1, xmm0

        cvtss2sd xmm0, xmm1
        movapd xmm7, xmm0
        addsd xmm7, xmm7
        addsd xmm7, xmm0
        mulsd xmm7, xmm0
        cvtss2sd xmm0, xmm2
        addsd xmm7, xmm0
        cvtsd2ss xmm0, xmm7

        movdqu xmm7, xmmword ptr [esp]
        add esp, 10h
        jmp dword ptr [g_photon_outer_ellipsoid_continue]
    }
}

extern "C" __declspec(naked) void photon_normal_dot_double_hook()
{
    __asm
    {
        sub esp, 10h
        movdqu xmmword ptr [esp], xmm7

        cvtss2sd xmm0, xmm0
        cvtss2sd xmm7, dword ptr [ebp - 9Ch]
        mulsd xmm0, xmm7
        cvtss2sd xmm2, xmm1
        cvtss2sd xmm7, dword ptr [ebp - 98h]
        mulsd xmm2, xmm7
        addsd xmm0, xmm2
        cvtss2sd xmm2, dword ptr [edx + 8]
        cvtss2sd xmm7, dword ptr [ebp - 94h]
        mulsd xmm2, xmm7
        addsd xmm0, xmm2
        cvtsd2ss xmm1, xmm0

        movdqu xmm7, xmmword ptr [esp]
        add esp, 10h
        jmp dword ptr [g_photon_normal_dot_continue]
    }
}

struct PluckerDouble
{
    double value[6];
};

struct WidenedLeafCall
{
    std::uint32_t magic;
    std::uint32_t magic_inverse;
    PluckerDouble plucker;
    double interval_min;
    double interval_max;
    double* hit_t_output;
};

template <typename T>
T read_value(const void* base, std::size_t offset)
{
    T value{};
    std::memcpy(&value, static_cast<const std::uint8_t*>(base) + offset, sizeof(value));
    return value;
}

template <typename T>
void write_value(void* base, std::size_t offset, const T& value)
{
    std::memcpy(static_cast<std::uint8_t*>(base) + offset, &value, sizeof(value));
}

std::uint8_t* read_pointer(const void* base, std::size_t offset)
{
    const std::uint32_t raw = read_value<std::uint32_t>(base, offset);
    return reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(raw));
}

float subtract_products_single(float a, float b, float c, float d)
{
    // Volatile temporaries force the two FP32 products to round before the
    // subtraction, matching MCC's original mulss/mulss/subss sequence.
    volatile float left = a * b;
    volatile float right = c * d;
    volatile float result = left - right;
    return result;
}

float subtract_single(float a, float b)
{
    volatile float result = a - b;
    return result;
}

extern "C" int __fastcall build_surface_edges_double(void* this_pointer, void*)
{
    InterlockedIncrement64(&g_surface_edge_builder_calls);

    auto* builder = static_cast<std::uint8_t*>(this_pointer);
    auto* edge_begin = read_pointer(builder, 0x4C);
    auto* edge_end = read_pointer(builder, 0x50);
    auto* vertices = read_pointer(builder, 0x40);
    auto* surface_topology = read_pointer(builder, 0x58);
    auto* surfaces = read_pointer(builder, 0x64);
    if (!edge_begin || !edge_end || edge_end < edge_begin || !vertices
        || !surface_topology || !surfaces)
    {
        append_log(
            "SURFACE_EDGE_BUILD_SKIPPED",
            "builder=0x%08lX edge_begin=0x%08lX edge_end=0x%08lX vertices=0x%08lX "
            "surface_topology=0x%08lX surfaces=0x%08lX",
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(builder)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(edge_begin)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(edge_end)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(vertices)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(surface_topology)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(surfaces)));
        return 0;
    }

    const std::ptrdiff_t edge_bytes = edge_end - edge_begin;
    const std::int32_t edge_count = static_cast<std::int32_t>(edge_bytes / 60);
    LONG64 changed_coefficients = 0;
    LONG64 copied_coefficients = 0;

    for (std::int32_t edge_index = 0; edge_index < edge_count; ++edge_index)
    {
        auto* edge = edge_begin + static_cast<std::size_t>(edge_index) * 60u;
        const std::uint32_t vertex_index0 = read_value<std::uint32_t>(edge, 0);
        const std::uint32_t vertex_index1 = read_value<std::uint32_t>(edge, 4);
        const auto* p0 = reinterpret_cast<const float*>(
            vertices + static_cast<std::size_t>(vertex_index0) * 32u);
        const auto* p1 = reinterpret_cast<const float*>(
            vertices + static_cast<std::size_t>(vertex_index1) * 32u);

        float coefficients[6] = {
            static_cast<float>(static_cast<double>(p0[0]) * static_cast<double>(p1[1])
                - static_cast<double>(p1[0]) * static_cast<double>(p0[1])),
            static_cast<float>(static_cast<double>(p1[2]) * static_cast<double>(p0[0])
                - static_cast<double>(p1[0]) * static_cast<double>(p0[2])),
            static_cast<float>(static_cast<double>(p0[0]) - static_cast<double>(p1[0])),
            static_cast<float>(static_cast<double>(p1[2]) * static_cast<double>(p0[1])
                - static_cast<double>(p0[2]) * static_cast<double>(p1[1])),
            static_cast<float>(static_cast<double>(p0[2]) - static_cast<double>(p1[2])),
            static_cast<float>(static_cast<double>(p1[1]) - static_cast<double>(p0[1])),
        };
        const float original_fp32[6] = {
            subtract_products_single(p0[0], p1[1], p1[0], p0[1]),
            subtract_products_single(p1[2], p0[0], p1[0], p0[2]),
            subtract_single(p0[0], p1[0]),
            subtract_products_single(p1[2], p0[1], p0[2], p1[1]),
            subtract_single(p0[2], p1[2]),
            subtract_single(p1[1], p0[1]),
        };

        for (int component = 0; component < 6; ++component)
        {
            if (std::memcmp(&coefficients[component], &original_fp32[component], sizeof(float)) != 0)
                ++changed_coefficients;
        }

        for (int adjacent_slot = 0; adjacent_slot < 2; ++adjacent_slot)
        {
            const std::int32_t adjacent = read_value<std::int32_t>(
                edge,
                40u + static_cast<std::size_t>(adjacent_slot) * 4u);
            if (adjacent == -1)
                continue;

            const std::uint32_t surface_index =
                static_cast<std::uint32_t>(adjacent) & 0x1FFFFFFFu;
            auto* surface = surfaces + static_cast<std::size_t>(surface_index) * 124u;
            auto* topology = surface_topology + static_cast<std::size_t>(surface_index) * 48u;

            for (int triangle_edge = 0; triangle_edge < 3; ++triangle_edge)
            {
                const std::int32_t edge_reference = read_value<std::int32_t>(
                    topology,
                    12u + static_cast<std::size_t>(triangle_edge) * 4u);
                if ((static_cast<std::uint32_t>(edge_reference) & 0x7FFFFFFFu)
                    != static_cast<std::uint32_t>(edge_index))
                {
                    continue;
                }

                std::memcpy(
                    surface + 28u + static_cast<std::size_t>(triangle_edge) * 24u,
                    coefficients,
                    sizeof(coefficients));
                copied_coefficients += 6;

                std::uint16_t flags = read_value<std::uint16_t>(surface, 102);
                const std::uint16_t orientation_bit = static_cast<std::uint16_t>(
                    1u << (triangle_edge + 7));
                if (edge_reference < 0)
                    flags = static_cast<std::uint16_t>(flags | orientation_bit);
                else
                    flags = static_cast<std::uint16_t>(flags & ~orientation_bit);
                write_value<std::uint16_t>(surface, 102, flags);
                break;
            }
        }
    }

    InterlockedExchangeAdd64(&g_surface_edges_built, edge_count);
    InterlockedExchangeAdd64(&g_surface_edge_coefficients_changed, changed_coefficients);
    append_log(
        "SURFACE_EDGE_BUILD_COMPLETE",
        "function=0x0049C09C h2tool_pair=0x004A01A0 helper=0x004C87B0 "
        "edges=%ld coefficients_copied=%I64d fp64_final_float_values_different_from_fp32=%I64d",
        static_cast<long>(edge_count),
        copied_coefficients,
        changed_coefficients);

    return edge_count > 0 ? edge_count * 60 : edge_count;
}

unsigned char finish_trace(bool hit, LONG call_number, const HitRecord* output)
{
    if (hit)
        InterlockedIncrement(&g_trace_hits);
    else
        InterlockedIncrement(&g_trace_misses);

    if (call_number == 1)
    {
        if (hit && output)
        {
            append_log(
                "FIRST_TRACE_EXIT",
                "result=hit t=%.9g hit_type=%ld owner=0x%08lX thread_id=%lu",
                static_cast<double>(read_value<float>(output, 0)),
                read_value<LONG>(output, 64),
                read_value<DWORD>(output, 48),
                GetCurrentThreadId());
        }
        else
        {
            append_log(
                "FIRST_TRACE_EXIT",
                "result=miss thread_id=%lu",
                GetCurrentThreadId());
        }
    }

    if (call_number == 1000 || (call_number > 0 && (call_number % 1000000) == 0))
        log_counters("CHECKPOINT");

    return hit ? 1 : 0;
}

float context_float(const std::uint8_t* context, int float_index)
{
    return read_value<float>(context, static_cast<std::size_t>(float_index) * sizeof(float));
}

int grid_count(const std::uint8_t* context, int axis)
{
    return read_value<std::int32_t>(context, 0xC40u + static_cast<std::size_t>(axis) * 4u);
}

double grid_minimum(const std::uint8_t* context, int axis)
{
    return static_cast<double>(read_value<float>(context, 0x184Cu + static_cast<std::size_t>(axis) * 8u));
}

double grid_maximum(const std::uint8_t* context, int axis)
{
    return static_cast<double>(read_value<float>(context, 0x1850u + static_cast<std::size_t>(axis) * 8u));
}

double cell_minimum(const std::uint8_t* context, int axis, int cell)
{
    if (cell == 0)
        return grid_minimum(context, axis);
    return static_cast<double>(context_float(context, 3 * cell + 784 + axis));
}

double cell_maximum(const std::uint8_t* context, int axis, int cell)
{
    return static_cast<double>(context_float(context, 3 * cell + 787 + axis));
}

std::uint32_t pack_cell(const int cell[3])
{
    return (static_cast<std::uint32_t>(cell[0]) << 16)
        | (static_cast<std::uint32_t>(cell[1]) << 8)
        | static_cast<std::uint32_t>(cell[2]);
}

void unpack_cell(std::uint32_t packed, int cell[3])
{
    cell[0] = static_cast<int>((packed >> 16) & 0xFFu);
    cell[1] = static_cast<int>((packed >> 8) & 0xFFu);
    cell[2] = static_cast<int>(packed & 0xFFu);
}

bool clip_aabb_double(
    const double origin_relative[3],
    const float direction[3],
    const double extent[3],
    double& t_min,
    double& t_max)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        const double o = origin_relative[axis];
        const double d = static_cast<double>(direction[axis]);
        const double e = extent[axis];

        if (d == 0.0)
        {
            if (o < 0.0 || o > e)
                return false;
            continue;
        }

        double near_t = -o / d;
        double far_t = (e - o) / d;
        if (near_t > far_t)
            std::swap(near_t, far_t);

        if (near_t > t_min)
            t_min = near_t;
        if (far_t < t_max)
            t_max = far_t;
        if (t_min > t_max)
            return false;
    }

    return std::isfinite(t_min) && std::isfinite(t_max);
}

std::int32_t position_to_grid_double(
    const std::uint8_t* context,
    const double position[3],
    bool force_inside)
{
    if (!force_inside)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            if (position[axis] < grid_minimum(context, axis)
                || position[axis] > grid_maximum(context, axis))
            {
                return -1;
            }
        }
    }

    int cell[3]{};
    for (int axis = 0; axis < 3; ++axis)
    {
        const int count = grid_count(context, axis);
        if (count <= 0 || count > 256)
            return -1;

        int low = 0;
        int high = count;
        while (low < high)
        {
            const int middle = (low + high) >> 1;
            if (position[axis] <= cell_maximum(context, axis, middle))
                high = middle;
            else
                low = middle + 1;
        }

        if (low < 0)
            low = 0;
        if (low >= count)
            low = count - 1;
        cell[axis] = low;
    }

    return static_cast<std::int32_t>(pack_cell(cell));
}

const CellEntry* find_cell_entry(const std::uint8_t* context, std::uint32_t packed_cell)
{
    const std::uint8_t hash = static_cast<std::uint8_t>(
        packed_cell ^ ((packed_cell ^ (packed_cell >> 6)) >> 7));
    const int table_index = 3 * (static_cast<int>(hash) + 5);

    const std::uint8_t* begin_raw = read_pointer(context, static_cast<std::size_t>(table_index) * 4u);
    const std::uint8_t* end_raw = read_pointer(context, static_cast<std::size_t>(table_index + 1) * 4u);
    if (!begin_raw || !end_raw || end_raw <= begin_raw)
        return nullptr;

    const std::ptrdiff_t byte_count = end_raw - begin_raw;
    if (byte_count < static_cast<std::ptrdiff_t>(sizeof(CellEntry))
        || (byte_count % static_cast<std::ptrdiff_t>(sizeof(CellEntry))) != 0)
    {
        return nullptr;
    }

    const auto* entries = reinterpret_cast<const CellEntry*>(begin_raw);
    int low = 0;
    int high = static_cast<int>(byte_count / sizeof(CellEntry)) - 1;
    while (low <= high)
    {
        const int middle = low + ((high - low) >> 1);
        if (packed_cell < entries[middle].packed_cell)
            high = middle - 1;
        else if (packed_cell > entries[middle].packed_cell)
            low = middle + 1;
        else
            return &entries[middle];
    }

    return nullptr;
}

double edge_value(const std::uint8_t* surface, int edge, const PluckerDouble& ray)
{
    static constexpr std::size_t offsets[3][6] = {
        {44, 48, 40, 36, 28, 32},
        {68, 72, 64, 60, 52, 56},
        {92, 96, 88, 84, 76, 80},
    };

    double result = 0.0;
    for (int component = 0; component < 6; ++component)
    {
        result += static_cast<double>(read_value<float>(surface, offsets[edge][component]))
            * ray.value[component];
    }
    return result;
}

bool edge_side(double value, bool invert)
{
    return invert ? value >= 0.0 : value < 0.0;
}

bool intersect_static_surface_double(
    std::uint8_t* mesh,
    std::int32_t local_surface_index,
    const PluckerDouble& plucker,
    const Ray& ray,
    double interval_min,
    double interval_max,
    HitRecord& hit,
    double& hit_t,
    bool paired_fp64_call)
{
    const LONG leaf_call_number = InterlockedIncrement(&g_leaf_fp64_tests);
    if (leaf_call_number == 1)
    {
        append_log(
            "FIRST_LEAF_FP64_ENTER",
            "function=0x004B4507 route=%s local_surface_index=%ld "
            "interval_min=%.17g interval_max=%.17g "
            "plucker=(%.17g,%.17g,%.17g,%.17g,%.17g,%.17g) thread_id=%lu",
            paired_fp64_call ? "paired_native_fp64" : "external_float_abi_promoted",
            static_cast<long>(local_surface_index),
            interval_min,
            interval_max,
            plucker.value[0],
            plucker.value[1],
            plucker.value[2],
            plucker.value[3],
            plucker.value[4],
            plucker.value[5],
            GetCurrentThreadId());
    }

    if (!mesh)
        return false;

    std::uint8_t* mesh_data = read_pointer(mesh, 0);
    if (!mesh_data)
        return false;

    const std::int32_t base_surface_index = read_value<std::int32_t>(mesh, 8);
    const std::int32_t surface_index = local_surface_index + base_surface_index;
    std::uint8_t* surface_array = read_pointer(mesh_data, 0x64);
    if (!surface_array || surface_index < 0)
        return false;

    std::uint8_t* surface = surface_array + static_cast<std::size_t>(surface_index) * 124u;
    const std::uint16_t flags = read_value<std::uint16_t>(surface, 102);
    if ((flags & 1u) == 0)
        return false;

    const double normal[3] = {
        static_cast<double>(read_value<float>(surface, 0)),
        static_cast<double>(read_value<float>(surface, 4)),
        static_cast<double>(read_value<float>(surface, 8)),
    };

    if (read_value<std::int32_t>(surface, 104) != -1)
    {
        const double facing = normal[0] * static_cast<double>(ray.direction[0])
            + normal[1] * static_cast<double>(ray.direction[1])
            + normal[2] * static_cast<double>(ray.direction[2]);
        if (facing > 0.0)
            return false;
    }

    const bool side0 = edge_side(edge_value(surface, 0, plucker), (flags & 0x80u) != 0);
    const bool side1 = edge_side(edge_value(surface, 1, plucker), (flags & 0x100u) != 0);
    if (side0 != side1)
        return false;
    const bool side2 = edge_side(edge_value(surface, 2, plucker), (flags & 0x200u) != 0);
    if (side0 != side2)
        return false;

    const double denominator = normal[0] * static_cast<double>(ray.direction[0])
        + normal[1] * static_cast<double>(ray.direction[1])
        + normal[2] * static_cast<double>(ray.direction[2]);
    const double numerator = normal[0] * static_cast<double>(ray.origin[0])
        + normal[1] * static_cast<double>(ray.origin[1])
        + normal[2] * static_cast<double>(ray.origin[2])
        - static_cast<double>(read_value<float>(surface, 12));

    double candidate_t = static_cast<double>(FLT_MAX);
    if ((denominator >= kDirectionEpsilon || denominator <= -kDirectionEpsilon)
        && (numerator >= kDirectionEpsilon
            || numerator <= -kDirectionEpsilon
            || denominator <= 0.0))
    {
        candidate_t = -numerator / denominator;
    }

    if (!std::isfinite(candidate_t)
        || candidate_t < interval_min - kHitEpsilon
        || candidate_t > interval_max + kHitEpsilon)
    {
        return false;
    }

    const std::uint16_t material = read_value<std::uint16_t>(surface, 100);
    std::int32_t hit_type = 0;
    if (material == 0xFFFFu)
    {
        if (ray.suppress_sky != 0)
            return false;

        if (ray.filter != -1)
        {
            std::uint8_t* cluster_array = read_pointer(mesh_data, 0x58);
            if (!cluster_array)
                return false;
            const std::int16_t cluster = read_value<std::int16_t>(
                cluster_array + static_cast<std::size_t>(surface_index) * 48u,
                32);
            if (static_cast<std::int32_t>(cluster) != ray.filter)
                return false;
        }
        hit_type = 3;
    }
    else
    {
        if (ray.filter != -1)
            return false;

        if ((flags & 2u) != 0)
            hit_type = 2;
        else if ((flags & 8u) != 0)
            hit_type = 1;
        else
            hit_type = 0;
    }

    write_value<float>(&hit, 0, static_cast<float>(candidate_t));
    write_value<std::int32_t>(&hit, 52, surface_index);
    write_value<std::int32_t>(&hit, 64, hit_type);
    hit_t = candidate_t;
    InterlockedIncrement(&g_leaf_fp64_hits);
    return true;
}

extern "C" unsigned char __fastcall static_surface_double_entry(
    void* mesh_pointer,
    void*,
    std::int32_t local_surface_index,
    const void* plucker_argument,
    Ray* ray,
    HitRecord* output)
{
    InterlockedIncrement(&g_leaf_entry_calls);
    if (!mesh_pointer || !plucker_argument || !ray || !output)
        return 0;

    PluckerDouble plucker{};
    double interval_min = static_cast<double>(ray->t_min);
    double interval_max = static_cast<double>(ray->t_max);
    double* hit_t_output = nullptr;
    bool paired_fp64_call = false;

    const auto* widened = static_cast<const WidenedLeafCall*>(plucker_argument);
    if (widened->magic == kWidenedLeafMagic
        && widened->magic_inverse == kWidenedLeafMagicInverse)
    {
        plucker = widened->plucker;
        interval_min = widened->interval_min;
        interval_max = widened->interval_max;
        hit_t_output = widened->hit_t_output;
        paired_fp64_call = true;
    }
    else
    {
        // Original sub_4B4507 receives a seven-dword packet: a cache key followed
        // by six FP32 Plucker coordinates. This compatibility path exists only for
        // an unexpected caller outside sub_4B2DD4; IDA shows no such caller.
        const auto* original = static_cast<const float*>(plucker_argument);
        for (int component = 0; component < 6; ++component)
            plucker.value[component] = static_cast<double>(original[component + 1]);
    }

    double hit_t = interval_max;
    const bool accepted = intersect_static_surface_double(
        static_cast<std::uint8_t*>(mesh_pointer),
        local_surface_index,
        plucker,
        *ray,
        interval_min,
        interval_max,
        *output,
        hit_t,
        paired_fp64_call);
    if (accepted && hit_t_output)
        *hit_t_output = hit_t;
    return accepted ? 1 : 0;
}

int choose_step_axis(const double next_t[3])
{
    if (next_t[0] <= next_t[1])
        return next_t[0] > next_t[2] ? 2 : 0;
    return next_t[1] > next_t[2] ? 2 : 1;
}

using DynamicTrace = unsigned char(__thiscall*)(void*, Ray*, HitRecord*);
using FinalizeStaticHit = int(__thiscall*)(void*, Ray*, HitRecord*);
using StaticSurfaceTrace = unsigned char(__thiscall*)(
    void*, std::int32_t, const void*, Ray*, HitRecord*);

extern "C" unsigned char __fastcall trace_double(
    void* context_pointer,
    void*,
    const InitialCell* initial,
    Ray* ray,
    HitRecord* output)
{
    const LONG call_number = InterlockedIncrement(&g_trace_calls);
    if (!context_pointer || !initial || !ray || !output)
    {
        if (call_number == 1)
        {
            append_log(
                "FIRST_TRACE_ENTER",
                "invalid_arguments context=0x%08lX initial=0x%08lX ray=0x%08lX output=0x%08lX thread_id=%lu",
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(context_pointer)),
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(initial)),
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(ray)),
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(output)),
                GetCurrentThreadId());
        }
        return finish_trace(false, call_number, output);
    }

    if (call_number == 1)
    {
        append_log(
            "FIRST_TRACE_ENTER",
            "origin=(%.9g,%.9g,%.9g) direction=(%.9g,%.9g,%.9g) t_min=%.9g t_max=%.9g "
            "initial_cell=0x%08lX context=0x%08lX thread_id=%lu",
            static_cast<double>(ray->origin[0]),
            static_cast<double>(ray->origin[1]),
            static_cast<double>(ray->origin[2]),
            static_cast<double>(ray->direction[0]),
            static_cast<double>(ray->direction[1]),
            static_cast<double>(ray->direction[2]),
            static_cast<double>(ray->t_min),
            static_cast<double>(ray->t_max),
            static_cast<DWORD>(initial->packed_cell),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(context_pointer)),
            GetCurrentThreadId());
    }

    auto* context = static_cast<std::uint8_t*>(context_pointer);
    const float saved_t_min = ray->t_min;
    const float saved_t_max = ray->t_max;
    const double ray_limit = static_cast<double>(saved_t_max);

    double endpoint[3]{};
    for (int axis = 0; axis < 3; ++axis)
    {
        endpoint[axis] = static_cast<double>(ray->origin[axis])
            + static_cast<double>(ray->direction[axis]) * ray_limit;
    }

    PluckerDouble plucker{};
    plucker.value[0] = endpoint[1] * static_cast<double>(ray->origin[0])
        - static_cast<double>(ray->origin[1]) * endpoint[0];
    plucker.value[1] = endpoint[2] * static_cast<double>(ray->origin[0])
        - static_cast<double>(ray->origin[2]) * endpoint[0];
    plucker.value[2] = static_cast<double>(ray->origin[0]) - endpoint[0];
    plucker.value[3] = endpoint[2] * static_cast<double>(ray->origin[1])
        - static_cast<double>(ray->origin[2]) * endpoint[1];
    plucker.value[4] = static_cast<double>(ray->origin[2]) - endpoint[2];
    plucker.value[5] = endpoint[1] - static_cast<double>(ray->origin[1]);

    std::int32_t packed_cell = initial->packed_cell;
    double traversal_base_t = 0.0;
    double traversal_position[3] = {
        static_cast<double>(ray->origin[0]),
        static_cast<double>(ray->origin[1]),
        static_cast<double>(ray->origin[2]),
    };

    if (packed_cell == -1)
    {
        double relative_origin[3]{};
        double extent[3]{};
        for (int axis = 0; axis < 3; ++axis)
        {
            relative_origin[axis] = static_cast<double>(ray->origin[axis]) - grid_minimum(context, axis);
            extent[axis] = grid_maximum(context, axis) - grid_minimum(context, axis);
        }

        double clipped_min = 0.0;
        double clipped_max = ray_limit;
        if (!clip_aabb_double(relative_origin, ray->direction, extent, clipped_min, clipped_max))
        {
            InterlockedIncrement(&g_aabb_rejects);
            ray->t_min = saved_t_min;
            ray->t_max = saved_t_max;
            return finish_trace(false, call_number, output);
        }

        traversal_base_t = clipped_min;
        for (int axis = 0; axis < 3; ++axis)
        {
            traversal_position[axis] = static_cast<double>(ray->origin[axis])
                + static_cast<double>(ray->direction[axis]) * clipped_min;
        }

        packed_cell = position_to_grid_double(context, traversal_position, true);
        if (packed_cell == -1)
        {
            ray->t_min = saved_t_min;
            ray->t_max = saved_t_max;
            return finish_trace(false, call_number, output);
        }
    }

    int cell[3]{};
    unpack_cell(static_cast<std::uint32_t>(packed_cell), cell);

    int step[3]{};
    double inverse_direction[3]{};
    double next_t[3]{};
    for (int axis = 0; axis < 3; ++axis)
    {
        const double direction = static_cast<double>(ray->direction[axis]);
        step[axis] = direction >= 0.0 ? 1 : -1;
        inverse_direction[axis] = std::fabs(direction) >= kDirectionEpsilon
            ? 1.0 / direction
            : static_cast<double>(FLT_MAX);

        const double boundary = step[axis] < 0
            ? cell_minimum(context, axis, cell[axis])
            : cell_maximum(context, axis, cell[axis]);
        double candidate = std::fabs(direction) >= kDirectionEpsilon
            ? (boundary - traversal_position[axis]) / direction + traversal_base_t
            : static_cast<double>(FLT_MAX);
        if (candidate < traversal_base_t)
            candidate = traversal_base_t;
        next_t[axis] = candidate;
    }

    bool found_hit = false;
    double cell_start_t = static_cast<double>(saved_t_min);
    const int maximum_steps = grid_count(context, 0)
        + grid_count(context, 1)
        + grid_count(context, 2)
        + 4;

    for (int iteration = 0; iteration < maximum_steps; ++iteration)
    {
        InterlockedIncrement(&g_cells_visited);
        const int step_axis = choose_step_axis(next_t);
        const std::uint32_t packed = pack_cell(cell);
        const CellEntry* entry = find_cell_entry(context, packed);

        if (entry)
        {
            const double cell_end_t = std::min(next_t[step_axis], ray_limit);
            std::uint8_t* range_array = read_pointer(context, 0x24);
            std::uint8_t* primitive_array = read_pointer(context, 0x30);
            if (range_array && primitive_array)
            {
                const std::uint8_t* range = range_array + static_cast<std::size_t>(entry->range_index) * 8u;
                const std::int32_t first = read_value<std::int32_t>(range, 0);
                const std::int32_t count = read_value<std::int32_t>(range, 4);

                if (first >= 0 && count > 0)
                {
                    ray->t_min = static_cast<float>(cell_start_t);
                    ray->t_max = static_cast<float>(cell_end_t);

                    bool cell_hit = false;
                    double best_t = cell_end_t;
                    HitRecord best{};

                    for (std::int32_t item = 0; item < count; ++item)
                    {
                        const std::int32_t reference = read_value<std::int32_t>(
                            primitive_array,
                            static_cast<std::size_t>(first + item) * 4u);
                        HitRecord candidate{};
                        double candidate_t = cell_end_t;
                        bool accepted = false;

                        if (reference < 0)
                        {
                            InterlockedIncrement(&g_static_tests);
                            std::uint8_t* mesh = read_pointer(context, 0x14);
                            WidenedLeafCall leaf_call{};
                            leaf_call.magic = kWidenedLeafMagic;
                            leaf_call.magic_inverse = kWidenedLeafMagicInverse;
                            leaf_call.plucker = plucker;
                            leaf_call.interval_min = cell_start_t;
                            leaf_call.interval_max = cell_end_t;
                            leaf_call.hit_t_output = &candidate_t;

                            // Call through the patched 0x4B4507 entry point. The
                            // private packet keeps Plucker coordinates and cell
                            // bounds in FP64 across the 0x4B2DD4 -> 0x4B4507 edge.
                            const auto static_surface = reinterpret_cast<StaticSurfaceTrace>(
                                g_exe_base + kStaticSurfaceRva);
                            accepted = static_surface(
                                mesh,
                                reference & 0x7FFFFFFF,
                                &leaf_call,
                                ray,
                                &candidate) != 0;
                            if (accepted)
                            {
                                InterlockedIncrement(&g_static_hits);
                                write_value<std::uint32_t>(&candidate, 48, static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(context)));
                            }
                        }
                        else
                        {
                            std::uint8_t* object_array = read_pointer(context, 0x18);
                            if (object_array)
                            {
                                std::uint8_t* object = read_pointer(object_array, static_cast<std::size_t>(reference) * 4u);
                                if (object && read_value<std::uint8_t>(object, 8) == 0)
                                {
                                    std::uint8_t* vtable = read_pointer(object, 0);
                                    if (vtable)
                                    {
                                        const auto dynamic_trace = reinterpret_cast<DynamicTrace>(
                                            read_value<std::uint32_t>(vtable, 4));
                                        if (dynamic_trace)
                                        {
                                            InterlockedIncrement(&g_dynamic_tests);
                                            if (dynamic_trace(object, ray, &candidate))
                                            {
                                                InterlockedIncrement(&g_dynamic_hits);
                                                accepted = true;
                                                candidate_t = static_cast<double>(read_value<float>(&candidate, 0));
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        if (accepted && candidate_t < best_t)
                        {
                            best_t = candidate_t;
                            best = candidate;
                            cell_hit = true;
                        }
                    }

                    if (cell_hit)
                    {
                        *output = best;
                        found_hit = true;
                        break;
                    }
                }
            }
        }

        const double boundary_t = next_t[step_axis];
        if (boundary_t > ray_limit)
            break;

        cell[step_axis] += step[step_axis];
        const int count = grid_count(context, step_axis);
        if (cell[step_axis] < 0 || cell[step_axis] >= count)
            break;

        const double width = cell_maximum(context, step_axis, cell[step_axis])
            - cell_minimum(context, step_axis, cell[step_axis]);
        next_t[step_axis] = boundary_t
            + width * static_cast<double>(step[step_axis]) * inverse_direction[step_axis];
        cell_start_t = boundary_t;
    }

    ray->t_min = saved_t_min;
    ray->t_max = saved_t_max;

    const std::uint32_t output_owner = read_value<std::uint32_t>(output, 48);
    const std::uint32_t context_value = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(context));
    if (!found_hit && output_owner == context_value)
    {
        for (std::size_t offset = 4; offset <= 36; offset += 4)
            write_value<std::uint32_t>(output, offset, 0u);
    }
    else if (found_hit && output_owner == context_value)
    {
        std::uint8_t* mesh = read_pointer(context, 0x14);
        const auto finalize = reinterpret_cast<FinalizeStaticHit>(g_exe_base + kFinalizeRva);
        if (mesh && finalize)
            finalize(mesh, ray, output);
    }

    return finish_trace(found_hit, call_number, output);
}

float next_gather_random()
{
    auto** state_slot = reinterpret_cast<std::uint32_t**>(g_exe_base + kRandomStatePointerRva);
    std::uint32_t* state_pointer = state_slot ? *state_slot : nullptr;
    if (!state_pointer)
        return 0.0f;

    const std::uint32_t state = 1664525u * *state_pointer + 1013904223u;
    *state_pointer = state;
    const std::uint32_t high_word = state >> 16;
    return static_cast<float>(static_cast<double>(high_word) * 0.00001525902189314365);
}

void normalize_float3_vista(float vector[3])
{
    const double length_squared_double =
        static_cast<double>(vector[0]) * static_cast<double>(vector[0])
        + static_cast<double>(vector[1]) * static_cast<double>(vector[1])
        + static_cast<double>(vector[2]) * static_cast<double>(vector[2]);
    const float length_squared = static_cast<float>(length_squared_double);
    const float length = static_cast<float>(std::sqrt(static_cast<double>(length_squared)));
    const float distance_from_zero = static_cast<float>(static_cast<double>(length) - 0.0);
    if (std::fabs(static_cast<double>(distance_from_zero)) < kNormalizeEpsilon)
        return;

    const float inverse_length = static_cast<float>(1.0 / static_cast<double>(length));
    vector[0] = static_cast<float>(static_cast<double>(inverse_length) * static_cast<double>(vector[0]));
    vector[1] = static_cast<float>(static_cast<double>(inverse_length) * static_cast<double>(vector[1]));
    vector[2] = static_cast<float>(static_cast<double>(inverse_length) * static_cast<double>(vector[2]));
}

void build_tangent_frame_double(const float normal[3], float frame[9])
{
    const double nx = static_cast<double>(normal[0]);
    const double ny = static_cast<double>(normal[1]);
    const double nz = static_cast<double>(normal[2]);
    double tangent_x = 0.0;
    double tangent_y = 0.0;
    double tangent_z = 0.0;

    const double axis_distance = std::fabs(std::fabs(nx) - 1.0);
    if (axis_distance >= 0.10000000149011612)
    {
        tangent_x = 0.0;
        tangent_y = nz;
        tangent_z = -ny;
    }
    else
    {
        tangent_x = nz;
        tangent_y = 0.0;
        tangent_z = -nx;
    }

    const double tangent_length = std::sqrt(
        tangent_x * tangent_x + tangent_y * tangent_y + tangent_z * tangent_z);
    if (std::fabs(tangent_length) >= kNormalizeEpsilon)
    {
        const double inverse_length = 1.0 / tangent_length;
        tangent_x *= inverse_length;
        tangent_y *= inverse_length;
        tangent_z *= inverse_length;
    }

    frame[0] = static_cast<float>(tangent_x);
    frame[1] = static_cast<float>(tangent_y);
    frame[2] = static_cast<float>(tangent_z);
    frame[3] = static_cast<float>(tangent_y * nz - tangent_z * ny);
    frame[4] = static_cast<float>(tangent_z * nx - tangent_x * nz);
    frame[5] = static_cast<float>(tangent_x * ny - tangent_y * nx);
    frame[6] = normal[0];
    frame[7] = normal[1];
    frame[8] = normal[2];
}

void transform_direction_double(const float frame[9], const float local[3], float world[3])
{
    world[0] = static_cast<float>(
        static_cast<double>(frame[3]) * static_cast<double>(local[1])
        + static_cast<double>(frame[0]) * static_cast<double>(local[0])
        + static_cast<double>(frame[6]) * static_cast<double>(local[2]));
    world[1] = static_cast<float>(
        static_cast<double>(frame[1]) * static_cast<double>(local[0])
        + static_cast<double>(frame[4]) * static_cast<double>(local[1])
        + static_cast<double>(frame[7]) * static_cast<double>(local[2]));
    world[2] = static_cast<float>(
        static_cast<double>(frame[2]) * static_cast<double>(local[0])
        + static_cast<double>(frame[5]) * static_cast<double>(local[1])
        + static_cast<double>(frame[8]) * static_cast<double>(local[2]));
}

unsigned char call_gather_trace(
    const InitialCell* initial,
    Ray* ray,
    HitRecord* hit,
    float transmission[3])
{
    const std::uintptr_t function_address = g_exe_base + kGatherTraceRva;
    unsigned char result = 0;
    __asm
    {
        push transmission
        push hit
        mov edx, ray
        mov ecx, initial
        call function_address
        add esp, 8
        mov result, al
    }
    return result;
}

unsigned char call_hit_radiance(HitRecord* hit, float radiance[3])
{
    using HitRadianceFunction = unsigned char(__fastcall*)(HitRecord*, float*);
    const auto function = reinterpret_cast<HitRadianceFunction>(g_exe_base + kHitRadianceRva);
    return function(hit, radiance);
}

int call_hit_sky_index(HitRecord* hit)
{
    using HitSkyIndexFunction = int(__thiscall*)(HitRecord*);
    const auto function = reinterpret_cast<HitSkyIndexFunction>(g_exe_base + kHitSkyIndexRva);
    return function(hit);
}

void call_sky_radiance(int sky_index, const float direction[3], float radiance[3])
{
    const std::uintptr_t function_address = g_exe_base + kSkyRadianceRva;
    __asm
    {
        push 0
        push radiance
        mov edx, direction
        mov ecx, sky_index
        call function_address
        add esp, 8
    }
}

float luminance_double(const float color[3])
{
    constexpr float kRed = 0.21267099678516388f;
    constexpr float kGreen = 0.71516001224517822f;
    constexpr float kBlue = 0.072168998420238495f;
    return static_cast<float>(
        static_cast<double>(color[0]) * static_cast<double>(kRed)
        + static_cast<double>(color[1]) * static_cast<double>(kGreen)
        + static_cast<double>(color[2]) * static_cast<double>(kBlue));
}

void add_directional_energy(float accumulator[3], const float direction[3], float luminance)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        const float term = static_cast<float>(
            static_cast<double>(direction[axis]) * static_cast<double>(luminance));
        accumulator[axis] = static_cast<float>(
            static_cast<double>(accumulator[axis]) + static_cast<double>(term));
    }
}

void add_weighted_radiance(float accumulator[3], const float radiance[3], float weight)
{
    for (int channel = 0; channel < 3; ++channel)
    {
        accumulator[channel] = static_cast<float>(
            static_cast<double>(radiance[channel]) * static_cast<double>(weight)
            + static_cast<double>(accumulator[channel]));
    }
}

extern "C" float* __cdecl mc_final_gather_double(
    const float* origin,
    const float* normal,
    void*,
    const InitialCell* initial,
    float* radiance_output,
    float* directional_output,
    float* dominant_output)
{
    const LONG64 call_number = InterlockedIncrement64(&g_gather_calls);
    if (!origin || !normal || !initial || !radiance_output || !directional_output || !dominant_output)
    {
        append_log(
            "GATHER_ARGUMENT_ERROR",
            "call=%I64d origin=0x%08lX normal=0x%08lX initial=0x%08lX radiance=0x%08lX "
            "directional=0x%08lX dominant=0x%08lX",
            call_number,
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(origin)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(normal)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(initial)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(radiance_output)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(directional_output)),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(dominant_output)));
        return dominant_output;
    }

    radiance_output[0] = 0.0f;
    radiance_output[1] = 0.0f;
    radiance_output[2] = 0.0f;
    directional_output[0] = 0.0f;
    directional_output[1] = 0.0f;
    directional_output[2] = 0.0f;
    dominant_output[0] = 0.0f;
    dominant_output[1] = 0.0f;
    dominant_output[2] = 0.0f;

    const int main_samples = *reinterpret_cast<const int*>(g_exe_base + kMainSamplesRva);
    const int secondary_samples = *reinterpret_cast<const int*>(g_exe_base + kSecondarySamplesRva);
    std::uint32_t* random_state = *reinterpret_cast<std::uint32_t**>(g_exe_base + kRandomStatePointerRva);

    if (call_number == 1)
    {
        append_log(
            "FIRST_GATHER_ENTER",
            "main_samples=%d secondary_samples=%d origin=(%.9g,%.9g,%.9g) normal=(%.9g,%.9g,%.9g) "
            "initial_owner=0x%08lX initial_cell=0x%08lX rng_state=0x%08lX thread_id=%lu",
            main_samples,
            secondary_samples,
            static_cast<double>(origin[0]),
            static_cast<double>(origin[1]),
            static_cast<double>(origin[2]),
            static_cast<double>(normal[0]),
            static_cast<double>(normal[1]),
            static_cast<double>(normal[2]),
            initial->owner,
            static_cast<DWORD>(initial->packed_cell),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(random_state)),
            GetCurrentThreadId());
    }

    LONG64 local_samples = 0;
    LONG64 local_trace_hits = 0;
    LONG64 local_trace_misses = 0;
    LONG64 local_sky_hits = 0;
    LONG64 local_surface_hits = 0;
    float maximum_luminance = 0.0f;
    float dominant_direction[3] = {0.0f, 0.0f, 0.0f};

    if (main_samples > 0 && secondary_samples > 0)
    {
        const int total_samples = main_samples * secondary_samples;
        const float weight = static_cast<float>(
            1.0 / static_cast<double>(static_cast<float>(total_samples)));

        for (int outer = 0; outer < main_samples; ++outer)
        {
            const float outer_index = static_cast<float>(outer);
            for (int inner = 0; inner < secondary_samples; ++inner)
            {
                ++local_samples;
                const float outer_random = next_gather_random();
                const float inner_random = next_gather_random();

                const float inner_fraction = static_cast<float>(
                    (static_cast<double>(inner_random) + static_cast<double>(inner))
                    / static_cast<double>(static_cast<float>(secondary_samples)));
                const float root = static_cast<float>(
                    std::sqrt(static_cast<double>(inner_fraction)));
                const float clamped_root = std::max(-1.0f, std::min(1.0f, root));
                const float theta = static_cast<float>(
                    std::acos(static_cast<double>(clamped_root)));

                const float outer_fraction = static_cast<float>(
                    (static_cast<double>(outer_random) + static_cast<double>(outer_index))
                    / static_cast<double>(static_cast<float>(main_samples)));
                const float phi = static_cast<float>(
                    static_cast<double>(outer_fraction) * 6.2831854820251465
                    - 3.1415927410125732);

                const float cos_theta = static_cast<float>(std::cos(static_cast<double>(theta)));
                const float local_direction[3] = {
                    static_cast<float>(
                        std::cos(static_cast<double>(phi)) * static_cast<double>(cos_theta)),
                    static_cast<float>(
                        std::sin(static_cast<double>(phi)) * static_cast<double>(cos_theta)),
                    std::max(
                        0.0f,
                        std::min(
                            1.0f,
                            static_cast<float>(std::sin(static_cast<double>(theta)))))};

                float frame[9]{};
                build_tangent_frame_double(normal, frame);
                float world_direction[3]{};
                transform_direction_double(frame, local_direction, world_direction);
                normalize_float3_vista(world_direction);

                Ray ray{};
                ray.origin[0] = origin[0];
                ray.origin[1] = origin[1];
                ray.origin[2] = origin[2];
                ray.direction[0] = world_direction[0];
                ray.direction[1] = world_direction[1];
                ray.direction[2] = world_direction[2];
                ray.t_min = 0.0f;
                ray.t_max = 2048.0f;
                ray.suppress_sky = 0;
                ray.filter = -1;

                HitRecord hit{};
                float transmission[3]{};
                const unsigned char traced = call_gather_trace(initial, &ray, &hit, transmission);
                volatile double* gather_ray_counter =
                    reinterpret_cast<volatile double*>(g_exe_base + kGatherRayCounterRva);
                *gather_ray_counter = *gather_ray_counter + 1.0;

                if (!traced)
                {
                    ++local_trace_misses;
                    continue;
                }

                ++local_trace_hits;
                float source_radiance[3]{};
                const int hit_type = read_value<int>(&hit, 64);
                if (hit_type == 3)
                {
                    ++local_sky_hits;
                    const int sky_index = call_hit_sky_index(&hit);
                    call_sky_radiance(sky_index, world_direction, source_radiance);
                }
                else
                {
                    ++local_surface_hits;
                    if (!call_hit_radiance(&hit, source_radiance))
                        continue;
                }

                float transmitted_radiance[3]{};
                for (int channel = 0; channel < 3; ++channel)
                {
                    transmitted_radiance[channel] = static_cast<float>(
                        static_cast<double>(source_radiance[channel])
                        * static_cast<double>(transmission[channel]));
                }

                const float luminance = luminance_double(transmitted_radiance);
                add_directional_energy(directional_output, world_direction, luminance);
                if (static_cast<double>(luminance) > static_cast<double>(maximum_luminance))
                {
                    maximum_luminance = luminance;
                    dominant_direction[0] = world_direction[0];
                    dominant_direction[1] = world_direction[1];
                    dominant_direction[2] = world_direction[2];
                }
                add_weighted_radiance(radiance_output, transmitted_radiance, weight);
            }
        }
    }

    normalize_float3_vista(directional_output);
    dominant_output[0] = static_cast<float>(
        static_cast<double>(dominant_direction[0]) * static_cast<double>(maximum_luminance));
    dominant_output[1] = static_cast<float>(
        static_cast<double>(dominant_direction[1]) * static_cast<double>(maximum_luminance));
    dominant_output[2] = static_cast<float>(
        static_cast<double>(dominant_direction[2]) * static_cast<double>(maximum_luminance));

    InterlockedExchangeAdd64(&g_gather_samples, local_samples);
    InterlockedExchangeAdd64(&g_gather_trace_hits, local_trace_hits);
    InterlockedExchangeAdd64(&g_gather_trace_misses, local_trace_misses);
    InterlockedExchangeAdd64(&g_gather_sky_hits, local_sky_hits);
    InterlockedExchangeAdd64(&g_gather_surface_hits, local_surface_hits);

    if (call_number == 1)
    {
        append_log(
            "FIRST_GATHER_EXIT",
            "samples=%I64d trace_hits=%I64d trace_misses=%I64d sky_hits=%I64d surface_hits=%I64d "
            "radiance=(%.9g,%.9g,%.9g) directional=(%.9g,%.9g,%.9g) dominant=(%.9g,%.9g,%.9g)",
            local_samples,
            local_trace_hits,
            local_trace_misses,
            local_sky_hits,
            local_surface_hits,
            static_cast<double>(radiance_output[0]),
            static_cast<double>(radiance_output[1]),
            static_cast<double>(radiance_output[2]),
            static_cast<double>(directional_output[0]),
            static_cast<double>(directional_output[1]),
            static_cast<double>(directional_output[2]),
            static_cast<double>(dominant_output[0]),
            static_cast<double>(dominant_output[1]),
            static_cast<double>(dominant_output[2]));
    }
    else if (call_number == 1000 || (call_number % 1000000) == 0)
    {
        log_counters("GATHER_CHECKPOINT");
    }

    return dominant_output;
}

extern "C" __declspec(naked) void mc_final_gather_entry()
{
    __asm
    {
        push ebp
        mov ebp, esp
        push dword ptr [ebp + 18h]
        push dword ptr [ebp + 14h]
        push dword ptr [ebp + 10h]
        push dword ptr [ebp + 0Ch]
        push dword ptr [ebp + 08h]
        push edx
        push ecx
        call mc_final_gather_double
        add esp, 1Ch
        mov esp, ebp
        pop ebp
        ret
    }
}

void normalize_producer_vector(float* value)
{
    const double x = static_cast<double>(value[0]);
    const double y = static_cast<double>(value[1]);
    const double z = static_cast<double>(value[2]);
    const double length = std::sqrt(x * x + y * y + z * z);
    if (std::fabs(length) < kNormalizeEpsilon)
        return;

    const double inverse_length = 1.0 / length;
    value[0] = static_cast<float>(x * inverse_length);
    value[1] = static_cast<float>(y * inverse_length);
    value[2] = static_cast<float>(z * inverse_length);
}

void transform_producer_position_and_vectors(
    const float* matrix,
    float* position,
    float* normal,
    float* plane_normal)
{
    double position_x = static_cast<double>(position[0]);
    double position_y = static_cast<double>(position[1]);
    double position_z = static_cast<double>(position[2]);
    if (matrix[0] != 1.0f)
    {
        const double scale = static_cast<double>(matrix[0]);
        position_x *= scale;
        position_y *= scale;
        position_z *= scale;
    }

    position[0] = static_cast<float>(
        static_cast<double>(matrix[1]) * position_x
        + static_cast<double>(matrix[4]) * position_y
        + static_cast<double>(matrix[7]) * position_z
        + static_cast<double>(matrix[10]));
    position[1] = static_cast<float>(
        static_cast<double>(matrix[2]) * position_x
        + static_cast<double>(matrix[5]) * position_y
        + static_cast<double>(matrix[8]) * position_z
        + static_cast<double>(matrix[11]));
    position[2] = static_cast<float>(
        static_cast<double>(matrix[3]) * position_x
        + static_cast<double>(matrix[6]) * position_y
        + static_cast<double>(matrix[9]) * position_z
        + static_cast<double>(matrix[12]));

    float* vectors[] = {normal, plane_normal};
    for (float* vector : vectors)
    {
        const double x = static_cast<double>(vector[0]);
        const double y = static_cast<double>(vector[1]);
        const double z = static_cast<double>(vector[2]);
        vector[0] = static_cast<float>(
            static_cast<double>(matrix[1]) * x
            + static_cast<double>(matrix[4]) * y
            + static_cast<double>(matrix[7]) * z);
        vector[1] = static_cast<float>(
            static_cast<double>(matrix[2]) * x
            + static_cast<double>(matrix[5]) * y
            + static_cast<double>(matrix[8]) * z);
        vector[2] = static_cast<float>(
            static_cast<double>(matrix[3]) * x
            + static_cast<double>(matrix[6]) * y
            + static_cast<double>(matrix[9]) * z);
    }
}

extern "C" void __cdecl interpolate_surface_sample_double(
    std::uint8_t* manager,
    float barycentric_u,
    float barycentric_v,
    std::int32_t packed_surface,
    float* position,
    float* normal,
    float* plane_normal,
    const float* object_transform,
    std::uint8_t* skip)
{
    InterlockedIncrement64(&g_interpolate_surface_calls);

    auto* surface_groups = *reinterpret_cast<std::uint8_t**>(manager + 72);
    auto* surface_group = *reinterpret_cast<std::uint8_t**>(
        surface_groups + 4 * (packed_surface >> 5));
    const std::uint32_t surface_index = *reinterpret_cast<const std::uint32_t*>(
        surface_group + 32 * (packed_surface & 0x1F));
    auto* mesh_data = *reinterpret_cast<std::uint8_t**>(manager + 92);
    auto* surfaces = *reinterpret_cast<std::uint8_t**>(mesh_data + 100);
    auto* surface = surfaces + 124 * surface_index;

    if ((*reinterpret_cast<const std::uint16_t*>(surface + 102) & 0x20u) != 0)
    {
        *skip = 1;
        return;
    }
    *skip = 0;

    auto* vertices = *reinterpret_cast<std::uint8_t**>(mesh_data + 64);
    const std::uint32_t index0 = *reinterpret_cast<const std::uint32_t*>(surface + 16);
    const std::uint32_t index1 = *reinterpret_cast<const std::uint32_t*>(surface + 20);
    const std::uint32_t index2 = *reinterpret_cast<const std::uint32_t*>(surface + 24);
    const float* vertex0 = reinterpret_cast<const float*>(vertices + 32 * index0);
    const float* vertex1 = reinterpret_cast<const float*>(vertices + 32 * index1);
    const float* vertex2 = reinterpret_cast<const float*>(vertices + 32 * index2);
    const double u = static_cast<double>(barycentric_u);
    const double v = static_cast<double>(barycentric_v);

    for (int axis = 0; axis < 3; ++axis)
    {
        const double base = static_cast<double>(vertex0[axis]);
        position[axis] = static_cast<float>(
            (static_cast<double>(vertex1[axis]) - base) * u
            + base
            + (static_cast<double>(vertex2[axis]) - base) * v);

        const double normal_base = static_cast<double>(vertex0[3 + axis]);
        normal[axis] = static_cast<float>(
            (static_cast<double>(vertex1[3 + axis]) - normal_base) * u
            + normal_base
            + (static_cast<double>(vertex2[3 + axis]) - normal_base) * v);

        plane_normal[axis] = reinterpret_cast<const float*>(surface)[axis];
    }

    normalize_producer_vector(normal);
    normalize_producer_vector(plane_normal);
    if (object_transform != nullptr)
    {
        transform_producer_position_and_vectors(
            object_transform, position, normal, plane_normal);
    }
}

extern "C" __declspec(naked) void interpolate_surface_sample_entry()
{
    __asm
    {
        push ebp
        mov ebp, esp
        push dword ptr [ebp + 1Ch]
        push dword ptr [ebp + 18h]
        push dword ptr [ebp + 14h]
        push dword ptr [ebp + 10h]
        push dword ptr [ebp + 0Ch]
        push dword ptr [ebp + 08h]
        sub esp, 4
        movss dword ptr [esp], xmm3
        sub esp, 4
        movss dword ptr [esp], xmm2
        push ecx
        call interpolate_surface_sample_double
        add esp, 24h
        mov esp, ebp
        pop ebp
        ret 18h
    }
}

std::uintptr_t g_finalize_gather_bias_continue = 0;
std::uintptr_t g_compute_light_bias_continue = 0;

extern "C" __declspec(naked) void finalize_gather_bias_double_hook()
{
    __asm
    {
        sub esp, 20h
        movdqu xmmword ptr [esp], xmm1
        movdqu xmmword ptr [esp + 10h], xmm3
        cvtss2sd xmm1, xmm1

        cvtss2sd xmm0, dword ptr [ecx]
        mulsd xmm0, xmm1
        cvtss2sd xmm3, dword ptr [eax]
        addsd xmm0, xmm3
        cvtsd2ss xmm0, xmm0
        movss dword ptr [ebp - 0A0h], xmm0

        cvtss2sd xmm0, dword ptr [ecx + 4]
        mulsd xmm0, xmm1
        cvtss2sd xmm3, dword ptr [eax + 4]
        addsd xmm0, xmm3
        cvtsd2ss xmm0, xmm0
        movss dword ptr [ebp - 9Ch], xmm0

        cvtss2sd xmm0, dword ptr [ecx + 8]
        mulsd xmm0, xmm1
        cvtss2sd xmm3, dword ptr [eax + 8]
        addsd xmm0, xmm3
        cvtsd2ss xmm0, xmm0
        mov dword ptr [ebp - 68h], ecx

        movdqu xmm3, xmmword ptr [esp + 10h]
        movdqu xmm1, xmmword ptr [esp]
        add esp, 20h
        jmp dword ptr [g_finalize_gather_bias_continue]
    }
}

extern "C" __declspec(naked) void compute_light_bias_double_hook()
{
    __asm
    {
        movss dword ptr [esp + 14h], xmm2
        movss dword ptr [esp + 10h], xmm2
        movss dword ptr [esp + 0Ch], xmm2
        movss dword ptr [esp + 08h], xmm2

        sub esp, 20h
        movdqu xmmword ptr [esp], xmm1
        movdqu xmmword ptr [esp + 10h], xmm3
        cvtss2sd xmm1, xmm1

        cvtss2sd xmm0, dword ptr [ecx]
        mulsd xmm0, xmm1
        cvtss2sd xmm3, dword ptr [eax]
        addsd xmm0, xmm3
        cvtsd2ss xmm0, xmm0
        movss dword ptr [esp + 90h], xmm0

        cvtss2sd xmm0, dword ptr [ecx + 4]
        mulsd xmm0, xmm1
        cvtss2sd xmm3, dword ptr [eax + 4]
        addsd xmm0, xmm3
        cvtsd2ss xmm0, xmm0
        movss dword ptr [esp + 94h], xmm0

        cvtss2sd xmm0, dword ptr [ecx + 8]
        mulsd xmm0, xmm1
        cvtss2sd xmm3, dword ptr [eax + 8]
        addsd xmm0, xmm3
        cvtsd2ss xmm0, xmm0
        movss dword ptr [esp + 98h], xmm0

        movdqu xmm3, xmmword ptr [esp + 10h]
        movdqu xmm1, xmmword ptr [esp]
        add esp, 20h
        xor eax, eax
        mov dword ptr [esp + 54h], eax
        jmp dword ptr [g_compute_light_bias_continue]
    }
}

using PhotonPatchFunction = void (*)();

struct PhotonPatch
{
    const char* name;
    std::uintptr_t target_rva;
    const std::uint8_t* expected;
    std::size_t size;
    PhotonPatchFunction replacement;
    std::uintptr_t* continuation;
    std::uintptr_t continuation_rva;
};

bool write_relative_jump(const PhotonPatch& patch)
{
    auto* target = reinterpret_cast<std::uint8_t*>(g_exe_base + patch.target_rva);
    auto* replacement = reinterpret_cast<std::uint8_t*>(patch.replacement);
    const std::intptr_t displacement = replacement - (target + 5);
    if (displacement < std::numeric_limits<std::int32_t>::min()
        || displacement > std::numeric_limits<std::int32_t>::max())
    {
        return false;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(target, patch.size, PAGE_EXECUTE_READWRITE, &old_protection))
        return false;

    target[0] = 0xE9;
    const std::int32_t relative = static_cast<std::int32_t>(displacement);
    std::memcpy(target + 1, &relative, sizeof(relative));
    if (patch.size > 5)
        std::memset(target + 5, 0x90, patch.size - 5);
    FlushInstructionCache(GetCurrentProcess(), target, patch.size);

    DWORD ignored = 0;
    VirtualProtect(target, patch.size, old_protection, &ignored);
    return true;
}

struct EntryHook
{
    const char* name;
    std::uintptr_t target_rva;
    const std::uint8_t* expected;
    std::size_t size;
    const void* replacement;
};

bool write_entry_jump(const EntryHook& hook)
{
    auto* target = reinterpret_cast<std::uint8_t*>(g_exe_base + hook.target_rva);
    const auto* replacement = static_cast<const std::uint8_t*>(hook.replacement);
    const std::intptr_t displacement = replacement - (target + 5);
    if (displacement < std::numeric_limits<std::int32_t>::min()
        || displacement > std::numeric_limits<std::int32_t>::max())
    {
        return false;
    }

    DWORD old_protection = 0;
    if (!VirtualProtect(target, hook.size, PAGE_EXECUTE_READWRITE, &old_protection))
        return false;

    target[0] = 0xE9;
    const std::int32_t relative = static_cast<std::int32_t>(displacement);
    std::memcpy(target + 1, &relative, sizeof(relative));
    if (hook.size > 5)
        std::memset(target + 5, 0x90, hook.size - 5);
    FlushInstructionCache(GetCurrentProcess(), target, hook.size);

    DWORD ignored = 0;
    VirtualProtect(target, hook.size, old_protection, &ignored);
    return true;
}

bool install_fp64_trace_and_leaf_hooks()
{
    static const std::uint8_t expected_trace_entry[] = {
        0x81, 0xEC, 0x1C, 0x01, 0x00, 0x00};
    static const std::uint8_t expected_leaf_entry[] = {
        0x83, 0xEC, 0x10, 0x8B, 0x41, 0x08};

    // Install the leaf first. Once the traversal hook is live, every static test
    // calls through 0x4B4507 with the widened private packet.
    const EntryHook hooks[] = {
        {"sub_4B4507_full_fp64", kStaticSurfaceRva, expected_leaf_entry,
            sizeof(expected_leaf_entry), reinterpret_cast<const void*>(&static_surface_double_entry)},
        {"sub_4B2DD4_full_fp64", kTraceRva, expected_trace_entry,
            sizeof(expected_trace_entry), reinterpret_cast<const void*>(&trace_double)},
    };

    for (const EntryHook& hook : hooks)
    {
        const auto* target = reinterpret_cast<const std::uint8_t*>(
            g_exe_base + hook.target_rva);
        if (std::memcmp(target, hook.expected, hook.size) != 0)
        {
            append_log(
                "FP64_HOOK_VERIFY_FAILED",
                "name=%s target=0x%08lX size=%lu",
                hook.name,
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(target)),
                static_cast<unsigned long>(hook.size));
            return false;
        }

        const auto* replacement = static_cast<const std::uint8_t*>(hook.replacement);
        const std::intptr_t displacement = replacement - (target + 5);
        if (displacement < std::numeric_limits<std::int32_t>::min()
            || displacement > std::numeric_limits<std::int32_t>::max())
        {
            append_log("FP64_HOOK_RANGE_FAILED", "name=%s", hook.name);
            return false;
        }

        append_log(
            "FP64_HOOK_VERIFIED",
            "name=%s target=0x%08lX size=%lu",
            hook.name,
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(target)),
            static_cast<unsigned long>(hook.size));
    }

    for (const EntryHook& hook : hooks)
    {
        if (!write_entry_jump(hook))
        {
            append_log("FP64_HOOK_WRITE_FAILED", "name=%s", hook.name);
            return false;
        }

        if (hook.target_rva == kStaticSurfaceRva)
            g_leaf_hook_installed = true;
        else if (hook.target_rva == kTraceRva)
            g_trace_hook_installed = true;

        append_log(
            "FP64_HOOK_INSTALLED",
            "name=%s target=0x%08lX replacement=0x%08lX size=%lu",
            hook.name,
            static_cast<DWORD>(g_exe_base + hook.target_rva),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(hook.replacement)),
            static_cast<unsigned long>(hook.size));
    }

    return g_trace_hook_installed && g_leaf_hook_installed;
}

bool install_input_producer_hooks()
{
    static const std::uint8_t expected_interpolate_entry[] = {
        0x51, 0x8B, 0x41, 0x48, 0x0F, 0x28, 0xE2};
    static const std::uint8_t expected_finalize_bias[] = {
        0xF3, 0x0F, 0x10, 0x01, 0xF3, 0x0F, 0x59, 0xC1, 0x89, 0x4D, 0x98,
        0xF3, 0x0F, 0x58, 0x00, 0xF3, 0x0F, 0x11, 0x85, 0x60, 0xFF, 0xFF,
        0xFF, 0xF3, 0x0F, 0x10, 0x41, 0x04, 0xF3, 0x0F, 0x59, 0xC1, 0xF3,
        0x0F, 0x58, 0x40, 0x04, 0xF3, 0x0F, 0x11, 0x85, 0x64, 0xFF, 0xFF,
        0xFF, 0xF3, 0x0F, 0x10, 0x41, 0x08, 0xF3, 0x0F, 0x59, 0xC1, 0xF3,
        0x0F, 0x58, 0x40, 0x08};
    static const std::uint8_t expected_compute_bias[] = {
        0xF3, 0x0F, 0x10, 0x01, 0xF3, 0x0F, 0x59, 0xC1, 0xF3, 0x0F, 0x11,
        0x54, 0x24, 0x14, 0xF3, 0x0F, 0x11, 0x54, 0x24, 0x10, 0xF3, 0x0F,
        0x58, 0x00, 0xF3, 0x0F, 0x11, 0x54, 0x24, 0x0C, 0xF3, 0x0F, 0x11,
        0x54, 0x24, 0x08, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x70, 0xF3, 0x0F,
        0x10, 0x41, 0x04, 0xF3, 0x0F, 0x59, 0xC1, 0xF3, 0x0F, 0x58, 0x40,
        0x04, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x74, 0xF3, 0x0F, 0x10, 0x41,
        0x08, 0xF3, 0x0F, 0x59, 0xC1, 0xF3, 0x0F, 0x58, 0x40, 0x08, 0x33,
        0xC0, 0x89, 0x44, 0x24, 0x54, 0xF3, 0x0F, 0x11, 0x44, 0x24, 0x78};

    g_finalize_gather_bias_continue = g_exe_base + (0x004A8D26u - kImageBase);
    g_compute_light_bias_continue = g_exe_base + (0x004AE237u - kImageBase);

    const EntryHook hooks[] = {
        {"interpolate_surface_sample_fp64_producer", kInterpolateSurfaceRva,
            expected_interpolate_entry, sizeof(expected_interpolate_entry),
            reinterpret_cast<const void*>(&interpolate_surface_sample_entry)},
        {"gather_and_finalize_origin_bias_fp64", kFinalizeGatherBiasRva,
            expected_finalize_bias, sizeof(expected_finalize_bias),
            reinterpret_cast<const void*>(&finalize_gather_bias_double_hook)},
        {"compute_light_value_origin_bias_fp64", kComputeLightBiasRva,
            expected_compute_bias, sizeof(expected_compute_bias),
            reinterpret_cast<const void*>(&compute_light_bias_double_hook)},
    };

    for (const EntryHook& hook : hooks)
    {
        const auto* target = reinterpret_cast<const std::uint8_t*>(
            g_exe_base + hook.target_rva);
        if (std::memcmp(target, hook.expected, hook.size) != 0)
        {
            append_log(
                "PRODUCER_HOOK_VERIFY_FAILED",
                "name=%s target=0x%08lX size=%lu",
                hook.name,
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(target)),
                static_cast<unsigned long>(hook.size));
            return false;
        }
    }

    for (const EntryHook& hook : hooks)
    {
        if (!write_entry_jump(hook))
        {
            append_log("PRODUCER_HOOK_WRITE_FAILED", "name=%s", hook.name);
            return false;
        }

        if (hook.target_rva == kInterpolateSurfaceRva)
            g_interpolate_hook_installed = true;
        else if (hook.target_rva == kFinalizeGatherBiasRva)
            g_finalize_bias_hook_installed = true;
        else if (hook.target_rva == kComputeLightBiasRva)
            g_compute_bias_hook_installed = true;

        append_log(
            "PRODUCER_HOOK_INSTALLED",
            "name=%s target=0x%08lX replacement=0x%08lX size=%lu",
            hook.name,
            static_cast<DWORD>(g_exe_base + hook.target_rva),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(hook.replacement)),
            static_cast<unsigned long>(hook.size));
    }

    return g_interpolate_hook_installed
        && g_finalize_bias_hook_installed
        && g_compute_bias_hook_installed;
}

bool install_surface_edge_builder_hook()
{
    static const std::uint8_t expected_surface_edge_builder_entry[] = {
        0x83, 0xEC, 0x30, 0x53, 0x8B, 0xD9};
    const EntryHook hook = {
        "surface_edge_plucker_builder_fp64",
        kSurfaceEdgeBuilderRva,
        expected_surface_edge_builder_entry,
        sizeof(expected_surface_edge_builder_entry),
        reinterpret_cast<const void*>(&build_surface_edges_double),
    };

    const auto* target = reinterpret_cast<const std::uint8_t*>(
        g_exe_base + hook.target_rva);
    if (std::memcmp(target, hook.expected, hook.size) != 0)
    {
        append_log(
            "SURFACE_EDGE_HOOK_VERIFY_FAILED",
            "name=%s target=0x%08lX size=%lu",
            hook.name,
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(target)),
            static_cast<unsigned long>(hook.size));
        return false;
    }

    if (!write_entry_jump(hook))
    {
        append_log(
            "SURFACE_EDGE_HOOK_WRITE_FAILED",
            "name=%s target=0x%08lX size=%lu",
            hook.name,
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(target)),
            static_cast<unsigned long>(hook.size));
        return false;
    }

    g_surface_edge_builder_hook_installed = true;
    append_log(
        "SURFACE_EDGE_HOOK_INSTALLED",
        "name=%s target=0x%08lX replacement=0x%08lX size=%lu "
        "h2tool_builder=0x004A01A0 h2tool_fp64_helper=0x004C87B0",
        hook.name,
        static_cast<DWORD>(g_exe_base + hook.target_rva),
        static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(hook.replacement)),
        static_cast<unsigned long>(hook.size));
    return true;
}

bool install_photon_search_hooks()
{
    static const std::uint8_t expected_knn_entry[] = {
        0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};
    static const std::uint8_t expected_radius_divide[] = {
        0xF3, 0x0F, 0x5E, 0xD0, 0xF3, 0x0F, 0x11, 0x54, 0x24, 0x10};
    static const std::uint8_t expected_radius_squares[] = {
        0x0F, 0x28, 0xC2, 0x83, 0x64, 0x24, 0x14, 0x00, 0xF3, 0x0F, 0x11, 0x1D,
        0x40, 0x90, 0xC6, 0x00, 0xF3, 0x0F, 0x59, 0xDB, 0x8D, 0x04, 0xF8, 0xF3,
        0x0F, 0x59, 0xC2};
    static const std::uint8_t expected_radius_grow_four[] = {
        0xF3, 0x0F, 0x10, 0x54, 0x24, 0x10, 0xF3, 0x0F, 0x59, 0x15, 0xC0, 0x66,
        0xBA, 0x00, 0x6A, 0xFE, 0x58, 0xEB, 0x0D};
    static const std::uint8_t expected_radius_grow_two[] = {
        0xF3, 0x0F, 0x10, 0x54, 0x24, 0x10, 0x83, 0xC8, 0xFF, 0xF3, 0x0F, 0x58,
        0xD2};
    static const std::uint8_t expected_split_delta[] = {
        0xF3, 0x0F, 0x5C, 0x04, 0x8E};
    static const std::uint8_t expected_position_delta[] = {
        0xA1, 0x38, 0x90, 0xC6, 0x00, 0xF3, 0x0F, 0x10, 0x1E, 0xF3, 0x0F, 0x10,
        0x66, 0x04, 0xF3, 0x0F, 0x10, 0x6E, 0x08, 0xF3, 0x0F, 0x5C, 0x18, 0xF3,
        0x0F, 0x5C, 0x60, 0x04, 0xF3, 0x0F, 0x5C, 0x68, 0x08, 0xF3, 0x0F, 0x10,
        0x35, 0x44, 0x90, 0xC6, 0x00};
    static const std::uint8_t expected_distance_squared[] = {
        0x0F, 0x28, 0xC3, 0x0F, 0x28, 0xD4, 0xF3, 0x0F, 0x59, 0xC3, 0xF3, 0x0F,
        0x59, 0xD4, 0xF3, 0x0F, 0x58, 0xD0, 0x0F, 0x28, 0xC5, 0xF3, 0x0F, 0x59,
        0xC5, 0xF3, 0x0F, 0x58, 0xD0, 0x0F, 0x2F, 0xF2};
    static const std::uint8_t expected_inner_ellipsoid[] = {
        0xA1, 0x3C, 0x90, 0xC6, 0x00, 0x0F, 0x28, 0xC3, 0xF3, 0x0F, 0x59, 0x00,
        0xF3, 0x0F, 0x10, 0x48, 0x04, 0xF3, 0x0F, 0x59, 0xCC, 0xF3, 0x0F, 0x58,
        0xC8, 0xF3, 0x0F, 0x10, 0x40, 0x08, 0xF3, 0x0F, 0x59, 0xC5, 0xF3, 0x0F,
        0x58, 0xC8, 0x0F, 0x28, 0xC1, 0xF3, 0x0F, 0x59, 0x05, 0xB0, 0x65, 0xBA,
        0x00, 0xF3, 0x0F, 0x59, 0xC1, 0xF3, 0x0F, 0x58, 0xC2};
    static const std::uint8_t expected_outer_ellipsoid[] = {
        0x8B, 0x15, 0x3C, 0x90, 0xC6, 0x00, 0x0F, 0x28, 0xC3, 0xF3, 0x0F, 0x59,
        0x02, 0xF3, 0x0F, 0x10, 0x4A, 0x04, 0xF3, 0x0F, 0x59, 0xCC, 0xF3, 0x0F,
        0x58, 0xC8, 0xF3, 0x0F, 0x10, 0x42, 0x08, 0xF3, 0x0F, 0x59, 0xC5, 0xF3,
        0x0F, 0x58, 0xC8, 0x0F, 0x28, 0xC1, 0xF3, 0x0F, 0x59, 0x05, 0xB0, 0x65,
        0xBA, 0x00, 0xF3, 0x0F, 0x59, 0xC1, 0xF3, 0x0F, 0x58, 0xC2};
    static const std::uint8_t expected_normal_dot[] = {
        0xF3, 0x0F, 0x59, 0x85, 0x64, 0xFF, 0xFF, 0xFF, 0xF3, 0x0F, 0x59, 0x8D,
        0x68, 0xFF, 0xFF, 0xFF, 0xF3, 0x0F, 0x58, 0xC8, 0xF3, 0x0F, 0x10, 0x42,
        0x08, 0xF3, 0x0F, 0x59, 0x85, 0x6C, 0xFF, 0xFF, 0xFF, 0xF3, 0x0F, 0x58,
        0xC8};

    PhotonPatch patches[] = {
        {"knn_entry_counter", kPhotonKnearestRva, expected_knn_entry,
            sizeof(expected_knn_entry), &photon_knn_entry_hook, &g_photon_knn_continue,
            kPhotonKnearestRva + sizeof(expected_knn_entry)},
        {"radius_divide", kPhotonRadiusDivideRva, expected_radius_divide,
            sizeof(expected_radius_divide), &photon_radius_divide_double_hook,
            &g_photon_radius_divide_continue,
            kPhotonRadiusDivideRva + sizeof(expected_radius_divide)},
        {"radius_squares", kPhotonRadiusSquaresRva, expected_radius_squares,
            sizeof(expected_radius_squares), &photon_radius_squares_double_hook,
            &g_photon_radius_squares_continue,
            kPhotonRadiusSquaresRva + sizeof(expected_radius_squares)},
        {"radius_grow_four", kPhotonRadiusGrowFourRva, expected_radius_grow_four,
            sizeof(expected_radius_grow_four), &photon_radius_grow_four_double_hook,
            &g_photon_radius_grow_four_continue,
            kPhotonRadiusGrowFourRva + sizeof(expected_radius_grow_four)},
        {"radius_grow_two", kPhotonRadiusGrowTwoRva, expected_radius_grow_two,
            sizeof(expected_radius_grow_two), &photon_radius_grow_two_double_hook,
            &g_photon_radius_grow_two_continue,
            kPhotonRadiusGrowTwoRva + sizeof(expected_radius_grow_two)},
        {"split_delta", kPhotonSplitDeltaRva, expected_split_delta,
            sizeof(expected_split_delta), &photon_split_delta_double_hook,
            &g_photon_split_delta_continue,
            kPhotonSplitDeltaRva + sizeof(expected_split_delta)},
        {"position_delta", kPhotonPositionDeltaRva, expected_position_delta,
            sizeof(expected_position_delta), &photon_position_delta_double_hook,
            &g_photon_position_delta_continue,
            kPhotonPositionDeltaRva + sizeof(expected_position_delta)},
        {"distance_squared", kPhotonDistanceSquaredRva, expected_distance_squared,
            sizeof(expected_distance_squared), &photon_distance_squared_double_hook,
            &g_photon_distance_squared_continue,
            kPhotonDistanceSquaredRva + sizeof(expected_distance_squared)},
        {"inner_ellipsoid", kPhotonInnerEllipsoidRva, expected_inner_ellipsoid,
            sizeof(expected_inner_ellipsoid), &photon_inner_ellipsoid_double_hook,
            &g_photon_inner_ellipsoid_continue,
            kPhotonInnerEllipsoidRva + sizeof(expected_inner_ellipsoid)},
        {"outer_ellipsoid", kPhotonOuterEllipsoidRva, expected_outer_ellipsoid,
            sizeof(expected_outer_ellipsoid), &photon_outer_ellipsoid_double_hook,
            &g_photon_outer_ellipsoid_continue,
            kPhotonOuterEllipsoidRva + sizeof(expected_outer_ellipsoid)},
        {"photon_normal_dot", kPhotonNormalDotRva, expected_normal_dot,
            sizeof(expected_normal_dot), &photon_normal_dot_double_hook,
            &g_photon_normal_dot_continue,
            kPhotonNormalDotRva + sizeof(expected_normal_dot)},
    };

    g_photon_query_position_slot = g_exe_base + kPhotonQueryPositionSlotRva;
    g_photon_query_normal_slot = g_exe_base + kPhotonQueryNormalSlotRva;
    g_photon_inner_radius_pointer = g_exe_base + kPhotonInnerRadiusRva;
    g_photon_inner_radius_squared_pointer = g_exe_base + kPhotonInnerRadiusSquaredRva;

    for (const PhotonPatch& patch : patches)
    {
        auto* target = reinterpret_cast<const std::uint8_t*>(g_exe_base + patch.target_rva);
        if (std::memcmp(target, patch.expected, patch.size) != 0)
        {
            append_log(
                "PATCH_VERIFY_FAILED",
                "name=%s target=0x%08lX size=%lu",
                patch.name,
                static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(target)),
                static_cast<unsigned long>(patch.size));
            return false;
        }

        auto* replacement = reinterpret_cast<const std::uint8_t*>(patch.replacement);
        const std::intptr_t displacement = replacement - (target + 5);
        if (displacement < std::numeric_limits<std::int32_t>::min()
            || displacement > std::numeric_limits<std::int32_t>::max())
        {
            append_log("PATCH_RANGE_FAILED", "name=%s", patch.name);
            return false;
        }
        *patch.continuation = g_exe_base + patch.continuation_rva;
    }

    for (const PhotonPatch& patch : patches)
    {
        if (!write_relative_jump(patch))
        {
            append_log("PATCH_WRITE_FAILED", "name=%s", patch.name);
            return false;
        }
        append_log(
            "PATCH_INSTALLED",
            "name=%s target=0x%08lX size=%lu replacement=0x%08lX continuation=0x%08lX",
            patch.name,
            static_cast<DWORD>(g_exe_base + patch.target_rva),
            static_cast<unsigned long>(patch.size),
            static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(patch.replacement)),
            static_cast<DWORD>(*patch.continuation));
    }
    return true;
}
} // namespace

// Entry point invoked from H2ToolHooks::hook() when the LIGHTMAP_PRECISION_FIX flag is set. Installs the
// complete R7 six-hook FP64 precision path. Signature-gated on the edge-builder prologue so it is a safe
// no-op on any binary that is not stock MCC tool_fast (H2V H2Tool, byte-patched tool_fast, etc.). Logging
// is left disabled (initialize_logger is intentionally not called), so append_log() early-returns.
bool apply_lightmap_precision_fix()
{
    g_exe_base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (g_exe_base == 0)
        return true;

    // Only stock MCC tool_fast has this edge-builder prologue (sub_49C09C). On any other binary, skip
    // cleanly rather than reporting a hook failure to the launcher.
    static const std::uint8_t kEdgeSig[] = {0x83, 0xEC, 0x30, 0x53, 0x8B, 0xD9};
    if (std::memcmp(reinterpret_cast<const void*>(g_exe_base + kSurfaceEdgeBuilderRva),
                    kEdgeSig, sizeof(kEdgeSig)) != 0)
    {
        OutputDebugStringA("[LM PRECISION R7] edge-builder signature not found; skipping (safe no-op)\n");
        return true;
    }

    // Order matches R7's DllMain: producers, then trace/leaf pair (widened FP64 packet), then the root
    // edge-coefficient producer. Each install function signature-verifies its own target before writing.
    const bool producer_hooks_installed = install_input_producer_hooks();
    const bool trace_hooks_installed = install_fp64_trace_and_leaf_hooks();
    const bool surface_edge_hook_installed = install_surface_edge_builder_hook();
    g_hook_installed = producer_hooks_installed && trace_hooks_installed && surface_edge_hook_installed;

    char msg[192];
    _snprintf_s(msg, sizeof(msg), _TRUNCATE,
        "[LM PRECISION R7] installed=%s (producers=%s trace/leaf=%s edge=%s) targets "
        "edge=0x%08lX interp=0x%08lX finalbias=0x%08lX clvbias=0x%08lX trace=0x%08lX leaf=0x%08lX\n",
        g_hook_installed ? "true" : "false",
        producer_hooks_installed ? "true" : "false",
        trace_hooks_installed ? "true" : "false",
        surface_edge_hook_installed ? "true" : "false",
        static_cast<DWORD>(g_exe_base + kSurfaceEdgeBuilderRva),
        static_cast<DWORD>(g_exe_base + kInterpolateSurfaceRva),
        static_cast<DWORD>(g_exe_base + kFinalizeGatherBiasRva),
        static_cast<DWORD>(g_exe_base + kComputeLightBiasRva),
        static_cast<DWORD>(g_exe_base + kTraceRva),
        static_cast<DWORD>(g_exe_base + kStaticSurfaceRva));
    OutputDebugStringA(msg);

    return g_hook_installed;
}
