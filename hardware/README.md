# 基板データ（自分で作りたい人向け）

このモニターで使っている 2 枚の基板の製造データです。どちらも [EasyEDA Pro](https://pro.easyeda.com/) で設計し、[JLCPCB](https://jlcpcb.com/) で基板の製造と部品の実装（PCBA）まで頼める形になっています。

| 基板 | 役割 | サイズ | OSHWLab |
|---|---|---|---|
| [センサー基板](EnvMonitor_SCD41-SHT35-BMP581_I2C/) | SCD41（CO2）/ SHT35（温度・湿度）/ BMP581（気圧）を載せた本体 | 50.3 × 24.9 mm | <https://oshwlab.com/yunayuna/project_ibxqleze> |
| [変換基板](grove2qwiic-3v3/) | Tab5 の PORT.A（Grove, 5V）を 3.3V に下げて Qwiic に変換 | 27.7 × 14.2 mm | <https://oshwlab.com/yunayuna/project_aofrdeaz> |

> [!NOTE]
> OSHWLab のページは審査中のため、まだ開けないことがあります。開けない場合は、このフォルダのファイルで注文できます。

つなぎ方:

```
Tab5 PORT.A ──Grove ケーブル──> 変換基板 ──Qwiic ケーブル──> センサー基板
```

## 注文のしかた

### OSHWLab から（かんたん）

1. 上の表の OSHWLab のページを開く
2. プロジェクトを自分の EasyEDA アカウントにコピー（クローン）する
3. EasyEDA Pro で開き、PCB 画面から JLCPCB に注文する（基板と部品実装をまとめて頼めます）

### ファイルから

各フォルダの 3 つのファイルを JLCPCB の注文画面でアップロードします。

| ファイル | 使う場所 |
|---|---|
| `Gerber_*.zip` | 最初の基板の注文画面（ガーバー） |
| `BOM_*.xlsx` | 部品実装（PCB Assembly）を ON にしたあとの BOM |
| `PickAndPlace_*.csv` | 同じく CPL（部品の位置） |

### 注文時の設定

作者が注文したときの設定です。書いていない項目は標準のままです。

| 項目 | センサー基板 | 変換基板 |
|---|---|---|
| Base Material | FR-4（TG135） | FR-4（TG135） |
| Layers | 2 | 2 |
| PCB Thickness | 1.6mm | 1.6mm |
| PCB Color / Silkscreen | Green / White | Green / White |
| Surface Finish | **ENIG**（1U"） | LeadFree HASL |
| Outer Copper Weight | 1 oz | 1 oz |
| Via Covering | Tented | Tented |
| Electrical Test | Flying Probe Fully Test | Flying Probe Fully Test |
| Mark on PCB | Order Number | Remove Mark |

- センサー基板の表面処理を ENIG にしているのは、SCD41・BMP581（LGA）や SHT35（DFN）のように足が裏に隠れる部品を、平らなパッドにきれいにはんだ付けするためです
- センサー基板には、熱を伝えにくくするための切り欠き（スロット）があります。JLCPCB に消されないよう、Remark に次の文を入れてください

  ```
  The slots near U2 and between U4 and U1 are intentional for thermal isolation. Please keep them as designed.
  ```

- 部品実装（PCBA）は **表面（Top）のみ** です。全部品を JLCPCB で実装できます（手ではんだ付けする部品はありません）
- 部品実装のため、センサー基板は JLCPCB 側でまわりにレール（捨て板）を付けた状態で製造され、注文画面のサイズは約 70 × 71 mm と表示されます。届く前にレールは切り離されるので、届くのは 50.3 × 24.9 mm の基板です

#### センサー基板の部品実装オプション

**SCD41（C3659294）は、JLCPCB 標準の高温はんだでは壊れます。** 注文時に自分で中温はんだ（Medium temp.）を選んでください。

作者が標準のはんだで注文したときは、JLCPCB の製造前チェックで気づいてもらえて、メールで確認が来ました（「Confirm Production file」は No のままでした）。ただし、毎回気づいてもらえるとは限らないので、最初から選んでおくのが安全です。

| 項目 | 設定 | 理由 |
|---|---|---|
| Solder Paste | **Medium temp.** | SCD41 を高温で壊さないため（追加料金あり） |
| Packaging | ESD + Cardboard | 中温はんだはつきが弱いので、輸送中に部品が外れないよう梱包の指定が必要 |
| Depanel boards & edge rail before delivery | Yes | レールを切り離してから届けてもらう |
| Special parts for cleaning | Yes | 中温はんだと梱包を指定したときに JLCPCB 側で追加された項目（追加料金なし） |

中温はんだは高温より接合が弱く、はんだに黒い点が出ることがあると JLCPCB から説明があります。

Assembly remark（部品実装へのメモ）は書かなくてかまいません。作者は部品の向きの確認をお願いしましたが、このデータの向きは問題ないことを確認済みです。

#### 変換基板の部品実装オプション

特別な指定はありません（標準のはんだで大丈夫です）。

注文後、製造用データができたら、注文履歴（Order History）で部品の位置と向きを確認してください。

> [!IMPORTANT]
> 注文画面の実装プレビューで、**部品の向き**を必ず確認してください。特に IC の 1 番ピンの印と、コネクタの向きがずれやすいです。

注意:

- 基板は 5 枚から、部品実装は 2 枚からの注文になります
- SCD41・SHT35・BMP581・コネクタなどは JLCPCB の Extended パーツで、部品の種類ごとに追加料金がかかります
- LCSC の在庫は変わります。欠品しているときは、下の部品表の「LCSC 番号」と同じ仕様の部品に置き換えてください

## センサー基板

| 部品番号 | 部品 | LCSC 番号 |
|---|---|---|
| U4_SCD41 | SCD41-D-R2（CO2・温度・湿度, I2C 0x62） | C3659294 |
| U2_SHT35 | SHT35-DIS-B2.5KS（温度・湿度, I2C 0x44） | C90161 |
| U1_BMP581 | BMP581（気圧・温度, I2C 0x47） | C5362283 |
| CN1 | Qwiic コネクタ SM04B-SRSS-TB | C160404 |
| R_I2C_SDA, R_I2C_SCL | 4.7kΩ 0603（I2C のプルアップ） | C23162 |
| C_SCD41_0V1, C_SHT35_0V1, C_BMP581_VDD, C_BMP581_VDDIO | 100nF 0603 | C14663 |
| C_SENS_BULK | 10µF 0805 | C15850 |

- 電源は **3.3V 専用**です（レギュレーターは載っていません）
- SHT35 のまわりには、ほかの部品の熱が伝わりにくいように切り欠きがあります
- Qwiic のピン: 1 GND / 2 3.3V / 3 SDA / 4 SCL

## 変換基板

| 部品番号 | 部品 | LCSC 番号 |
|---|---|---|
| U1 | ME6211C33M5G-N（3.3V レギュレーター, SOT-23-5） | C82942 |
| J1 | Grove コネクタ（HY2.0-4P, 2.0-4PWTDK） | C3008564 |
| J2 | Qwiic コネクタ SM04B-SRSS-TB | C160404 |
| C1, C2 | 1µF 0603（入力・出力） | C15849 |
| C3 | 10µF 0603（出力） | C19702 |

- Grove（J1）のピン: 1 SCL / 2 SDA / 3 5V / 4 GND
- SDA / SCL はそのまま通すだけです。プルアップはセンサー基板側にあります

> [!WARNING]
> 基板が届いたら、**センサー基板をつなぐ前に** Qwiic 側の 3.3V–GND が約 3.3V になっているかをテスターで確かめてください。5V が出ていると BMP581 が壊れます。

## ライセンス

リポジトリ全体と同じ [MIT License](../LICENSE) です。
