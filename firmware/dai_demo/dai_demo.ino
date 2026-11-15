// ISHI-KI 台（dai）デモ用ファーム：PC から色と光り方を受け取り、NFC で石に書き込む
//
// PC（ブラウザの UI、Web Serial）→ USB シリアル（1 行 1 命令）→ 台 → ISO 15693 で石の ST25DV に書く → 石が読んで光る。
// 台は起動したら磁界を出し続ける（石はこの磁界だけで動く）。ST25R3916B の操作は ST の RFAL に任せる。
//
// 命令（改行で終わる。返事も 1 行）。数値は 0〜255、pattern は patterns.h の番号
//   S0 <pattern> <speed> <r> <g> <b>  石へ（フィギュアの LED）、いつもの光り方。NFC で書いて読み返す。返事は "OK <seq>" か "NG <理由>"
//   S1 <pattern> <speed> <r> <g> <b> <秒×10>  タッチされたときの光り方と、続ける時間（0.1 秒単位、触れている間は延びる）。
//                                    両方を石に書いておき、切り替えは石が自分でする（スマホの上でも同じ動き）。石を置いたときにも両方書く
//   L0 <pattern> <speed> <r> <g> <b>  台座の 4 個の LED、石がないとき（待機）。返事は "OK"
//   L1 <pattern> <speed> <r> <g> <b>  台座の 4 個の LED、石が置かれているとき。切り替わりは 0.4 秒でなめらかに
//   M1 / M0                                      石への書き込み：M1 はリアルタイム（S を受けたら、石が置かれたらすぐ書く）、
//                                                M0 は給電だけ（石には書かない。石は自分のメモリにある光り方で光る。S は覚えるだけ）
//   F1 / F0                                      磁界 ON / OFF
//   ?                                            状態（磁界、石の UID、最後の書き込み）
//   R                                            調べる用（レジスタ、振幅、VDD_RF）
//   U <url>                                      石の NFC タグとしての URL を書く（スマホがかざして開く URL）
//   U?                                           石の今の URL を読む。返事は "URL <url>"
// 台から PC へ（命令への返事とは別に、いつでも来る）
//   T <回数> <触れているか>                       石がタッチされた（石がブロック 125 に書いた値を、台が 0.2 秒ごとに読む）
//   P <置かれているか> <振幅> <基準> <位相> <基準>  0.2 秒ごと。アンテナの振幅と位相（ST25R3916B の測定、0〜255）と、
//                                                石がないときの値。石のコイルが近いほど、どちらもずれる（ずれが強さ・距離の目安）
//   I <UID>                                      石が置かれたとき、その石の UID（16 桁の 16 進）
//   B                                            USB DFU に戻る（書き換え用）
//
// 石の ST25DV への置き場所（ユーザーメモリの最後のほう）
//   126: 'I' 'K' seq pattern     127: speed r g b                    いつも
//   123: pattern speed r g        124: b 続ける時間(0.1 秒) 'T' 'K'   タッチされたとき
//   石に送る pattern の最上位ビット（0x80）は「石の基板の青 LED も光らせる」（台はそのまま書くだけ）
//   seq は書くたびに 1 増える。石は seq が変わったときだけ読み直す
//
// ビルド: STMicroelectronics:stm32:GenC0:pnum=GENERIC_C071KBUX,usb=CDCgen（boards.local.txt を足しておく）
//   ライブラリ: STM32duino ST25R3916 2.1.0、STM32duino NFC-RFAL 2.0.2
// 書き込み: USB DFU（手順は firmware/README.md）

#include <SPI.h>
#include "rfal_nfc.h"
#include "rfal_rfst25r3916.h"
#include "patterns.h"
#include "stm32yyxx_ll_rcc.h"
#include "stm32yyxx_ll_system.h"
#include "stm32yyxx_ll_utils.h"

// ---- ピン ----
const uint32_t PIN_CS = PA15;
const uint32_t PIN_IRQ = PB6;

SPIClass stSpi(PB5, PB4, PB3);
RfalRfST25R3916Class rfst(&stSpi, PIN_CS, PIN_IRQ);
RfalNfcClass nfc(&rfst);

const uint8_t BLOCK_A = 126, BLOCK_B = 127, BLOCK_TOUCH = 125, BLOCK_TA = 123, BLOCK_TB = 124;

// ---- SK6812 × 4（PA8 = TIM1_CH1、オープンドレイン、1kΩ で 5V へプルアップ） ----
// 1 ビット 1.25µs（48MHz で 60 カウント）の PWM で、0 は 0.33µs、1 は 0.67µs だけ High にする。
// 次のビットの比較値は、更新イベントを待って書く（プリロードで次の周期から効く）

const uint8_t LED_COUNT = 4;
const uint16_t WS_PERIOD = 60, WS_T0 = 16, WS_T1 = 32;

void ledInit() {
  pinmap_pinout(PA_8, PinMap_TIM);
  GPIOA->OTYPER |= GPIO_OTYPER_OT8;  // オープンドレイン
  __HAL_RCC_TIM1_CLK_ENABLE();
  TIM1->PSC = 0;
  TIM1->ARR = WS_PERIOD - 1;
  TIM1->CCR1 = 0;
  TIM1->CCMR1 = (6 << TIM_CCMR1_OC1M_Pos) | TIM_CCMR1_OC1PE;  // PWM モード 1
  TIM1->CCER = TIM_CCER_CC1E;
  TIM1->BDTR = TIM_BDTR_MOE;
  TIM1->CR1 = TIM_CR1_ARPE;
  TIM1->EGR = TIM_EGR_UG;
  TIM1->CR1 |= TIM_CR1_CEN;
}

void ledShow(const uint8_t rgb[][3]) {
  noInterrupts();
  // 周期の頭にそろえてから最初のビットを書く。そろえないと、書いた直後に更新が来たとき最初のビットを
  // 取りこぼし、全体が 1 ビットずれる（最後の LED4 だけ色が変になる・消える、2026-11-14）
  TIM1->SR = 0;
  while (!(TIM1->SR & TIM_SR_UIF)) {}
  TIM1->SR = 0;
  for (uint8_t n = 0; n < LED_COUNT; n++) {
    uint8_t grb[3] = {rgb[n][1], rgb[n][0], rgb[n][2]};
    for (uint8_t k = 0; k < 3; k++) {
      for (int8_t b = 7; b >= 0; b--) {
        TIM1->CCR1 = (grb[k] >> b) & 1 ? WS_T1 : WS_T0;
        while (!(TIM1->SR & TIM_SR_UIF)) {}
        TIM1->SR = 0;
      }
    }
  }
  TIM1->CCR1 = 0;
  for (uint8_t i = 0; i < 2; i++) {
    while (!(TIM1->SR & TIM_SR_UIF)) {}
    TIM1->SR = 0;
  }
  interrupts();
  delayMicroseconds(100);  // ラッチ（80µs 以上 Low）
}

void ledAll(uint8_t r, uint8_t g, uint8_t b) {
  uint8_t c[LED_COUNT][3];
  for (uint8_t n = 0; n < LED_COUNT; n++) {
    c[n][0] = r;
    c[n][1] = g;
    c[n][2] = b;
  }
  ledShow(c);
}


// ---- USB DFU（ST のブートローダー）に戻る ----
// 工場出荷のオプションバイトでは BOOT0 ピンが効かないことがある（nBOOT_SEL）ので、ファームで入る。
//   ・シリアルで 'B' を送る
//   ・J5 の SWCLK（PA14）を 3V3 につないだまま USB を挿す
// どちらも、リセット直後のクロック設定より前にブートローダーへ飛ぶ（USB などが動き出す前）

const uint32_t BOOT_MAGIC = 0xB007DF00;
const uint32_t SYSTEM_MEMORY = 0x1FFF0000;
uint32_t bootRequest __attribute__((section(".noinit")));

void jumpToBootloader() {
  SysTick->CTRL = 0;
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();
  uint32_t sp = *(volatile uint32_t *)SYSTEM_MEMORY;
  uint32_t pc = *(volatile uint32_t *)(SYSTEM_MEMORY + 4);
  __set_MSP(sp);
  ((void (*)(void))pc)();
}

bool bootPinHigh() {
  __HAL_RCC_GPIOA_CLK_ENABLE();
  // PA14 を一時的にプルダウン付きの入力にして読む（読んだら SWCLK に戻す）
  uint32_t moder = GPIOA->MODER, pupdr = GPIOA->PUPDR;
  GPIOA->PUPDR = (pupdr & ~(3UL << 28)) | (2UL << 28);
  GPIOA->MODER = moder & ~(3UL << 28);
  for (volatile uint32_t i = 0; i < 2000; i++) {}
  bool high = GPIOA->IDR & (1UL << 14);
  GPIOA->MODER = moder;
  GPIOA->PUPDR = pupdr;
  return high;
}

void rebootToBootloader() {
  Serial.println("-> USB DFU");
  delay(100);
  bootRequest = BOOT_MAGIC;
  NVIC_SystemReset();
}

// ---- クロック：HSI 48MHz、USB は HSIUSB48 を USB の SOF に合わせて自動補正（CRS） ----

extern "C" void SystemClock_Config(void) {
  if (bootRequest == BOOT_MAGIC || bootPinHigh()) {
    bootRequest = 0;
    jumpToBootloader();
  }
  LL_FLASH_SetLatency(LL_FLASH_LATENCY_1);
  LL_RCC_HSI_Enable();
  while (LL_RCC_HSI_IsReady() != 1) {}
  LL_RCC_HSI48_Enable();
  while (LL_RCC_HSI48_IsReady() != 1) {}
  LL_RCC_SetHSIDiv(LL_RCC_HSI_DIV_1);
  LL_RCC_SetAHBPrescaler(LL_RCC_HCLK_DIV_1);
  LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSI);
  while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_HSI) {}
  LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
  LL_SetSystemCoreClock(48000000);
  HAL_InitTick(TICK_INT_PRIORITY);

  __HAL_RCC_CRS_CLK_ENABLE();
  RCC_CRSInitTypeDef crs = {};
  crs.Prescaler = RCC_CRS_SYNC_DIV1;
  crs.Source = RCC_CRS_SYNC_SOURCE_USB;
  crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
  crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&crs);
}

// ---- 石との NFC ----

bool fieldIsOn = false;
uint8_t seq = 0;
uint8_t lastUid[8];
bool haveUid = false;
ReturnCode lastErr = ERR_NONE;

bool fieldStart() {
  ReturnCode err = nfc.rfalNfcvPollerInitialize();
  if (err == ERR_NONE) err = rfst.rfalFieldOnAndStartGT();
  fieldIsOn = err == ERR_NONE;
  lastErr = err;
  return fieldIsOn;
}

void fieldStop() {
  rfst.rfalFieldOff();
  fieldIsOn = false;
}

// 石がいるか（UID を読めるか）。いれば lastUid に入れる
bool findStone() {
  rfalNfcvInventoryRes inv;
  uint16_t len = 0;
  ReturnCode err = nfc.rfalNfcvPollerInventory(RFAL_NFCV_NUM_SLOTS_1, 0, NULL, &inv, &len);
  haveUid = err == ERR_NONE;
  if (haveUid) memcpy(lastUid, inv.UID, 8);
  lastErr = err;
  return haveUid;
}

// 1 ブロック（4 バイト）を書いて、読み返して確かめる。書き込み中の EEPROM で失敗したら数回やり直す
bool writeBlock(uint8_t block, const uint8_t data[4]) {
  for (uint8_t tryNo = 0; tryNo < 4; tryNo++) {
    ReturnCode err = nfc.rfalNfcvPollerWriteSingleBlock(RFAL_NFCV_REQ_FLAG_DEFAULT, NULL, block, data, 4);
    if (err == ERR_NONE) {
      uint8_t rx[1 + 4 + 2];
      uint16_t rcv = 0;
      err = nfc.rfalNfcvPollerReadSingleBlock(RFAL_NFCV_REQ_FLAG_DEFAULT, NULL, block, rx, sizeof(rx), &rcv);
      if (err == ERR_NONE && rcv >= 5 && memcmp(rx + 1, data, 4) == 0) return true;
      if (err == ERR_NONE) err = ERR_WRITE;
    }
    lastErr = err;
    delay(10);
  }
  return false;
}

// 台座の LED の光り方。[0] 石がないとき、[1] 置かれているとき。loop で 20ms ごとに描き直す
struct Look {
  uint8_t pat, spd, r, g, b;
};
Look looks[2] = {{3, 60, 30, 60, 160}, {6, 140, 200, 200, 200}};  // 待機は青の点滅、置いたら虹

bool parse5(char *args, int v[5]) {
  if (sscanf(args, "%d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4]) != 5) return false;
  for (int i = 0; i < 5; i++) v[i] = constrain(v[i], 0, 255);
  return true;
}

bool present = false;  // 石が置かれているか（pollStone が決める）
bool liveWrite = true;  // M1：画面の設定をすぐ石に書く／M0：給電だけ
// 置かれてから光り方を書くまで待つ（石の起動中に RF で書かない）
const uint16_t LOOKS_DELAY_MS = 1500;
uint32_t looksDueAt = 0;
uint8_t looksTries = 0;

// 石の光り方。[0] いつも、[1] タッチされたとき
Look stoneLooks[2] = {{2, 120, 255, 255, 255}, {1, 128, 255, 255, 255}};
uint8_t touchHoldDs = 20;

// 両方を書く。中身（127・123・124）を先に、最後に seq の入った 126 を書く。石は seq が変わったときだけ読み直す
bool writeStoneLooks() {
  const Look &N = stoneLooks[0], &T = stoneLooks[1];
  seq++;
  uint8_t a[4] = {'I', 'K', seq, N.pat};
  uint8_t b[4] = {N.spd, N.r, N.g, N.b};
  uint8_t ta[4] = {T.pat, T.spd, T.r, T.g};
  uint8_t tb[4] = {T.b, touchHoldDs, 'T', 'K'};
  return writeBlock(BLOCK_B, b) && writeBlock(BLOCK_TA, ta) && writeBlock(BLOCK_TB, tb) && writeBlock(BLOCK_A, a);
}

void cmdStone(char *args) {
  int v[6];
  uint8_t slot = args[0] == '1' ? 1 : 0;
  if (args[0] != '0' && args[0] != '1') {
    Serial.println("NG args");
    return;
  }
  int n = sscanf(args + 1, "%d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
  if (n < 5 || (slot == 1 && n < 6)) {
    Serial.println("NG args");
    return;
  }
  for (int i = 0; i < 5; i++) v[i] = constrain(v[i], 0, 255);
  stoneLooks[slot] = {(uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3], (uint8_t)v[4]};
  if (slot == 1) touchHoldDs = constrain(v[5], 1, 255);
  // 石がないときは覚えるだけ（置かれたときに書く）
  if (!present || looksDueAt || !liveWrite) {  // 置かれた直後なら起動を待ってから書く（looksDueAt）。給電だけなら書かない
    Serial.println("OK");
    return;
  }
  if (!fieldIsOn && !fieldStart()) {
    Serial.println("NG field");
    return;
  }
  if (writeStoneLooks()) {
    Serial.print("OK ");
    Serial.println(seq);
  } else {
    Serial.print("NG write err ");
    Serial.println(lastErr);
  }
}

void cmdLed(char *args) {
  int v[5];
  uint8_t slot = args[0] == '1' ? 1 : 0;
  if ((args[0] != '0' && args[0] != '1') || !parse5(args + 1, v)) {
    Serial.println("NG args");
    return;
  }
  looks[slot] = {(uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3], (uint8_t)v[4]};
  Serial.println("OK");
}

void printStatus() {
  Serial.print("field ");
  Serial.print(fieldIsOn ? "on" : "off");
  if (fieldIsOn && findStone()) {
    Serial.print("  stone UID ");
    for (int8_t i = 7; i >= 0; i--) {
      if (lastUid[i] < 16) Serial.print('0');
      Serial.print(lastUid[i], HEX);
    }
  } else {
    Serial.print("  no stone (err ");
    Serial.print(lastErr);
    Serial.print(")");
  }
  Serial.print("  seq ");
  Serial.println(seq);
}

// 調べる用：主なレジスタ、アンテナの振幅、VDD_RF（立ち上げ用ファームの値と比べる）
void printDebug() {
  const uint16_t regs[] = {0x01, 0x02, 0x03, 0x28, 0x2C, 0x31, 0x40 | 0x2C, 0x40 | 0x28};
  for (uint16_t r : regs) {
    uint8_t v = 0;
    rfst.rfalChipReadReg(r, &v, 1);
    Serial.print(r > 0x3F ? "B" : "");
    Serial.print(r & 0x3F, HEX);
    Serial.print('=');
    Serial.print(v, HEX);
    Serial.print(' ');
  }
  uint8_t amp = 0, vrf = 0;
  rfst.rfalChipMeasureAmplitude(&amp);
  rfst.rfalChipMeasurePowerSupply(3, &vrf);  // mpsv 3 = VDD_RF
  Serial.print(" amp ");
  Serial.print(amp);
  Serial.print(" VDD_RF ");
  Serial.print(vrf * 234UL / 10);
  Serial.println("mV");
}

// ---- NFC タグとしての URL（NDEF） ----
// ブロック 0 は CC（タグの目印、石が起動時に書いている）。ブロック 1 から NDEF の TLV（03 長さ レコード FE）を書く。
// デモの値（ブロック 123〜127）の手前までに収める。URL の頭は NFC Forum の URI の略号に置き換える
const char *URI_PREFIX[] = {"", "http://www.", "https://www.", "http://", "https://"};
const uint16_t NDEF_MAX = (BLOCK_TA - 1) * 4;

bool readBlock(uint8_t block, uint8_t out[4]) {
  uint8_t rx[1 + 4 + 2];
  uint16_t rcv = 0;
  for (uint8_t tryNo = 0; tryNo < 3; tryNo++) {
    if (nfc.rfalNfcvPollerReadSingleBlock(RFAL_NFCV_REQ_FLAG_DEFAULT, NULL, block, rx, sizeof(rx), &rcv) == ERR_NONE && rcv >= 5) {
      memcpy(out, rx + 1, 4);
      return true;
    }
    delay(5);
  }
  return false;
}

void cmdUrl(char *arg) {
  while (*arg == ' ') arg++;
  if (!fieldIsOn && !fieldStart()) {
    Serial.println("NG field");
    return;
  }
  uint8_t cc[4];
  if (!readBlock(0, cc) || cc[0] != 0xE1) {
    Serial.println("NG no tag or not formatted");
    return;
  }
  if (arg[0] == '?') {
    // 読む：TLV の長さを見て、必要なブロックだけ読む
    uint8_t buf[NDEF_MAX];
    if (!readBlock(1, buf)) {
      Serial.println("NG read");
      return;
    }
    uint16_t total = 2 + buf[1];
    for (uint16_t b = 1; b * 4 < total && b * 4 < NDEF_MAX; b++)
      if (!readBlock(1 + b, buf + b * 4)) {
        Serial.println("NG read");
        return;
      }
    // 03 L D1 01 PL 55 code url...
    if (buf[0] != 0x03 || buf[1] < 5 || buf[2] != 0xD1 || buf[5] != 'U') {
      Serial.println("URL (none)");
      return;
    }
    Serial.print("URL ");
    if (buf[6] < 5) Serial.print(URI_PREFIX[buf[6]]);
    for (uint16_t i = 0; i < buf[4] - 1u; i++) Serial.print((char)buf[7 + i]);
    Serial.println();
    return;
  }
  // 書く：いちばん長く当てはまる頭を略号にする
  uint8_t code = 0;
  for (uint8_t c = 1; c < 5; c++) {
    size_t n = strlen(URI_PREFIX[c]);
    if (strncmp(arg, URI_PREFIX[c], n) == 0 && n > strlen(URI_PREFIX[code])) code = c;
  }
  const char *rest = arg + strlen(URI_PREFIX[code]);
  size_t restLen = strlen(rest);
  size_t payload = 1 + restLen;       // 略号 + 残り
  size_t record = 4 + payload;        // D1 01 PL 55 + payload
  size_t tlv = 2 + record + 1;        // 03 L ... FE
  if (record > 254 || tlv > NDEF_MAX) {
    Serial.println("NG too long");
    return;
  }
  uint8_t buf[NDEF_MAX] = {0};
  size_t i = 0;
  buf[i++] = 0x03;
  buf[i++] = record;
  buf[i++] = 0xD1;
  buf[i++] = 0x01;
  buf[i++] = payload;
  buf[i++] = 'U';
  buf[i++] = code;
  memcpy(buf + i, rest, restLen);
  i += restLen;
  buf[i++] = 0xFE;
  for (size_t b = 0; b * 4 < i; b++)
    if (!writeBlock(1 + b, buf + b * 4)) {
      Serial.print("NG write err ");
      Serial.println(lastErr);
      return;
    }
  Serial.print("OK url ");
  Serial.print(i);
  Serial.println(" bytes");
}

char line[300];
uint16_t lineLen = 0;

void handleLine(char *s) {
  switch (s[0]) {
    case 'S': cmdStone(s + 1); break;
    case 'L': cmdLed(s + 1); break;
    case 'M':
      liveWrite = s[1] == '1';
      // リアルタイムに戻したら、石が置かれていれば今の設定をすぐ書く（起動待ちの途中なら、その予定に任せる）
      if (liveWrite && present && !looksDueAt) {
        looksDueAt = millis();
        looksTries = 0;
      }
      if (!liveWrite) looksDueAt = 0;
      Serial.println(liveWrite ? "OK live" : "OK power only");
      break;
    case 'F':
      if (s[1] == '1') Serial.println(fieldStart() ? "OK field on" : "NG field");
      else {
        fieldStop();
        Serial.println("OK field off");
      }
      break;
    case '?': printStatus(); break;
    case 'R': printDebug(); break;
    case 'U': cmdUrl(s + 1); break;
    case 'B': rebootToBootloader(); break;
    default: Serial.println("NG unknown"); break;
  }
}

void setup() {
  ledInit();
  ledAll(0, 0, 20);
  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 2000) {}

  stSpi.begin();
  ReturnCode err = nfc.rfalNfcInitialize();
  if (err != ERR_NONE) {
    Serial.print("NG rfal init ");
    Serial.println(err);
    ledAll(40, 0, 0);
    return;
  }
  if (fieldStart()) {
    ledAll(0, 20, 0);
    Serial.println("ISHI-KI dai demo ready (field on)");
  } else {
    ledAll(40, 0, 0);
    Serial.print("NG field ");
    Serial.println(lastErr);
  }
}

uint32_t lastFrame, lastPoll;
int lastTouchCount = -1, lastTouching = -1;

// ---- 石が置かれているか・どのくらい近いか ----
// 置かれているか：石の ST25DV が返事をするか（ブロック 125 を読めるか）。3 回続けて返事がなければ外れた
// 近さ：アンテナの振幅。石のコイルが磁界を吸うので、近いほど下がる。石がないときの振幅（基準）との差を PC に送る。
//   基準は、石がないあいだ少しずつ追いかける（温度などのゆっくりした変化）。振幅が基準より上がったら、すぐ基準を上げる
//   （起動時に何か乗っていた場合など）。返事はないが近くにある石（読める距離の少し外）で基準が下がらないよう、
//   追いかけるのは差がノイズ程度のときだけ
const uint32_t POLL_MS = 200;
const uint8_t MISS_TO_REMOVE = 3;
uint8_t misses = 0;
int32_t base16 = -1, phBase16 = -1;  // 基準 ×16
uint8_t lastAmp = 0, lastPh = 0;
uint32_t placedAt = 0, removedAt = 0;

void printUid(const uint8_t uid[8]) {
  for (int8_t i = 7; i >= 0; i--) {
    if (uid[i] < 16) Serial.print('0');
    Serial.print(uid[i], HEX);
  }
}

// 石が書いたタッチの回数を読む。読めたら石はいる。変わっていたら PC に知らせる
bool pollTouch() {
  uint8_t rx[1 + 4 + 2];
  uint16_t rcv = 0;
  if (nfc.rfalNfcvPollerReadSingleBlock(RFAL_NFCV_REQ_FLAG_DEFAULT, NULL, BLOCK_TOUCH, rx, sizeof(rx), &rcv) != ERR_NONE || rcv < 5)
    return false;
  if (rx[1] != 'T') return true;
  if (rx[2] == lastTouchCount && rx[3] == lastTouching) return true;
  bool first = lastTouchCount < 0;
  lastTouchCount = rx[2];
  lastTouching = rx[3];
  if (first) return true;  // 置かれた直後の 1 回目は、前からある値なので知らせない
  Serial.print("T ");
  Serial.print(lastTouchCount);
  Serial.print(' ');
  Serial.println(lastTouching);
  return true;
}

void pollStone() {
  uint8_t amp = 0, ph = 0;
  bool ampOk = rfst.rfalChipMeasureAmplitude(&amp) == ERR_NONE;
  bool phOk = rfst.rfalChipMeasurePhase(&ph) == ERR_NONE;
  bool seen = pollTouch();
  if (seen) {
    misses = 0;
    if (!present) {
      present = true;
      placedAt = millis();
      // 置かれた石に、画面で選んだ光り方を書く。ただし石の起動（I2C で ST25DV を準備する間）が終わるまで待つ。
      // RF で書いている間は石の I2C が NACK になり、古い石のファームは起動に失敗して止まった（2026-11-15）
      if (liveWrite) {
        looksDueAt = millis() + LOOKS_DELAY_MS;
        looksTries = 0;
      }
      if (findStone()) {
        Serial.print("I ");
        printUid(lastUid);
        Serial.println();
      }
    }
  } else if (present && ++misses >= MISS_TO_REMOVE) {
    present = false;
    removedAt = millis();
    lastTouchCount = lastTouching = -1;  // 次に置かれた石の古い回数を知らせないように
  }
  if (ampOk) {
    lastAmp = amp;
    int32_t a16 = (int32_t)amp * 16;
    if (base16 < 0 || a16 > base16) base16 = a16;
    else if (!present && base16 - a16 <= 2 * 16) base16 += (a16 - base16) / 16;
  }
  // 位相は石でどちら向きにずれるか決まっていないので、石がないあいだだけ平均で追いかける
  if (phOk) {
    lastPh = ph;
    int32_t p16 = (int32_t)ph * 16;
    if (phBase16 < 0) phBase16 = p16;
    else if (!present) phBase16 += (p16 - phBase16) / 8;
  }
  if (Serial && Serial.availableForWrite() > 32) {
    Serial.print("P ");
    Serial.print(present ? 1 : 0);
    Serial.print(' ');
    Serial.print(lastAmp);
    Serial.print(' ');
    Serial.print(base16 < 0 ? 0 : (base16 + 8) / 16);
    Serial.print(' ');
    Serial.print(lastPh);
    Serial.print(' ');
    Serial.println(phBase16 < 0 ? 0 : (phBase16 + 8) / 16);
  }
}

// 待機と置いたときの光り方を、0.4 秒かけて混ぜて切り替える
const uint16_t FADE_MS = 400;

void drawLeds(uint32_t now) {
  uint32_t since = now - (present ? placedAt : removedAt);
  uint16_t f = since >= FADE_MS ? 255 : since * 255 / FADE_MS;  // 今の状態の割合
  if (placedAt == 0 && removedAt == 0) f = 255;
  const Look &to = looks[present ? 1 : 0], &from = looks[present ? 0 : 1];
  PatternOut a = patternAt(now, to.pat, to.spd, to.r, to.g, to.b);
  if (f < 255) {
    PatternOut b = patternAt(now, from.pat, from.spd, from.r, from.g, from.b);
    a.r = (a.r * f + b.r * (255 - f)) / 255;
    a.g = (a.g * f + b.g * (255 - f)) / 255;
    a.b = (a.b * f + b.b * (255 - f)) / 255;
  }
  ledAll(a.r, a.g, a.b);
}

void loop() {
  uint32_t now = millis();
  if (now - lastFrame >= 20) {
    lastFrame = now;
    drawLeds(now);
  }
  if (fieldIsOn && now - lastPoll >= POLL_MS) {
    lastPoll = now;
    pollStone();
    if (present && looksDueAt && (int32_t)(now - looksDueAt) >= 0) {
      if (writeStoneLooks() || ++looksTries >= 5) looksDueAt = 0;
    }
  }
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line[lineLen] = 0;
      if (lineLen) handleLine(line);
      lineLen = 0;
    } else if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = c;
    }
  }
}
