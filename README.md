# D3D12 2D Metaball サンプル

Kayac さんのブログ記事
[メタボールを実装してみた (Unity Advent Calendar 2018)](https://techblog.kayac.com/unity_advent_calendar_2018_22)
で解説されている 2D メタボールのシェーダー手法を Direct3D 12 + HLSL に移植したサンプルです。

## 操作

画面右上に2つのスライダーが表示されます (マウスでドラッグ)。

| スライダー | 動作 |
|---|---|
| 1本目 (上) | 2つのメタボールの距離 (近づくとくっつく) |
| 2本目 (下) | ブレンド距離 (大きいほど遠くからくっつき始める) |
| ESCキー | 終了 |

現在の値はウィンドウタイトルに表示されます。
スライダー自体もピクセルシェーダー (`PSUi`) で描画しています。

画面左上には `cbuffer Params` の全フィールドがデバッグ表示されます
(GDIでテキストをDIBに描き、毎フレームテクスチャへ転送して `PSDebugText` で合成)。

## ビルドと実行

Visual Studio (C++ ワークロード) がインストールされていれば:

```
build.bat
Metaball2D.exe
```

`shaders.hlsl` は実行時に `D3DCompileFromFile` でコンパイルされるので、
exe と同じフォルダーに置いてください (シェーダーだけならリビルド不要で編集できます)。

## 仕組み (記事の手法との対応)

記事の Unity 実装と同じ「蓄積 → しきい値処理」の構成に、UI描画とデバッグ表示のパスを加えた4パスです。

### Pass 1: パーティクル蓄積 (記事の MetaballParticle + MetaBallCamera/RenderTexture)

- メタボール 1 個 = Quad 1 枚。頂点バッファは使わず `SV_VertexID` / `SV_InstanceID` から生成
- ピクセルシェーダーで中心からの距離をフィールド値に変換し、
  **加算ブレンド (ONE, ONE)** で `R16G16B16A16_FLOAT` のオフスクリーン RT に蓄積 → Σ の実装
- float RT なので加算で 1.0 を超えても飽和しない

### Pass 2: しきい値処理 (記事の MetaballRenderer)

フルスクリーントライアングルで蓄積テクスチャをサンプリングし、記事と同じ処理:

```hlsl
clip(color.a - _Cutoff);                              // 外側は破棄
color = color.a < _Stroke ? _StrokeColor : _Color;    // 輪郭と塗りを分ける
```

2つのボールが近づくとフィールド値の和がしきい値を超える領域がつながり、
「水滴がひっつく」メタボール特有のマージ表現になります。

### Pass 3: スライダーUI

フルスクリーントライアングルで、線分への距離 (SDF) からトラックとつまみを
ピクセルシェーダーで直接描画し、プリマルチプライドアルファで合成します。
ヒットテストとドラッグは Win32 側 (`WM_LBUTTONDOWN` / `WM_MOUSEMOVE`) で処理します。

### 記事からの変更点

- **フィールド関数を逆二乗から指数関数フォールオフに変更**:
  記事の `a = _Scale / d^2` は `_Scale` を変えるとボールの大きさ自体が変わって
  しまうため、「ブレンド距離」を独立に操作できる次の形にしています:

  ```hlsl
  a = _Cutoff * exp((R*R - d*d) / (B*B));  // R=ボール半径, B=ブレンド距離
  ```

  単独ボールの表面 (`a = _Cutoff`) はちょうど `d = R` になるので、
  B (スライダー2) を変えてもボールの大きさは変わらず、
  くっつき始める距離だけが変わります (中心間距離 `2·√(R² + B²·ln2)` でマージ)。
- フィールド計算は Quad 相対 UV ではなく**ワールド空間の距離**で行い、
  Quad は十分大きく (`quadHalf = 1.5`) 取ってフィールドの実質的な打ち切りを
  `particleCutoff` の `clip` に任せています (Quad 境界の段差が輪郭に出ないように)。
- 輪郭のしきい値 (`_Stroke` 相当) は「表面から一定幅内側」のフィールド値を
  C++ 側で毎フレーム計算し、B によらず輪郭の太さがほぼ一定になるようにしています。

## パラメーター

[main.cpp](main.cpp) の `Render()` 内で設定しています:

| 変数 | 記事での対応 | 値 |
|---|---|---|
| `ballRadius` | (`_Scale` から換算) | 0.2 |
| `blend` | — (スライダー2) | 0.08〜0.6 |
| `particleCutoff` | パーティクル側 `_Cutoff` | 0.001 |
| `cutoff` | しきい値パス側 `_Cutoff` | 0.25 |
| `stroke` | `_Stroke` | ブレンド距離から自動計算 |

色は [shaders.hlsl](shaders.hlsl) の `PSThreshold` 内 (`kFillColor` / `kStrokeColor`)、
スライダーの配置は `shaders.hlsl` と `main.cpp` 双方の定数 (`kTrackLen` など) で調整できます。
