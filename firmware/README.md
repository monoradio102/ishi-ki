# ファーム

> [!IMPORTANT]
> 開発中です。命令やメモリの使い方は、これから変わることがあります。

| フォルダ | 書き込む先 | 中身 |
|---|---|---|
| [ishi_demo/](ishi_demo/) | タグ（ATtiny1616） | 台座が書いた色と光り方を読んで、LED を光らせる。タッチで光り方を切り替える |
| [dai_demo/](dai_demo/) | 専用の台座（STM32C071） | PC から受けた設定を、NFC でタグに書く。台座の LED を光らせる |

`patterns.h`（光り方の計算）は 2 つで同じものです。ブラウザの画面の見本も、同じ計算で動いています。

## タグ（ishi_demo）

- Arduino の [megaTinyCore](https://github.com/SpenceKonde/megaTinyCore) を使います
- ボードの設定: ATtiny1616、Clock 1 MHz internal、BOD Disabled
- 工場出荷のヒューズ OSCCFG は 0x02 です。1 MHz のビルドは 0x01 が前提なので、最初の書き込みで一緒に書きます

```bash
arduino-cli compile -b megaTinyCore:megaavr:atxy6:chip=1616,clock=1internal,bodmode=disabled --output-dir build ishi_demo
```

書き込みは SerialUPDI です。USB シリアル変換の RX を UPDI に、TX を 470Ω〜1kΩ を通して UPDI につなぎ、タグの裏のパッド（UPDI・V・G）に当てます。megaTinyCore に入っている `prog.py` で書きます。

```bash
python3 prog.py -t uart -u <シリアルポート> -b 115200 -d attiny1616 --fuses 2:0x01 -f build/ishi_demo.ino.hex -a write
```

一度書けば、そのあとはスマホや台座の磁界だけで動きます。

## 専用の台座（dai_demo）

- Arduino の STM32duino 3.1.0 を使います。STM32C071KBU6 のボード登録がないので、[boards.local.txt](dai_demo/boards.local.txt) を STM32duino のフォルダに置きます
- ライブラリ: STM32duino ST25R3916 2.1.0、STM32duino NFC-RFAL 2.0.2

```bash
arduino-cli compile -b STMicroelectronics:stm32:GenC0:pnum=GENERIC_C071KBUX,usb=CDCgen --output-dir build dai_demo
```

書き込みは USB DFU です。動いているファームに USB シリアルで `B` を送ると DFU に入ります（何も書いていない基板は、最初から DFU です）。

```bash
dfu-util -a 0 -s 0x08000000:leave -D build/dai_demo.ino.bin
```

## 台座の USB シリアルの命令

1 行に 1 命令で、改行で終わります。数値は 0〜255 です。pattern は `patterns.h` の番号です。

| 命令 | 意味 |
|---|---|
| `S0 <pattern> <speed> <r> <g> <b>` | タグのいつもの光り方 |
| `S1 <pattern> <speed> <r> <g> <b> <秒×10>` | タッチしたときの光り方と、続ける時間 |
| `L0 <pattern> <speed> <r> <g> <b>` | 台座の LED（タグがないとき） |
| `L1 <pattern> <speed> <r> <g> <b>` | 台座の LED（タグが置かれているとき） |
| `M1` / `M0` | タグに書く ／ 給電だけ（タグは覚えている光り方で光る） |
| `U <url>` / `U?` | タグの NFC タグとしての URL を書く ／ 読む |
| `F1` / `F0` | 磁界 ON ／ OFF |
| `?` | 状態 |
| `B` | USB DFU に入る |

台座からは、返事とは別に次の行が来ます。

| 行 | 意味 |
|---|---|
| `P <置かれているか> <振幅> <基準> <位相> <基準>` | 0.2 秒ごと。タグが近いほど、アンテナの振幅と位相がずれる |
| `I <UID>` | タグが置かれた。タグの UID |
| `T <回数> <触れているか>` | タグがタッチされた |

## タグのメモリ（ST25DV のユーザーメモリ）

| ブロック | 中身 |
|---|---|
| 126 | `'I'` `'K'` seq pattern（いつもの光り方） |
| 127 | speed r g b |
| 123 | pattern speed r g（タッチしたとき） |
| 124 | b 続ける時間（0.1 秒） `'T'` `'K'` |
| 125 | `'T'` 回数 触れているか 0（タグが書く） |

- 台座は 123・124・127 を書いてから、最後に 126 の seq を変えます。タグは seq が変わったときだけ読み直します
- pattern の最上位ビット（0x80）は「基板の確認用の青 LED も光らせる」です
- NDEF（URL など）は、ブロック 1 から 122 までに入ります
