//----------------------------------------------------------------------------
// 2D Metaball シェーダー (D3D12 / HLSL)
//
// 参考: https://techblog.kayac.com/unity_advent_calendar_2018_22
//   Pass 1 (MetaballParticle 相当):
//     各メタボールを Quad として描画し、中心からの距離に応じたフィールド値を
//     加算ブレンドでオフスクリーンRTに蓄積する (記事の Σ の実装)。
//     法線用に、フィールド値で重み付けした中心からのオフセットも一緒に蓄積する
//   Pass 2 (MetaballRenderer 相当):
//     蓄積結果に対して clip(color.a - _Cutoff) でしきい値処理し、
//     表示モードに応じて 単色 / UV / 法線 を描く
//   Pass 3 (UI):
//     スライダー2本をピクセルシェーダーで直接描画してアルファ合成する
//   Pass 4 (デバッグ表示):
//     C++側でGDI描画したパネル (cbuffer Params の一覧 + 表示モードメニュー) を左上に合成する
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
    float gScreenW;        // 画面サイズ (UI描画用)
    float gScreenH;
    float gDist01;         // スライダー1のつまみ位置 [0,1]
    float gBlend01;        // スライダー2のつまみ位置 [0,1]
    float gDbgW;           // デバッグテキストテクスチャのサイズ (px)
    float gDbgH;
    uint  gViewMode;       // 表示モード (VIEW_*)。デバッグメニューで切替
};

Texture2D    gAccum  : register(t0); // Pass1 の蓄積結果 (RenderTexture 相当)。rg = Σa·(p - c), a = Σa
Texture2D    gDbgTex : register(t1); // デバッグパネル (C++側でGDI描画、黒地)
SamplerState gSamp   : register(s0);

//----------------------------------------------------------------------------
// Pass 1: メタボールパーティクル (加算合成で蓄積)
//----------------------------------------------------------------------------
struct VSOut1
{
    float4 pos    : SV_Position;
    float2 offset : TEXCOORD0; // ボール中心からのオフセット p - c (ワールド単位)
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
    o.offset = corner * gQuadHalf;
    return o;
}

float4 PSParticle(VSOut1 i) : SV_Target
{
    // 指数関数フォールオフ: 単独なら d = gBallRadius がちょうど表面になる
    float d2 = dot(i.offset, i.offset);
    float a  = gCutoff * exp((gBallRadius * gBallRadius - d2) / (gBlend * gBlend));
    clip(a - gParticleCutoff);

    // 加算ブレンド (ONE, ONE) でRTに蓄積される
    //   rg = Σa·(p - c) … 法線用 (フィールド値で重み付けした中心からのオフセット)
    //   a  = Σa         … しきい値処理用 (記事の Σ)
    return float4(a * i.offset, 0.0, a);
}

//----------------------------------------------------------------------------
// Pass 2: しきい値処理 (蓄積テクスチャ → バックバッファ)
//----------------------------------------------------------------------------
// 表示モード (main.cpp の kViewModeNames と並びを一致させること)
static const uint VIEW_COLOR  = 0; // 通常 (単色)
static const uint VIEW_UV     = 1;
static const uint VIEW_NORMAL = 2;

static const float PI = 3.14159265;

struct VSOut2
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// フルスクリーントライアングル (Pass 3, 4 と共用)
VSOut2 VSThreshold(uint vid : SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut2 o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv  = uv;
    return o;
}

// 球体状の法線 (x: 右, y: 上, z: 手前)。
//   蓄積値から高さ h = B·sqrt(ln(Σa / _Cutoff)) を復元すると、単独のボールでは
//   ちょうど半球の高さ sqrt(R² - d²) になり、法線も球の法線と一致する。
//   ボールが複数あると h² = B²·ln Σexp((R² - dᵢ²) / B²) (各球の高さ² を滑らかに max 合成したもの)
//   になり、くっついた部分も滑らかにつながる。この高さ場の法線は (Σa·(p - c) / Σa, h) の向き。
float3 MetaballNormal(float4 acc)
{
    float h = gBlend * sqrt(max(log(acc.a / gCutoff), 0.0));
    return normalize(float3(acc.rg / acc.a, h));
}

// 球体状の UV: 法線の経度・緯度を [0,1] に割り当てる
// (地球儀のように、正面の半球全体にテクスチャが巻き付く)
float2 SphereUV(float3 n)
{
    float lon = atan2(n.x, n.z);          // 左端 -π/2 … 右端 +π/2
    float lat = atan2(n.y, length(n.xz)); // 下端 -π/2 … 上端 +π/2
    return float2(0.5 + lon / PI, 0.5 - lat / PI); // v は D3D のテクスチャ座標と同じく下向き
}

float4 PSThreshold(VSOut2 i) : SV_Target
{
    static const float4 kFillColor = float4(0.20, 0.55, 0.95, 1.0); // メタボールの色

    float4 acc = gAccum.Sample(gSamp, i.uv);

    // しきい値処理: フィールド値の和が _Cutoff 未満のピクセルは棄却
    clip(acc.a - gCutoff);

    float3 n  = MetaballNormal(acc);
    float2 uv = SphereUV(n);

    // UV表示用: 歪みが分かるよう 1/8 ごとのグリッド線 (幅 約1px) を重ねる
    float2 lineDist = abs(frac(uv * 8.0 + 0.5) - 0.5) / fwidth(uv * 8.0);
    float  grid     = 1.0 - saturate(min(lineDist.x, lineDist.y));

    if (gViewMode == VIEW_UV)     return float4(lerp(float3(uv, 0.0), 1.0, grid * 0.6), 1.0); // u→R, v→G
    if (gViewMode == VIEW_NORMAL) return float4(n * 0.5 + 0.5, 1.0);                          // xyz→RGB
    return kFillColor;
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

//----------------------------------------------------------------------------
// Pass 4: デバッグパネル (左上: cbuffer Params の一覧 + 表示モードメニュー)
//   C++側でGDIにより黒地に文字を描いたテクスチャを、半透明の黒パネル付きで合成する。
//   黒地に描いた文字の色は「文字色 × カバレッジ」なので、そのままプリマルチプライド色になる
//----------------------------------------------------------------------------
static const float2 kDbgOrigin = float2(12.0, 12.0); // パネル左上の位置 (px)。main.cpp の kDbgX/kDbgY と一致させること

float4 PSDebugText(VSOut2 i) : SV_Target
{
    float2 px    = i.uv * float2(gScreenW, gScreenH);
    float2 local = px - kDbgOrigin;

    // パネル矩形の外は棄却
    clip(local);
    clip(float2(gDbgW, gDbgH) - local);

    float3 text     = gDbgTex.Sample(gSamp, local / float2(gDbgW, gDbgH)).rgb;
    float  coverage = max(text.r, max(text.g, text.b));

    // 文字 (プリマルチプライド) + 読みやすさのための半透明黒パネル
    return float4(text, lerp(0.45, 1.0, coverage));
}
