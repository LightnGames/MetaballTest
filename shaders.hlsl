//----------------------------------------------------------------------------
// 2D Metaball シェーダー (D3D12 / HLSL)
//
// 参考: https://techblog.kayac.com/unity_advent_calendar_2018_22
//   Pass 1 (MetaballParticle 相当):
//     各メタボールを Quad として描画し、中心からの距離に応じたフィールド値を
//     加算ブレンドでオフスクリーンRTに蓄積する (記事の Σ の実装)
//   Pass 2 (MetaballRenderer 相当):
//     蓄積結果に対して
//       clip(color.a - _Cutoff);
//       color = color.a < _Stroke ? _StrokeColor : _Color;
//     でしきい値処理し、塗りと輪郭を分ける
//   Pass 3 (UI):
//     スライダー2本をピクセルシェーダーで直接描画してアルファ合成する
//
// フィールド関数について:
//   記事は逆二乗 (a = _Scale / d^2) だが、ここでは「ブレンド距離」を
//   独立したパラメーターにするため指数関数フォールオフを使う:
//     a = _Cutoff * exp((R^2 - d^2) / B^2)
//   単独ボールの表面 (a = _Cutoff) はちょうど d = R になり、
//   B (ブレンド距離) を変えてもボールの見た目の大きさは変わらず、
//   「どれだけ離れていてもくっつき始めるか」だけが変わる。
//----------------------------------------------------------------------------

cbuffer Params : register(b0)
{
    float gDistance;       // 2つのメタボール中心間の距離 (スライダー1)
    float gAspect;         // ウィンドウの 幅/高さ
    float gQuadHalf;       // パーティクルQuadの半径 (フィールドの有効範囲)
    float gBallRadius;     // 単独ボールの表面半径 R
    float gBlend;          // ブレンド距離 B (スライダー2)
    float gParticleCutoff; // 記事の _Cutoff (パーティクル側)
    float gCutoff;         // 記事の _Cutoff (しきい値パス側)
    float gStroke;         // 記事の _Stroke
    float gScreenW;        // 画面サイズ (UI描画用)
    float gScreenH;
    float gDist01;         // スライダー1のつまみ位置 [0,1]
    float gBlend01;        // スライダー2のつまみ位置 [0,1]
};

Texture2D    gAccum : register(t0); // Pass1 の蓄積結果 (RenderTexture 相当)
SamplerState gSamp  : register(s0);

//----------------------------------------------------------------------------
// Pass 1: メタボールパーティクル (加算合成で蓄積)
//----------------------------------------------------------------------------
struct VSOut1
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// 頂点バッファなしで SV_VertexID から Quad (トライアングルストリップ4頂点) を生成。
// SV_InstanceID (0/1) で左右のメタボールの中心位置を決める。
VSOut1 VSParticle(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    float2 corner = float2((vid & 1) ? 1.0 : -1.0,
                           (vid & 2) ? 1.0 : -1.0);

    // 2つのボールを原点対称に配置 (中心間距離 = gDistance)
    float  cx    = ((iid == 0) ? -0.5 : 0.5) * gDistance;
    float2 world = float2(cx, 0.0) + corner * gQuadHalf;

    VSOut1 o;
    // ワールド座標: yは[-1,1]、xはアスペクト比で補正 (円が真円になるように)
    o.pos = float4(world.x / gAspect, world.y, 0.0, 1.0);
    // 記事の「i.texcoord - 0.5」に相当する中心からのオフセット (ワールド単位)
    o.uv  = corner * gQuadHalf;
    return o;
}

float4 PSParticle(VSOut1 i) : SV_Target
{
    // 指数関数フォールオフ: 単独なら d = gBallRadius がちょうど表面になる
    float d2 = dot(i.uv, i.uv);
    float a  = gCutoff * exp((gBallRadius * gBallRadius - d2) / (gBlend * gBlend));
    clip(a - gParticleCutoff);

    // 加算ブレンド (ONE, ONE) でRTに蓄積される → Σ a
    return float4(a, a, a, a);
}

//----------------------------------------------------------------------------
// Pass 2: しきい値処理 (蓄積テクスチャ → バックバッファ)
//----------------------------------------------------------------------------
struct VSOut2
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// フルスクリーントライアングル (Pass 3 と共用)
VSOut2 VSThreshold(uint vid : SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut2 o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = uv;
    return o;
}

float4 PSThreshold(VSOut2 i) : SV_Target
{
    static const float4 kStrokeColor = float4(0.85, 0.96, 1.00, 1.0); // 輪郭色
    static const float4 kFillColor   = float4(0.20, 0.55, 0.95, 1.0); // 内側の色

    float4 color = gAccum.Sample(gSamp, i.uv);

    // 記事の MetaballRenderer と同じしきい値処理
    clip(color.a - gCutoff);
    return (color.a < gStroke) ? kStrokeColor : kFillColor;
}

//----------------------------------------------------------------------------
// Pass 3: スライダーUI (プリマルチプライドアルファで合成)
//   ※ 配置の定数は main.cpp のヒットテストと一致させること
//----------------------------------------------------------------------------
static const float kTrackLen = 220.0; // トラックの長さ (px)
static const float kTrackPad = 40.0;  // 画面右端からの余白 (px)
static const float kSlider1Y = 40.0;  // スライダー1 (距離) のY座標
static const float kSlider2Y = 80.0;  // スライダー2 (ブレンド距離) のY座標

// 線分 ab への距離
float SegDist(float2 p, float2 a, float2 b)
{
    float2 pa = p - a;
    float2 ba = b - a;
    float  h  = saturate(dot(pa, ba) / dot(ba, ba));
    return length(pa - ba * h);
}

// スライダー1本を col (プリマルチプライド) に合成する
void DrawSlider(inout float4 col, float2 px, float y, float t)
{
    float2 a    = float2(gScreenW - kTrackPad - kTrackLen, y);
    float2 b    = float2(gScreenW - kTrackPad, y);
    float2 knob = lerp(a, b, saturate(t));

    // トラック (丸端の細長いバー)。つまみより左は塗り色、右は無効色
    float  dTrack = SegDist(px, a, b);
    float  trackA = 1.0 - smoothstep(2.5, 3.5, dTrack);
    float3 trackC = (px.x <= knob.x) ? float3(0.35, 0.65, 1.00)
                                     : float3(0.30, 0.34, 0.42);
    col = lerp(col, float4(trackC, 1.0) * 0.95, trackA);

    // つまみ (円)
    float dKnob = length(px - knob);
    float knobA = 1.0 - smoothstep(8.0, 9.5, dKnob);
    col = lerp(col, float4(0.95, 0.98, 1.00, 1.0), knobA);
}

float4 PSUi(VSOut2 i) : SV_Target
{
    float2 px  = i.uv * float2(gScreenW, gScreenH);
    float4 col = float4(0.0, 0.0, 0.0, 0.0);

    DrawSlider(col, px, kSlider1Y, gDist01);  // メタボール間の距離
    DrawSlider(col, px, kSlider2Y, gBlend01); // ブレンド距離

    clip(col.a - 0.003); // UI以外のピクセルは棄却
    return col;          // ブレンドは (ONE, INV_SRC_ALPHA)
}
