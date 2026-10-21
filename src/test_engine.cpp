#include "dlss5_engine.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

static void ConsoleLogger(int level, const char *msg)
{
    const char *tag = (level == 100) ? "ERR" : (level == 200) ? "WRN" : "INF";
    printf("[%s] %s\n", tag, msg);
    fflush(stdout);
}

int main()
{
    printf("=== DLSS 5 for OBS - Real Frame Benchmark & Quality Test ===\n");

    wchar_t exe_path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    wchar_t *slash = wcsrchr(exe_path, L'\\');
    if (slash) *slash = 0;

    Dlss5_SetConfig(exe_path, ConsoleLogger);

    IDXGIFactory1 *factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
    IDXGIAdapter *nv_adapter = nullptr;
    if (factory) {
        IDXGIAdapter *a = nullptr;
        for (UINT i = 0; factory->EnumAdapters(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC desc = {};
            a->GetDesc(&desc);
            if (desc.VendorId == 0x10DE) {
                nv_adapter = a;
                break;
            }
            a->Release();
        }
        factory->Release();
    }

    ID3D11Device *dev11 = nullptr;
    ID3D11DeviceContext *ctx11 = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    D3D11CreateDevice(nv_adapter, nv_adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                      nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev11, nullptr, &ctx11);
    if (nv_adapter) nv_adapter->Release();

    const uint32_t W = 1920;
    const uint32_t H = 1080;
    std::vector<uint8_t> pixels(W * H * 4);
    FILE *fp = fopen("build\\test_input.rgba", "rb");
    if (fp) {
        fread(pixels.data(), 1, pixels.size(), fp);
        fclose(fp);
    }

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = W; td.Height = H; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    D3D11_SUBRESOURCE_DATA init_data = { pixels.data(), W * 4, 0 };
    ID3D11Texture2D *src_tex = nullptr, *dst_tex = nullptr;
    dev11->CreateTexture2D(&td, &init_data, &src_tex);
    dev11->CreateTexture2D(&td, nullptr, &dst_tex);

    Dlss5Instance *inst = Dlss5_CreateInstance();
    Dlss5Params params = {};
    params.enabled = true;
    params.preset = 1;
    params.style = 1;
    params.intensity = 1.0f;
    params.local_tone = 0.5f;
    params.local_structure = 1.0f;
    params.skin_structure = 1.0f;
    params.work_scale_pct = 65;
    params.residual_composite = true;
    params.residual_strength = 1.0f;
    params.passes = 1;
    params.use_nvofa = true;
    params.sharpness = 0.0f;

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    for (int f = 0; f <= 60; ++f) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        bool ok = Dlss5_ProcessFrameD3D11(inst, dev11, src_tex, dst_tex, W, H, params);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
        if (f < 5 || f == 10 || f == 30 || f == 60)
            printf("Frame %2d: %s | %.2f ms (%.1f FPS max)\n", f, ok ? "OK" : "FAIL", ms, 1000.0 / ms);
    }

    D3D11_TEXTURE2D_DESC st_desc = td;
    st_desc.Usage = D3D11_USAGE_STAGING;
    st_desc.BindFlags = 0;
    st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *stage_tex = nullptr;
    dev11->CreateTexture2D(&st_desc, nullptr, &stage_tex);
    ctx11->CopyResource(stage_tex, dst_tex);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(ctx11->Map(stage_tex, 0, D3D11_MAP_READ, 0, &mapped))) {
        double sum_diff = 0.0;
        double sum_in = 0.0, sum_out = 0.0;
        for (uint32_t y = 0; y < H; ++y) {
            const uint8_t *out_row = (const uint8_t *)mapped.pData + y * mapped.RowPitch;
            const uint8_t *in_row  = pixels.data() + y * W * 4;
            for (uint32_t x = 0; x < W; ++x) {
                for (int c = 0; c < 3; ++c) {
                    int iv = in_row[x * 4 + c];
                    int ov = out_row[x * 4 + c];
                    sum_in  += iv;
                    sum_out += ov;
                    sum_diff += std::abs(ov - iv);
                }
            }
        }
        double total_ch = (double)W * H * 3.0;
        printf("Mean Input RGB : %.2f\n", sum_in / total_ch);
        printf("Mean Output RGB: %.2f\n", sum_out / total_ch);
        printf("Mean Abs Delta : %.2f (out of 255)\n", sum_diff / total_ch);
        ctx11->Unmap(stage_tex, 0);
    }
    stage_tex->Release();

    Dlss5_DestroyInstance(inst);
    Dlss5_ShutdownGlobal();
    src_tex->Release(); dst_tex->Release(); ctx11->Release(); dev11->Release();
    return 0;
}
