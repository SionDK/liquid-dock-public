#pragma once
#include "common.h"
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// Constant buffer shared by every pass (must match HLSL cbuffer CB).
struct GlassCB {
    float rect[4];   // x, y, w, h  (window pixels)
    float shape[4];  // radius, bezel width, refraction px, opacity
    float look[4];   // chroma, tint amount, saturation, highlight
    float misc[4];   // tex w, tex h, shadow alpha, shadow radius
    float blur[4];   // texel dx, texel dy, sigma (texels), taps
    float tint[4];   // tint rgb, brightness
};

class Gfx {
public:
    bool Init();
    bool Reset();                 // re-create after a GPU reset / driver update
    bool DeviceLost() const;

    ComPtr<ID3D11Device>          d3d;
    ComPtr<ID3D11DeviceContext>   ctx;
    ComPtr<IDXGIDevice>           dxgi;
    ComPtr<IDXGIFactory2>         factory;
    ComPtr<ID2D1Factory3>         d2dFactory;
    ComPtr<ID2D1Device2>          d2dDevice;
    ComPtr<ID2D1DeviceContext2>   dc;
    ComPtr<IDWriteFactory>        dw;
    ComPtr<IDCompositionDevice>   dcomp;
    ComPtr<IWICImagingFactory>    wic;

    ComPtr<ID3D11VertexShader>    vs;
    ComPtr<ID3D11PixelShader>     psDown, psBlur, psGlass, psLum, psRecord;
    ComPtr<ID3D11SamplerState>    sampLinear;
    ComPtr<ID3D11Buffer>          cb;

    // text
    ComPtr<IDWriteTextFormat>     fmtTooltip, fmtClock, fmtClockSmall, fmtGlyph, fmtTiny;
    // the "PC 상태" panel (dock_sensors.cpp): its title, small dim text (card titles, models, footer), the rows'
    // text, values set right (R), the drives' bold temperatures and the cards' big temperature. The leading
    // small and row formats end a text that does not fit with an ellipsis.
    ComPtr<IDWriteTextFormat>     fmtPanelTitle, fmtPanelSmall, fmtPanelSmallR, fmtPanelText, fmtPanelTextR,
                                  fmtPanelTextBoldR, fmtPanelTemp;
    // Dedicated clock formats: changing the clock must not resize sensor/panel text.
    ComPtr<IDWriteTextFormat>     fmtTime, fmtDate, fmtTimeLarge, fmtDateLarge;
    ComPtr<ID3D11BlendState>      blendOver;   // premultiplied "over"
    float fontScale = 0;
    void EnsureFonts(float scale);
    struct FontSet { float scale; ComPtr<IDWriteTextFormat> f[16]; };
    std::vector<FontSet> fontSets;

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{ nullptr };

    // A linear gradient brush with these stops, from `from` to `to`. Realizing the stops makes a texture on
    // the GPU (about 0.1 ms, every frame, for the Start icon and as much for the OLED button), so the brushes
    // of the last few stop sets are kept: an icon whose colours stay as they were reuses its brush. Valid
    // until the next call; nullptr if it cannot be made.
    ID2D1LinearGradientBrush* GradientBrush(const D2D1_GRADIENT_STOP* stops, UINT32 count, D2D1_POINT_2F from, D2D1_POINT_2F to);
    struct Gradient { std::vector<D2D1_GRADIENT_STOP> stops; ComPtr<ID2D1LinearGradientBrush> brush; uint64_t used = 0; };
    std::vector<Gradient> gradients;   // device resources: dropped by Reset
    uint64_t gradientUses = 0;
    int gradientsMade = 0;             // stop collections realized (tests count them)

    void UpdateCB(const GlassCB& c);
    void FullscreenPass(ID3D11RenderTargetView* rtv, int w, int h, ID3D11PixelShader* ps,
                        ID3D11ShaderResourceView* srv0, ID3D11ShaderResourceView* srv1, bool blend = false);
};

extern Gfx g_gfx;

// Render target + SRV texture helper.
struct RTTexture {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
    bool Create(int w, int h, bool renderTarget);
};
