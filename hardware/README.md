# 基板データ（自分で作りたい人向け）

このモニターでは 2 枚の基板を使います。どちらも [EasyEDA Pro](https://pro.easyeda.com/) で設計し、[JLCPCB](https://jlcpcb.com/) で基板の製造と部品の実装（PCBA）まで頼める形になっています。

| 基板 | 役割 | 製造データ |
|---|---|---|
| センサー基板 | SCD41（CO2）/ SHT35（温度・湿度）/ BMP581（気圧）を載せた本体 | **[env-sensor-board](https://github.com/ynug/env-sensor-board)**（別リポジトリ。ほかのプロジェクトと共通） |
| 変換基板 | Tab5 の PORT.A（Grove, 5V）を 3.3V に下げて Qwiic に変換 | このフォルダの [grove2qwiic-3v3/](grove2qwiic-3v3/) |

つなぎ方:

```
Tab5 PORT.A ──Grove ケーブル──> 変換基板 ──Qwiic ケーブル──> センサー基板
```

センサー基板の回路・注文方法・注文時の設定（SCD41 には中温はんだが必要、など）は [env-sensor-board の README](https://github.com/ynug/env-sensor-board) を見てください。以下は変換基板の説明です。

## 変換基板（grove2qwiic-3v3）

| 項目 | 内容 |
|---|---|
| サイズ | 27.7 × 14.2 mm、2 層 |
| OSHWLab | <https://oshwlab.com/yunayuna/project_aofrdeaz> |

> [!NOTE]
> OSHWLab のページは審査中のため、まだ開けないことがあります。開けない場合は、このフォルダのファイルで注文できます。

### 回路

| 部品番号 | 部品 | LCSC 番号 |
|---|---|---|
| U1 | ME6211C33M5G-N（3.3V レギュレーター, SOT-23-5） | C82942 |
| J1 | Grove コネクタ（HY2.0-4P, 2.0-4PWTDK） | C3008564 |
| J2 | Qwiic コネクタ SM04B-SRSS-TB | C160404 |
| C1, C2 | 1µF 0603（入力・出力） | C15849 |
| C3 | 10µF 0603（出力） | C19702 |

- Grove（J1）のピン: 1 SCL / 2 SDA / 3 5V / 4 GND
- Qwiic（J2）のピン: 1 GND / 2 3.3V / 3 SDA / 4 SCL
- SDA / SCL はそのまま通すだけです。プルアップはセンサー基板側にあります

> [!WARNING]
> 基板が届いたら、**センサー基板をつなぐ前に** Qwiic 側の 3.3V–GND が約 3.3V になっているかをテスターで確かめてください。5V が出ていると BMP581 が壊れます。

### 注文のしかた

**OSHWLab から（かんたん）**

1. 上の OSHWLab のページを開く
2. プロジェクトを自分の EasyEDA アカウントにコピー（クローン）する
3. EasyEDA Pro で開き、PCB 画面から JLCPCB に注文する（基板と部品実装をまとめて頼めます）

**ファイルから**

[grove2qwiic-3v3/](grove2qwiic-3v3/) の 3 つのファイルを JLCPCB の注文画面でアップロードします。

| ファイル | 使う場所 |
|---|---|
| `Gerber_*.zip` | 最初の基板の注文画面（ガーバー） |
| `BOM_*.xlsx` | 部品実装（PCB Assembly）を ON にしたあとの BOM |
| `PickAndPlace_*.csv` | 同じく CPL（部品の位置） |

### 注文時の設定

作者が注文したときの設定です。書いていない項目は標準のままです。

| 項目 | 設定 |
|---|---|
| Base Material | FR-4（TG135） |
| Layers | 2 |
| PCB Thickness | 1.6mm |
| PCB Color / Silkscreen | Green / White |
| Surface Finish | LeadFree HASL |
| Outer Copper Weight | 1 oz |
| Via Covering | Tented |
| Electrical Test | Flying Probe Fully Test |
| Mark on PCB | Remove Mark |

- 部品実装（PCBA）は **表面（Top）のみ** です。全部品を JLCPCB で実装できます（手ではんだ付けする部品はありません）
- 部品実装オプションに特別な指定はありません（標準のはんだで大丈夫です）

> [!IMPORTANT]
> 注文画面の実装プレビューで、**部品の向き**を必ず確認してください。特にレギュレーターの 1 番ピンの印と、コネクタの向きがずれやすいです。

注意:

- 基板は 5 枚から、部品実装は 2 枚からの注文になります
- コネクタなどは JLCPCB の Extended パーツで、部品の種類ごとに追加料金がかかります
- LCSC の在庫は変わります。欠品しているときは、上の部品表の「LCSC 番号」と同じ仕様の部品に置き換えてください

## ライセンス

リポジトリ全体と同じ [MIT License](../LICENSE) です。
