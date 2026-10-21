# 🚀 DLSS 5 for OBS (`obs-dlss5-filter`)

[![Platform](https://img.shields.io/badge/Platform-Windows%20x64-0078D6?logo=windows)](https://github.com/montillarc/DLSS5-for-OBS)
[![OBS Studio](https://img.shields.io/badge/OBS%20Studio-29%2B%20%7C%2030%2B%20%7C%2032%2B-302E31?logo=obsstudio)](https://obsproject.com/)
[![GPU](https://img.shields.io/badge/GPU-NVIDIA%20RTX%2020%2F30%2F40%2F50%20Series-76B900?logo=nvidia)](https://www.nvidia.com/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

> **Plugin de filtro nativo para OBS Studio** que aplica **NVIDIA DLSS 5 Neural Rendering (`nvngx_dlssnr.dll`, Feature `18`) + Hardware Optical Flow (`NVOFA D3D12`)** en tiempo real y con **cero copias a RAM (100% GPU Zero-Copy)** directamente sobre cualquier fuente de video específica en OBS (capturadoras de consola/PC, captura de juego, cámaras, multimedia o escenas).

---

## ✨ Características Principales

- **🎯 Por Fuente Individual en OBS (`OBS_SOURCE_TYPE_FILTER`):** Aplica el renderizado neural exclusivamente a la fuente que elijas (ej. `Capturadora Game`) sin afectar tus overlays, alertas, webcam ni el resto de la escena.
- **⚡ Interop Asíncrono D3D11 ↔ D3D12 de Ultra Baja Latencia (< 0.1 ms en OBS):**
  - Comparte texturas en VRAM entre el motor Direct3D 11 de OBS y un motor Direct3D 12 nativo mediante `DXGI_RESOURCE_MISC_SHARED_NTHANDLE` y fences compartidos (`ID3D11Fence` ↔ `ID3D12Fence`).
  - Ejecuta **NVOFA + DLSS 5** en un **hilo trabajador D3D12 asíncrono con doble buffer de salida**, logrando que el hilo de renderizado de OBS nunca se bloquee esperando a la evaluación neural (**60/120 FPS estables**).
- **🎨 Espacio de Color sRGB / Lineal FP16 Nativo (`OBS_SOURCE_SRGB`):** Integración exacta con el pipeline de color lineal de OBS Studio 30/32 sin alterar la exposición ni lavar las sombras.
- **🌀 NVIDIA Optical Flow por Hardware (`NVOFA D3D12`):** Calcula vectores de movimiento reales en el silicio dedicado de la GPU RTX para alimentar la reconstrucción temporal de DLSS 5 incluso en fuentes de video externas (capturadoras HDMI/USB) que no proveen motion vectors del motor del juego.
- **🔓 Compatible con todas las GPUs NVIDIA RTX (Series 20, 30, 40 y 50):**
  - Soporta de forma nativa arquitecturas **Blackwell (RTX 50 Series)** y **Rubin**.
  - Incluye el *forwarder* `nvngx.dll_ns-forwarder.dll` y *arch-spoof* en memoria para habilitar `NVSDK_NGX_Feature_Reserved18` en GPUs **Turing (RTX 20)**, **Ampere (RTX 30)** y **Ada Lovelace (RTX 40)**.
- **🛠️ Pipeline Completo de Post-Procesado y Calidad:**
  - **Modo Boost + Composición Residual:** Ejecuta la red neural a escala optimizada (ej. `75%`) y reinyecta el detalle de alta frecuencia sobre la resolución nativa 100%.
  - **Filtro Bilateral Anti-Halos en 3 Pasos:** Elimina artefactos de borde en alto contraste guiado por vectores de movimiento.
  - **Post-Finish Enhancer:** Nitidez adaptativa **CAS (Contrast Adaptive Sharpening)** con clamp temporal, suavizado de bordes Sobel y etalonaje en vivo (Brillo, Contraste, Saturación, Calidez).
  - **Comparación Dividida (Split-Screen):** Modo *Antes / Después* interactivo en tiempo real dentro de OBS.

---

## 📋 Requisitos del Sistema

- **Sistema Operativo:** Windows 10 / Windows 11 (64-bit)
- **OBS Studio:** Versión 29.0 o superior (probado en **OBS Studio 32.x**)
- **Tarjeta Gráfica:** NVIDIA GeForce RTX Serie 20, 30, 40 o 50 con drivers actualizados
- **Renderizador en OBS:** Direct3D 11 (predeterminado en OBS para Windows)

---

## 🚀 Instalación Rápida (1 Clic)

1. Clona o descarga este repositorio:
   ```cmd
   git clone https://github.com/montillarc/DLSS5-for-OBS.git
   cd DLSS5-for-OBS
   ```
2. Ejecuta **`install.cmd`** (como Administrador si tu carpeta `C:\ProgramData\obs-studio` requiere permisos):
   ```cmd
   install.cmd
   ```
   > El script ensambla automáticamente el binario `nvngx_dlssnr.dll` desde `runtime/` e instala los 3 módulos (`obs-dlss5-filter.dll`, `nvngx.dll_ns-forwarder.dll` y `nvngx_dlssnr.dll`) en `C:\ProgramData\obs-studio\plugins\obs-dlss5-filter\bin\64bit\`.
3. Abre (o reinicia) **OBS Studio**.

---

## 🎮 Cómo Usarlo en OBS Studio

1. En OBS Studio, haz **clic derecho** sobre la fuente que deseas mejorar (por ejemplo, tu **Capturadora de Video** o **Captura de Juego**) y selecciona **Filtros**.
2. En la sección **Filtros de efecto**, haz clic en **`+`** y elige **`DLSS 5 Neural Rendering (RTX)`**.
3. El filtro viene preconfigurado con los **Ajustes Óptimos v2 (Calidad + 60 FPS)**:
   - **Preset Neural:** `Preset B - Natural / Suave`
   - **Estilo de Iluminación Neural:** `Estilo 1 - Realce Neural (PBR)`
   - **Tono Local (Iluminación):** `0.35` *(preserva la profundidad de las sombras)*
   - **Estructura y Detalle:** `1.35` *(realza micro-texturas de superficies)*
   - **Escala de Resolución Neural (% Boost):** `75%` con **Composición Residual (`1.15`)**
   - **Reducción de Halos:** `0.25`
   - **Nitidez Adaptativa (CAS):** `0.35`
4. Puedes activar la casilla **`Modo Comparación Dividida (Antes / Después)`** para comparar en tiempo real la imagen original (izquierda) contra la imagen procesada por DLSS 5 (derecha).

---

## 🛠️ Compilación desde el Código Fuente (Opcional)

El repositorio incluye todas las cabeceras y librerías de importación necesarias (`third_party/ngx` y `third_party/obs`). Para recompilar desde cero con **Visual Studio 2022 (Desktop development with C++)**:

```cmd
build.cmd
```

También incluye un ejecutable de diagnóstico y benchmark autónomo (`build\test_engine.exe`) que prueba la tubería completa D3D11 ↔ D3D12 + NVOFA + DLSS 5 fuera de OBS:

```cmd
build\test_engine.exe
```

---

## 📁 Estructura del Proyecto

```text
DLSS5-for-OBS/
├── src/
│   ├── obs_dlss5_filter.cpp     # Plugin nativo de filtro OBS (OBS_SOURCE_TYPE_FILTER, sRGB pipeline)
│   ├── dlss5_engine.h           # Interfaz pública del motor DLSS 5 D3D11 <-> D3D12
│   ├── dlss5_engine.cpp         # Motor asíncrono D3D12 + NGX Feature 18 + NVOFA + Compute Shaders
│   └── test_engine.cpp          # Benchmark y verificador autónomo en consola
├── third_party/
│   ├── ngx/                     # SDK NVIDIA NGX, NVOFA D3D12, ngx_forwarder y shaders HLSL
│   └── obs/                     # Cabeceras y librería de importación de libobs
├── runtime/                     # Binarios precompilados y partes de nvngx_dlssnr.dll (<100 MB p/GitHub)
├── build.cmd                    # Script de compilación (MSVC x64)
├── install.cmd                  # Instalador automático para OBS Studio
└── README.md
```

---

## 🌐 English Summary

**DLSS 5 for OBS (`obs-dlss5-filter`)** is a native Windows x64 filter plugin (`OBS_SOURCE_TYPE_FILTER`) for **OBS Studio** that brings real-time **NVIDIA DLSS 5 Neural Rendering (`nvngx_dlssnr.dll`, Feature `18`)** and **Hardware Optical Flow (`NVOFA D3D12`)** directly to any individual OBS source (capture cards, game capture, cameras, or media sources).

- **Zero-Copy GPU Pipeline:** Shares textures directly in VRAM between OBS's D3D11 device and an asynchronous D3D12 worker thread via NT shared handles and shared fences (< 0.1 ms render thread overhead).
- **Accurate Linear/sRGB Color Pipeline:** Uses `OBS_SOURCE_SRGB` and `GS_CS_SRGB` to preserve exact black levels and exposure in OBS 30/32+.
- **1-Click Install:** Run `install.cmd` and add the **`DLSS 5 Neural Rendering (RTX)`** effect filter to any source in OBS Studio.

---

## 🙏 Créditos / Credits

- Desarrollado por **[montillarc](https://github.com/montillarc)**.
- Basado en la investigación de interoperabilidad NGX Feature 18 (`nvngx_dlssnr.dll`) y shaders de post-procesamiento de **DLSS 5 Manager (NODIX-TECH)**.
