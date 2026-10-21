#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <cstdio>
#include <cstring>

#include "dlss5_engine.h"

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_name(void)
{
    return "DLSS 5 Neural Rendering Filter";
}

MODULE_EXPORT const char *obs_module_description(void)
{
    return "NVIDIA DLSS 5 Neural Rendering (Feature 18 + NVOFA) real-time video filter for OBS Studio.";
}

// -----------------------------------------------------------------------------
// Property Keys
// -----------------------------------------------------------------------------
#define S_STATUS             "dlss5_status"
#define S_ENABLED            "dlss5_enabled"
#define S_PRESET             "dlss5_preset"
#define S_STYLE              "dlss5_style"
#define S_INTENSITY          "dlss5_intensity"
#define S_LOCAL_TONE         "dlss5_local_tone"
#define S_LOCAL_STRUCTURE    "dlss5_local_structure"
#define S_SKIN_STRUCTURE     "dlss5_skin_structure"
#define S_AUTO_MASK          "dlss5_auto_mask"
#define S_UI_CORRECTION      "dlss5_ui_correction"
#define S_WORK_SCALE_PCT     "dlss5_work_scale_pct"
#define S_RESIDUAL_COMPOSITE "dlss5_residual_composite"
#define S_RESIDUAL_STRENGTH  "dlss5_residual_strength"
#define S_PASSES             "dlss5_passes"
#define S_USE_NVOFA          "dlss5_use_nvofa"
#define S_HALO_REDUCTION     "dlss5_halo_reduction"
#define S_SHARPNESS          "dlss5_sharpness"
#define S_EDGE_SMOOTH        "dlss5_edge_smooth"
#define S_BRIGHTNESS         "dlss5_brightness"
#define S_CONTRAST           "dlss5_contrast"
#define S_SATURATION         "dlss5_saturation"
#define S_WARMTH             "dlss5_warmth"
#define S_SPLIT_ENABLE       "dlss5_split_enable"
#define S_SPLIT_POS          "dlss5_split_pos"
#define S_OPTIMAL_BTN        "dlss5_optimal_btn"
#define S_RESET_BTN          "dlss5_reset_btn"
#define S_CFG_VER            "dlss5_cfg_ver"

struct dlss5_filter_data
{
    obs_source_t   *context      = nullptr;
    Dlss5Instance  *engine       = nullptr;
    gs_texrender_t *texrender    = nullptr;
    gs_texture_t   *out_tex      = nullptr;
    uint32_t        width        = 0;
    uint32_t        height       = 0;
    Dlss5Params     params       = {};
    bool            reset_queued = false;
};

static void ObsLoggerCallback(int level, const char *msg)
{
    blog(level, "%s", msg);
}

static const char *dlss5_filter_get_name(void *unused)
{
    (void)unused;
    return "DLSS 5 Neural Rendering (RTX)";
}

static void ApplyOptimalSettingsToData(obs_data_t *settings)
{
    obs_data_set_int   (settings, S_CFG_VER,            2);
    obs_data_set_bool  (settings, S_ENABLED,            true);
    obs_data_set_int   (settings, S_PRESET,             1);
    obs_data_set_int   (settings, S_STYLE,              1);
    obs_data_set_double(settings, S_INTENSITY,          1.0);
    obs_data_set_double(settings, S_LOCAL_TONE,         0.35);
    obs_data_set_double(settings, S_LOCAL_STRUCTURE,    1.35);
    obs_data_set_double(settings, S_SKIN_STRUCTURE,     1.0);
    obs_data_set_bool  (settings, S_AUTO_MASK,          true);
    obs_data_set_bool  (settings, S_UI_CORRECTION,      true);
    obs_data_set_int   (settings, S_WORK_SCALE_PCT,     75);
    obs_data_set_bool  (settings, S_RESIDUAL_COMPOSITE, true);
    obs_data_set_double(settings, S_RESIDUAL_STRENGTH,  1.15);
    obs_data_set_int   (settings, S_PASSES,             1);
    obs_data_set_bool  (settings, S_USE_NVOFA,          true);
    obs_data_set_double(settings, S_HALO_REDUCTION,     0.25);
    obs_data_set_double(settings, S_SHARPNESS,          0.35);
    obs_data_set_double(settings, S_EDGE_SMOOTH,        0.0);
    obs_data_set_double(settings, S_BRIGHTNESS,         0.0);
    obs_data_set_double(settings, S_CONTRAST,           0.0);
    obs_data_set_double(settings, S_SATURATION,         0.0);
    obs_data_set_double(settings, S_WARMTH,             0.0);
}

static void dlss5_filter_update(void *data, obs_data_t *settings)
{
    auto *f = static_cast<dlss5_filter_data *>(data);
    if (!f) return;

    // Auto-migrate v1 filter instances to v2 optimal quality + 60fps defaults
    if (obs_data_get_int(settings, S_CFG_VER) < 2) {
        ApplyOptimalSettingsToData(settings);
    }

    f->params.enabled            = obs_data_get_bool(settings, S_ENABLED);
    f->params.preset             = (uint32_t)obs_data_get_int(settings, S_PRESET);
    f->params.style              = (uint32_t)obs_data_get_int(settings, S_STYLE);
    f->params.intensity          = (float)obs_data_get_double(settings, S_INTENSITY);
    f->params.local_tone         = (float)obs_data_get_double(settings, S_LOCAL_TONE);
    f->params.local_structure    = (float)obs_data_get_double(settings, S_LOCAL_STRUCTURE);
    f->params.skin_structure     = (float)obs_data_get_double(settings, S_SKIN_STRUCTURE);
    f->params.auto_mask          = obs_data_get_bool(settings, S_AUTO_MASK);
    f->params.ui_correction      = obs_data_get_bool(settings, S_UI_CORRECTION);

    f->params.work_scale_pct     = (uint32_t)obs_data_get_int(settings, S_WORK_SCALE_PCT);
    f->params.residual_composite = obs_data_get_bool(settings, S_RESIDUAL_COMPOSITE);
    f->params.residual_strength  = (float)obs_data_get_double(settings, S_RESIDUAL_STRENGTH);
    f->params.passes             = (uint32_t)obs_data_get_int(settings, S_PASSES);

    f->params.use_nvofa          = obs_data_get_bool(settings, S_USE_NVOFA);
    f->params.halo_reduction     = (float)obs_data_get_double(settings, S_HALO_REDUCTION);

    f->params.sharpness          = (float)obs_data_get_double(settings, S_SHARPNESS);
    f->params.edge_smooth        = (float)obs_data_get_double(settings, S_EDGE_SMOOTH);
    f->params.brightness         = (float)obs_data_get_double(settings, S_BRIGHTNESS);
    f->params.contrast           = (float)obs_data_get_double(settings, S_CONTRAST);
    f->params.saturation         = (float)obs_data_get_double(settings, S_SATURATION);
    f->params.warmth             = (float)obs_data_get_double(settings, S_WARMTH);

    f->params.split_enable       = obs_data_get_bool(settings, S_SPLIT_ENABLE);
    f->params.split_pos          = (float)obs_data_get_double(settings, S_SPLIT_POS);
}

static void dlss5_filter_defaults(obs_data_t *settings)
{
    obs_data_set_default_int   (settings, S_CFG_VER,            2);
    obs_data_set_default_string(settings, S_STATUS,             "Esperando cuadros de video...");
    obs_data_set_default_bool  (settings, S_ENABLED,            true);
    obs_data_set_default_int   (settings, S_PRESET,             1);
    obs_data_set_default_int   (settings, S_STYLE,              1);
    obs_data_set_default_double(settings, S_INTENSITY,          1.0);
    obs_data_set_default_double(settings, S_LOCAL_TONE,         0.35);
    obs_data_set_default_double(settings, S_LOCAL_STRUCTURE,    1.35);
    obs_data_set_default_double(settings, S_SKIN_STRUCTURE,     1.0);
    obs_data_set_default_bool  (settings, S_AUTO_MASK,          true);
    obs_data_set_default_bool  (settings, S_UI_CORRECTION,      true);

    obs_data_set_default_int   (settings, S_WORK_SCALE_PCT,     75);
    obs_data_set_default_bool  (settings, S_RESIDUAL_COMPOSITE, true);
    obs_data_set_default_double(settings, S_RESIDUAL_STRENGTH,  1.15);
    obs_data_set_default_int   (settings, S_PASSES,             1);

    obs_data_set_default_bool  (settings, S_USE_NVOFA,          true);
    obs_data_set_default_double(settings, S_HALO_REDUCTION,     0.25);

    obs_data_set_default_double(settings, S_SHARPNESS,          0.35);
    obs_data_set_default_double(settings, S_EDGE_SMOOTH,        0.0);
    obs_data_set_default_double(settings, S_BRIGHTNESS,         0.0);
    obs_data_set_default_double(settings, S_CONTRAST,           0.0);
    obs_data_set_default_double(settings, S_SATURATION,         0.0);
    obs_data_set_default_double(settings, S_WARMTH,             0.0);

    obs_data_set_default_bool  (settings, S_SPLIT_ENABLE,       false);
    obs_data_set_default_double(settings, S_SPLIT_POS,          0.5);
}

static void *dlss5_filter_create(obs_data_t *settings, obs_source_t *source)
{
    auto *f = new dlss5_filter_data();
    f->context = source;
    f->engine  = Dlss5_CreateInstance();

    obs_enter_graphics();
    f->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    obs_leave_graphics();

    dlss5_filter_update(f, settings);
    return f;
}

static void dlss5_filter_destroy(void *data)
{
    auto *f = static_cast<dlss5_filter_data *>(data);
    if (!f) return;

    obs_enter_graphics();
    if (f->texrender) {
        gs_texrender_destroy(f->texrender);
        f->texrender = nullptr;
    }
    if (f->out_tex) {
        gs_texture_destroy(f->out_tex);
        f->out_tex = nullptr;
    }
    obs_leave_graphics();

    if (f->engine) {
        Dlss5_DestroyInstance(f->engine);
        f->engine = nullptr;
    }
    delete f;
}

static uint32_t dlss5_filter_get_width(void *data)
{
    auto *f = static_cast<dlss5_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(f->context);
    return target ? obs_source_get_base_width(target) : f->width;
}

static uint32_t dlss5_filter_get_height(void *data)
{
    auto *f = static_cast<dlss5_filter_data *>(data);
    obs_source_t *target = obs_filter_get_target(f->context);
    return target ? obs_source_get_base_height(target) : f->height;
}

static void DrawOutputTexture(gs_texture_t *tex, uint32_t cx, uint32_t cy)
{
    const bool prev_linear  = gs_set_linear_srgb(true);
    const bool linear_srgb  = gs_get_linear_srgb();
    const bool prev_fb_srgb = gs_framebuffer_srgb_enabled();
    gs_enable_framebuffer_srgb(linear_srgb);

    gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image  = gs_effect_get_param_by_name(effect, "image");
    if (linear_srgb)
        gs_effect_set_texture_srgb(image, tex);
    else
        gs_effect_set_texture(image, tex);

    while (gs_effect_loop(effect, "Draw")) {
        gs_draw_sprite(tex, 0, cx, cy);
    }

    gs_enable_framebuffer_srgb(prev_fb_srgb);
    gs_set_linear_srgb(prev_linear);
}

static void dlss5_filter_video_render(void *data, gs_effect_t *effect)
{
    (void)effect;
    auto *f = static_cast<dlss5_filter_data *>(data);
    if (!f || !f->context) return;

    obs_source_t *target = obs_filter_get_target(f->context);
    obs_source_t *parent = obs_filter_get_parent(f->context);
    if (!target || !parent || !f->params.enabled) {
        obs_source_skip_video_filter(f->context);
        return;
    }

    uint32_t cx = obs_source_get_base_width(target);
    uint32_t cy = obs_source_get_base_height(target);
    if (cx < 64 || cy < 64) {
        obs_source_skip_video_filter(f->context);
        return;
    }

    if (gs_get_device_type() != GS_DEVICE_DIRECT3D_11) {
        obs_source_skip_video_filter(f->context);
        return;
    }

    if (!f->texrender) {
        f->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    }

    // 1. Render upstream filter target into f->texrender in non-linear sRGB space (GS_CS_SRGB)
    // exactly matching obs_source_process_filter_begin so DLSS 5 receives standard sRGB 8-bit
    // and no double-gamma exposure boost occurs!
    const bool prev_linear = gs_set_linear_srgb(false);
    gs_texrender_reset(f->texrender);
    gs_blend_state_push();
    gs_blend_function_separate(
        GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

    if (!gs_texrender_begin_with_color_space(f->texrender, cx, cy, GS_CS_SRGB)) {
        gs_blend_state_pop();
        gs_set_linear_srgb(prev_linear);
        obs_source_skip_video_filter(f->context);
        return;
    }

    struct vec4 clear_color = {};
    vec4_zero(&clear_color);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

    uint32_t parent_flags = obs_source_get_output_flags(target);
    bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
    bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;

    if (target == parent && !custom_draw && !async) {
        obs_source_default_render(target);
    } else {
        obs_source_video_render(target);
    }

    gs_texrender_end(f->texrender);
    gs_blend_state_pop();
    gs_set_linear_srgb(prev_linear);

    gs_texture_t *src_gs_tex = gs_texrender_get_texture(f->texrender);
    if (!src_gs_tex) {
        obs_source_skip_video_filter(f->context);
        return;
    }

    // 2. Ensure output texture matches (cx, cy)
    if (!f->out_tex || f->width != cx || f->height != cy) {
        if (f->out_tex) {
            gs_texture_destroy(f->out_tex);
            f->out_tex = nullptr;
        }
        f->out_tex = gs_texture_create(cx, cy, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
        f->width  = cx;
        f->height = cy;
    }

    if (!f->out_tex) {
        DrawOutputTexture(src_gs_tex, cx, cy);
        return;
    }

    // 3. Extract underlying D3D11 device & textures
    auto *dev11     = static_cast<ID3D11Device *>(gs_get_device_obj());
    auto *src_tex11 = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(src_gs_tex));
    auto *dst_tex11 = static_cast<ID3D11Texture2D *>(gs_texture_get_obj(f->out_tex));

    Dlss5Params frame_params = f->params;
    if (f->reset_queued) {
        frame_params.force_reset = true;
        f->reset_queued = false;
    }

    bool ok = Dlss5_ProcessFrameD3D11(
        f->engine, dev11, src_tex11, dst_tex11, cx, cy, frame_params);

    // 4. Draw processed output (or fallback to upstream texture)
    DrawOutputTexture(ok ? f->out_tex : src_gs_tex, cx, cy);
}

static bool OnOptimalSettingsClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
    (void)props;
    (void)property;
    auto *f = static_cast<dlss5_filter_data *>(data);
    if (f && f->context) {
        obs_data_t *settings = obs_source_get_settings(f->context);
        if (settings) {
            ApplyOptimalSettingsToData(settings);
            dlss5_filter_update(f, settings);
            f->reset_queued = true;
            obs_data_set_string(settings, S_STATUS, Dlss5_GetInstanceStatus(f->engine));
            obs_data_release(settings);
        }
    }
    return true;
}

static bool OnResetHistoryClicked(obs_properties_t *props, obs_property_t *property, void *data)
{
    (void)props;
    (void)property;
    auto *f = static_cast<dlss5_filter_data *>(data);
    if (f) {
        f->reset_queued = true;
        if (f->context) {
            obs_data_t *settings = obs_source_get_settings(f->context);
            if (settings) {
                obs_data_set_string(settings, S_STATUS, Dlss5_GetInstanceStatus(f->engine));
                obs_data_release(settings);
            }
        }
    }
    return true;
}

static obs_properties_t *dlss5_filter_get_properties(void *data)
{
    auto *f = static_cast<dlss5_filter_data *>(data);
    obs_properties_t *props = obs_properties_create();

    if (f && f->context && f->engine) {
        obs_data_t *settings = obs_source_get_settings(f->context);
        if (settings) {
            obs_data_set_string(settings, S_STATUS, Dlss5_GetInstanceStatus(f->engine));
            obs_data_release(settings);
        }
    }

    obs_property_t *status_prop = obs_properties_add_text(
        props, S_STATUS, "Estado del Motor DLSS 5", OBS_TEXT_DEFAULT);
    obs_property_set_enabled(status_prop, false);

    obs_properties_add_bool(props, S_ENABLED, "Activar DLSS 5 Neural Rendering");

    // --- Neural Rendering Core ---
    obs_property_t *preset_list = obs_properties_add_list(
        props, S_PRESET, "Preset Neural (DLSS 5)", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(preset_list, "Preset A - Equilibrado (Original)", 0);
    obs_property_list_add_int(preset_list, "Preset B - Natural / Suave",        1);
    obs_property_list_add_int(preset_list, "Preset C - Cinematográfico",        2);

    obs_property_t *style_list = obs_properties_add_list(
        props, S_STYLE, "Estilo de Iluminación Neural", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(style_list, "Estilo 0 - Limpio / Neutro",    0);
    obs_property_list_add_int(style_list, "Estilo 1 - Realce Neural (PBR)", 1);
    obs_property_list_add_int(style_list, "Estilo 2 - Alto Rango Dinámico", 2);

    obs_properties_add_float_slider(props, S_INTENSITY,       "Intensidad Global",        0.0, 2.0, 0.05);
    obs_properties_add_float_slider(props, S_LOCAL_TONE,      "Tono Local (Iluminación)", 0.0, 2.0, 0.05);
    obs_properties_add_float_slider(props, S_LOCAL_STRUCTURE, "Estructura y Detalle",     0.0, 2.0, 0.05);
    obs_properties_add_float_slider(props, S_SKIN_STRUCTURE,  "Estructura de Piel/Caras", 0.0, 2.0, 0.05);

    obs_properties_add_bool(props, S_AUTO_MASK,     "Auto-Máscara Inteligente (Escena/Bordes)");
    obs_properties_add_bool(props, S_UI_CORRECTION, "Protección de Texto / HUD / Interfaz");

    // --- Performance & Boost ---
    obs_properties_add_int_slider(props, S_WORK_SCALE_PCT,
        "Escala de Resolución Neural (% Boost)", 50, 100, 5);
    obs_properties_add_bool(props, S_RESIDUAL_COMPOSITE,
        "Composición Residual (Preservar Nitidez Nativa)");
    obs_properties_add_float_slider(props, S_RESIDUAL_STRENGTH,
        "Fuerza de Inyección Residual", 0.0, 2.0, 0.05);
    obs_properties_add_int_slider(props, S_PASSES,
        "Capas Multi-Pass (1x - 4x)", 1, 4, 1);

    // --- Motion & Artifact Control ---
    obs_properties_add_bool(props, S_USE_NVOFA,
        "Optical Flow por Hardware (NVIDIA NVOFA)");
    obs_properties_add_float_slider(props, S_HALO_REDUCTION,
        "Reducción de Halos en Bordes", 0.0, 1.0, 0.05);

    // --- Post-Finish Enhancer ---
    obs_properties_add_float_slider(props, S_SHARPNESS,   "Nitidez Adaptativa (CAS)",     0.0,  1.0, 0.05);
    obs_properties_add_float_slider(props, S_EDGE_SMOOTH, "Suavizado de Bordes (Sobel)",  0.0,  1.0, 0.05);
    obs_properties_add_float_slider(props, S_BRIGHTNESS,  "Brillo",                      -0.5,  0.5, 0.02);
    obs_properties_add_float_slider(props, S_CONTRAST,    "Contraste",                   -0.5,  0.5, 0.02);
    obs_properties_add_float_slider(props, S_SATURATION,  "Saturación",                  -0.5,  0.5, 0.02);
    obs_properties_add_float_slider(props, S_WARMTH,      "Calidez (Temperatura Color)", -0.5,  0.5, 0.02);

    // --- Split Screen Comparison ---
    obs_properties_add_bool(props, S_SPLIT_ENABLE, "Modo Comparación Dividida (Antes / Después)");
    obs_properties_add_float_slider(props, S_SPLIT_POS, "Posición de Línea Divisoria", 0.05, 0.95, 0.01);

    obs_properties_add_button(props, S_OPTIMAL_BTN,
        "Aplicar Ajustes Óptimos (Calidad + 60 FPS)", OnOptimalSettingsClicked);

    obs_properties_add_button(props, S_RESET_BTN,
        "Reiniciar Historial Temporal / Actualizar Estado", OnResetHistoryClicked);

    return props;
}

bool obs_module_load(void)
{
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&obs_module_load),
        &hSelf);

    wchar_t mod_dir[MAX_PATH] = {};
    if (hSelf && GetModuleFileNameW(hSelf, mod_dir, MAX_PATH)) {
        wchar_t *slash = wcsrchr(mod_dir, L'\\');
        if (slash) *slash = 0;
    }

    Dlss5_SetConfig(mod_dir, ObsLoggerCallback);

    struct obs_source_info info = {};
    info.id             = "dlss5_neural_rendering_filter";
    info.type           = OBS_SOURCE_TYPE_FILTER;
    info.output_flags   = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
    info.get_name       = dlss5_filter_get_name;
    info.create         = dlss5_filter_create;
    info.destroy        = dlss5_filter_destroy;
    info.update         = dlss5_filter_update;
    info.get_defaults   = dlss5_filter_defaults;
    info.get_width      = dlss5_filter_get_width;
    info.get_height     = dlss5_filter_get_height;
    info.video_render   = dlss5_filter_video_render;
    info.get_properties = dlss5_filter_get_properties;

    obs_register_source(&info);
    blog(LOG_INFO, "[DLSS5-OBS] Plugin loaded successfully (dir: %ls)", mod_dir);
    return true;
}

void obs_module_unload(void)
{
    Dlss5_ShutdownGlobal();
    blog(LOG_INFO, "[DLSS5-OBS] Plugin unloaded.");
}
