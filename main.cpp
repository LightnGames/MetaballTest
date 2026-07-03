//----------------------------------------------------------------------------
// 2D Metaball サンプル (Direct3D 12)
//
// 参考: https://techblog.kayac.com/unity_advent_calendar_2018_22
//
// 構成 (記事の Unity 実装を D3D12 に移植):
//   Pass 1: メタボール2個を Quad として加算ブレンドでオフスクリーンRTに描画
//           (記事の MetaBallCamera + RenderTexture に相当)
//   Pass 2: 蓄積結果にしきい値処理をかけてバックバッファへ描画
//           (記事の MetaballRenderer に相当)
//
// 操作: マウスホイール回転で2つのメタボールの距離を変更 / ESCで終了
//----------------------------------------------------------------------------

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>
#include <cstdio>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

//----------------------------------------------------------------------------
// 定数
//----------------------------------------------------------------------------
static const UINT  kWidth       = 1280;
static const UINT  kHeight      = 720;
static const UINT  kFrameCount  = 2;
static const DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
// 蓄積用RT。加算合成で1.0を超えても飽和しないよう浮動小数点フォーマットにする
static const DXGI_FORMAT kAccumFormat      = DXGI_FORMAT_R16G16B16A16_FLOAT;

// シェーダーの cbuffer Params と一致させること
struct Params
{
    float distance;        // メタボール中心間の距離 (スライダー1)
    float aspect;          // 幅/高さ
    float quadHalf;        // パーティクルQuadの半径 (フィールドの有効範囲)
    float ballRadius;      // 単独ボールの表面半径
    float blend;           // ブレンド距離 (スライダー2)
    float particleCutoff;  // 記事の _Cutoff (パーティクル側)
    float cutoff;          // 記事の _Cutoff (しきい値側)
    float stroke;          // 記事の _Stroke
    float screenW;         // 画面サイズ (UI描画用)
    float screenH;
    float dist01;          // スライダー1のつまみ位置 [0,1]
    float blend01;         // スライダー2のつまみ位置 [0,1]
};

//----------------------------------------------------------------------------
// グローバル状態
//----------------------------------------------------------------------------
static HWND                        gHwnd = nullptr;
static ComPtr<ID3D12Device>        gDevice;
static ComPtr<ID3D12CommandQueue>  gQueue;
static ComPtr<IDXGISwapChain3>     gSwapChain;
static ComPtr<ID3D12DescriptorHeap> gRtvHeap;   // [0..1]=バックバッファ, [2]=蓄積RT
static ComPtr<ID3D12DescriptorHeap> gSrvHeap;   // [0]=蓄積RTのSRV
static UINT                        gRtvStride = 0;
static ComPtr<ID3D12Resource>      gBackBuffers[kFrameCount];
static ComPtr<ID3D12Resource>      gAccumTex;
static ComPtr<ID3D12CommandAllocator>    gCmdAlloc;
static ComPtr<ID3D12GraphicsCommandList> gCmdList;
static ComPtr<ID3D12RootSignature> gRootSig;
static ComPtr<ID3D12PipelineState> gPsoParticle;  // Pass1 (加算ブレンド)
static ComPtr<ID3D12PipelineState> gPsoThreshold; // Pass2
static ComPtr<ID3D12PipelineState> gPsoUi;        // Pass3 (スライダーUI)
static ComPtr<ID3D12Fence>         gFence;
static UINT64                      gFenceValue = 0;
static HANDLE                      gFenceEvent = nullptr;
static UINT                        gFrameIndex = 0;

// スライダーで操作するパラメーター
static float gDistance = 0.55f;              // スライダー1: メタボール間の距離
static const float kDistanceMax = 1.6f;      // (最小は 0)
static float gBlend = 0.25f;                 // スライダー2: ブレンド距離
static const float kBlendMin = 0.08f;
static const float kBlendMax = 0.6f;

// スライダーUIの配置 (shaders.hlsl の定数と一致させること)
static const int kTrackLen = 220; // トラックの長さ (px)
static const int kTrackPad = 40;  // 画面右端からの余白 (px)
static const int kSlider1Y = 40;  // スライダー1 (距離) のY座標
static const int kSlider2Y = 80;  // スライダー2 (ブレンド距離) のY座標
static const int kHitHalfH = 16;  // ヒットテストの上下半幅
static int gDrag = 0;             // 0=なし, 1=スライダー1, 2=スライダー2

//----------------------------------------------------------------------------
static void ThrowIfFailed(HRESULT hr, const char* msg)
{
    if (FAILED(hr))
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (hr=0x%08X)", msg, (unsigned)hr);
        MessageBoxA(nullptr, buf, "D3D12 Metaball - Error", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
}

static void UpdateTitle()
{
    wchar_t title[128];
    swprintf_s(title, L"D3D12 2D Metaball - distance: %.2f / blend: %.2f (スライダーで操作)",
               gDistance, gBlend);
    SetWindowTextW(gHwnd, title);
}

//----------------------------------------------------------------------------
// スライダーのヒットテストとドラッグ処理
//----------------------------------------------------------------------------
static int HitSlider(int x, int y)
{
    const int x0 = (int)kWidth - kTrackPad - kTrackLen;
    const int x1 = (int)kWidth - kTrackPad;
    if (x < x0 - 12 || x > x1 + 12) return 0; // つまみの分だけ左右に広げる
    if (y >= kSlider1Y - kHitHalfH && y <= kSlider1Y + kHitHalfH) return 1;
    if (y >= kSlider2Y - kHitHalfH && y <= kSlider2Y + kHitHalfH) return 2;
    return 0;
}

static void DragSliderTo(int x)
{
    const int x0 = (int)kWidth - kTrackPad - kTrackLen;
    float t = (float)(x - x0) / (float)kTrackLen;
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;

    if (gDrag == 1)      gDistance = t * kDistanceMax;
    else if (gDrag == 2) gBlend    = kBlendMin + t * (kBlendMax - kBlendMin);
    UpdateTitle();
}

//----------------------------------------------------------------------------
// ウィンドウプロシージャ
//----------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        gDrag = HitSlider(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (gDrag != 0)
        {
            SetCapture(hwnd); // ウィンドウ外までドラッグしても追従させる
            DragSliderTo(GET_X_LPARAM(lParam));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (gDrag != 0) DragSliderTo(GET_X_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        if (gDrag != 0)
        {
            gDrag = 0;
            ReleaseCapture();
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

//----------------------------------------------------------------------------
// シェーダーコンパイル (shaders.hlsl を実行時コンパイル)
//----------------------------------------------------------------------------
static ComPtr<ID3DBlob> CompileShader(const wchar_t* file, const char* entry, const char* target)
{
    UINT flags = 0;
#if defined(_DEBUG)
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3DCompileFromFile(file, nullptr, nullptr, entry, target, flags, 0, &blob, &error);
    if (FAILED(hr))
    {
        const char* detail = error ? (const char*)error->GetBufferPointer()
                                   : "shaders.hlsl が見つからないか、コンパイルに失敗しました";
        MessageBoxA(nullptr, detail, "Shader Compile Error", MB_OK | MB_ICONERROR);
        ExitProcess(1);
    }
    return blob;
}

//----------------------------------------------------------------------------
// バリアのヘルパー
//----------------------------------------------------------------------------
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* res,
                                         D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter  = after;
    return b;
}

//----------------------------------------------------------------------------
// D3D12 初期化
//----------------------------------------------------------------------------
static void InitD3D12()
{
    UINT dxgiFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        {
            debug->EnableDebugLayer();
            dxgiFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

    ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gDevice)),
                  "D3D12CreateDevice");

    // コマンドキュー
    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(gDevice->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&gQueue)), "CreateCommandQueue");

    // スワップチェーン
    DXGI_SWAP_CHAIN_DESC1 scDesc = {};
    scDesc.BufferCount = kFrameCount;
    scDesc.Width       = kWidth;
    scDesc.Height      = kHeight;
    scDesc.Format      = kBackBufferFormat;
    scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scDesc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scDesc.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> sc1;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(gQueue.Get(), gHwnd, &scDesc,
                                                  nullptr, nullptr, &sc1),
                  "CreateSwapChainForHwnd");
    ThrowIfFailed(sc1.As(&gSwapChain), "IDXGISwapChain3");
    factory->MakeWindowAssociation(gHwnd, DXGI_MWA_NO_ALT_ENTER);
    gFrameIndex = gSwapChain->GetCurrentBackBufferIndex();

    // RTVヒープ: バックバッファ2枚 + 蓄積RT1枚
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = kFrameCount + 1;
    ThrowIfFailed(gDevice->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&gRtvHeap)),
                  "CreateDescriptorHeap(RTV)");
    gRtvStride = gDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // SRVヒープ (シェーダー可視): 蓄積RTのSRV
    D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
    srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvDesc.NumDescriptors = 1;
    srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(gDevice->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&gSrvHeap)),
                  "CreateDescriptorHeap(SRV)");

    // バックバッファのRTV
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(gSwapChain->GetBuffer(i, IID_PPV_ARGS(&gBackBuffers[i])), "GetBuffer");
        gDevice->CreateRenderTargetView(gBackBuffers[i].Get(), nullptr, rtv);
        rtv.ptr += gRtvStride;
    }

    // 蓄積RT (記事の RenderTexture に相当)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width  = kWidth;
        desc.Height = kHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = kAccumFormat;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags  = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clear = {};
        clear.Format = kAccumFormat; // クリア色は黒 (0,0,0,0)

        ThrowIfFailed(gDevice->CreateCommittedResource(
                          &heap, D3D12_HEAP_FLAG_NONE, &desc,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clear,
                          IID_PPV_ARGS(&gAccumTex)),
                      "CreateCommittedResource(AccumTex)");

        // RTV (RTVヒープの3番目)
        D3D12_CPU_DESCRIPTOR_HANDLE accumRtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
        accumRtv.ptr += gRtvStride * kFrameCount;
        gDevice->CreateRenderTargetView(gAccumTex.Get(), nullptr, accumRtv);

        // SRV
        gDevice->CreateShaderResourceView(gAccumTex.Get(), nullptr,
                                          gSrvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // ルートシグネチャ: [0]=32bit定数(b0), [1]=SRVテーブル(t0), 静的サンプラー(s0)
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = sizeof(Params) / 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 1;
        params[1].DescriptorTable.pDescriptorRanges = &range;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp = {};
        samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
        rsDesc.NumParameters = _countof(params);
        rsDesc.pParameters = params;
        rsDesc.NumStaticSamplers = 1;
        rsDesc.pStaticSamplers = &samp;
        rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, error;
        HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &error);
        if (FAILED(hr))
        {
            MessageBoxA(nullptr, error ? (const char*)error->GetBufferPointer() : "unknown",
                        "SerializeRootSignature Error", MB_OK | MB_ICONERROR);
            ExitProcess(1);
        }
        ThrowIfFailed(gDevice->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&gRootSig)),
                      "CreateRootSignature");
    }

    // シェーダーコンパイル
    ComPtr<ID3DBlob> vsParticle  = CompileShader(L"shaders.hlsl", "VSParticle",  "vs_5_0");
    ComPtr<ID3DBlob> psParticle  = CompileShader(L"shaders.hlsl", "PSParticle",  "ps_5_0");
    ComPtr<ID3DBlob> vsThreshold = CompileShader(L"shaders.hlsl", "VSThreshold", "vs_5_0");
    ComPtr<ID3DBlob> psThreshold = CompileShader(L"shaders.hlsl", "PSThreshold", "ps_5_0");
    ComPtr<ID3DBlob> psUi        = CompileShader(L"shaders.hlsl", "PSUi",        "ps_5_0");

    // 共通PSO設定
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = gRootSig.Get();
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.SampleMask = UINT_MAX;
    pso.NumRenderTargets = 1;
    pso.SampleDesc.Count = 1;
    for (int i = 0; i < 8; ++i)
        pso.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Pass1 PSO: 加算ブレンド (Blend One One) で蓄積
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p = pso;
        p.VS = { vsParticle->GetBufferPointer(), vsParticle->GetBufferSize() };
        p.PS = { psParticle->GetBufferPointer(), psParticle->GetBufferSize() };
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.RTVFormats[0] = kAccumFormat;

        D3D12_RENDER_TARGET_BLEND_DESC& b = p.BlendState.RenderTarget[0];
        b.BlendEnable    = TRUE;
        b.SrcBlend       = D3D12_BLEND_ONE;
        b.DestBlend      = D3D12_BLEND_ONE;
        b.BlendOp        = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha  = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOpAlpha   = D3D12_BLEND_OP_ADD;

        ThrowIfFailed(gDevice->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&gPsoParticle)),
                      "CreateGraphicsPipelineState(Particle)");
    }

    // Pass2 PSO: ブレンドなし、バックバッファへ
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p = pso;
        p.VS = { vsThreshold->GetBufferPointer(), vsThreshold->GetBufferSize() };
        p.PS = { psThreshold->GetBufferPointer(), psThreshold->GetBufferSize() };
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.RTVFormats[0] = kBackBufferFormat;

        ThrowIfFailed(gDevice->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&gPsoThreshold)),
                      "CreateGraphicsPipelineState(Threshold)");
    }

    // Pass3 PSO: スライダーUI。プリマルチプライドアルファ (ONE, INV_SRC_ALPHA)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC p = pso;
        p.VS = { vsThreshold->GetBufferPointer(), vsThreshold->GetBufferSize() }; // VSは共用
        p.PS = { psUi->GetBufferPointer(), psUi->GetBufferSize() };
        p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        p.RTVFormats[0] = kBackBufferFormat;

        D3D12_RENDER_TARGET_BLEND_DESC& b = p.BlendState.RenderTarget[0];
        b.BlendEnable    = TRUE;
        b.SrcBlend       = D3D12_BLEND_ONE;
        b.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp        = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha  = D3D12_BLEND_ONE;
        b.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOpAlpha   = D3D12_BLEND_OP_ADD;

        ThrowIfFailed(gDevice->CreateGraphicsPipelineState(&p, IID_PPV_ARGS(&gPsoUi)),
                      "CreateGraphicsPipelineState(Ui)");
    }

    // コマンドアロケーター / リスト / フェンス
    ThrowIfFailed(gDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&gCmdAlloc)),
                  "CreateCommandAllocator");
    ThrowIfFailed(gDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             gCmdAlloc.Get(), nullptr,
                                             IID_PPV_ARGS(&gCmdList)),
                  "CreateCommandList");
    gCmdList->Close();

    ThrowIfFailed(gDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gFence)),
                  "CreateFence");
    gFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

//----------------------------------------------------------------------------
// GPU完了待ち (サンプルなので毎フレームフラッシュする単純な方式)
//----------------------------------------------------------------------------
static void WaitForGpu()
{
    const UINT64 v = ++gFenceValue;
    gQueue->Signal(gFence.Get(), v);
    if (gFence->GetCompletedValue() < v)
    {
        gFence->SetEventOnCompletion(v, gFenceEvent);
        WaitForSingleObject(gFenceEvent, INFINITE);
    }
}

//----------------------------------------------------------------------------
// 描画
//----------------------------------------------------------------------------
static void Render()
{
    gCmdAlloc->Reset();
    gCmdList->Reset(gCmdAlloc.Get(), nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)kWidth, (float)kHeight, 0.0f, 1.0f };
    D3D12_RECT     sc = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    gCmdList->RSSetViewports(1, &vp);
    gCmdList->RSSetScissorRects(1, &sc);
    gCmdList->SetGraphicsRootSignature(gRootSig.Get());

    ID3D12DescriptorHeap* heaps[] = { gSrvHeap.Get() };
    gCmdList->SetDescriptorHeaps(1, heaps);

    // シェーダー定数 (記事の _Cutoff / _Stroke に相当する値ほか)
    Params params = {};
    params.distance       = gDistance;
    params.aspect         = (float)kWidth / (float)kHeight;
    // Quadは十分大きく取り、フィールドの実質的な打ち切りは particleCutoff の
    // clip に任せる (Quad境界での打ち切り段差が輪郭に出ないようにする)
    params.quadHalf       = 1.5f;
    params.ballRadius     = 0.2f;
    params.blend          = gBlend;
    params.particleCutoff = 0.001f;
    params.cutoff         = 0.25f;
    // 輪郭(ストローク)のしきい値: 指数カーネルはブレンド距離Bで山の高さが
    // 変わるため、固定値だとBによって輪郭の太さが大きく変化してしまう。
    // 「表面から kStrokeWidth 内側」のフィールド値を計算して、Bによらず
    // ほぼ一定の太さの輪郭になるようにする。
    {
        const float kStrokeWidth = 0.04f; // 輪郭の太さ (ワールド単位)
        const float r  = params.ballRadius;
        const float ri = r - kStrokeWidth;
        params.stroke = params.cutoff * expf((r * r - ri * ri) / (gBlend * gBlend));
    }
    params.screenW        = (float)kWidth;
    params.screenH        = (float)kHeight;
    params.dist01         = gDistance / kDistanceMax;
    params.blend01        = (gBlend - kBlendMin) / (kBlendMax - kBlendMin);
    gCmdList->SetGraphicsRoot32BitConstants(0, sizeof(Params) / 4, &params, 0);

    //--- Pass 1: 蓄積RTにメタボールパーティクルを加算描画 ---
    {
        D3D12_RESOURCE_BARRIER toRT = Transition(gAccumTex.Get(),
                                                 D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
        gCmdList->ResourceBarrier(1, &toRT);

        D3D12_CPU_DESCRIPTOR_HANDLE accumRtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
        accumRtv.ptr += gRtvStride * kFrameCount;

        const float clearAccum[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        gCmdList->OMSetRenderTargets(1, &accumRtv, FALSE, nullptr);
        gCmdList->ClearRenderTargetView(accumRtv, clearAccum, 0, nullptr);

        gCmdList->SetPipelineState(gPsoParticle.Get());
        gCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        gCmdList->DrawInstanced(4, 2, 0, 0); // Quad(4頂点) x メタボール2個

        D3D12_RESOURCE_BARRIER toSRV = Transition(gAccumTex.Get(),
                                                  D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        gCmdList->ResourceBarrier(1, &toSRV);
    }

    //--- Pass 2: しきい値処理してバックバッファへ ---
    {
        D3D12_RESOURCE_BARRIER toRT = Transition(gBackBuffers[gFrameIndex].Get(),
                                                 D3D12_RESOURCE_STATE_PRESENT,
                                                 D3D12_RESOURCE_STATE_RENDER_TARGET);
        gCmdList->ResourceBarrier(1, &toRT);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = gRtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += gRtvStride * gFrameIndex;

        const float clearColor[4] = { 0.05f, 0.07f, 0.10f, 1.0f }; // 背景色
        gCmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        gCmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

        gCmdList->SetPipelineState(gPsoThreshold.Get());
        gCmdList->SetGraphicsRootDescriptorTable(1, gSrvHeap->GetGPUDescriptorHandleForHeapStart());
        gCmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        gCmdList->DrawInstanced(3, 1, 0, 0); // フルスクリーントライアングル

        //--- Pass 3: スライダーUIをアルファ合成で重ねる ---
        gCmdList->SetPipelineState(gPsoUi.Get());
        gCmdList->DrawInstanced(3, 1, 0, 0);

        D3D12_RESOURCE_BARRIER toPresent = Transition(gBackBuffers[gFrameIndex].Get(),
                                                      D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                      D3D12_RESOURCE_STATE_PRESENT);
        gCmdList->ResourceBarrier(1, &toPresent);
    }

    gCmdList->Close();
    ID3D12CommandList* lists[] = { gCmdList.Get() };
    gQueue->ExecuteCommandLists(1, lists);

    gSwapChain->Present(1, 0);
    WaitForGpu();
    gFrameIndex = gSwapChain->GetCurrentBackBufferIndex();
}

//----------------------------------------------------------------------------
// エントリポイント
//----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"D3D12MetaballWindow";
    RegisterClassExW(&wc);

    // リサイズ不可の固定サイズウィンドウ (クライアント領域 = kWidth x kHeight)
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = { 0, 0, (LONG)kWidth, (LONG)kHeight };
    AdjustWindowRect(&rc, style, FALSE);

    gHwnd = CreateWindowExW(0, wc.lpszClassName, L"D3D12 2D Metaball", style,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top,
                            nullptr, nullptr, hInstance, nullptr);

    InitD3D12();
    UpdateTitle();
    ShowWindow(gHwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        else
        {
            Render();
        }
    }

    WaitForGpu();
    CloseHandle(gFenceEvent);
    return 0;
}
