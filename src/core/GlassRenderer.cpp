#include "core/GlassRenderer.h"
#include "core/Log.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {

// Full-screen triangle; uv 0..1 with y down.
const char* kVS = R"(
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO main(uint id : SV_VertexID) {
    VO o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
}
)";

// Gradient mesh: four soft blobs on slow closed paths (integer frequencies of one 45 s loop),
// leaning towards the cursor, plus a vignette and film grain so the gradients don't band.
const char* kBackgroundPS = R"(
cbuffer P : register(b0) {
    float2 res; float time; float grain;
    float4 c0; float4 c1; float4 c2; float4 c3;
    float4 base;
    float2 mouse; float mouseAmt; float alpha;
    float solid; float3 pad;
};
float hash(float2 p) { p = frac(p * float2(123.34, 456.21)); p += dot(p, p + 45.32); return frac(p.x * p.y); }
float blob(float2 p, float2 c, float r) { float2 d = p - c; return exp(-dot(d, d) / (r * r)); }
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float aspect = res.x / res.y;
    float2 p = float2(uv.x * aspect, uv.y);
    float2 m = float2(mouse.x * aspect, mouse.y);
    float t = time * 6.2831853 / 45.0;
    float3 col = base.rgb;
    if (solid < 0.5) {
        float2 k0 = float2(aspect * (0.22 + 0.16 * sin(t + 0.3)),        0.28 + 0.18 * cos(t * 2.0 + 1.1));
        float2 k1 = float2(aspect * (0.78 + 0.14 * cos(t + 2.0)),        0.30 + 0.20 * sin(t + 0.7));
        float2 k2 = float2(aspect * (0.62 + 0.20 * sin(t * 2.0 + 4.0)),  0.82 + 0.12 * cos(t + 2.6));
        float2 k3 = float2(aspect * (0.20 + 0.12 * cos(t * 3.0 + 1.9)),  0.78 + 0.14 * sin(t * 2.0 + 5.1));
        k0 += (m - k0) * mouseAmt; k1 += (m - k1) * mouseAmt; k2 += (m - k2) * mouseAmt; k3 += (m - k3) * mouseAmt;
        float r0 = 0.50 + 0.07 * sin(t * 2.0 + 0.4), r1 = 0.44 + 0.06 * cos(t + 1.3);
        float r2 = 0.46 + 0.07 * sin(t + 2.2),       r3 = 0.38 + 0.05 * cos(t * 3.0 + 0.9);
        col += c0.rgb * c0.a * blob(p, k0, r0);
        col += c1.rgb * c1.a * blob(p, k1, r1);
        col += c2.rgb * c2.a * blob(p, k2, r2);
        col += c3.rgb * c3.a * blob(p, k3, r3);
        col = 1.0 - exp(-col * 1.35);   // soft shoulder: overlapping blobs glow instead of clipping
    } else {
        col = lerp(base.rgb, c0.rgb, 0.10 + 0.06 * (1.0 - uv.y));
    }
    float2 v = uv - 0.5;
    col *= 1.0 - 0.35 * dot(v, v);
    col += (hash(pos.xy + frac(time * 7.0) * 61.0) - 0.5) * grain;
    return float4(saturate(col), alpha);
}
)";

// Dual-Kawase down and up samples. `sat` is applied on the passes that produce a final texture.
const char* kDownPS = R"(
Texture2D src : register(t0); SamplerState smp : register(s0);
cbuffer P : register(b0) { float2 hp; float offset; float sat; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 o = hp * offset;
    float4 s = src.Sample(smp, uv) * 4.0;
    s += src.Sample(smp, uv - o);
    s += src.Sample(smp, uv + o);
    s += src.Sample(smp, uv + float2(o.x, -o.y));
    s += src.Sample(smp, uv - float2(o.x, -o.y));
    return s / 8.0;
}
)";
const char* kUpPS = R"(
Texture2D src : register(t0); SamplerState smp : register(s0);
cbuffer P : register(b0) { float2 hp; float offset; float sat; };
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 o = hp * offset;
    float4 s = src.Sample(smp, uv + float2(-o.x * 2.0, 0.0));
    s += src.Sample(smp, uv + float2(-o.x, o.y)) * 2.0;
    s += src.Sample(smp, uv + float2(0.0, o.y * 2.0));
    s += src.Sample(smp, uv + float2(o.x, o.y)) * 2.0;
    s += src.Sample(smp, uv + float2(o.x * 2.0, 0.0));
    s += src.Sample(smp, uv + float2(o.x, -o.y)) * 2.0;
    s += src.Sample(smp, uv + float2(0.0, -o.y * 2.0));
    s += src.Sample(smp, uv + float2(-o.x, -o.y)) * 2.0;
    s /= 12.0;
    float l = dot(s.rgb, float3(0.299, 0.587, 0.114));
    s.rgb = max(lerp(l.xxx, s.rgb, sat), 0.0);
    s.a = 1.0;
    return s;
}
)";

struct BgConstants {
    float res[2], time, grain;
    float c[4][4];
    float base[4];
    float mouse[2], mouseAmt, alpha;
    float solid, pad[3];
};
static_assert(sizeof(BgConstants) % 16 == 0, "constant buffer size");

struct BlurConstants {
    float hp[2], offset, sat;
};

struct Target {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    int w = 0, h = 0;
};

ComPtr<ID3DBlob> Compile(const char* src, const char* target)
{
    ComPtr<ID3DBlob> code, errors;
    const HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        Log("Glass shader compile failed (0x%08lx): %s", (unsigned long)hr, errors ? (const char*)errors->GetBufferPointer() : "");
        return nullptr;
    }
    return code;
}

void Mix3(const float a[3], const float b[3], float t, float out[4], float strength)
{
    for (int i = 0; i < 3; ++i) out[i] = a[i] + (b[i] - a[i]) * t;
    out[3] = strength;
}

} // namespace

struct GlassRenderer::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> bgPS, downPS, upPS;
    ComPtr<ID3D11Buffer> bgCB, blurCB;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> noBlend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11DepthStencilState> noDepth;
    bool ok = false;
    int width = 0, height = 0;
    Target bg;
    Target down[4];          // 1/2, 1/4, 1/8, 1/16
    Target blurred;          // 1/4, from 1/8
    Target strong;           // 1/8, from 1/16
    double lastTime = -1.0;
    float lastParams[16] = {};
    bool dirty = true;

    bool MakeTarget(Target& t, int w, int h, DXGI_FORMAT fmt)
    {
        t = Target{};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)std::max(1, w);
        td.Height = (UINT)std::max(1, h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &t.tex))) return false;
        if (FAILED(device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv))) return false;
        if (FAILED(device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv))) return false;
        t.w = (int)td.Width;
        t.h = (int)td.Height;
        return true;
    }

    void Pass(Target& dst, ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, int srcW, int srcH, float offset, float sat)
    {
        if (src) {
            BlurConstants c{ { 0.5f / (float)srcW, 0.5f / (float)srcH }, offset, sat };
            ctx->UpdateSubresource(blurCB.Get(), 0, nullptr, &c, 0, 0);
            ctx->PSSetConstantBuffers(0, 1, blurCB.GetAddressOf());
        }
        ID3D11ShaderResourceView* none = nullptr;
        ctx->PSSetShaderResources(0, 1, &none);
        ID3D11RenderTargetView* rtv = dst.rtv.Get();
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)dst.w, (float)dst.h, 0.0f, 1.0f };
        ctx->RSSetViewports(1, &vp);
        ctx->PSSetShader(ps, nullptr, 0);
        if (src) ctx->PSSetShaderResources(0, 1, &src);
        ctx->Draw(3, 0);
    }
};

GlassRenderer::GlassRenderer() : d_(new Impl) {}
GlassRenderer::~GlassRenderer() { delete d_; }

bool GlassRenderer::Init(ID3D11Device* device, ID3D11DeviceContext* context)
{
    Impl& d = *d_;
    d.device = device;
    d.ctx = context;
    d.ok = false;
    if (!device || !context) return false;
    auto vs = Compile(kVS, "vs_4_0");
    auto bg = Compile(kBackgroundPS, "ps_4_0");
    auto dn = Compile(kDownPS, "ps_4_0");
    auto up = Compile(kUpPS, "ps_4_0");
    if (!vs || !bg || !dn || !up) return false;
    if (FAILED(device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &d.vs))) return false;
    if (FAILED(device->CreatePixelShader(bg->GetBufferPointer(), bg->GetBufferSize(), nullptr, &d.bgPS))) return false;
    if (FAILED(device->CreatePixelShader(dn->GetBufferPointer(), dn->GetBufferSize(), nullptr, &d.downPS))) return false;
    if (FAILED(device->CreatePixelShader(up->GetBufferPointer(), up->GetBufferSize(), nullptr, &d.upPS))) return false;

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.ByteWidth = sizeof(BgConstants);
    if (FAILED(device->CreateBuffer(&bd, nullptr, &d.bgCB))) return false;
    bd.ByteWidth = 16;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &d.blurCB))) return false;

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&sd, &d.sampler))) return false;

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = FALSE;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bl, &d.noBlend))) return false;
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &d.raster))) return false;
    D3D11_DEPTH_STENCIL_DESC ds{};
    ds.DepthEnable = FALSE;
    ds.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&ds, &d.noDepth))) return false;
    d.ok = true;
    Log("Glass renderer ready (GPU background + blur)");
    return true;
}

void GlassRenderer::Prepare(int width, int height)
{
    Impl& d = *d_;
    if (!d.ok || width <= 0 || height <= 0) return;
    if (width == d.width && height == d.height && d.bg.srv) return;
    d.width = width;
    d.height = height;
    bool good = d.MakeTarget(d.bg, width, height, DXGI_FORMAT_R8G8B8A8_UNORM);
    int w = width, h = height;
    for (int i = 0; i < 4 && good; ++i) {
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
        good = d.MakeTarget(d.down[i], w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    }
    if (good) good = d.MakeTarget(d.blurred, d.down[1].w, d.down[1].h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (good) good = d.MakeTarget(d.strong, d.down[2].w, d.down[2].h, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!good) {
        Log("Glass renderer: couldn't create %dx%d textures; using the simple backdrop", width, height);
        d.ok = false;
        return;
    }
    d.dirty = true;
}

void GlassRenderer::Render(const GlassFrameParams& p)
{
    Impl& d = *d_;
    if (!d.ok || !d.bg.srv) return;
    // Redraw at most 30 times a second, and only when something changed.
    const float key[16] = { 0, 0, p.accent[0], p.accent[1], p.accent[2], p.accent2[0], p.accent2[1], p.accent2[2],
                            (float)p.style, p.alpha, p.saturation, p.mouseAmount, 0, 0, 0, 0 };
    const bool paramsChanged = memcmp(key, d.lastParams, sizeof(key)) != 0;
    const bool animate = p.moving && p.style == 0;
    if (!d.dirty && !paramsChanged && (!animate || p.time - d.lastTime < 1.0 / 30.0)) return;
    if (!d.dirty && !paramsChanged && !animate) return;
    memcpy(d.lastParams, key, sizeof(key));
    d.lastTime = p.time;
    d.dirty = false;

    BgConstants c{};
    c.res[0] = (float)d.bg.w;
    c.res[1] = (float)d.bg.h;
    c.time = (float)(p.style == 0 ? std::fmod(p.time, 45.0 * 64.0) : 8.0);
    c.grain = 0.025f;
    const float violet[3] = { 0.545f, 0.361f, 1.0f }, blue[3] = { 0.231f, 0.510f, 0.965f }, teal[3] = { 0.078f, 0.722f, 0.651f };
    // Blob colours follow the accent: accent, accent towards blue, teal, accent2.
    Mix3(p.accent, violet, 0.25f, c.c[0], 0.80f);
    Mix3(p.accent, blue, 0.70f, c.c[1], 0.70f);
    Mix3(teal, p.accent, 0.15f, c.c[2], 0.55f);
    Mix3(p.accent2, p.accent2, 0.0f, c.c[3], 0.65f);
    c.base[0] = 0.043f;
    c.base[1] = 0.024f;
    c.base[2] = 0.078f;
    c.base[3] = 1.0f;
    c.mouse[0] = p.mouse[0];
    c.mouse[1] = p.mouse[1];
    c.mouseAmt = p.style == 0 ? p.mouseAmount : 0.0f;
    c.alpha = p.alpha;
    c.solid = p.style == 2 ? 1.0f : 0.0f;
    d.ctx->UpdateSubresource(d.bgCB.Get(), 0, nullptr, &c, 0, 0);

    // Our own pipeline state (ImGui's backend sets everything it needs again when it draws).
    d.ctx->IASetInputLayout(nullptr);
    d.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d.ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    d.ctx->VSSetShader(d.vs.Get(), nullptr, 0);
    d.ctx->GSSetShader(nullptr, nullptr, 0);
    d.ctx->HSSetShader(nullptr, nullptr, 0);
    d.ctx->DSSetShader(nullptr, nullptr, 0);
    d.ctx->CSSetShader(nullptr, nullptr, 0);
    const float bf[4] = {};
    d.ctx->OMSetBlendState(d.noBlend.Get(), bf, 0xffffffff);
    d.ctx->OMSetDepthStencilState(d.noDepth.Get(), 0);
    d.ctx->RSSetState(d.raster.Get());
    d.ctx->PSSetSamplers(0, 1, d.sampler.GetAddressOf());

    d.ctx->PSSetConstantBuffers(0, 1, d.bgCB.GetAddressOf());
    d.Pass(d.bg, d.bgPS.Get(), nullptr, 0, 0, 0, 1);
    d.Pass(d.down[0], d.downPS.Get(), d.bg.srv.Get(), d.bg.w, d.bg.h, 1.0f, 1.0f);
    for (int i = 1; i < 4; ++i) d.Pass(d.down[i], d.downPS.Get(), d.down[i - 1].srv.Get(), d.down[i - 1].w, d.down[i - 1].h, 1.5f, 1.0f);
    d.Pass(d.blurred, d.upPS.Get(), d.down[2].srv.Get(), d.down[2].w, d.down[2].h, 1.5f, p.saturation);
    d.Pass(d.strong, d.upPS.Get(), d.down[3].srv.Get(), d.down[3].w, d.down[3].h, 1.5f, p.saturation);

    ID3D11ShaderResourceView* none = nullptr;
    d.ctx->PSSetShaderResources(0, 1, &none);
    d.ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

bool GlassRenderer::Ok() const { return d_->ok && d_->bg.srv; }
uint64_t GlassRenderer::Background() const { return Ok() ? (uint64_t)(uintptr_t)d_->bg.srv.Get() : 0; }
uint64_t GlassRenderer::Blurred() const { return Ok() ? (uint64_t)(uintptr_t)d_->blurred.srv.Get() : 0; }
uint64_t GlassRenderer::BlurredStrong() const { return Ok() ? (uint64_t)(uintptr_t)d_->strong.srv.Get() : 0; }

} // namespace cs
