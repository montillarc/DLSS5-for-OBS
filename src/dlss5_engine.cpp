#include "dlss5_engine.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_helpers.h"
#include "nvofa/nvOpticalFlowD3D12.h"

// ---------------------------------------------------------------------------
// Logging & Configuration
// ---------------------------------------------------------------------------

static wchar_t    g_runtime_dir[MAX_PATH] = {};
static Dlss5LogFn g_log_fn                = nullptr;
static std::mutex g_global_mutex;

enum LogLevel { LOG_ERR = 100, LOG_WARN = 200, LOG_INFO = 300 };

static void EngineLog(int level, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (g_log_fn)
    {
        g_log_fn(level, buf);
    }
    else
    {
        fprintf(stderr, "[DLSS5-OBS] %s\n", buf);
        fflush(stderr);
    }
}

void Dlss5_SetConfig(const wchar_t *runtime_dir, Dlss5LogFn log_fn)
{
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (log_fn) g_log_fn = log_fn;
    if (runtime_dir && runtime_dir[0] != L'\0')
    {
        wcsncpy_s(g_runtime_dir, runtime_dir, _TRUNCATE);
        size_t len = wcslen(g_runtime_dir);
        if (len > 0 && g_runtime_dir[len - 1] != L'\\' && g_runtime_dir[len - 1] != L'/')
            wcscat_s(g_runtime_dir, L"\\");
    }
}

static const char *NgxResultName(NVSDK_NGX_Result r)
{
    switch (static_cast<unsigned>(r))
    {
    case 0x1:        return "Success";
    case 0xBAD00001: return "FeatureNotSupported";
    case 0xBAD00002: return "PlatformError (caller module path check)";
    case 0xBAD00003: return "FeatureAlreadyExists";
    case 0xBAD00004: return "FeatureNotFound";
    case 0xBAD00005: return "InvalidParameter";
    case 0xBAD00007: return "NotInitialized";
    case 0xBAD00008: return "UnsupportedInputFormat";
    case 0xBAD0000A: return "MissingInput";
    case 0xBAD0000B: return "UnableToInitializeFeature";
    case 0xBAD0000D: return "OutOfGPUMemory";
    case 0xBAD0000E: return "UnsupportedFormat";
    default:         return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// GPU Architecture Spoof for RTX 20/30/40 (skipped automatically on RTX 50)
// ---------------------------------------------------------------------------

static constexpr unsigned NVAPI_ID_INITIALIZE = 0x0150E828u;
static constexpr unsigned NVAPI_ID_ENUM_GPUS  = 0xE5AC921Fu;
static constexpr unsigned NVAPI_ID_GET_ARCH   = 0xD8265D24u;

static constexpr unsigned NV_ARCH_TURING    = 0x160u;
static constexpr unsigned NV_ARCH_AMPERE    = 0x170u;
static constexpr unsigned NV_ARCH_ADA       = 0x190u;
static constexpr unsigned NV_ARCH_BLACKWELL = 0x1B0u;

struct NvArchInfo
{
    unsigned version, architecture, implementation, revision;
};

using PFN_NvQueryInterface = void *(__cdecl *)(unsigned);
using PFN_NvGetArchInfo    = int(__cdecl *)(void *, NvArchInfo *);

static const int  kArchMaxGpus = 64;
static void      *g_arch_handles[kArchMaxGpus];
static NvArchInfo g_arch_cache[kArchMaxGpus];
static int        g_arch_count = 0;
static void      *g_arch_target = nullptr;
static bool       g_arch_patched = false;

static int __cdecl ArchInfoHook(void *gpu, NvArchInfo *info)
{
    if (info == nullptr) return -1;
    const unsigned want = info->version;
    int idx = -1;
    for (int i = 0; i < g_arch_count; ++i)
        if (g_arch_handles[i] == gpu) { idx = i; break; }
    if (idx < 0) idx = 0;
    *info = g_arch_cache[idx];
    info->version = want;
    const unsigned group = info->architecture & 0xFFFFFFF0u;
    if (idx == 0 && (group == NV_ARCH_TURING || group == NV_ARCH_AMPERE || group == NV_ARCH_ADA))
    {
        info->architecture   = NV_ARCH_BLACKWELL;
        info->implementation = 0x3u;
        info->revision       = 0xA1u;
    }
    return 0;
}

static void SetupArchSpoofIfNeeded()
{
    if (g_arch_patched) return;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (!nvapi) return;
    auto qi = reinterpret_cast<PFN_NvQueryInterface>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (!qi) return;
    auto init      = reinterpret_cast<int(__cdecl *)()>(qi(NVAPI_ID_INITIALIZE));
    auto enum_gpus = reinterpret_cast<int(__cdecl *)(void **, unsigned *)>(qi(NVAPI_ID_ENUM_GPUS));
    auto get_arch  = reinterpret_cast<PFN_NvGetArchInfo>(qi(NVAPI_ID_GET_ARCH));
    if (!init || !enum_gpus || !get_arch || init() != 0) return;

    void *handles[kArchMaxGpus] = {};
    unsigned count = 0;
    if (enum_gpus(handles, &count) != 0 || count == 0) return;
    if (count > (unsigned)kArchMaxGpus) count = kArchMaxGpus;

    g_arch_count = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        NvArchInfo one = {};
        one.version = (unsigned)sizeof(NvArchInfo) | (2u << 16);
        if (get_arch(handles[i], &one) != 0)
        {
            one.version = (unsigned)sizeof(NvArchInfo) | (1u << 16);
            if (get_arch(handles[i], &one) != 0) continue;
        }
        g_arch_handles[g_arch_count] = handles[i];
        g_arch_cache[g_arch_count]   = one;
        ++g_arch_count;
    }
    if (g_arch_count == 0) return;

    const unsigned real_group = g_arch_cache[0].architecture & 0xFFFFFFF0u;
    if (real_group >= NV_ARCH_BLACKWELL)
    {
        EngineLog(LOG_INFO, "GPU architecture 0x%X (Blackwell+) natively supported; no spoof needed.",
                  g_arch_cache[0].architecture);
        return;
    }

    g_arch_target = reinterpret_cast<void *>(get_arch);
    BYTE code[12] = { 0x48, 0xB8 };
    void *dst = reinterpret_cast<void *>(&ArchInfoHook);
    memcpy(code + 2, &dst, sizeof(dst));
    code[10] = 0xFF; code[11] = 0xE0;

    DWORD old_prot = 0;
    if (VirtualProtect(g_arch_target, sizeof(code), PAGE_EXECUTE_READWRITE, &old_prot))
    {
        memcpy(g_arch_target, code, sizeof(code));
        DWORD tmp = 0;
        VirtualProtect(g_arch_target, sizeof(code), old_prot, &tmp);
        FlushInstructionCache(GetCurrentProcess(), g_arch_target, sizeof(code));
        g_arch_patched = true;
        EngineLog(LOG_INFO, "Installed architecture spoof for GPU arch 0x%X -> 0x%X",
                  g_arch_cache[0].architecture, NV_ARCH_BLACKWELL);
    }
}

// ---------------------------------------------------------------------------
// HLSL Compute Shaders (Compiled once in Global Context)
// ---------------------------------------------------------------------------

// 1. Area-averaging RGBA8 -> R8 Luminance (for NVOFA Optical Flow)
static const char kGrayHlsl[] = R"hlsl(
Texture2D<float4>  gSrc : register(t0);
RWTexture2D<float> gDst : register(u0);
cbuffer Sizes : register(b0) { uint gSrcW; uint gSrcH; uint gDstW; uint gDstH; };
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstW || id.y >= gDstH) return;
    float2 extent = float2(gSrcW, gSrcH) / float2(gDstW, gDstH);
    float2 lo = float2(id.xy) * extent;
    float2 hi = min(lo + extent, float2(gSrcW, gSrcH));
    uint x0 = (uint)floor(lo.x), y0 = (uint)floor(lo.y);
    uint x1 = min((uint)ceil(hi.x), gSrcW), y1 = min((uint)ceil(hi.y), gSrcH);
    float sum = 0.0f;
    for (uint y = y0; y < y1; ++y)
        for (uint x = x0; x < x1; ++x)
        {
            float4 c = gSrc.Load(int3(x, y, 0));
            float weight = max(0.0f, min(hi.x, x + 1.0f) - max(lo.x, (float)x))
                         * max(0.0f, min(hi.y, y + 1.0f) - max(lo.y, (float)y));
            sum += weight * dot(c.rgb, float3(0.299f, 0.587f, 0.114f));
        }
    gDst[id.xy] = sum / max((hi.x - lo.x) * (hi.y - lo.y), 1e-6f);
}
)hlsl";

// 2. NVOFA S10.5 Flow Grid -> Work-Res RG16F Motion Vectors
static const char kNvofaExpandHlsl[] = R"hlsl(
Texture2D<int2>     flow   : register(t0);
RWTexture2D<float2> motion : register(u0);
cbuffer Params : register(b0) { uint w, h, iw, ih, grid, reset; };
[numthreads(8, 8, 1)]
void CSMain(uint3 p : SV_DispatchThreadID)
{
    if (p.x >= w || p.y >= h) return;
    uint fw, fh;
    flow.GetDimensions(fw, fh);
    float2 q = (float2(p.xy) + 0.5f) * float2(iw, ih) / float2(w, h) / float(grid) - 0.5f;
    int2 a = int2(floor(q));
    float2 t = frac(q);
    int2 limit = int2(int(fw) - 1, int(fh) - 1);
    float2 v = lerp(
        lerp(float2(flow.Load(int3(clamp(a, int2(0,0), limit), 0))),
             float2(flow.Load(int3(clamp(a + int2(1,0), int2(0,0), limit), 0))), t.x),
        lerp(float2(flow.Load(int3(clamp(a + int2(0,1), int2(0,0), limit), 0))),
             float2(flow.Load(int3(clamp(a + int2(1,1), int2(0,0), limit), 0))), t.x),
        t.y);
    v = (v / 32.0f) * float2(w, h) / float2(iw, ih);
    motion[p.xy] = (reset != 0 || !all(isfinite(v))) ? float2(0.0f, 0.0f) : v;
}
)hlsl";

// 3. Area Downscale / Bilinear Upscale RGBA8
static const char kScale4Hlsl[] = R"hlsl(
Texture2D<float4>   gSrc : register(t0);
RWTexture2D<float4> gDst : register(u0);
cbuffer Sizes : register(b0) { uint gDstW; uint gDstH; uint gSrcW; uint gSrcH; };
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gDstW || id.y >= gDstH) return;
    int2 hi = int2(int(gSrcW) - 1, int(gSrcH) - 1);
    if (gSrcW > gDstW || gSrcH > gDstH)
    {
        float2 lo = float2(id.xy) * float2(gSrcW, gSrcH) / float2(gDstW, gDstH);
        float2 end = float2(id.xy + 1) * float2(gSrcW, gSrcH) / float2(gDstW, gDstH);
        float4 sum = 0.0f;
        [loop] for (int y = int(floor(lo.y)); y < int(ceil(end.y)); ++y)
        {
            float wy = max(0.0f, min(end.y, float(y + 1)) - max(lo.y, float(y)));
            [loop] for (int x = int(floor(lo.x)); x < int(ceil(end.x)); ++x)
            {
                float wx = max(0.0f, min(end.x, float(x + 1)) - max(lo.x, float(x)));
                sum += gSrc[clamp(int2(x, y), int2(0, 0), hi)] * wx * wy;
            }
        }
        gDst[id.xy] = sum / ((end.x - lo.x) * (end.y - lo.y));
        return;
    }
    float2 p = (float2(id.xy) + 0.5f) * float2(gSrcW, gSrcH) / float2(gDstW, gDstH) - 0.5f;
    int2 a = int2(floor(p));
    float2 f = frac(p);
    float4 tl = gSrc[clamp(a, int2(0, 0), hi)];
    float4 tr = gSrc[clamp(a + int2(1, 0), int2(0, 0), hi)];
    float4 bl = gSrc[clamp(a + int2(0, 1), int2(0, 0), hi)];
    float4 br = gSrc[clamp(a + int2(1, 1), int2(0, 0), hi)];
    gDst[id.xy] = lerp(lerp(tl, tr, f.x), lerp(bl, br, f.x), f.y);
}
)hlsl";

// 4. Halo Reduction (Cross-Bilateral Blur + Motion-Border Fix)
static const char kHaloBlurHlsl[] = R"hlsl(
Texture2D<float4>   gGuide : register(t0);
Texture2D<float4>   gA     : register(t1);
Texture2D<float4>   gB     : register(t2);
RWTexture2D<float4> gDst   : register(u0);
cbuffer HC : register(b0) { uint gW; uint gH; int gDirX; int gDirY; int gRadius; int gStep; uint gMode; float gSigmaR; uint gMvW; uint gMvH; };
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy);
    int2 hi = int2(int(gW) - 1, int(gH) - 1);
    int2 dir = int2(gDirX, gDirY);
    float lc = dot(gGuide[p].rgb, kLuma);
    float ss = max(1.0f, float(gRadius) * 0.5f);
    float inv_ss = 1.0f / (2.0f * ss * ss);
    float inv_sr = 1.0f / (2.0f * gSigmaR * gSigmaR);
    float4 acc = 0.0f;
    float wsum = 0.0f;
    [loop] for (int k = -gRadius; k <= gRadius; k += gStep)
    {
        int2 q = clamp(p + dir * k, int2(0, 0), hi);
        float dl = dot(gGuide[q].rgb, kLuma) - lc;
        float w = exp(-float(k * k) * inv_ss - dl * dl * inv_sr);
        float4 s = gMode == 0 ? float4(gA[q].rgb - gB[q].rgb, 0.0f) : gA[q];
        acc += s * w;
        wsum += w;
    }
    gDst[p] = acc / max(wsum, 1e-6f);
}
)hlsl";

static const char kHaloFixHlsl[] = R"hlsl(
Texture2D<float4>   gIn  : register(t0);
Texture2D<float4>   gOut : register(t1);
Texture2D<float4>   gBil : register(t2);
Texture2D<float2>   gMv  : register(t3);
RWTexture2D<float4> gDst : register(u0);
cbuffer HC : register(b0) { uint gW; uint gH; int gDirX; int gDirY; int gRadius; int gStep; uint gMode; float gHalo; uint gMvW; uint gMvH; };
float2 MotionAt(int2 q, int2 mhi)
{
    float2 m = gMv[clamp(q, int2(0, 0), mhi)];
    return all(isfinite(m)) ? m : float2(0.0f, 0.0f);
}
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy);
    int2 hi = int2(int(gW) - 1, int(gH) - 1);
    float3 e = 0.0f;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        int2 q = clamp(p + int2(x, y), int2(0, 0), hi);
        e += (gOut[q].rgb - gIn[q].rgb) - gBil[q].rgb;
    }
    float3 d = gOut[p].rgb - gIn[p].rgb;
    float gain = gHalo;
    if (gMvW > 0 && gMvH > 0)
    {
        float2 toMv = float2(gMvW, gMvH) / float2(gW, gH);
        int2 mhi = int2(int(gMvW) - 1, int(gMvH) - 1);
        int2 q = int2((float2(p) + 0.5f) * toMv);
        float2 m0 = MotionAt(q, mhi);
        float spread = 0.0f;
        [unroll] for (int ring = 0; ring < 2; ++ring)
        {
            float2 r = (ring == 0 ? 5.0f : 12.0f) * toMv;
            int2 rx = int2(max(1.0f, round(r.x)), 0);
            int2 ry = int2(0, max(1.0f, round(r.y)));
            spread = max(spread, length(MotionAt(q + rx, mhi) - m0));
            spread = max(spread, length(MotionAt(q - rx, mhi) - m0));
            spread = max(spread, length(MotionAt(q + ry, mhi) - m0));
            spread = max(spread, length(MotionAt(q - ry, mhi) - m0));
        }
        float border = saturate((spread / max(toMv.x, 1e-4f) - 0.75f) / 3.25f);
        gain = gHalo + (1.0f - gHalo) * 0.6f * border;
    }
    gDst[p] = float4(d - gain * (e / 9.0f), 0.0f);
}
)hlsl";

// 5. Residual Composite (native + (nr_out - nr_in) * strength, preserving native alpha)
static const char kResidualHlsl[] = R"hlsl(
Texture2D<float4>   gNative : register(t0);
Texture2D<float4>   gNrIn   : register(t1);
Texture2D<float4>   gNrOut  : register(t2);
Texture2D<float4>   gDelta  : register(t3);
RWTexture2D<float4> gDst    : register(u0);
cbuffer RC : register(b0) { uint gW; uint gH; float gStrength; float gHalo; };
SamplerState gSamp : register(s0);
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    float2 uv = (float2(id.xy) + 0.5f) / float2(gW, gH);
    float4 nat = gNative[id.xy];
    float3 native = nat.rgb;
    float3 delta;
    if (gHalo <= 0.0f)
    {
        float3 nrIn  = gNrIn.SampleLevel(gSamp, uv, 0).rgb;
        float3 nrOut = gNrOut.SampleLevel(gSamp, uv, 0).rgb;
        delta = nrOut - nrIn;
    }
    else
    {
        uint sw, sh;
        gNrIn.GetDimensions(sw, sh);
        float2 p = uv * float2(sw, sh) - 0.5f;
        float2 b = floor(p);
        float2 f = p - b;
        int2 a = int2(b);
        int2 hi = int2(int(sw) - 1, int(sh) - 1);
        float nl = dot(native, kLuma);
        float3 num = 0.0f;
        float den = 0.0f;
        [unroll] for (int j = 0; j < 2; ++j)
        [unroll] for (int i = 0; i < 2; ++i)
        {
            int2 q = clamp(a + int2(i, j), int2(0, 0), hi);
            float wb = (i == 0 ? 1.0f - f.x : f.x) * (j == 0 ? 1.0f - f.y : f.y);
            float dl = dot(gNrIn[q].rgb, kLuma) - nl;
            float wr = exp(-dl * dl * 50.0f);
            float w = wb * (1.0f - gHalo + gHalo * wr) + 1e-5f;
            num += gDelta[q].rgb * w;
            den += w;
        }
        delta = num / den;
    }
    float3 result = saturate(native + delta * gStrength);
    gDst[id.xy] = float4(result, nat.a);
}
)hlsl";

// 6. Post-Finish (Temporal stability + Edge Smooth + CAS Sharpness + Color Grading)
static const char kFinishHlsl[] = R"hlsl(
Texture2D<float4>   gSrc  : register(t0);
Texture2D<float2>   gMv   : register(t1);
Texture2D<float4>   gHist : register(t2);
RWTexture2D<float4> gDst  : register(u0);
RWTexture2D<float4> gNext : register(u1);
SamplerState        gLin  : register(s0);
cbuffer PC : register(b0)
{
    uint gW; uint gH; float gSharp; float gEdge;
    uint gMvW; uint gMvH; uint gFlags; float gBright;
    float gContrast; float gSat; float gWarm; float gPad;
};
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);
static const float3 kLuma709 = float3(0.2126f, 0.7152f, 0.0722f);
float L(float3 c) { return dot(c, kLuma); }

float3 Grade(float3 c)
{
    c = saturate(c);
    if (gBright != 0.0f)
        c = pow(max(c, 1e-5f), exp2(-gBright * 0.7f));
    if (gContrast > 0.0f)
        c = lerp(c, c * c * (3.0f - 2.0f * c), gContrast * 0.85f);
    else if (gContrast < 0.0f)
        c = lerp(c, 0.5f + (c - 0.5f) * 0.5f, -gContrast);
    if (gSat != 0.0f)
    {
        float l = dot(c, kLuma709);
        float chroma = max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b));
        float gain = gSat > 0.0f ? 1.0f + gSat * (1.0f - 0.6f * saturate(chroma)) : 1.0f + gSat;
        c = l + (c - l) * gain;
    }
    if (gWarm != 0.0f)
    {
        float l0 = dot(c, kLuma709);
        float3 t = c * float3(1.0f + 0.10f * gWarm, 1.0f + 0.015f * gWarm, 1.0f - 0.10f * gWarm);
        float l1 = dot(t, kLuma709);
        c = l1 > 1e-4f ? t * (l0 / l1) : t;
    }
    return saturate(c);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy);
    int2 hi = int2(int(gW) - 1, int(gH) - 1);
    float2 size = float2(gW, gH);
    float4 srcPx = gSrc[p];
    float3 c  = srcPx.rgb;
    float3 n  = gSrc[clamp(p + int2(0, -1), int2(0, 0), hi)].rgb;
    float3 s  = gSrc[clamp(p + int2(0, 1), int2(0, 0), hi)].rgb;
    float3 e  = gSrc[clamp(p + int2(1, 0), int2(0, 0), hi)].rgb;
    float3 w  = gSrc[clamp(p + int2(-1, 0), int2(0, 0), hi)].rgb;
    float3 mn = min(c, min(min(n, s), min(e, w)));
    float3 mx = max(c, max(max(n, s), max(e, w)));

    float2 mv = 0.0f;
    if (gMvW > 0 && gMvH > 0)
    {
        int2 q = clamp(int2((float2(p) + 0.5f) * float2(gMvW, gMvH) / size),
                       int2(0, 0), int2(int(gMvW) - 1, int(gMvH) - 1));
        mv = gMv[q] * size / float2(gMvW, gMvH);
        if (!all(isfinite(mv))) mv = 0.0f;
    }
    float speed = length(mv);

    float3 center = c;
    float temporal = 0.30f * gEdge + 0.15f * gSharp;
    if ((gFlags & 1u) != 0u && temporal > 0.0f)
    {
        float2 prev = float2(p) + 0.5f + mv;
        if (all(prev >= 0.0f) && all(prev <= size))
        {
            float3 h = gHist.SampleLevel(gLin, prev / size, 0).rgb;
            float3 held = clamp(h, mn, mx);
            float trust = saturate(1.0f - length(h - held) * 12.0f) *
                          (1.0f - saturate((speed - 6.0f) / 26.0f));
            center = lerp(c, held, temporal * trust);
        }
    }

    float3 aa = center;
    float edgeAmount = 0.0f;
    if (gEdge > 0.0f)
    {
        float lnw = L(gSrc[clamp(p + int2(-1, -1), int2(0, 0), hi)].rgb);
        float lne = L(gSrc[clamp(p + int2(1, -1), int2(0, 0), hi)].rgb);
        float lsw = L(gSrc[clamp(p + int2(-1, 1), int2(0, 0), hi)].rgb);
        float lse = L(gSrc[clamp(p + int2(1, 1), int2(0, 0), hi)].rgb);
        float ln = L(n), ls = L(s), le = L(e), lw = L(w), lc = L(c);
        float gx = (lne + 2.0f * le + lse) - (lnw + 2.0f * lw + lsw);
        float gy = (lsw + 2.0f * ls + lse) - (lnw + 2.0f * ln + lne);
        float grad = sqrt(gx * gx + gy * gy);
        float range = max(max(max(ln, ls), max(le, lw)), lc) - min(min(min(ln, ls), min(le, lw)), lc);
        if (grad > 0.06f && range > 0.04f)
        {
            float2 tangent = float2(-gy, gx) / grad;
            float2 texel = 1.0f / size;
            float2 uv = (float2(p) + 0.5f) * texel;
            float3 a = gSrc.SampleLevel(gLin, uv + tangent * texel * 0.75f, 0).rgb;
            float3 b = gSrc.SampleLevel(gLin, uv - tangent * texel * 0.75f, 0).rgb;
            float3 a2 = gSrc.SampleLevel(gLin, uv + tangent * texel * 1.75f, 0).rgb;
            float3 b2 = gSrc.SampleLevel(gLin, uv - tangent * texel * 1.75f, 0).rgb;
            float3 along = (center * 2.0f + a * 2.0f + b * 2.0f + a2 + b2) / 8.0f;
            edgeAmount = gEdge * saturate((grad - 0.06f) * 6.0f);
            aa = lerp(center, along, edgeAmount);
        }
    }

    gNext[p] = float4(aa, srcPx.a);

    float3 result = aa;
    if (gSharp > 0.0f)
    {
        float3 mnc = min(mn, aa);
        float3 mxc = max(mx, aa);
        float3 amp = sqrt(saturate(min(mnc, 1.0f - mxc) / max(mxc, 1e-4f)));
        float calm = (1.0f - 0.65f * edgeAmount) * (1.0f - 0.5f * saturate((speed - 4.0f) / 20.0f));
        float3 wgt = -amp * lerp(0.125f, 0.20f, gSharp) * gSharp * calm;
        result = saturate((aa + (n + s + e + w) * wgt) / (1.0f + 4.0f * wgt));
    }

    if ((gFlags & 2u) != 0u)
        result = Grade(result);

    gDst[p] = float4(result, srcPx.a);
}
)hlsl";

// 7. Split-Screen Wipe (Left = Original, Right = DLSS 5, 2px Accent Line)
static const char kSplitHlsl[] = R"hlsl(
Texture2D<float4>   gOrig : register(t0);
RWTexture2D<float4> gOut  : register(u0);
cbuffer SC : register(b0) { uint gW; uint gH; uint gSplitX; uint gPad; };
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gW || id.y >= gH) return;
    if (id.x < gSplitX)
        gOut[id.xy] = gOrig[id.xy];
    else if (id.x >= gSplitX && id.x < gSplitX + 2u)
        gOut[id.xy] = float4(0.46f, 0.72f, 0.0f, 1.0f); // NVIDIA Green divider
}
)hlsl";

// ---------------------------------------------------------------------------
// Global D3D12 + NGX + NVOFA API Context
// ---------------------------------------------------------------------------

using PFN_NR_InitExt  = NVSDK_NGX_Result(NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);
using PFN_NR_Create   = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using PFN_NR_Evaluate = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
using PFN_NR_Release  = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);

struct GlobalContext
{
    bool                       inited = false;
    bool                       failed = false;
    std::string                init_error;

    LUID                       adapter_luid = {};
    ID3D12Device              *dev12        = nullptr;
    ID3D12CommandQueue        *queue12      = nullptr;

    HMODULE                    fwd_mod      = nullptr;
    PFN_NR_InitExt             nr_init_ext  = nullptr;
    PFN_NR_Create              nr_create    = nullptr;
    PFN_NR_Evaluate            nr_evaluate  = nullptr;
    PFN_NR_Release             nr_release   = nullptr;
    bool                       ngx_inited   = false;

    HMODULE                    nvofa_mod    = nullptr;
    NV_OF_D3D12_API_FUNCTION_LIST nvofa_api = {};
    bool                       nvofa_avail  = false;

    // Compute Root Signatures & PSOs
    ID3D12RootSignature       *rs_1srv_1uav_4c  = nullptr; // Gray, Scale4, Split
    ID3D12RootSignature       *rs_nvofa_expand  = nullptr; // 6 constants + 1 SRV + 1 UAV
    ID3D12RootSignature       *rs_halo          = nullptr; // 10 constants + 4 SRV + 1 UAV
    ID3D12RootSignature       *rs_residual      = nullptr; // 4 constants + 4 SRV + 1 UAV + sampler
    ID3D12RootSignature       *rs_finish        = nullptr; // 12 constants + 3 SRV + 2 UAV + sampler

    ID3D12PipelineState       *pso_gray         = nullptr;
    ID3D12PipelineState       *pso_nvofa_expand = nullptr;
    ID3D12PipelineState       *pso_scale4       = nullptr;
    ID3D12PipelineState       *pso_halo_blur    = nullptr;
    ID3D12PipelineState       *pso_halo_fix     = nullptr;
    ID3D12PipelineState       *pso_residual     = nullptr;
    ID3D12PipelineState       *pso_finish       = nullptr;
    ID3D12PipelineState       *pso_split        = nullptr;
};

static GlobalContext g_ctx;
static NVSDK_NGX_FeatureCommonInfo g_ngx_common = {};
static void NVSDK_CONV NgxDiscardCallback(const char *, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {}

static D3D12_RESOURCE_BARRIER MakeBarrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

static ID3D12Resource *CreateTex2D(ID3D12Device *dev, UINT w, UINT h, DXGI_FORMAT fmt, bool allow_uav,
                                   D3D12_RESOURCE_STATES init_state = D3D12_RESOURCE_STATE_COMMON)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width            = w;
    rd.Height           = h;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.Format           = fmt;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags            = allow_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *tex = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, init_state, nullptr, IID_PPV_ARGS(&tex));
    return tex;
}

static bool CompileComputeShader(const char *src, size_t len, const char *name,
                                 ID3D12RootSignature *rs, ID3D12PipelineState **out_pso)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src, len, name, nullptr, nullptr, "CSMain", "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr) || !code)
    {
        EngineLog(LOG_ERR, "Shader %s compile failed 0x%08X: %s",
                  name, (unsigned)hr, err ? (const char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs;
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength  = code->GetBufferSize();
    hr = g_ctx.dev12->CreateComputePipelineState(&pd, IID_PPV_ARGS(out_pso));
    code->Release();
    if (FAILED(hr))
    {
        EngineLog(LOG_ERR, "CreateComputePipelineState(%s) failed 0x%08X", name, (unsigned)hr);
        return false;
    }
    return true;
}

static bool BuildRootSig(UINT num_consts, UINT num_srvs, UINT num_uavs, bool with_linear_sampler,
                         ID3D12RootSignature **out_rs)
{
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors                    = num_srvs;
    ranges[0].BaseShaderRegister                = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;

    ranges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors                    = num_uavs;
    ranges[1].BaseShaderRegister                = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = num_srvs;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType            = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = num_consts;
    params[0].ShaderVisibility         = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges   = ranges;
    params[1].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD           = D3D12_FLOAT32_MAX;
    samp.ShaderRegister   = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters     = 2;
    rsd.pParameters       = params;
    rsd.NumStaticSamplers = with_linear_sampler ? 1u : 0u;
    rsd.pStaticSamplers   = with_linear_sampler ? &samp : nullptr;

    ID3DBlob *sig = nullptr, *err = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
    if (err) err->Release();
    if (FAILED(hr) || !sig) return false;
    hr = g_ctx.dev12->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(out_rs));
    sig->Release();
    return SUCCEEDED(hr);
}

static bool InitGlobalContext(ID3D11Device *dev11)
{
    if (g_ctx.inited) return true;
    if (g_ctx.failed) return false;

    // Determine runtime directory if not explicitly configured
    if (g_runtime_dir[0] == L'\0')
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&InitGlobalContext), &self);
        GetModuleFileNameW(self, g_runtime_dir, MAX_PATH);
        if (wchar_t *s = wcsrchr(g_runtime_dir, L'\\')) *(s + 1) = L'\0';
    }

    // Find matching adapter LUID from OBS D3D11 device (or first NVIDIA RTX adapter)
    LUID want_luid = {};
    bool have_want = false;
    if (dev11)
    {
        IDXGIDevice *dxgi_dev = nullptr;
        IDXGIAdapter *adap    = nullptr;
        DXGI_ADAPTER_DESC ad  = {};
        if (SUCCEEDED(dev11->QueryInterface(IID_PPV_ARGS(&dxgi_dev))) &&
            SUCCEEDED(dxgi_dev->GetAdapter(&adap)) &&
            SUCCEEDED(adap->GetDesc(&ad)))
        {
            want_luid = ad.AdapterLuid;
            have_want = true;
            EngineLog(LOG_INFO, "OBS D3D11 is running on GPU: %ls (Vendor 0x%04X)", ad.Description, ad.VendorId);
        }
        if (adap) adap->Release();
        if (dxgi_dev) dxgi_dev->Release();
    }

    IDXGIFactory2 *factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        g_ctx.init_error = "CreateDXGIFactory1 failed";
        g_ctx.failed = true;
        return false;
    }

    IDXGIAdapter1 *chosen = nullptr;
    for (UINT i = 0;; ++i)
    {
        IDXGIAdapter1 *cand = nullptr;
        if (factory->EnumAdapters1(i, &cand) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        cand->GetDesc1(&desc);
        const bool is_nvidia = (desc.VendorId == 0x10DE) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        if (have_want &&
            desc.AdapterLuid.LowPart == want_luid.LowPart &&
            desc.AdapterLuid.HighPart == want_luid.HighPart &&
            is_nvidia)
        {
            chosen = cand;
            break;
        }
        if (!chosen && is_nvidia)
        {
            chosen = cand;
            continue;
        }
        cand->Release();
    }
    factory->Release();

    if (!chosen)
    {
        g_ctx.init_error = "No compatible NVIDIA RTX GPU found";
        EngineLog(LOG_ERR, "%s", g_ctx.init_error.c_str());
        g_ctx.failed = true;
        return false;
    }

    DXGI_ADAPTER_DESC1 chosen_desc = {};
    chosen->GetDesc1(&chosen_desc);
    g_ctx.adapter_luid = chosen_desc.AdapterLuid;
    EngineLog(LOG_INFO, "Selected D3D12 Adapter: %ls (VRAM %llu MB)",
              chosen_desc.Description, (unsigned long long)(chosen_desc.DedicatedVideoMemory >> 20));

    HRESULT hr = D3D12CreateDevice(chosen, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_ctx.dev12));
    chosen->Release();
    if (FAILED(hr) || !g_ctx.dev12)
    {
        g_ctx.init_error = "D3D12CreateDevice failed";
        EngineLog(LOG_ERR, "D3D12CreateDevice failed: 0x%08X", (unsigned)hr);
        g_ctx.failed = true;
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = g_ctx.dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_ctx.queue12));
    if (FAILED(hr) || !g_ctx.queue12)
    {
        g_ctx.init_error = "CreateCommandQueue failed";
        g_ctx.failed = true;
        return false;
    }

    // Compile all compute shaders & root signatures
    if (!BuildRootSig(4, 1, 1, false, &g_ctx.rs_1srv_1uav_4c) ||
        !BuildRootSig(6, 1, 1, false, &g_ctx.rs_nvofa_expand) ||
        !BuildRootSig(10, 4, 1, false, &g_ctx.rs_halo) ||
        !BuildRootSig(4, 4, 1, true, &g_ctx.rs_residual) ||
        !BuildRootSig(12, 3, 2, true, &g_ctx.rs_finish) ||
        !CompileComputeShader(kGrayHlsl, sizeof(kGrayHlsl) - 1, "gray", g_ctx.rs_1srv_1uav_4c, &g_ctx.pso_gray) ||
        !CompileComputeShader(kNvofaExpandHlsl, sizeof(kNvofaExpandHlsl) - 1, "nvofa_expand", g_ctx.rs_nvofa_expand, &g_ctx.pso_nvofa_expand) ||
        !CompileComputeShader(kScale4Hlsl, sizeof(kScale4Hlsl) - 1, "scale4", g_ctx.rs_1srv_1uav_4c, &g_ctx.pso_scale4) ||
        !CompileComputeShader(kHaloBlurHlsl, sizeof(kHaloBlurHlsl) - 1, "halo_blur", g_ctx.rs_halo, &g_ctx.pso_halo_blur) ||
        !CompileComputeShader(kHaloFixHlsl, sizeof(kHaloFixHlsl) - 1, "halo_fix", g_ctx.rs_halo, &g_ctx.pso_halo_fix) ||
        !CompileComputeShader(kResidualHlsl, sizeof(kResidualHlsl) - 1, "residual", g_ctx.rs_residual, &g_ctx.pso_residual) ||
        !CompileComputeShader(kFinishHlsl, sizeof(kFinishHlsl) - 1, "finish", g_ctx.rs_finish, &g_ctx.pso_finish) ||
        !CompileComputeShader(kSplitHlsl, sizeof(kSplitHlsl) - 1, "split", g_ctx.rs_1srv_1uav_4c, &g_ctx.pso_split))
    {
        g_ctx.init_error = "Compute shader compilation failed";
        g_ctx.failed = true;
        return false;
    }

    // Initialize NVOFA D3D12 driver API (optional but recommended for motion vectors)
    g_ctx.nvofa_mod = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (g_ctx.nvofa_mod)
    {
        auto create_ofa = reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(
            GetProcAddress(g_ctx.nvofa_mod, "NvOFAPICreateInstanceD3D12"));
        if (create_ofa && create_ofa(NV_OF_API_VERSION, &g_ctx.nvofa_api) == NV_OF_SUCCESS &&
            g_ctx.nvofa_api.nvCreateOpticalFlowD3D12 && g_ctx.nvofa_api.nvOFInit &&
            g_ctx.nvofa_api.nvOFExecuteD3D12 && g_ctx.nvofa_api.nvOFRegisterResourceD3D12)
        {
            g_ctx.nvofa_avail = true;
            EngineLog(LOG_INFO, "NVIDIA Optical Flow (NVOFA D3D12) driver API initialized.");
        }
    }

    // Setup architecture spoof for pre-Blackwell RTX cards before loading nvngx_dlssnr.dll
    SetupArchSpoofIfNeeded();

    // Load forwarder DLL (whose filename contains "nvngx.dll") and nvngx_dlssnr.dll
    wchar_t fwd_path[MAX_PATH] = {};
    wchar_t nr_path[MAX_PATH]  = {};
    swprintf_s(fwd_path, L"%snvngx.dll_ns-forwarder.dll", g_runtime_dir);
    swprintf_s(nr_path, L"%snvngx_dlssnr.dll", g_runtime_dir);

    g_ctx.fwd_mod = LoadLibraryW(fwd_path);
    if (!g_ctx.fwd_mod)
    {
        g_ctx.init_error = "Could not load nvngx.dll_ns-forwarder.dll";
        EngineLog(LOG_ERR, "Failed to load %ls (err=%lu)", fwd_path, GetLastError());
        g_ctx.failed = true;
        return false;
    }

    auto fn_load  = reinterpret_cast<int (*)(const wchar_t *)>(GetProcAddress(g_ctx.fwd_mod, "NsFwdLoad"));
    g_ctx.nr_init_ext = reinterpret_cast<PFN_NR_InitExt>(GetProcAddress(g_ctx.fwd_mod, "NsFwdInitExt"));
    g_ctx.nr_create   = reinterpret_cast<PFN_NR_Create>(GetProcAddress(g_ctx.fwd_mod, "NsFwdCreate"));
    g_ctx.nr_evaluate = reinterpret_cast<PFN_NR_Evaluate>(GetProcAddress(g_ctx.fwd_mod, "NsFwdEvaluate"));
    g_ctx.nr_release  = reinterpret_cast<PFN_NR_Release>(GetProcAddress(g_ctx.fwd_mod, "NsFwdRelease"));

    if (!fn_load || !g_ctx.nr_init_ext || !g_ctx.nr_create || !g_ctx.nr_evaluate || !g_ctx.nr_release)
    {
        g_ctx.init_error = "Forwarder DLL missing exports";
        g_ctx.failed = true;
        return false;
    }

    int load_rc = fn_load(nr_path);
    if (load_rc != 0)
    {
        g_ctx.init_error = "Could not load nvngx_dlssnr.dll";
        EngineLog(LOG_ERR, "NsFwdLoad(%ls) failed with code %d", nr_path, load_rc);
        g_ctx.failed = true;
        return false;
    }

    // Initialize NGX Core + Direct DLSSNR
    g_ngx_common = {};
    g_ngx_common.LoggingInfo.LoggingCallback          = NgxDiscardCallback;
    g_ngx_common.LoggingInfo.MinimumLoggingLevel      = NVSDK_NGX_LOGGING_LEVEL_OFF;
    g_ngx_common.LoggingInfo.DisableOtherLoggingSinks = true;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, g_runtime_dir, g_ctx.dev12, &g_ngx_common, NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04",
                                                NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
                                                g_runtime_dir, g_ctx.dev12, &g_ngx_common, NVSDK_NGX_Version_API);
    }
    if (NVSDK_NGX_FAILED(r))
    {
        g_ctx.init_error = std::string("NVSDK_NGX_D3D12_Init failed: ") + NgxResultName(r);
        EngineLog(LOG_ERR, "%s (0x%08X)", g_ctx.init_error.c_str(), (unsigned)r);
        g_ctx.failed = true;
        return false;
    }
    g_ctx.ngx_inited = true;

    NVSDK_NGX_Parameter *init_params = nullptr;
    r = NVSDK_NGX_D3D12_AllocateParameters(&init_params);
    if (NVSDK_NGX_FAILED(r) || !init_params)
    {
        g_ctx.init_error = "NVSDK_NGX_D3D12_AllocateParameters failed";
        g_ctx.failed = true;
        return false;
    }

    r = g_ctx.nr_init_ext(0x1000000ULL, g_runtime_dir, g_ctx.dev12, NVSDK_NGX_Version_API, init_params);
    NVSDK_NGX_D3D12_DestroyParameters(init_params);
    if (NVSDK_NGX_FAILED(r))
    {
        g_ctx.init_error = std::string("DLSSNR Init_Ext failed: ") + NgxResultName(r);
        EngineLog(LOG_ERR, "%s (0x%08X)", g_ctx.init_error.c_str(), (unsigned)r);
        g_ctx.failed = true;
        return false;
    }

    EngineLog(LOG_INFO, "DLSS 5 Neural Rendering (NVSDK_NGX_Feature_Reserved18) initialized successfully!");
    g_ctx.inited = true;
    return true;
}

// ---------------------------------------------------------------------------
// SEH Guarded NGX Wrappers (no C++ destructors in scope)
// ---------------------------------------------------------------------------

static NVSDK_NGX_Result SafeNrCreate(ID3D12GraphicsCommandList *list, NVSDK_NGX_Parameter *params,
                                     NVSDK_NGX_Handle **out_handle, DWORD *out_seh)
{
    *out_seh = 0;
    __try
    {
        return g_ctx.nr_create(list, NVSDK_NGX_Feature_Reserved18, params, out_handle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out_seh = GetExceptionCode();
        return NVSDK_NGX_Result_Fail;
    }
}

static NVSDK_NGX_Result SafeNrEvaluate(ID3D12GraphicsCommandList *list, NVSDK_NGX_Handle *handle,
                                       NVSDK_NGX_Parameter *params, DWORD *out_seh)
{
    *out_seh = 0;
    __try
    {
        return g_ctx.nr_evaluate(list, handle, params, nullptr);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out_seh = GetExceptionCode();
        return NVSDK_NGX_Result_Fail;
    }
}

static void SafeNrRelease(NVSDK_NGX_Handle *handle)
{
    if (!handle || !g_ctx.nr_release) return;
    __try
    {
        g_ctx.nr_release(handle);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ---------------------------------------------------------------------------
// Per-Filter Instance State
// ---------------------------------------------------------------------------

static constexpr unsigned kMaxPasses = 4;

enum HeapSlots : UINT
{
    SLOT_GRAY          = 0,  // 2 descriptors (t0, u0)
    SLOT_NVOFA_EXPAND  = 2,  // 2 descriptors (t0, u0)
    SLOT_SCALE_DOWN    = 4,  // 2 descriptors (t0, u0)
    SLOT_SCALE_UP      = 6,  // 2 descriptors (t0, u0)
    SLOT_HALO_0        = 8,  // 5 descriptors (t0..t3, u0)
    SLOT_HALO_1        = 13, // 5 descriptors (t0..t3, u0)
    SLOT_HALO_2        = 18, // 5 descriptors (t0..t3, u0)
    SLOT_RESIDUAL      = 23, // 5 descriptors (t0..t3, u0)
    SLOT_FINISH_0      = 28, // 5 descriptors (t0..t2, u0..u1)
    SLOT_FINISH_1      = 33, // 5 descriptors (t0..t2, u0..u1)
    SLOT_SPLIT         = 38, // 2 descriptors (t0, u0)
    HEAP_TOTAL_DESCS   = 40
};

struct Dlss5Instance
{
    std::mutex                 mutex;
    std::string                status_text = "Esperando fotogramas...";

    // D3D12 command submission per instance
    ID3D12CommandAllocator    *alloc       = nullptr;
    ID3D12GraphicsCommandList *list        = nullptr;
    ID3D12Fence               *fence12     = nullptr;
    HANDLE                     fence_event = nullptr;
    UINT64                     fence_val   = 0;

    // Cross-API D3D11 <-> D3D12 shared fence
    ID3D11Device              *bound_dev11 = nullptr;
    ID3D11Device5             *dev11_5     = nullptr;
    ID3D11DeviceContext       *ctx11       = nullptr;
    ID3D11DeviceContext4      *ctx11_4     = nullptr;
    HANDLE                     shared_fence_nt = nullptr;
    ID3D11Fence               *fence11     = nullptr;

    // Dimensions & configuration tracking
    UINT                       full_w      = 0;
    UINT                       full_h      = 0;
    UINT                       work_w      = 0;
    UINT                       work_h      = 0;
    uint32_t                   preset      = 0;
    uint32_t                   passes      = 1;
    bool                       nr_small    = false;

    // Shared D3D11 <-> D3D12 textures (full_w x full_h, RGBA8)
    // Double-buffered output textures so OBS D3D11 never waits on D3D12 evaluation
    ID3D11Texture2D           *in_d11      = nullptr;
    HANDLE                     in_nt       = nullptr;
    ID3D12Resource            *in_d12      = nullptr; // NON_PIXEL_SHADER_RESOURCE at rest

    ID3D11Texture2D           *out_d11[2]  = { nullptr, nullptr };
    HANDLE                     out_nt[2]   = { nullptr, nullptr };
    ID3D12Resource            *out_d12[2]  = { nullptr, nullptr }; // COPY_DEST at rest
    std::atomic<int>           ready_out_slot{0};
    std::atomic<bool>          has_valid_output{false};

    // Asynchronous D3D12 worker thread state
    std::thread                worker_thread;
    std::mutex                 worker_mtx;
    std::condition_variable    worker_cv;
    std::condition_variable    worker_done_cv;
    bool                       worker_stop          = false;
    bool                       worker_has_job       = false;
    bool                       worker_busy          = false;
    UINT64                     job_copy_in_fence    = 0;
    int                        job_write_slot       = 0;
    Dlss5Params                job_params           = {};
    bool                       pending_reset        = false;
    double                     last_gpu_ms          = 0.0;

    // Internal D3D12 textures
    ID3D12Resource            *mv_tex      = nullptr; // work_w x work_h, R16G16_FLOAT (NON_PIXEL_SHADER_RESOURCE at rest)
    ID3D12Resource            *nr_in       = nullptr; // work_w x work_h, RGBA8 (NON_PIXEL_SHADER_RESOURCE at rest)
    ID3D12Resource            *nr_out      = nullptr; // work_w x work_h, RGBA8 (UNORDERED_ACCESS at rest)
    ID3D12Resource            *work_out    = nullptr; // full_w x full_h, RGBA8 (UNORDERED_ACCESS at rest)
    ID3D12Resource            *pass_scratch = nullptr; // work_w x work_h, RGBA8 (UNORDERED_ACCESS at rest)

    // Halo targets (at work_w x work_h)
    ID3D12Resource            *halo_raw    = nullptr; // full_w x full_h, RGBA8 (UNORDERED_ACCESS at rest)
    ID3D12Resource            *halo_th     = nullptr; // work_w x work_h, RGBA16F (UNORDERED_ACCESS at rest)
    ID3D12Resource            *halo_tb     = nullptr; // work_w x work_h, RGBA16F (UNORDERED_ACCESS at rest)
    ID3D12Resource            *halo_dc     = nullptr; // work_w x work_h, RGBA16F (UNORDERED_ACCESS at rest)

    // Post-Finish targets (at full_w x full_h)
    ID3D12Resource            *finish_tmp     = nullptr; // NON_PIXEL_SHADER_RESOURCE at rest
    ID3D12Resource            *finish_hist[2] = { nullptr, nullptr }; // NON_PIXEL_SHADER_RESOURCE at rest
    UINT                       finish_ping    = 0;
    bool                       finish_valid   = false;

    // Per-instance NVOFA session & grayscale buffers
    UINT                       ofa_w          = 0;
    UINT                       ofa_h          = 0;
    UINT                       ofa_grid       = 4;
    ID3D12Resource            *ofa_gray       = nullptr; // ofa_w x ofa_h, R8_UNORM (UNORDERED_ACCESS at rest)
    ID3D12Resource            *ofa_inputs[2]  = { nullptr, nullptr }; // COMMON at rest
    ID3D12Resource            *ofa_flow       = nullptr; // COMMON at rest
    NvOFHandle                 ofa_session    = nullptr;
    NvOFGPUBufferHandle        ofa_reg[3]     = { nullptr, nullptr, nullptr };
    ID3D12Fence               *ofa_fence      = nullptr;
    UINT64                     ofa_fence_val  = 0;
    UINT                       ofa_curr       = 0;
    bool                       ofa_valid      = false;
    bool                       ofa_failed     = false;

    // Per-instance Descriptor Heap
    ID3D12DescriptorHeap      *heap           = nullptr;

    // Per-instance NGX Feature Handles
    NVSDK_NGX_Parameter       *ngx_params     = nullptr;
    NVSDK_NGX_Handle          *ngx_feature    = nullptr;
    NVSDK_NGX_Handle          *ngx_extra[kMaxPasses - 1] = {};
    uint64_t                   frame_count    = 0;
};

static bool WaitFence12(Dlss5Instance *inst, UINT64 val, DWORD timeout_ms = 5000)
{
    if (!inst->fence12) return false;
    if (val == 0) return true;
    if (inst->fence12->GetCompletedValue() == UINT64_MAX) return false;
    if (inst->fence12->GetCompletedValue() >= val) return true;
    ResetEvent(inst->fence_event);
    if (inst->fence12->GetCompletedValue() >= val) return true;
    if (FAILED(inst->fence12->SetEventOnCompletion(val, inst->fence_event))) return false;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    for (;;)
    {
        const ULONGLONG now = GetTickCount64();
        const DWORD left = now >= deadline ? 0 : (DWORD)(deadline - now);
        if (WaitForSingleObject(inst->fence_event, left) != WAIT_OBJECT_0) return false;
        const UINT64 cur = inst->fence12->GetCompletedValue();
        if (cur == UINT64_MAX) return false;
        if (cur >= val) return true;
    }
}

static bool BeginInstCommands(Dlss5Instance *inst)
{
    if (!WaitFence12(inst, inst->fence_val, 5000)) return false;
    if (FAILED(inst->alloc->Reset())) return false;
    return SUCCEEDED(inst->list->Reset(inst->alloc, nullptr));
}

static UINT64 EndInstCommands(Dlss5Instance *inst, bool wait_cpu = true)
{
    if (FAILED(inst->list->Close())) return 0;
    ID3D12CommandList *lists[] = { inst->list };
    g_ctx.queue12->ExecuteCommandLists(1, lists);
    const UINT64 v = ++inst->fence_val;
    if (FAILED(g_ctx.queue12->Signal(inst->fence12, v))) return 0;
    if (wait_cpu && !WaitFence12(inst, v, 10000)) return 0;
    return v;
}

static void WaitForWorkerIdle(Dlss5Instance *inst)
{
    std::unique_lock<std::mutex> wlock(inst->worker_mtx);
    inst->worker_done_cv.wait(wlock, [&]() { return !inst->worker_busy && !inst->worker_has_job; });
}

static void ReleaseInstanceResources(Dlss5Instance *inst)
{
    WaitForWorkerIdle(inst);

    if (inst->fence12 && inst->fence_val > 0)
        WaitFence12(inst, inst->fence_val, 5000);

    // Release NVOFA session & registered buffers
    if (inst->ofa_session && g_ctx.nvofa_avail)
    {
        if (inst->ofa_fence && inst->ofa_fence_val > 0)
        {
            if (inst->ofa_fence->GetCompletedValue() < inst->ofa_fence_val)
            {
                inst->ofa_fence->SetEventOnCompletion(inst->ofa_fence_val, inst->fence_event);
                WaitForSingleObject(inst->fence_event, 2000);
            }
        }
        for (int i = 0; i < 3; ++i)
        {
            if (inst->ofa_reg[i])
            {
                NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 unreg = {};
                unreg.hOFGpuBuffer = inst->ofa_reg[i];
                g_ctx.nvofa_api.nvOFUnregisterResourceD3D12(&unreg);
                inst->ofa_reg[i] = nullptr;
            }
        }
    }
    for (int i = 0; i < 2; ++i)
        if (inst->ofa_inputs[i]) { inst->ofa_inputs[i]->Release(); inst->ofa_inputs[i] = nullptr; }
    if (inst->ofa_flow) { inst->ofa_flow->Release(); inst->ofa_flow = nullptr; }
    if (inst->ofa_gray) { inst->ofa_gray->Release(); inst->ofa_gray = nullptr; }
    if (inst->ofa_session && g_ctx.nvofa_avail)
    {
        g_ctx.nvofa_api.nvOFDestroy(inst->ofa_session);
        inst->ofa_session = nullptr;
    }
    if (inst->ofa_fence) { inst->ofa_fence->Release(); inst->ofa_fence = nullptr; }
    inst->ofa_fence_val = 0;
    inst->ofa_valid     = false;

    // Release NGX features
    for (auto &ef : inst->ngx_extra)
    {
        if (ef) { SafeNrRelease(ef); ef = nullptr; }
    }
    if (inst->ngx_feature)
    {
        SafeNrRelease(inst->ngx_feature);
        inst->ngx_feature = nullptr;
    }

    // Release textures
    auto rel = [](IUnknown *&p) { if (p) { p->Release(); p = nullptr; } };
    rel((IUnknown *&)inst->in_d12);
    rel((IUnknown *&)inst->in_d11);
    if (inst->in_nt) { CloseHandle(inst->in_nt); inst->in_nt = nullptr; }

    for (int i = 0; i < 2; ++i)
    {
        rel((IUnknown *&)inst->out_d12[i]);
        rel((IUnknown *&)inst->out_d11[i]);
        if (inst->out_nt[i]) { CloseHandle(inst->out_nt[i]); inst->out_nt[i] = nullptr; }
    }
    inst->ready_out_slot.store(0);
    inst->has_valid_output.store(false);

    rel((IUnknown *&)inst->mv_tex);
    rel((IUnknown *&)inst->nr_in);
    rel((IUnknown *&)inst->nr_out);
    rel((IUnknown *&)inst->work_out);
    rel((IUnknown *&)inst->pass_scratch);
    rel((IUnknown *&)inst->halo_raw);
    rel((IUnknown *&)inst->halo_th);
    rel((IUnknown *&)inst->halo_tb);
    rel((IUnknown *&)inst->halo_dc);
    rel((IUnknown *&)inst->finish_tmp);
    rel((IUnknown *&)inst->finish_hist[0]);
    rel((IUnknown *&)inst->finish_hist[1]);

    inst->full_w = inst->full_h = 0;
    inst->work_w = inst->work_h = 0;
    inst->finish_valid = false;
    inst->frame_count  = 0;
}

static bool CreateSharedTexPair(ID3D11Device *dev11, UINT w, UINT h,
                                ID3D11Texture2D **out_d11, HANDLE *out_nt, ID3D12Resource **out_d12)
{
    D3D11_TEXTURE2D_DESC td = {};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags        = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

    if (FAILED(dev11->CreateTexture2D(&td, nullptr, out_d11))) return false;

    IDXGIResource1 *res1 = nullptr;
    if (FAILED((*out_d11)->QueryInterface(IID_PPV_ARGS(&res1))) ||
        FAILED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, out_nt)))
    {
        if (res1) res1->Release();
        return false;
    }
    res1->Release();

    if (FAILED(g_ctx.dev12->OpenSharedHandle(*out_nt, IID_PPV_ARGS(out_d12))))
        return false;

    return true;
}

static bool InitInstanceNvofa(Dlss5Instance *inst)
{
    if (!g_ctx.nvofa_avail) return false;
    inst->ofa_failed = false;

    const double scale = std::min({ 0.5, 1280.0 / inst->full_w, 720.0 / inst->full_h });
    inst->ofa_w = std::max(160u, static_cast<UINT>(inst->full_w * scale) & ~3u);
    inst->ofa_h = std::max(160u, static_cast<UINT>(inst->full_h * scale) & ~3u);

    auto &api = g_ctx.nvofa_api;
    if (api.nvCreateOpticalFlowD3D12(g_ctx.dev12, &inst->ofa_session) != NV_OF_SUCCESS || !inst->ofa_session)
        return false;

    uint32_t count = 0;
    inst->ofa_grid = 4u;
    if (api.nvOFGetCaps(inst->ofa_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &count) == NV_OF_SUCCESS && count > 0 && count <= 32)
    {
        std::vector<uint32_t> grids(count);
        if (api.nvOFGetCaps(inst->ofa_session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &count) == NV_OF_SUCCESS)
        {
            for (uint32_t want : { 2u, 4u, 1u })
                if (std::find(grids.begin(), grids.end(), want) != grids.end()) { inst->ofa_grid = want; break; }
        }
    }

    NV_OF_INIT_PARAMS ip = {};
    ip.width             = inst->ofa_w;
    ip.height            = inst->ofa_h;
    ip.outGridSize       = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(inst->ofa_grid);
    ip.mode              = NV_OF_MODE_OPTICALFLOW;
    ip.perfLevel         = NV_OF_PERF_LEVEL_FAST;
    ip.predDirection     = NV_OF_PRED_DIRECTION_FORWARD;
    ip.enableOutputCost  = NV_OF_FALSE;
    ip.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;

    if (api.nvOFInit(inst->ofa_session, &ip) != NV_OF_SUCCESS)
        return false;

    if (FAILED(g_ctx.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&inst->ofa_fence))))
        return false;

    const UINT fw = (inst->ofa_w + inst->ofa_grid - 1) / inst->ofa_grid;
    const UINT fh = (inst->ofa_h + inst->ofa_grid - 1) / inst->ofa_grid;
    inst->ofa_gray      = CreateTex2D(g_ctx.dev12, inst->ofa_w, inst->ofa_h, DXGI_FORMAT_R8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst->ofa_inputs[0] = CreateTex2D(g_ctx.dev12, inst->ofa_w, inst->ofa_h, DXGI_FORMAT_R8_UNORM, false, D3D12_RESOURCE_STATE_COMMON);
    inst->ofa_inputs[1] = CreateTex2D(g_ctx.dev12, inst->ofa_w, inst->ofa_h, DXGI_FORMAT_R8_UNORM, false, D3D12_RESOURCE_STATE_COMMON);
    inst->ofa_flow      = CreateTex2D(g_ctx.dev12, fw, fh, DXGI_FORMAT_R16G16_SINT, true, D3D12_RESOURCE_STATE_COMMON);
    if (!inst->ofa_gray || !inst->ofa_inputs[0] || !inst->ofa_inputs[1] || !inst->ofa_flow)
        return false;

    ID3D12Resource *res[3] = { inst->ofa_inputs[0], inst->ofa_inputs[1], inst->ofa_flow };
    for (int i = 0; i < 3; ++i)
    {
        NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 rp = {};
        rp.resource         = res[i];
        rp.hOFGpuBuffer     = &inst->ofa_reg[i];
        rp.inputFencePoint  = { inst->fence12, inst->fence_val };
        rp.outputFencePoint = { inst->ofa_fence, ++inst->ofa_fence_val };
        if (api.nvOFRegisterResourceD3D12(inst->ofa_session, &rp) != NV_OF_SUCCESS)
            return false;
        if (inst->ofa_fence->GetCompletedValue() < inst->ofa_fence_val)
        {
            inst->ofa_fence->SetEventOnCompletion(inst->ofa_fence_val, inst->fence_event);
            WaitForSingleObject(inst->fence_event, 5000);
        }
    }
    return true;
}

static void WriteInstanceDescriptors(Dlss5Instance *inst)
{
    const UINT stride = g_ctx.dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE base = inst->heap->GetCPUDescriptorHandleForHeapStart();

    auto at = [&](UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = base;
        h.ptr += static_cast<SIZE_T>(slot) * stride;
        return h;
    };
    auto srv = [&](UINT slot, ID3D12Resource *res, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format                  = fmt;
        sd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels     = 1;
        g_ctx.dev12->CreateShaderResourceView(res, &sd, at(slot));
    };
    auto uav = [&](UINT slot, ID3D12Resource *res, DXGI_FORMAT fmt) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format        = fmt;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        g_ctx.dev12->CreateUnorderedAccessView(res, nullptr, &ud, at(slot));
    };

    // 1. Gray (SLOT_GRAY: t0=in_d12, u0=ofa_gray)
    if (inst->ofa_gray)
    {
        srv(SLOT_GRAY + 0, inst->in_d12, DXGI_FORMAT_R8G8B8A8_UNORM);
        uav(SLOT_GRAY + 1, inst->ofa_gray, DXGI_FORMAT_R8_UNORM);
    }
    // 2. NvofaExpand (SLOT_NVOFA_EXPAND: t0=ofa_flow, u0=mv_tex)
    if (inst->ofa_flow)
    {
        srv(SLOT_NVOFA_EXPAND + 0, inst->ofa_flow, DXGI_FORMAT_R16G16_SINT);
        uav(SLOT_NVOFA_EXPAND + 1, inst->mv_tex, DXGI_FORMAT_R16G16_FLOAT);
    }
    // 3. ScaleDown (SLOT_SCALE_DOWN: t0=in_d12, u0=nr_in)
    srv(SLOT_SCALE_DOWN + 0, inst->in_d12, DXGI_FORMAT_R8G8B8A8_UNORM);
    uav(SLOT_SCALE_DOWN + 1, inst->nr_in, DXGI_FORMAT_R8G8B8A8_UNORM);

    // 4. ScaleUp (SLOT_SCALE_UP: t0=nr_out, u0=work_out)
    srv(SLOT_SCALE_UP + 0, inst->nr_out, DXGI_FORMAT_R8G8B8A8_UNORM);
    uav(SLOT_SCALE_UP + 1, inst->work_out, DXGI_FORMAT_R8G8B8A8_UNORM);

    // 5. Halo sets (SLOT_HALO_0, SLOT_HALO_1, SLOT_HALO_2)
    ID3D12Resource *guide  = inst->nr_small ? inst->nr_in : inst->in_d12;
    ID3D12Resource *nr_res = inst->nr_small ? inst->nr_out : inst->halo_raw;

    // Halo 0 (H-blur)
    srv(SLOT_HALO_0 + 0, guide,  DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_HALO_0 + 1, nr_res, DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_HALO_0 + 2, guide,  DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_HALO_0 + 3, inst->mv_tex, DXGI_FORMAT_R16G16_FLOAT);
    uav(SLOT_HALO_0 + 4, inst->halo_th, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Halo 1 (V-blur)
    srv(SLOT_HALO_1 + 0, guide,         DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_HALO_1 + 1, inst->halo_th, DXGI_FORMAT_R16G16B16A16_FLOAT);
    srv(SLOT_HALO_1 + 2, guide,         DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_HALO_1 + 3, inst->mv_tex,  DXGI_FORMAT_R16G16_FLOAT);
    uav(SLOT_HALO_1 + 4, inst->halo_tb, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // Halo 2 (Fix)
    srv(SLOT_HALO_2 + 0, guide,         DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_HALO_2 + 1, nr_res,        DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_HALO_2 + 2, inst->halo_tb, DXGI_FORMAT_R16G16B16A16_FLOAT);
    srv(SLOT_HALO_2 + 3, inst->mv_tex,  DXGI_FORMAT_R16G16_FLOAT);
    uav(SLOT_HALO_2 + 4, inst->halo_dc, DXGI_FORMAT_R16G16B16A16_FLOAT);

    // 6. ResidualCompose (SLOT_RESIDUAL: t0=in_d12, t1=guide, t2=nr_res, t3=halo_dc, u0=work_out)
    srv(SLOT_RESIDUAL + 0, inst->in_d12,  DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_RESIDUAL + 1, guide,         DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_RESIDUAL + 2, nr_res,        DXGI_FORMAT_R8G8B8A8_UNORM);
    srv(SLOT_RESIDUAL + 3, inst->halo_dc, DXGI_FORMAT_R16G16B16A16_FLOAT);
    uav(SLOT_RESIDUAL + 4, inst->work_out, DXGI_FORMAT_R8G8B8A8_UNORM);

    // 7. Finish ping-pong (SLOT_FINISH_0, SLOT_FINISH_1)
    for (UINT p = 0; p < 2; ++p)
    {
        const UINT base_slot = (p == 0) ? SLOT_FINISH_0 : SLOT_FINISH_1;
        srv(base_slot + 0, inst->finish_tmp,          DXGI_FORMAT_R8G8B8A8_UNORM);
        srv(base_slot + 1, inst->mv_tex,              DXGI_FORMAT_R16G16_FLOAT);
        srv(base_slot + 2, inst->finish_hist[p],      DXGI_FORMAT_R8G8B8A8_UNORM);
        uav(base_slot + 3, inst->work_out,            DXGI_FORMAT_R8G8B8A8_UNORM);
        uav(base_slot + 4, inst->finish_hist[1 - p],  DXGI_FORMAT_R8G8B8A8_UNORM);
    }

    // 8. Split Wipe (SLOT_SPLIT: t0=in_d12, u0=work_out)
    srv(SLOT_SPLIT + 0, inst->in_d12,   DXGI_FORMAT_R8G8B8A8_UNORM);
    uav(SLOT_SPLIT + 1, inst->work_out, DXGI_FORMAT_R8G8B8A8_UNORM);
}

static bool CreateNgxFeatureHandle(Dlss5Instance *inst, UINT w, UINT h, uint32_t preset,
                                   NVSDK_NGX_Handle **out_handle, NVSDK_NGX_Result *out_res)
{
    inst->ngx_params->Reset();
    inst->ngx_params->Set("CreationNodeMask", 1u);
    inst->ngx_params->Set("VisibilityNodeMask", 1u);
    inst->ngx_params->Set("DLSSNR.Width", w);
    inst->ngx_params->Set("DLSSNR.Height", h);
    inst->ngx_params->Set("DLSSNR.InputWidth", w);
    inst->ngx_params->Set("DLSSNR.InputHeight", h);
    inst->ngx_params->Set("DLSSNR.OutputWidth", w);
    inst->ngx_params->Set("DLSSNR.OutputHeight", h);
    inst->ngx_params->Set("DLSSNR.Output.Width", w);
    inst->ngx_params->Set("DLSSNR.Output.Height", h);
    inst->ngx_params->Set("DLSSNR.Upscaling", 0u);
    inst->ngx_params->Set("DLSSNR.Scale", 1.0f);
    inst->ngx_params->Set("DLSSNR.ScalingRatio", 1.0f);
    inst->ngx_params->Set("DLSSNR.Hint.Render.Preset", preset);
    inst->ngx_params->Set("DLSS.Feature.Create.Flags", 0u);

    if (!BeginInstCommands(inst)) return false;
    DWORD seh = 0;
    NVSDK_NGX_Result rf = SafeNrCreate(inst->list, inst->ngx_params, out_handle, &seh);
    if (out_res) *out_res = rf;
    if (seh != 0)
    {
        *out_handle = nullptr;
        EngineLog(LOG_ERR, "CreateFeature raised SEH exception 0x%08X", seh);
        return false;
    }
    if (EndInstCommands(inst, true) == 0)
    {
        *out_handle = nullptr;
        return false;
    }
    if (NVSDK_NGX_FAILED(rf) || *out_handle == nullptr)
    {
        *out_handle = nullptr;
        EngineLog(LOG_ERR, "CreateFeature(18) %ux%u failed: 0x%08X (%s)", w, h, (unsigned)rf, NgxResultName(rf));
        return false;
    }
    return true;
}

static bool EnsureInstanceResources(Dlss5Instance *inst, ID3D11Device *dev11,
                                    UINT full_w, UINT full_h, const Dlss5Params &p)
{
    const uint32_t scale_pct = std::clamp(p.work_scale_pct, 50u, 100u);
    const bool nr_small      = (scale_pct < 100u);
    const UINT work_w        = nr_small ? std::max(64u, (full_w * scale_pct / 100u) & ~1u) : full_w;
    const UINT work_h        = nr_small ? std::max(64u, (full_h * scale_pct / 100u) & ~1u) : full_h;
    const uint32_t passes    = std::clamp(p.passes, 1u, kMaxPasses);

    // Check if D3D11 device changed
    if (inst->bound_dev11 != dev11)
    {
        ReleaseInstanceResources(inst);
        if (inst->fence11) { inst->fence11->Release(); inst->fence11 = nullptr; }
        if (inst->ctx11_4) { inst->ctx11_4->Release(); inst->ctx11_4 = nullptr; }
        if (inst->ctx11)   { inst->ctx11->Release();   inst->ctx11 = nullptr; }
        if (inst->dev11_5) { inst->dev11_5->Release(); inst->dev11_5 = nullptr; }
        inst->bound_dev11 = nullptr;

        if (FAILED(dev11->QueryInterface(IID_PPV_ARGS(&inst->dev11_5)))) return false;
        dev11->GetImmediateContext(&inst->ctx11);
        if (!inst->ctx11 || FAILED(inst->ctx11->QueryInterface(IID_PPV_ARGS(&inst->ctx11_4)))) return false;
        if (FAILED(inst->dev11_5->OpenSharedFence(inst->shared_fence_nt, IID_PPV_ARGS(&inst->fence11))))
            return false;
        inst->bound_dev11 = dev11;
    }

    if (inst->ngx_feature != nullptr &&
        inst->full_w == full_w && inst->full_h == full_h &&
        inst->work_w == work_w && inst->work_h == work_h &&
        inst->preset == p.preset && inst->passes == passes)
    {
        return true;
    }

    ReleaseInstanceResources(inst);

    inst->full_w   = full_w;
    inst->full_h   = full_h;
    inst->work_w   = work_w;
    inst->work_h   = work_h;
    inst->nr_small = nr_small;
    inst->preset   = p.preset;
    inst->passes   = passes;

    if (!CreateSharedTexPair(dev11, full_w, full_h, &inst->in_d11, &inst->in_nt, &inst->in_d12) ||
        !CreateSharedTexPair(dev11, full_w, full_h, &inst->out_d11[0], &inst->out_nt[0], &inst->out_d12[0]) ||
        !CreateSharedTexPair(dev11, full_w, full_h, &inst->out_d11[1], &inst->out_nt[1], &inst->out_d12[1]))
    {
        inst->status_text = "Error: Fallo al crear texturas compartidas D3D11/D3D12";
        ReleaseInstanceResources(inst);
        return false;
    }

    inst->mv_tex       = CreateTex2D(g_ctx.dev12, work_w, work_h, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    inst->nr_in        = CreateTex2D(g_ctx.dev12, work_w, work_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    inst->nr_out       = CreateTex2D(g_ctx.dev12, work_w, work_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst->work_out     = CreateTex2D(g_ctx.dev12, full_w, full_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst->pass_scratch = CreateTex2D(g_ctx.dev12, work_w, work_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    inst->halo_raw     = CreateTex2D(g_ctx.dev12, full_w, full_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst->halo_th      = CreateTex2D(g_ctx.dev12, work_w, work_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst->halo_tb      = CreateTex2D(g_ctx.dev12, work_w, work_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst->halo_dc      = CreateTex2D(g_ctx.dev12, work_w, work_h, DXGI_FORMAT_R16G16B16A16_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    inst->finish_tmp     = CreateTex2D(g_ctx.dev12, full_w, full_h, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    inst->finish_hist[0] = CreateTex2D(g_ctx.dev12, full_w, full_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    inst->finish_hist[1] = CreateTex2D(g_ctx.dev12, full_w, full_h, DXGI_FORMAT_R8G8B8A8_UNORM, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (!inst->mv_tex || !inst->nr_in || !inst->nr_out || !inst->work_out || !inst->pass_scratch ||
        !inst->halo_raw || !inst->halo_th || !inst->halo_tb || !inst->halo_dc ||
        !inst->finish_tmp || !inst->finish_hist[0] || !inst->finish_hist[1])
    {
        inst->status_text = "Error: Memoria VRAM insuficiente para texturas D3D12";
        ReleaseInstanceResources(inst);
        return false;
    }

    // Initialize NVOFA for this instance (non-fatal if unavailable)
    if (!InitInstanceNvofa(inst))
    {
        inst->ofa_failed = true;
    }

    WriteInstanceDescriptors(inst);

    // Create primary NGX Feature 18
    NVSDK_NGX_Result rf = NVSDK_NGX_Result_Fail;
    if (!CreateNgxFeatureHandle(inst, work_w, work_h, p.preset, &inst->ngx_feature, &rf))
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "Error al crear DLSS 5 Feature 18: 0x%08X (%s)", (unsigned)rf, NgxResultName(rf));
        inst->status_text = msg;
        ReleaseInstanceResources(inst);
        return false;
    }

    // Create extra multi-pass layers if requested
    for (uint32_t i = 0; i < passes - 1; ++i)
    {
        NVSDK_NGX_Result er = NVSDK_NGX_Result_Fail;
        if (!CreateNgxFeatureHandle(inst, work_w, work_h, p.preset, &inst->ngx_extra[i], &er))
        {
            inst->passes = i + 1;
            EngineLog(LOG_WARN, "Multi-pass layer %u creation failed (0x%08X); running %u layers.",
                      i + 2, (unsigned)er, inst->passes);
            break;
        }
    }

    char status_buf[256];
    snprintf(status_buf, sizeof(status_buf),
             "Activo: %ux%u (Red: %ux%u, %u pass%s, NVOFA: %s)",
             full_w, full_h, work_w, work_h, inst->passes,
             inst->passes > 1 ? "es" : "",
             (!inst->ofa_failed && inst->ofa_session) ? "ON" : "OFF");
    inst->status_text = status_buf;
    EngineLog(LOG_INFO, "Instance ready -> %s", status_buf);
    return true;
}

static bool ExecuteD3D12FrameJob(Dlss5Instance *inst, UINT64 copy_in_fence, int write_slot, const Dlss5Params &p)
{
    LARGE_INTEGER qpc_freq = {}, qpc_t0 = {}, qpc_t1 = {};
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_t0);

    const UINT stride = g_ctx.dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu_at = [&](UINT slot) {
        D3D12_GPU_DESCRIPTOR_HANDLE h = inst->heap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slot) * stride;
        return h;
    };

    const bool reset = (inst->frame_count == 0) || p.force_reset || inst->pending_reset;
    inst->pending_reset = false;

    // D3D12 queue waits for D3D11 copy to finish on GPU timeline
    g_ctx.queue12->Wait(inst->fence12, copy_in_fence);

    // 2. NVOFA Optical Flow step (if enabled and session ready)
    const bool run_ofa = p.use_nvofa && !inst->ofa_failed && inst->ofa_session != nullptr;
    if (run_ofa)
    {
        if (!BeginInstCommands(inst)) return false;
        ID3D12DescriptorHeap *heaps[] = { inst->heap };
        inst->list->SetDescriptorHeaps(1, heaps);

        // Compute R8 luminance into ofa_gray
        inst->list->SetComputeRootSignature(g_ctx.rs_1srv_1uav_4c);
        inst->list->SetPipelineState(g_ctx.pso_gray);
        UINT gray_consts[4] = { inst->full_w, inst->full_h, inst->ofa_w, inst->ofa_h };
        inst->list->SetComputeRoot32BitConstants(0, 4, gray_consts, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_GRAY));
        inst->list->Dispatch((inst->ofa_w + 7) / 8, (inst->ofa_h + 7) / 8, 1);

        // Copy ofa_gray into ofa_inputs[curr] (and both on first/reset frame)
        D3D12_RESOURCE_BARRIER b_src = MakeBarrier(inst->ofa_gray, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        inst->list->ResourceBarrier(1, &b_src);
        for (UINT i = 0; i < 2; ++i)
        {
            if (i == inst->ofa_curr || !inst->ofa_valid || reset)
            {
                D3D12_RESOURCE_BARRIER b_in = MakeBarrier(inst->ofa_inputs[i], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
                inst->list->ResourceBarrier(1, &b_in);
                inst->list->CopyResource(inst->ofa_inputs[i], inst->ofa_gray);
                D3D12_RESOURCE_BARRIER b_back = MakeBarrier(inst->ofa_inputs[i], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
                inst->list->ResourceBarrier(1, &b_back);
            }
        }
        D3D12_RESOURCE_BARRIER b_src_back = MakeBarrier(inst->ofa_gray, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        inst->list->ResourceBarrier(1, &b_src_back);

        const UINT64 gray_done = EndInstCommands(inst, false);
        if (gray_done == 0) return false;

        NV_OF_FENCE_POINT fp_in  = { inst->fence12, gray_done };
        NV_OF_FENCE_POINT fp_out = { inst->ofa_fence, ++inst->ofa_fence_val };
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 of_in = {};
        of_in.inputFrame           = inst->ofa_reg[inst->ofa_curr];
        of_in.referenceFrame       = inst->ofa_reg[1 - inst->ofa_curr];
        of_in.disableTemporalHints = (reset || !inst->ofa_valid) ? NV_OF_TRUE : NV_OF_FALSE;
        of_in.numFencePoints       = 1;
        of_in.fencePoint           = &fp_in;

        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 of_out = {};
        of_out.outputBuffer = inst->ofa_reg[2];
        of_out.fencePoint   = &fp_out;

        if (g_ctx.nvofa_api.nvOFExecuteD3D12(inst->ofa_session, &of_in, &of_out) == NV_OF_SUCCESS)
        {
            g_ctx.queue12->Wait(inst->ofa_fence, inst->ofa_fence_val);
            inst->ofa_valid = true;
            inst->ofa_curr  = 1 - inst->ofa_curr;
        }
        else
        {
            --inst->ofa_fence_val;
            inst->ofa_failed = true;
        }
    }

    // 3. Main Evaluation + Post-processing Command List
    if (!BeginInstCommands(inst)) return false;
    ID3D12DescriptorHeap *heaps[] = { inst->heap };
    inst->list->SetDescriptorHeaps(1, heaps);

    // Expand optical flow into mv_tex (or clear mv_tex if NVOFA is off)
    if (run_ofa && !inst->ofa_failed)
    {
        D3D12_RESOURCE_BARRIER pre[2] = {
            MakeBarrier(inst->ofa_flow, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            MakeBarrier(inst->mv_tex,   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        inst->list->ResourceBarrier(2, pre);

        inst->list->SetComputeRootSignature(g_ctx.rs_nvofa_expand);
        inst->list->SetPipelineState(g_ctx.pso_nvofa_expand);
        UINT exp_consts[6] = { inst->work_w, inst->work_h, inst->ofa_w, inst->ofa_h, inst->ofa_grid, reset ? 1u : 0u };
        inst->list->SetComputeRoot32BitConstants(0, 6, exp_consts, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_NVOFA_EXPAND));
        inst->list->Dispatch((inst->work_w + 7) / 8, (inst->work_h + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER post[2] = {
            MakeBarrier(inst->mv_tex,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            MakeBarrier(inst->ofa_flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON)
        };
        inst->list->ResourceBarrier(2, post);
    }

    // Downscale color into nr_in if nr_small (Boost mode) is active
    if (inst->nr_small)
    {
        D3D12_RESOURCE_BARRIER to_uav = MakeBarrier(inst->nr_in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        inst->list->ResourceBarrier(1, &to_uav);

        inst->list->SetComputeRootSignature(g_ctx.rs_1srv_1uav_4c);
        inst->list->SetPipelineState(g_ctx.pso_scale4);
        UINT sc_consts[4] = { inst->work_w, inst->work_h, inst->full_w, inst->full_h };
        inst->list->SetComputeRoot32BitConstants(0, 4, sc_consts, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_SCALE_DOWN));
        inst->list->Dispatch((inst->work_w + 7) / 8, (inst->work_h + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER to_srv = MakeBarrier(inst->nr_in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inst->list->ResourceBarrier(1, &to_srv);
    }

    const float halo_amt  = std::clamp(p.halo_reduction, 0.0f, 1.0f);
    const bool  halo_full = !inst->nr_small && (halo_amt > 0.0f);

    ID3D12Resource *nr_color  = inst->nr_small ? inst->nr_in  : inst->in_d12;
    ID3D12Resource *nr_result = inst->nr_small ? inst->nr_out : (halo_full ? inst->halo_raw : inst->work_out);

    // Configure DLSS 5 Neural Rendering parameters
    inst->ngx_params->Reset();
    inst->ngx_params->Set("DLSSNR.Color", nr_color);
    inst->ngx_params->Set("DLSSNR.Output", nr_result);
    inst->ngx_params->Set("DLSSNR.MVec", inst->mv_tex);
    inst->ngx_params->Set("DLSSNR.ColorSubrectBaseX", 0u);
    inst->ngx_params->Set("DLSSNR.ColorSubrectBaseY", 0u);
    inst->ngx_params->Set("DLSSNR.ColorSubrectWidth", inst->work_w);
    inst->ngx_params->Set("DLSSNR.ColorSubrectHeight", inst->work_h);
    inst->ngx_params->Set("DLSSNR.MVecSubrectBaseX", 0u);
    inst->ngx_params->Set("DLSSNR.MVecSubrectBaseY", 0u);
    inst->ngx_params->Set("DLSSNR.MVecSubrectWidth", inst->work_w);
    inst->ngx_params->Set("DLSSNR.MVecSubrectHeight", inst->work_h);
    inst->ngx_params->Set("DLSSNR.OutputSubrectBaseX", 0u);
    inst->ngx_params->Set("DLSSNR.OutputSubrectBaseY", 0u);
    inst->ngx_params->Set("DLSSNR.OutputSubrectWidth", inst->work_w);
    inst->ngx_params->Set("DLSSNR.OutputSubrectHeight", inst->work_h);
    inst->ngx_params->Set("DLSSNR.MVecScaleX", 1.0f);
    inst->ngx_params->Set("DLSSNR.MVecScaleY", 1.0f);
    inst->ngx_params->Set("DLSSNR.Enabled", 1u);
    inst->ngx_params->Set("DLSSNR.Reset", reset ? 1u : 0u);
    inst->ngx_params->Set("DLSSNR.Intensity", p.intensity);
    inst->ngx_params->Set("DLSSNR.LocalToneStrength", p.local_tone);
    inst->ngx_params->Set("DLSSNR.LocalStructureStrength", p.local_structure);
    inst->ngx_params->Set("DLSSNR.SkinStructureStrength", p.skin_structure);
    inst->ngx_params->Set("DLSSNR.UseAutoMask", p.auto_mask ? 1u : 0u);
    inst->ngx_params->Set("DLSSNR.Style", p.style);
    inst->ngx_params->Set("DLSSNR.UICorrection", p.ui_correction ? 1u : 0u);
    inst->ngx_params->Set("DLSS.Pre.Exposure", 1.0f);
    inst->ngx_params->Set("DLSS.Exposure.Scale", 1.0f);

    DWORD seh = 0;
    NVSDK_NGX_Result eval_res = SafeNrEvaluate(inst->list, inst->ngx_feature, inst->ngx_params, &seh);
    if (seh != 0 || NVSDK_NGX_FAILED(eval_res))
    {
        inst->list->Close();
        EngineLog(LOG_ERR, "DLSSNR Evaluate failed: res=0x%08X (%s) seh=0x%08X",
                  (unsigned)eval_res, NgxResultName(eval_res), seh);
        return false;
    }

    // Extra Multi-Pass layers
    for (uint32_t i = 0; i < inst->passes - 1; ++i)
    {
        if (!inst->ngx_extra[i]) break;
        D3D12_RESOURCE_BARRIER b_read = MakeBarrier(nr_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inst->list->ResourceBarrier(1, &b_read);

        inst->ngx_params->Set("DLSSNR.Color", nr_result);
        inst->ngx_params->Set("DLSSNR.Output", inst->pass_scratch);
        inst->ngx_params->Set("DLSSNR.Reset", reset ? 1u : 0u);
        inst->ngx_params->Set("DLSSNR.LocalToneStrength", 0.0f);

        eval_res = SafeNrEvaluate(inst->list, inst->ngx_extra[i], inst->ngx_params, &seh);
        if (seh != 0 || NVSDK_NGX_FAILED(eval_res))
        {
            inst->list->Close();
            return false;
        }

        D3D12_RESOURCE_BARRIER b_copy[2] = {
            MakeBarrier(nr_result,          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            MakeBarrier(inst->pass_scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,          D3D12_RESOURCE_STATE_COPY_SOURCE)
        };
        inst->list->ResourceBarrier(2, b_copy);
        inst->list->CopyResource(nr_result, inst->pass_scratch);
        D3D12_RESOURCE_BARRIER b_back[2] = {
            MakeBarrier(nr_result,          D3D12_RESOURCE_STATE_COPY_DEST,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            MakeBarrier(inst->pass_scratch, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        inst->list->ResourceBarrier(2, b_back);
    }

    // Re-bind our descriptor heap after NGX (NGX binds its own internal descriptor heap!)
    inst->list->SetDescriptorHeaps(1, heaps);

    auto run_halo_passes = [&](UINT w, UINT h) {
        inst->list->SetComputeRootSignature(g_ctx.rs_halo);
        const float sigma_r = 0.06f;

        // Pass 0: Horizontal bilateral blur
        inst->list->SetPipelineState(g_ctx.pso_halo_blur);
        UINT32 c0[10] = { w, h, 1u, 0u, 64u, 4u, 0u, 0u, inst->work_w, inst->work_h };
        memcpy(&c0[7], &sigma_r, sizeof(float));
        inst->list->SetComputeRoot32BitConstants(0, 10, c0, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_HALO_0));
        inst->list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER b1 = MakeBarrier(inst->halo_th, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inst->list->ResourceBarrier(1, &b1);

        // Pass 1: Vertical bilateral blur
        UINT32 c1[10] = { w, h, 0u, 1u, 64u, 4u, 1u, 0u, inst->work_w, inst->work_h };
        memcpy(&c1[7], &sigma_r, sizeof(float));
        inst->list->SetComputeRoot32BitConstants(0, 10, c1, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_HALO_1));
        inst->list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER b2 = MakeBarrier(inst->halo_tb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inst->list->ResourceBarrier(1, &b2);

        // Pass 2: Halo Fix
        inst->list->SetPipelineState(g_ctx.pso_halo_fix);
        UINT32 c2[10] = { w, h, 0u, 0u, 64u, 4u, 2u, 0u, inst->work_w, inst->work_h };
        memcpy(&c2[7], &halo_amt, sizeof(float));
        inst->list->SetComputeRoot32BitConstants(0, 10, c2, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_HALO_2));
        inst->list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER b3[3] = {
            MakeBarrier(inst->halo_dc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            MakeBarrier(inst->halo_th, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            MakeBarrier(inst->halo_tb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        inst->list->ResourceBarrier(3, b3);
    };

    auto run_residual = [&](float strength, float halo_flag) {
        inst->list->SetComputeRootSignature(g_ctx.rs_residual);
        inst->list->SetPipelineState(g_ctx.pso_residual);
        UINT32 rc[4] = { inst->full_w, inst->full_h, 0u, 0u };
        memcpy(&rc[2], &strength, sizeof(float));
        memcpy(&rc[3], &halo_flag, sizeof(float));
        inst->list->SetComputeRoot32BitConstants(0, 4, rc, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_RESIDUAL));
        inst->list->Dispatch((inst->full_w + 7) / 8, (inst->full_h + 7) / 8, 1);
    };

    // Compose / Upscale into work_out
    if (inst->nr_small)
    {
        D3D12_RESOURCE_BARRIER to_srv = MakeBarrier(inst->nr_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inst->list->ResourceBarrier(1, &to_srv);

        if (p.residual_composite && halo_amt > 0.0f)
        {
            run_halo_passes(inst->work_w, inst->work_h);
            run_residual(p.residual_strength, halo_amt);
            D3D12_RESOURCE_BARRIER dc_back = MakeBarrier(inst->halo_dc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            inst->list->ResourceBarrier(1, &dc_back);
        }
        else if (p.residual_composite)
        {
            run_residual(p.residual_strength, 0.0f);
        }
        else
        {
            inst->list->SetComputeRootSignature(g_ctx.rs_1srv_1uav_4c);
            inst->list->SetPipelineState(g_ctx.pso_scale4);
            UINT sc_consts[4] = { inst->full_w, inst->full_h, inst->work_w, inst->work_h };
            inst->list->SetComputeRoot32BitConstants(0, 4, sc_consts, 0);
            inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_SCALE_UP));
            inst->list->Dispatch((inst->full_w + 7) / 8, (inst->full_h + 7) / 8, 1);
        }

        D3D12_RESOURCE_BARRIER back = MakeBarrier(inst->nr_out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        inst->list->ResourceBarrier(1, &back);
    }
    else if (halo_full)
    {
        D3D12_RESOURCE_BARRIER to_srv = MakeBarrier(inst->halo_raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inst->list->ResourceBarrier(1, &to_srv);

        run_halo_passes(inst->full_w, inst->full_h);
        run_residual(1.0f, halo_amt);

        D3D12_RESOURCE_BARRIER back[2] = {
            MakeBarrier(inst->halo_dc,  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            MakeBarrier(inst->halo_raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        inst->list->ResourceBarrier(2, back);
    }

    // Post-Finish pass (Sharpness, Edge Smooth, Color Grading)
    const bool colors_on = (std::fabs(p.brightness) > 1e-4f || std::fabs(p.contrast) > 1e-4f ||
                            std::fabs(p.saturation) > 1e-4f || std::fabs(p.warmth) > 1e-4f);
    if (p.sharpness > 0.0f || p.edge_smooth > 0.0f || colors_on)
    {
        const UINT set = inst->finish_ping;
        ID3D12Resource *next_hist = inst->finish_hist[1 - set];

        D3D12_RESOURCE_BARRIER pre[2] = {
            MakeBarrier(inst->work_out,   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,          D3D12_RESOURCE_STATE_COPY_SOURCE),
            MakeBarrier(inst->finish_tmp, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)
        };
        inst->list->ResourceBarrier(2, pre);
        inst->list->CopyResource(inst->finish_tmp, inst->work_out);

        D3D12_RESOURCE_BARRIER mid[3] = {
            MakeBarrier(inst->work_out,   D3D12_RESOURCE_STATE_COPY_SOURCE,               D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            MakeBarrier(inst->finish_tmp, D3D12_RESOURCE_STATE_COPY_DEST,                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            MakeBarrier(next_hist,        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        inst->list->ResourceBarrier(3, mid);

        inst->list->SetComputeRootSignature(g_ctx.rs_finish);
        inst->list->SetPipelineState(g_ctx.pso_finish);
        const UINT flags = ((inst->finish_valid && !reset) ? 1u : 0u) | (colors_on ? 2u : 0u);
        UINT32 fc[12] = { inst->full_w, inst->full_h, 0u, 0u, inst->work_w, inst->work_h, flags, 0u, 0u, 0u, 0u, 0u };
        memcpy(&fc[2],  &p.sharpness,   sizeof(float));
        memcpy(&fc[3],  &p.edge_smooth, sizeof(float));
        memcpy(&fc[7],  &p.brightness,  sizeof(float));
        memcpy(&fc[8],  &p.contrast,    sizeof(float));
        memcpy(&fc[9],  &p.saturation,  sizeof(float));
        memcpy(&fc[10], &p.warmth,      sizeof(float));
        inst->list->SetComputeRoot32BitConstants(0, 12, fc, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(set == 0 ? SLOT_FINISH_0 : SLOT_FINISH_1));
        inst->list->Dispatch((inst->full_w + 7) / 8, (inst->full_h + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER post = MakeBarrier(next_hist, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        inst->list->ResourceBarrier(1, &post);
        inst->finish_ping  = 1 - set;
        inst->finish_valid = true;
    }
    else
    {
        inst->finish_valid = false;
    }

    // Split-Screen Before/After Wipe
    if (p.split_enable)
    {
        const UINT split_x = std::clamp(static_cast<UINT>(p.split_pos * inst->full_w), 0u, inst->full_w);
        inst->list->SetComputeRootSignature(g_ctx.rs_1srv_1uav_4c);
        inst->list->SetPipelineState(g_ctx.pso_split);
        UINT sp_consts[4] = { inst->full_w, inst->full_h, split_x, 0u };
        inst->list->SetComputeRoot32BitConstants(0, 4, sp_consts, 0);
        inst->list->SetComputeRootDescriptorTable(1, gpu_at(SLOT_SPLIT));
        inst->list->Dispatch((inst->full_w + 7) / 8, (inst->full_h + 7) / 8, 1);
    }

    // Copy work_out -> shared out_d12[write_slot]
    D3D12_RESOURCE_BARRIER copy_out_pre = MakeBarrier(inst->work_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    inst->list->ResourceBarrier(1, &copy_out_pre);
    inst->list->CopyResource(inst->out_d12[write_slot], inst->work_out);
    D3D12_RESOURCE_BARRIER copy_out_post = MakeBarrier(inst->work_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    inst->list->ResourceBarrier(1, &copy_out_post);

    // Wait on worker thread until D3D12 finishes writing out_d12[write_slot]
    // so OBS's D3D11 thread can copy out_d11[ready_out_slot] with zero CPU/GPU wait!
    const UINT64 eval_done_fence = EndInstCommands(inst, true);
    if (eval_done_fence == 0)
        return false;

    QueryPerformanceCounter(&qpc_t1);
    inst->last_gpu_ms = (double)(qpc_t1.QuadPart - qpc_t0.QuadPart) * 1000.0 / (double)qpc_freq.QuadPart;

    if ((inst->frame_count % 30) == 0)
    {
        char status_buf[256];
        snprintf(status_buf, sizeof(status_buf),
                 "Activo: %ux%u (Red: %ux%u, %u pass%s, NVOFA: %s, GPU: %.1f ms)",
                 inst->full_w, inst->full_h, inst->work_w, inst->work_h, inst->passes,
                 inst->passes > 1 ? "es" : "",
                 (!inst->ofa_failed && inst->ofa_session) ? "ON" : "OFF",
                 inst->last_gpu_ms);
        inst->status_text = status_buf;
    }

    ++inst->frame_count;
    return true;
}

static void WorkerThreadLoop(Dlss5Instance *inst)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    for (;;)
    {
        UINT64 copy_in_fence = 0;
        int write_slot = 0;
        Dlss5Params p = {};

        {
            std::unique_lock<std::mutex> wlock(inst->worker_mtx);
            inst->worker_cv.wait(wlock, [&]() { return inst->worker_stop || inst->worker_has_job; });
            if (inst->worker_stop)
                break;

            copy_in_fence        = inst->job_copy_in_fence;
            write_slot           = inst->job_write_slot;
            p                    = inst->job_params;
            inst->worker_has_job = false;
        }

        const bool ok = ExecuteD3D12FrameJob(inst, copy_in_fence, write_slot, p);
        if (ok)
        {
            inst->ready_out_slot.store(write_slot);
            inst->has_valid_output.store(true);
        }

        {
            std::lock_guard<std::mutex> wlock(inst->worker_mtx);
            inst->worker_busy = false;
        }
        inst->worker_done_cv.notify_all();
    }
}

Dlss5Instance *Dlss5_CreateInstance()
{
    Dlss5Instance *inst = new Dlss5Instance();
    return inst;
}

void Dlss5_DestroyInstance(Dlss5Instance *inst)
{
    if (!inst) return;
    if (inst->worker_thread.joinable())
    {
        {
            std::lock_guard<std::mutex> wlock(inst->worker_mtx);
            inst->worker_stop = true;
        }
        inst->worker_cv.notify_all();
        inst->worker_thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        ReleaseInstanceResources(inst);
        if (inst->ngx_params) { NVSDK_NGX_D3D12_DestroyParameters(inst->ngx_params); inst->ngx_params = nullptr; }
        if (inst->heap)       { inst->heap->Release(); inst->heap = nullptr; }
        if (inst->list)       { inst->list->Release(); inst->list = nullptr; }
        if (inst->alloc)      { inst->alloc->Release(); inst->alloc = nullptr; }
        if (inst->fence11)    { inst->fence11->Release(); inst->fence11 = nullptr; }
        if (inst->ctx11_4)    { inst->ctx11_4->Release(); inst->ctx11_4 = nullptr; }
        if (inst->ctx11)      { inst->ctx11->Release(); inst->ctx11 = nullptr; }
        if (inst->dev11_5)    { inst->dev11_5->Release(); inst->dev11_5 = nullptr; }
        if (inst->shared_fence_nt) { CloseHandle(inst->shared_fence_nt); inst->shared_fence_nt = nullptr; }
        if (inst->fence12)    { inst->fence12->Release(); inst->fence12 = nullptr; }
        if (inst->fence_event) { CloseHandle(inst->fence_event); inst->fence_event = nullptr; }
    }
    delete inst;
}

const char *Dlss5_GetInstanceStatus(Dlss5Instance *inst)
{
    if (!inst) return "No inicializado";
    return inst->status_text.c_str();
}

static bool InitInstanceBase(Dlss5Instance *inst)
{
    if (inst->list != nullptr) return true;

    if (FAILED(g_ctx.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&inst->alloc))))
        return false;
    if (FAILED(g_ctx.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, inst->alloc, nullptr, IID_PPV_ARGS(&inst->list))))
        return false;
    inst->list->Close();

    if (FAILED(g_ctx.dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&inst->fence12))))
        return false;
    if (FAILED(g_ctx.dev12->CreateSharedHandle(inst->fence12, nullptr, GENERIC_ALL, nullptr, &inst->shared_fence_nt)))
        return false;
    inst->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = HEAP_TOTAL_DESCS;
    hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_ctx.dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&inst->heap))))
        return false;

    if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&inst->ngx_params)) || !inst->ngx_params)
        return false;

    inst->worker_stop = false;
    inst->worker_thread = std::thread(WorkerThreadLoop, inst);
    return true;
}

bool Dlss5_ProcessFrameD3D11(
    Dlss5Instance     *inst,
    ID3D11Device      *dev11,
    ID3D11Texture2D   *src_tex11,
    ID3D11Texture2D   *dst_tex11,
    uint32_t           width,
    uint32_t           height,
    const Dlss5Params &p)
{
    if (!inst || !dev11 || !src_tex11 || !dst_tex11 || !p.enabled)
        return false;
    if (width < 64 || height < 64 || width > 7680 || height > 4320)
        return false;

    {
        std::lock_guard<std::mutex> glock(g_global_mutex);
        if (!InitGlobalContext(dev11))
        {
            inst->status_text = "Error: " + g_ctx.init_error;
            return false;
        }
    }

    std::lock_guard<std::mutex> ilock(inst->mutex);
    if (!InitInstanceBase(inst))
    {
        inst->status_text = "Error al inicializar recursos base D3D12";
        return false;
    }

    if (!EnsureInstanceResources(inst, dev11, width, height, p))
        return false;

    if (p.force_reset)
        inst->pending_reset = true;

    bool can_submit = false;
    {
        std::lock_guard<std::mutex> wlock(inst->worker_mtx);
        can_submit = !inst->worker_busy;
    }

    if (can_submit)
    {
        // 1. Copy OBS D3D11 source texture -> shared in_d11, signal shared fence, and FLUSH immediately!
        inst->ctx11->CopyResource(inst->in_d11, src_tex11);
        const UINT64 copy_in_fence = ++inst->fence_val;
        inst->ctx11_4->Signal(inst->fence11, copy_in_fence);
        inst->ctx11->Flush();

        const bool had_valid = inst->has_valid_output.load();
        const int write_slot = had_valid ? (1 - inst->ready_out_slot.load()) : 0;

        {
            std::lock_guard<std::mutex> wlock(inst->worker_mtx);
            inst->job_copy_in_fence = copy_in_fence;
            inst->job_write_slot    = write_slot;
            inst->job_params        = p;
            inst->worker_busy       = true;
            inst->worker_has_job    = true;
        }
        inst->worker_cv.notify_one();

        // Only wait synchronously on the very first frame after creation/resize so frame 0 is immediately valid
        if (!had_valid)
        {
            WaitForWorkerIdle(inst);
        }
    }

    if (!inst->has_valid_output.load())
        return false;

    // 2. Immediately copy the latest completed D3D12 output texture -> dst_tex11 with ZERO CPU/GPU stall!
    const int ready_slot = inst->ready_out_slot.load();
    inst->ctx11->CopyResource(dst_tex11, inst->out_d11[ready_slot]);
    return true;
}

void Dlss5_ShutdownGlobal()
{
    std::lock_guard<std::mutex> lock(g_global_mutex);
    if (!g_ctx.inited) return;

    auto rel = [](IUnknown *&p) { if (p) { p->Release(); p = nullptr; } };
    rel((IUnknown *&)g_ctx.pso_gray);
    rel((IUnknown *&)g_ctx.pso_nvofa_expand);
    rel((IUnknown *&)g_ctx.pso_scale4);
    rel((IUnknown *&)g_ctx.pso_halo_blur);
    rel((IUnknown *&)g_ctx.pso_halo_fix);
    rel((IUnknown *&)g_ctx.pso_residual);
    rel((IUnknown *&)g_ctx.pso_finish);
    rel((IUnknown *&)g_ctx.pso_split);

    rel((IUnknown *&)g_ctx.rs_1srv_1uav_4c);
    rel((IUnknown *&)g_ctx.rs_nvofa_expand);
    rel((IUnknown *&)g_ctx.rs_halo);
    rel((IUnknown *&)g_ctx.rs_residual);
    rel((IUnknown *&)g_ctx.rs_finish);

    if (g_ctx.ngx_inited && g_ctx.dev12)
    {
        NVSDK_NGX_D3D12_Shutdown1(g_ctx.dev12);
        g_ctx.ngx_inited = false;
    }

    rel((IUnknown *&)g_ctx.queue12);
    rel((IUnknown *&)g_ctx.dev12);

    if (g_ctx.nvofa_mod) { FreeLibrary(g_ctx.nvofa_mod); g_ctx.nvofa_mod = nullptr; }
    g_ctx.inited = false;
}
