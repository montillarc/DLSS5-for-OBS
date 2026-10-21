#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdint>

struct Dlss5Params
{
    bool     enabled            = true;
    uint32_t preset             = 1;     // 0 = Default, 1 = Natural, 2 = Cinematic
    uint32_t style              = 1;     // 0, 1, 2
    float    intensity          = 1.0f;  // 0.0 .. 2.0
    float    local_tone         = 0.35f; // 0.0 .. 2.0
    float    local_structure    = 1.35f; // 0.0 .. 2.0
    float    skin_structure     = 1.0f;  // 0.0 .. 2.0
    bool     auto_mask          = true;
    bool     ui_correction      = true;

    // Performance / Boost mode
    uint32_t work_scale_pct     = 75;    // 50 .. 100 (% of source resolution)
    bool     residual_composite = true;  // native + (nr_out - nr_in) * residual_strength
    float    residual_strength  = 1.15f; // 0.0 .. 2.0
    uint32_t passes             = 1;     // 1 .. 4 layers

    // Motion & Artifact Control
    bool     use_nvofa          = true;  // Hardware optical flow (NVOFA D3D12)
    float    halo_reduction     = 0.25f; // 0.0 .. 1.0

    // Post-Finish Enhancer (CAS Sharpness, Edge Smooth, Color Grading)
    float    sharpness          = 0.35f; // 0.0 .. 1.0
    float    edge_smooth        = 0.0f;  // 0.0 .. 1.0
    float    brightness         = 0.0f;  // -1.0 .. 1.0
    float    contrast           = 0.0f;  // -1.0 .. 1.0
    float    saturation         = 0.0f;  // -1.0 .. 1.0
    float    warmth             = 0.0f;  // -1.0 .. 1.0

    // Split-screen comparison (Before / After)
    bool     split_enable       = false;
    float     split_pos         = 0.5f;  // 0.0 .. 1.0

    // Manual temporal history reset trigger
    bool     force_reset        = false;
};

typedef void (*Dlss5LogFn)(int level, const char *msg);

struct Dlss5Instance;

// Set logger callback and directory where nvngx.dll_ns-forwarder.dll and nvngx_dlssnr.dll live.
void Dlss5_SetConfig(const wchar_t *runtime_dir, Dlss5LogFn log_fn);

// Create/Destroy per-filter instance
Dlss5Instance *Dlss5_CreateInstance();
void           Dlss5_DestroyInstance(Dlss5Instance *inst);

// Process a frame on GPU:
// Copies src_tex11 (ID3D11Texture2D, RGBA8) -> D3D12 shared texture -> NVOFA + DLSS 5 NR -> dst_tex11 (ID3D11Texture2D, RGBA8).
// Returns true if processed and written into dst_tex11; false if fallback/passthrough should be used.
bool Dlss5_ProcessFrameD3D11(
    Dlss5Instance   *inst,
    ID3D11Device    *dev11,
    ID3D11Texture2D *src_tex11,
    ID3D11Texture2D *dst_tex11,
    uint32_t         width,
    uint32_t         height,
    const Dlss5Params &params);

// Query human-readable status string for OBS properties UI
const char *Dlss5_GetInstanceStatus(Dlss5Instance *inst);

// Shutdown global D3D12 / NGX resources on module unload
void Dlss5_ShutdownGlobal();
