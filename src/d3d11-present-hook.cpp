#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <detours.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

constexpr std::uint32_t kSharedMagic = 0x4C474F43;
constexpr std::uint32_t kSharedVersion = 6;
constexpr std::uint32_t kFrameMagic = 0x52464843; // CHFR
constexpr std::uint32_t kFrameVersion = 1;

constexpr int kHudWidth = 145;
constexpr int kHudHeight = 50;
constexpr int kHudMargin = 12;
constexpr int kHudStride = kHudWidth * 4;
constexpr int kHudFrameBytes = kHudStride * kHudHeight;

enum DrawStage : LONG {
    DrawStageIdle = 0,
    DrawStageEligible = 10,
    DrawStageRendererEntry = 11,
    DrawStageRendererReady = 20,
    DrawStageViewportReady = 30,
    DrawStageStateCaptured = 40,
    DrawStageOverlayStateApplied = 50,
    DrawStageVerticesUploaded = 60,
    DrawStageDrawReturned = 70,
    DrawStageStateRestored = 80,
    DrawStageComplete = 90,
};

struct alignas(8) PresentShared {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t pid;
    volatile LONG liveFpsAck;
    volatile LONG64 presentCount;
    volatile LONG64 lastPresentTick;
    volatile LONG hookState;
    volatile LONG hookedMask;
    volatile LONG drawMarker;
    volatile LONG reservedControl;
    volatile LONG64 drawCount;
    volatile LONG64 lastDrawTick;
    volatile LONG renderMode;
    volatile LONG liveFpsInput;
    volatile LONG liveSessionSeconds;
    volatile LONG liveSessionSecondsAck;
    volatile LONG liveAudioLevels;
    volatile LONG liveAudioLevelsAck;
    volatile LONG liveHudStatus;
    volatile LONG liveHudStatusAck;
};

struct alignas(8) HudFrameShared {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t pid;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    volatile LONG sequence;
    volatile LONG activeIndex;
    std::uint8_t pixels[2][kHudFrameBytes];
};

struct HudVertex {
    float x;
    float y;
    float u;
    float v;
};

using PresentFn =
    HRESULT(STDMETHODCALLTYPE *)(
        IDXGISwapChain *,
        UINT,
        UINT);

PresentFn g_originalPresent = nullptr;
HANDLE g_controlMapping = nullptr;
PresentShared *g_shared = nullptr;
HANDLE g_frameMapping = nullptr;
HudFrameShared *g_frameShared = nullptr;

IDXGISwapChain *g_swapChainIdentity = nullptr;
ID3D11Device *g_device = nullptr;
ID3D11DeviceContext *g_context = nullptr;
ID3D11RenderTargetView *g_rtv = nullptr;
ID3D11VertexShader *g_vertexShader = nullptr;
ID3D11PixelShader *g_pixelShader = nullptr;
ID3D11InputLayout *g_inputLayout = nullptr;
ID3D11Buffer *g_vertexBuffer = nullptr;
ID3D11SamplerState *g_sampler = nullptr;
ID3D11BlendState *g_blendState = nullptr;
ID3D11DepthStencilState *g_depthState = nullptr;
ID3D11RasterizerState *g_rasterState = nullptr;
ID3D11Texture2D *g_hudTexture = nullptr;
ID3D11ShaderResourceView *g_hudSrv = nullptr;

UINT g_backBufferWidth = 0;
UINT g_backBufferHeight = 0;
LONG g_lastFrameSequence = -1;
std::vector<std::uint8_t> g_frameScratch(
    static_cast<size_t>(kHudFrameBytes));

thread_local bool g_insidePresent = false;

void SafeRelease(IUnknown *&value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

template <typename T>
void ReleasePtr(T *&value)
{
    if (value) {
        value->Release();
        value = nullptr;
    }
}

void SetDrawStage(LONG stage)
{
    if (g_shared)
        InterlockedExchange(
            &g_shared->reservedControl,
            stage);
}

bool CreateMappings()
{
    const DWORD pid = GetCurrentProcessId();

    wchar_t controlName[96] = {};
    swprintf_s(
        controlName,
        L"Local\\ClatashaHUD_OGL_%lu",
        pid);

    g_controlMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(PresentShared)),
        controlName);
    if (!g_controlMapping)
        return false;

    g_shared =
        static_cast<PresentShared *>(
            MapViewOfFile(
                g_controlMapping,
                FILE_MAP_READ | FILE_MAP_WRITE,
                0,
                0,
                sizeof(PresentShared)));
    if (!g_shared)
        return false;

    ZeroMemory(
        g_shared,
        sizeof(PresentShared));
    g_shared->magic = kSharedMagic;
    g_shared->version = kSharedVersion;
    g_shared->pid = pid;

    wchar_t frameName[96] = {};
    swprintf_s(
        frameName,
        L"Local\\ClatashaHUD_FRAME_%lu",
        pid);

    g_frameMapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(HudFrameShared)),
        frameName);
    if (!g_frameMapping)
        return false;

    g_frameShared =
        static_cast<HudFrameShared *>(
            MapViewOfFile(
                g_frameMapping,
                FILE_MAP_READ | FILE_MAP_WRITE,
                0,
                0,
                sizeof(HudFrameShared)));
    if (!g_frameShared)
        return false;

    ZeroMemory(
        g_frameShared,
        sizeof(HudFrameShared));
    g_frameShared->magic = kFrameMagic;
    g_frameShared->version = kFrameVersion;
    g_frameShared->pid = pid;
    g_frameShared->width = kHudWidth;
    g_frameShared->height = kHudHeight;
    g_frameShared->stride = kHudStride;

    return true;
}

void DestroyMappings()
{
    if (g_frameShared) {
        UnmapViewOfFile(g_frameShared);
        g_frameShared = nullptr;
    }
    if (g_frameMapping) {
        CloseHandle(g_frameMapping);
        g_frameMapping = nullptr;
    }

    if (g_shared) {
        UnmapViewOfFile(g_shared);
        g_shared = nullptr;
    }
    if (g_controlMapping) {
        CloseHandle(g_controlMapping);
        g_controlMapping = nullptr;
    }
}

void ReleaseRendererResources()
{
    ReleasePtr(g_hudSrv);
    ReleasePtr(g_hudTexture);
    ReleasePtr(g_rasterState);
    ReleasePtr(g_depthState);
    ReleasePtr(g_blendState);
    ReleasePtr(g_sampler);
    ReleasePtr(g_vertexBuffer);
    ReleasePtr(g_inputLayout);
    ReleasePtr(g_pixelShader);
    ReleasePtr(g_vertexShader);
    ReleasePtr(g_rtv);
    ReleasePtr(g_context);
    ReleasePtr(g_device);

    g_swapChainIdentity = nullptr;
    g_backBufferWidth = 0;
    g_backBufferHeight = 0;
    g_lastFrameSequence = -1;
}

bool CompileShader(
    const char *source,
    const char *entry,
    const char *target,
    ID3DBlob **blob)
{
    if (!source || !entry || !target || !blob)
        return false;

    *blob = nullptr;
    ID3DBlob *errors = nullptr;

    const HRESULT hr = D3DCompile(
        source,
        std::strlen(source),
        nullptr,
        nullptr,
        nullptr,
        entry,
        target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0,
        blob,
        &errors);

    if (errors)
        errors->Release();

    return SUCCEEDED(hr) && *blob;
}

bool CreatePipeline()
{
    if (!g_device)
        return false;

    static const char *kVertexShaderSource =
        "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
        "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
        "VSOut main(VSIn i) {"
        "  VSOut o;"
        "  o.pos = float4(i.pos, 0.0, 1.0);"
        "  o.uv = i.uv;"
        "  return o;"
        "}";

    static const char *kPixelShaderSource =
        "Texture2D hudTex : register(t0);"
        "SamplerState hudSampler : register(s0);"
        "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {"
        "  return hudTex.Sample(hudSampler, uv);"
        "}";

    ID3DBlob *vsBlob = nullptr;
    ID3DBlob *psBlob = nullptr;

    if (!CompileShader(
            kVertexShaderSource,
            "main",
            "vs_4_0",
            &vsBlob) ||
        !CompileShader(
            kPixelShaderSource,
            "main",
            "ps_4_0",
            &psBlob)) {
        if (vsBlob)
            vsBlob->Release();
        if (psBlob)
            psBlob->Release();
        return false;
    }

    HRESULT hr = g_device->CreateVertexShader(
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        nullptr,
        &g_vertexShader);
    if (FAILED(hr)) {
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    hr = g_device->CreatePixelShader(
        psBlob->GetBufferPointer(),
        psBlob->GetBufferSize(),
        nullptr,
        &g_pixelShader);
    if (FAILED(hr)) {
        vsBlob->Release();
        psBlob->Release();
        return false;
    }

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {
            "POSITION",
            0,
            DXGI_FORMAT_R32G32_FLOAT,
            0,
            0,
            D3D11_INPUT_PER_VERTEX_DATA,
            0,
        },
        {
            "TEXCOORD",
            0,
            DXGI_FORMAT_R32G32_FLOAT,
            0,
            8,
            D3D11_INPUT_PER_VERTEX_DATA,
            0,
        },
    };

    hr = g_device->CreateInputLayout(
        layout,
        static_cast<UINT>(
            std::size(layout)),
        vsBlob->GetBufferPointer(),
        vsBlob->GetBufferSize(),
        &g_inputLayout);

    vsBlob->Release();
    psBlob->Release();

    if (FAILED(hr))
        return false;

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth =
        static_cast<UINT>(
            sizeof(HudVertex) * 6);
    vbDesc.Usage = D3D11_USAGE_DYNAMIC;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = g_device->CreateBuffer(
        &vbDesc,
        nullptr,
        &g_vertexBuffer);
    if (FAILED(hr))
        return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter =
        D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD =
        D3D11_FLOAT32_MAX;

    hr = g_device->CreateSamplerState(
        &samplerDesc,
        &g_sampler);
    if (FAILED(hr))
        return false;

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend =
        D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend =
        D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp =
        D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha =
        D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha =
        D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha =
        D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = g_device->CreateBlendState(
        &blendDesc,
        &g_blendState);
    if (FAILED(hr))
        return false;

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;

    hr = g_device->CreateDepthStencilState(
        &depthDesc,
        &g_depthState);
    if (FAILED(hr))
        return false;

    D3D11_RASTERIZER_DESC rasterDesc{};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    rasterDesc.ScissorEnable = FALSE;
    rasterDesc.MultisampleEnable = TRUE;

    hr = g_device->CreateRasterizerState(
        &rasterDesc,
        &g_rasterState);
    if (FAILED(hr))
        return false;

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = kHudWidth;
    textureDesc.Height = kHudHeight;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format =
        DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage =
        D3D11_USAGE_DYNAMIC;
    textureDesc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE;
    textureDesc.CPUAccessFlags =
        D3D11_CPU_ACCESS_WRITE;

    hr = g_device->CreateTexture2D(
        &textureDesc,
        nullptr,
        &g_hudTexture);
    if (FAILED(hr))
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = textureDesc.Format;
    srvDesc.ViewDimension =
        D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    hr = g_device->CreateShaderResourceView(
        g_hudTexture,
        &srvDesc,
        &g_hudSrv);

    return SUCCEEDED(hr);
}

bool EnsureBackBuffer(
    IDXGISwapChain *swapChain)
{
    if (!swapChain)
        return false;

    ID3D11Device *device = nullptr;
    if (FAILED(
            swapChain->GetDevice(
                __uuidof(ID3D11Device),
                reinterpret_cast<void **>(&device))) ||
        !device) {
        return false;
    }

    if (device != g_device ||
        swapChain != g_swapChainIdentity) {
        ReleaseRendererResources();
        g_device = device;
        g_device->GetImmediateContext(&g_context);
        g_swapChainIdentity = swapChain;

        if (!g_context ||
            !CreatePipeline()) {
            return false;
        }
    } else {
        device->Release();
    }

    ID3D11Texture2D *backBuffer = nullptr;
    if (FAILED(
            swapChain->GetBuffer(
                0,
                __uuidof(ID3D11Texture2D),
                reinterpret_cast<void **>(&backBuffer))) ||
        !backBuffer) {
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    backBuffer->GetDesc(&desc);

    const bool needsRtv =
        !g_rtv ||
        desc.Width != g_backBufferWidth ||
        desc.Height != g_backBufferHeight;

    if (needsRtv) {
        ReleasePtr(g_rtv);
        if (FAILED(
                g_device->CreateRenderTargetView(
                    backBuffer,
                    nullptr,
                    &g_rtv))) {
            backBuffer->Release();
            return false;
        }

        g_backBufferWidth = desc.Width;
        g_backBufferHeight = desc.Height;
    }

    backBuffer->Release();

    return g_rtv &&
           g_backBufferWidth >=
               static_cast<UINT>(
                   kHudWidth + kHudMargin * 2) &&
           g_backBufferHeight >=
               static_cast<UINT>(
                   kHudHeight + kHudMargin * 2);
}

bool UpdateHudFrameTexture()
{
    if (!g_frameShared ||
        !g_context ||
        !g_hudTexture) {
        return false;
    }

    const LONG sequenceBefore =
        InterlockedCompareExchange(
            &g_frameShared->sequence,
            0,
            0);
    if (sequenceBefore <= 0 ||
        (sequenceBefore & 1) != 0) {
        return g_lastFrameSequence > 0;
    }

    if (sequenceBefore == g_lastFrameSequence)
        return true;

    const LONG activeIndex =
        InterlockedCompareExchange(
            &g_frameShared->activeIndex,
            0,
            0) &
        1;

    std::memcpy(
        g_frameScratch.data(),
        g_frameShared->pixels[activeIndex],
        kHudFrameBytes);

    MemoryBarrier();

    const LONG sequenceAfter =
        InterlockedCompareExchange(
            &g_frameShared->sequence,
            0,
            0);
    if (sequenceAfter != sequenceBefore ||
        (sequenceAfter & 1) != 0) {
        return g_lastFrameSequence > 0;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(
            g_context->Map(
                g_hudTexture,
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped))) {
        return false;
    }

    for (int y = 0; y < kHudHeight; ++y) {
        std::memcpy(
            static_cast<std::uint8_t *>(
                mapped.pData) +
                static_cast<size_t>(y) *
                    mapped.RowPitch,
            g_frameScratch.data() +
                static_cast<size_t>(y) *
                    kHudStride,
            kHudStride);
    }

    g_context->Unmap(
        g_hudTexture,
        0);
    g_lastFrameSequence = sequenceAfter;
    return true;
}

void AcknowledgeLiveState()
{
    if (!g_shared)
        return;

    const LONG fpsInput =
        InterlockedCompareExchange(
            &g_shared->liveFpsInput,
            0,
            0);
    InterlockedExchange(
        &g_shared->liveFpsAck,
        fpsInput);

    const LONG timer =
        InterlockedCompareExchange(
            &g_shared->liveSessionSeconds,
            0,
            0);
    InterlockedExchange(
        &g_shared->liveSessionSecondsAck,
        timer);

    const LONG audio =
        InterlockedCompareExchange(
            &g_shared->liveAudioLevels,
            0,
            0);
    InterlockedExchange(
        &g_shared->liveAudioLevelsAck,
        audio);

    const LONG status =
        InterlockedCompareExchange(
            &g_shared->liveHudStatus,
            0,
            0);
    InterlockedExchange(
        &g_shared->liveHudStatusAck,
        status);
}

void RecordPresent()
{
    if (!g_shared)
        return;

    InterlockedExchange(
        &g_shared->hookState,
        1);
    InterlockedExchange(
        &g_shared->hookedMask,
        1);
    InterlockedIncrement64(
        &g_shared->presentCount);
    InterlockedExchange64(
        &g_shared->lastPresentTick,
        static_cast<LONG64>(
            GetTickCount64()));

    AcknowledgeLiveState();
}

void DrawHud(
    IDXGISwapChain *swapChain)
{
    if (!g_shared ||
        InterlockedCompareExchange(
            &g_shared->drawMarker,
            0,
            0) == 0) {
        SetDrawStage(DrawStageIdle);
        return;
    }

    SetDrawStage(DrawStageEligible);
    SetDrawStage(DrawStageRendererEntry);

    if (!EnsureBackBuffer(swapChain) ||
        !UpdateHudFrameTexture()) {
        return;
    }

    SetDrawStage(DrawStageRendererReady);
    InterlockedExchange(
        &g_shared->renderMode,
        1);

    const float width =
        static_cast<float>(
            g_backBufferWidth);
    const float height =
        static_cast<float>(
            g_backBufferHeight);

    const float leftPx =
        width -
        static_cast<float>(kHudWidth) -
        static_cast<float>(kHudMargin);
    const float rightPx =
        leftPx +
        static_cast<float>(kHudWidth);
    const float topPx =
        static_cast<float>(kHudMargin);
    const float bottomPx =
        topPx +
        static_cast<float>(kHudHeight);

    const auto ndcX =
        [width](float px) {
            return px * 2.0f / width - 1.0f;
        };
    const auto ndcY =
        [height](float py) {
            return 1.0f - py * 2.0f / height;
        };

    const HudVertex vertices[6] = {
        {
            ndcX(leftPx),
            ndcY(bottomPx),
            0.0f,
            1.0f,
        },
        {
            ndcX(rightPx),
            ndcY(bottomPx),
            1.0f,
            1.0f,
        },
        {
            ndcX(rightPx),
            ndcY(topPx),
            1.0f,
            0.0f,
        },
        {
            ndcX(leftPx),
            ndcY(bottomPx),
            0.0f,
            1.0f,
        },
        {
            ndcX(rightPx),
            ndcY(topPx),
            1.0f,
            0.0f,
        },
        {
            ndcX(leftPx),
            ndcY(topPx),
            0.0f,
            0.0f,
        },
    };

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(
            g_context->Map(
                g_vertexBuffer,
                0,
                D3D11_MAP_WRITE_DISCARD,
                0,
                &mapped))) {
        return;
    }

    std::memcpy(
        mapped.pData,
        vertices,
        sizeof(vertices));
    g_context->Unmap(
        g_vertexBuffer,
        0);
    SetDrawStage(DrawStageVerticesUploaded);

    std::array<ID3D11RenderTargetView *, 8>
        oldRtvs{};
    ID3D11DepthStencilView *oldDsv = nullptr;
    g_context->OMGetRenderTargets(
        static_cast<UINT>(oldRtvs.size()),
        oldRtvs.data(),
        &oldDsv);

    ID3D11BlendState *oldBlend = nullptr;
    FLOAT oldBlendFactor[4] = {};
    UINT oldSampleMask = 0;
    g_context->OMGetBlendState(
        &oldBlend,
        oldBlendFactor,
        &oldSampleMask);

    ID3D11DepthStencilState *oldDepth = nullptr;
    UINT oldStencilRef = 0;
    g_context->OMGetDepthStencilState(
        &oldDepth,
        &oldStencilRef);

    ID3D11RasterizerState *oldRaster = nullptr;
    g_context->RSGetState(&oldRaster);

    UINT viewportCount =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_VIEWPORT,
               D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        oldViewports{};
    g_context->RSGetViewports(
        &viewportCount,
        oldViewports.data());

    UINT scissorCount =
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    std::array<D3D11_RECT,
               D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>
        oldScissors{};
    g_context->RSGetScissorRects(
        &scissorCount,
        oldScissors.data());

    ID3D11InputLayout *oldLayout = nullptr;
    g_context->IAGetInputLayout(
        &oldLayout);

    D3D11_PRIMITIVE_TOPOLOGY oldTopology{};
    g_context->IAGetPrimitiveTopology(
        &oldTopology);

    ID3D11Buffer *oldVertexBuffer = nullptr;
    UINT oldStride = 0;
    UINT oldOffset = 0;
    g_context->IAGetVertexBuffers(
        0,
        1,
        &oldVertexBuffer,
        &oldStride,
        &oldOffset);

    ID3D11VertexShader *oldVs = nullptr;
    ID3D11PixelShader *oldPs = nullptr;
    ID3D11GeometryShader *oldGs = nullptr;
    ID3D11HullShader *oldHs = nullptr;
    ID3D11DomainShader *oldDs = nullptr;

    g_context->VSGetShader(
        &oldVs,
        nullptr,
        nullptr);
    g_context->PSGetShader(
        &oldPs,
        nullptr,
        nullptr);
    g_context->GSGetShader(
        &oldGs,
        nullptr,
        nullptr);
    g_context->HSGetShader(
        &oldHs,
        nullptr,
        nullptr);
    g_context->DSGetShader(
        &oldDs,
        nullptr,
        nullptr);

    ID3D11ShaderResourceView *oldSrv = nullptr;
    g_context->PSGetShaderResources(
        0,
        1,
        &oldSrv);

    ID3D11SamplerState *oldSampler = nullptr;
    g_context->PSGetSamplers(
        0,
        1,
        &oldSampler);

    SetDrawStage(DrawStageStateCaptured);

    g_context->OMSetRenderTargets(
        1,
        &g_rtv,
        nullptr);

    const FLOAT blendFactor[4] = {
        0.0f,
        0.0f,
        0.0f,
        0.0f,
    };
    g_context->OMSetBlendState(
        g_blendState,
        blendFactor,
        0xFFFFFFFFu);
    g_context->OMSetDepthStencilState(
        g_depthState,
        0);
    g_context->RSSetState(
        g_rasterState);

    D3D11_VIEWPORT viewport{};
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    viewport.Width = width;
    viewport.Height = height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    g_context->RSSetViewports(
        1,
        &viewport);

    g_context->IASetInputLayout(
        g_inputLayout);
    g_context->IASetPrimitiveTopology(
        D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const UINT stride =
        sizeof(HudVertex);
    const UINT offset = 0;
    g_context->IASetVertexBuffers(
        0,
        1,
        &g_vertexBuffer,
        &stride,
        &offset);

    g_context->VSSetShader(
        g_vertexShader,
        nullptr,
        0);
    g_context->PSSetShader(
        g_pixelShader,
        nullptr,
        0);
    g_context->GSSetShader(
        nullptr,
        nullptr,
        0);
    g_context->HSSetShader(
        nullptr,
        nullptr,
        0);
    g_context->DSSetShader(
        nullptr,
        nullptr,
        0);

    g_context->PSSetShaderResources(
        0,
        1,
        &g_hudSrv);
    g_context->PSSetSamplers(
        0,
        1,
        &g_sampler);

    SetDrawStage(
        DrawStageOverlayStateApplied);

    g_context->Draw(6, 0);
    SetDrawStage(DrawStageDrawReturned);

    ID3D11ShaderResourceView *nullSrv = nullptr;
    g_context->PSSetShaderResources(
        0,
        1,
        &nullSrv);

    g_context->OMSetRenderTargets(
        static_cast<UINT>(oldRtvs.size()),
        oldRtvs.data(),
        oldDsv);
    g_context->OMSetBlendState(
        oldBlend,
        oldBlendFactor,
        oldSampleMask);
    g_context->OMSetDepthStencilState(
        oldDepth,
        oldStencilRef);
    g_context->RSSetState(
        oldRaster);

    if (viewportCount > 0) {
        g_context->RSSetViewports(
            viewportCount,
            oldViewports.data());
    }
    if (scissorCount > 0) {
        g_context->RSSetScissorRects(
            scissorCount,
            oldScissors.data());
    }

    g_context->IASetInputLayout(
        oldLayout);
    g_context->IASetPrimitiveTopology(
        oldTopology);
    g_context->IASetVertexBuffers(
        0,
        1,
        &oldVertexBuffer,
        &oldStride,
        &oldOffset);

    g_context->VSSetShader(
        oldVs,
        nullptr,
        0);
    g_context->PSSetShader(
        oldPs,
        nullptr,
        0);
    g_context->GSSetShader(
        oldGs,
        nullptr,
        0);
    g_context->HSSetShader(
        oldHs,
        nullptr,
        0);
    g_context->DSSetShader(
        oldDs,
        nullptr,
        0);

    g_context->PSSetShaderResources(
        0,
        1,
        &oldSrv);
    g_context->PSSetSamplers(
        0,
        1,
        &oldSampler);

    for (auto *rtv : oldRtvs) {
        if (rtv)
            rtv->Release();
    }
    if (oldDsv)
        oldDsv->Release();
    if (oldBlend)
        oldBlend->Release();
    if (oldDepth)
        oldDepth->Release();
    if (oldRaster)
        oldRaster->Release();
    if (oldLayout)
        oldLayout->Release();
    if (oldVertexBuffer)
        oldVertexBuffer->Release();
    if (oldVs)
        oldVs->Release();
    if (oldPs)
        oldPs->Release();
    if (oldGs)
        oldGs->Release();
    if (oldHs)
        oldHs->Release();
    if (oldDs)
        oldDs->Release();
    if (oldSrv)
        oldSrv->Release();
    if (oldSampler)
        oldSampler->Release();

    SetDrawStage(DrawStageStateRestored);

    InterlockedIncrement64(
        &g_shared->drawCount);
    InterlockedExchange64(
        &g_shared->lastDrawTick,
        static_cast<LONG64>(
            GetTickCount64()));
    SetDrawStage(DrawStageComplete);
}

HRESULT STDMETHODCALLTYPE HookPresent(
    IDXGISwapChain *swapChain,
    UINT syncInterval,
    UINT flags)
{
    if (g_insidePresent)
        return g_originalPresent(
            swapChain,
            syncInterval,
            flags);

    g_insidePresent = true;

    RecordPresent();
    DrawHud(swapChain);

    const HRESULT result =
        g_originalPresent(
            swapChain,
            syncInterval,
            flags);

    g_insidePresent = false;
    return result;
}

LRESULT CALLBACK DummyWindowProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam)
{
    return DefWindowProcW(
        hwnd,
        message,
        wParam,
        lParam);
}

bool ResolvePresentAddress(
    PresentFn *presentAddress)
{
    if (!presentAddress)
        return false;

    *presentAddress = nullptr;

    HINSTANCE instance =
        GetModuleHandleW(nullptr);

    const wchar_t *className =
        L"ClatashaHudD3D11Probe";

    WNDCLASSW wc{};
    wc.lpfnWndProc = DummyWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = className;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        className,
        L"",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        32,
        32,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!hwnd)
        return false;

    DXGI_SWAP_CHAIN_DESC swapDesc{};
    swapDesc.BufferCount = 1;
    swapDesc.BufferDesc.Width = 32;
    swapDesc.BufferDesc.Height = 32;
    swapDesc.BufferDesc.Format =
        DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.BufferUsage =
        DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.OutputWindow = hwnd;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.Windowed = TRUE;
    swapDesc.SwapEffect =
        DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    IDXGISwapChain *swapChain = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL createdLevel{};

    HRESULT hr =
        D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            featureLevels,
            static_cast<UINT>(
                std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &swapDesc,
            &swapChain,
            &device,
            &createdLevel,
            &context);

    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            0,
            featureLevels,
            static_cast<UINT>(
                std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &swapDesc,
            &swapChain,
            &device,
            &createdLevel,
            &context);
    }

    if (SUCCEEDED(hr) &&
        swapChain) {
        void **vtable =
            *reinterpret_cast<void ***>(
                swapChain);
        *presentAddress =
            reinterpret_cast<PresentFn>(
                vtable[8]);
    }

    if (context)
        context->Release();
    if (device)
        device->Release();
    if (swapChain)
        swapChain->Release();

    DestroyWindow(hwnd);
    UnregisterClassW(
        className,
        instance);

    return *presentAddress != nullptr;
}

DWORD WINAPI BootstrapThread(LPVOID)
{
    PresentFn presentAddress = nullptr;
    if (!ResolvePresentAddress(
            &presentAddress)) {
        if (g_shared)
            InterlockedExchange(
                &g_shared->hookState,
                2);
        return 0;
    }

    g_originalPresent = presentAddress;

    DetourTransactionBegin();
    DetourUpdateThread(
        GetCurrentThread());
    const LONG attachResult =
        DetourAttach(
            reinterpret_cast<PVOID *>(
                &g_originalPresent),
            HookPresent);
    const LONG commitResult =
        attachResult == NO_ERROR
            ? DetourTransactionCommit()
            : (DetourTransactionAbort(),
               attachResult);

    if (commitResult != NO_ERROR &&
        g_shared) {
        InterlockedExchange(
            &g_shared->hookState,
            2);
    }

    return 0;
}

} // namespace

BOOL WINAPI DllMain(
    HINSTANCE instance,
    DWORD reason,
    LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);

        if (!CreateMappings())
            return FALSE;

        HANDLE thread = CreateThread(
            nullptr,
            0,
            BootstrapThread,
            nullptr,
            0,
            nullptr);
        if (thread)
            CloseHandle(thread);
    } else if (
        reason == DLL_PROCESS_DETACH) {
        ReleaseRendererResources();
        DestroyMappings();
    }

    return TRUE;
}
