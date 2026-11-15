// ISHI-KI 石（ishi）デモ用ファーム：台が NFC で書いた色と光り方を読んで、作家用の LED を光らせる
//
// 台は ST25DV のユーザーメモリの最後のほうに書く
//   504: 'I' 'K' seq pattern     508: speed r g b              いつもの光り方
//   492: pattern speed r g        496: b 続ける時間(0.1 秒) 'T' 'K'  タッチされたときの光り方
//   pattern の最上位ビット（0x80）は「基板の確認用の青 LED も光らせる」。台は 492〜508 を書いてから最後に 504 の seq を変える。
//   石は seq が変わったら両方を読み直す。496 の 'T' 'K' がなければ、タッチ用の光り方はなし（触ったら一瞬光るだけ）
// タッチ：裏の A7 パッド（PA7）に線をつなぎ、その先の金属やアルミ箔を電極にする（PTC の自己容量）。
//   触られたら、タッチ用の光り方に切り替える（触れている間と、離してから決めた時間）。回数を ST25DV のブロック 125（バイト 500〜503: 'T' 回数 触れているか 0）に書く。
//   台がそれを NFC で読んで PC に知らせる。PA7 は起動時のシリアル出力にも使うので、出力が終わってから電極にする
// r・g・b を CH1（PA3）・CH2（PA4）・CH3（PA5）の明るさにする。外付けの LED が 1 個なら、どの穴につないでもよい
// （台の UI は、単色の LED 用に r=g=b を送る）。光り方は patterns.h（台と共通）。
//
// 起動時の準備（EH_MODE=0、CC の初期化）は ishi_bringup と同じ。新しい石は、書き込み器から電源をもらって
// 書き込んだ時点でこれが済むので、そのあとは台やスマホの磁界だけで起きる。
//
// 書き込み設定（megaTinyCore）: ATtiny1616, Clock 1 MHz internal, BOD Disabled（OSCCFG は 0x01）

#include <util/delay.h>
#include "patterns.h"
#include <ptc.h>

#define LOG_BAUD 4800

const uint8_t ADDR_USER = 0x53;  // ユーザーメモリと動的レジスタ
const uint8_t ADDR_SYS = 0x57;   // システム設定

// ---- シリアル出力（PA7、ソフトウェア、送信だけ） ----

void logByte(uint8_t c) {
  const uint16_t bitUs = 1000000UL / LOG_BAUD;
  uint8_t sreg = SREG;
  cli();
  PORTA.OUTCLR = PIN7_bm;  // スタートビット
  _delay_us(bitUs);
  for (uint8_t i = 0; i < 8; i++) {
    if (c & 1) PORTA.OUTSET = PIN7_bm;
    else PORTA.OUTCLR = PIN7_bm;
    c >>= 1;
    _delay_us(bitUs);
  }
  PORTA.OUTSET = PIN7_bm;  // ストップビット
  _delay_us(bitUs);
  SREG = sreg;
}

void logStr(const char *s) {
  while (*s) logByte(*s++);
}

void logHex(uint8_t v) {
  const char *h = "0123456789ABCDEF";
  logByte(h[v >> 4]);
  logByte(h[v & 15]);
}

// ---- ソフトウェア I2C（SCL = PB0、SDA = PC0、マイコン内蔵のプルアップ） ----
// Low にするときだけ出力にし、High は入力にしてプルアップに任せる（オープンドレインのまね）

#define I2C_HALF_US 10

inline void sclLow() { PORTB.DIRSET = PIN0_bm; }
inline void sclRelease() {
  PORTB.DIRCLR = PIN0_bm;
  for (uint8_t i = 0; i < 100 && !(PORTB.IN & PIN0_bm); i++) _delay_us(1);  // クロックストレッチ待ち
}
inline void sdaLow() { PORTC.DIRSET = PIN0_bm; }
inline void sdaRelease() { PORTC.DIRCLR = PIN0_bm; }
inline bool sdaRead() { return PORTC.IN & PIN0_bm; }

void i2cInit() {
  PORTB.OUTCLR = PIN0_bm;
  PORTC.OUTCLR = PIN0_bm;
  PORTB.PIN0CTRL = PORT_PULLUPEN_bm;
  PORTC.PIN0CTRL = PORT_PULLUPEN_bm;
  sdaRelease();
  sclRelease();
}

// ST25DV の電源を切る前に、I2C のピンからタグへ電流が流れ込まないようにする
void i2cOff() {
  PORTB.PIN0CTRL = 0;
  PORTC.PIN0CTRL = 0;
  PORTB.DIRCLR = PIN0_bm;
  PORTC.DIRCLR = PIN0_bm;
}

void i2cStart() {
  sdaRelease();
  sclRelease();
  _delay_us(I2C_HALF_US);
  sdaLow();
  _delay_us(I2C_HALF_US);
  sclLow();
}

void i2cStop() {
  sdaLow();
  _delay_us(I2C_HALF_US);
  sclRelease();
  _delay_us(I2C_HALF_US);
  sdaRelease();
  _delay_us(I2C_HALF_US);
}

bool i2cWrite(uint8_t b) {  // ACK が返れば true
  for (uint8_t i = 0; i < 8; i++) {
    if (b & 0x80) sdaRelease();
    else sdaLow();
    b <<= 1;
    _delay_us(I2C_HALF_US);
    sclRelease();
    _delay_us(I2C_HALF_US);
    sclLow();
  }
  sdaRelease();
  _delay_us(I2C_HALF_US);
  sclRelease();
  bool ack = !sdaRead();
  _delay_us(I2C_HALF_US);
  sclLow();
  return ack;
}

uint8_t i2cRead(bool ack) {
  uint8_t b = 0;
  sdaRelease();
  for (uint8_t i = 0; i < 8; i++) {
    _delay_us(I2C_HALF_US);
    sclRelease();
    b = (b << 1) | (sdaRead() ? 1 : 0);
    _delay_us(I2C_HALF_US);
    sclLow();
  }
  if (ack) sdaLow();
  _delay_us(I2C_HALF_US);
  sclRelease();
  _delay_us(I2C_HALF_US);
  sclLow();
  sdaRelease();
  return b;
}

// ST25DV は 16 ビットのアドレスで読み書きする
bool stRead(uint8_t dev, uint16_t addr, uint8_t *buf, uint8_t n) {
  i2cStart();
  bool ok = i2cWrite(dev << 1) && i2cWrite(addr >> 8) && i2cWrite(addr & 0xFF);
  if (ok) {
    i2cStart();
    ok = i2cWrite((dev << 1) | 1);
    for (uint8_t i = 0; ok && i < n; i++) buf[i] = i2cRead(i + 1 < n);
  }
  i2cStop();
  return ok;
}

// EEPROM への書き込みは、ACK が返るまで待つ（書き込み中は NACK）
bool stWaitReady(uint8_t dev) {
  for (uint8_t i = 0; i < 50; i++) {
    i2cStart();
    bool ack = i2cWrite(dev << 1);
    i2cStop();
    if (ack) return true;
    _delay_ms(1);
  }
  return false;
}

bool stWrite(uint8_t dev, uint16_t addr, const uint8_t *buf, uint8_t n) {
  i2cStart();
  bool ok = i2cWrite(dev << 1) && i2cWrite(addr >> 8) && i2cWrite(addr & 0xFF);
  for (uint8_t i = 0; ok && i < n; i++) ok = i2cWrite(buf[i]);
  i2cStop();
  return ok && stWaitReady(dev);
}

// スマホなどが RF で読んでいる間は、ST25DV は I2C に NACK を返す。起動時の読み書きは、少し待ってやり直す
// （1 回で諦めて止まると、スマホの上では「光ったり光らなかったり」になった、2026-11-15）
const uint8_t RETRY = 40;  // 25ms おき、最大 1 秒
bool stReadRetry(uint8_t dev, uint16_t addr, uint8_t *buf, uint8_t n) {
  for (uint8_t i = 0; i < RETRY; i++) {
    if (stRead(dev, addr, buf, n)) return true;
    delay(25);
  }
  return false;
}
bool stWriteRetry(uint8_t dev, uint16_t addr, const uint8_t *buf, uint8_t n) {
  for (uint8_t i = 0; i < RETRY; i++) {
    if (stWrite(dev, addr, buf, n)) return true;
    delay(25);
  }
  return false;
}

// ---- 青 LED ----

void blueBlink(uint8_t n, uint16_t onMs, uint16_t offMs) {
  for (uint8_t i = 0; i < n; i++) {
    PORTA.OUTSET = PIN6_bm;
    delay(onMs);
    PORTA.OUTCLR = PIN6_bm;
    delay(offMs);
  }
}

// 失敗した段を、速い点滅の回数で 1 度だけ知らせて、そのまま光らせる処理へ進む
// （前の起動で準備は済んでいることが多いので、止まるより、いつもの光り方で動き続けるほうがよい）
bool setupFailed = false;
void fail(uint8_t step, const char *msg) {
  logStr("NG ");
  logStr(msg);
  logStr("\r\n");
  blueBlink(step, 80, 220);
  setupFailed = true;
}

uint8_t step = 0;

// 起動時の準備（IC の確認、EH_MODE、CC）。途中で失敗したら、そこで打ち切る（残りの段を壊れた値で進めない）
bool prepareTag() {
  // 段 2：ST25DV に給電して、IC の情報を読む
  step = 2;
  uint8_t icRef, uid[8], mem[3];
  if (!stReadRetry(ADDR_SYS, 0x0017, &icRef, 1)) { fail(step, "no ACK from ST25DV (0x57)"); return false; }
  if (!stReadRetry(ADDR_SYS, 0x0018, uid, 8)) { fail(step, "UID read"); return false; }
  if (!stReadRetry(ADDR_SYS, 0x0014, mem, 3)) { fail(step, "MEM_SIZE read"); return false; }
  uint16_t blocks = (mem[0] | (mem[1] << 8)) + 1;
  uint16_t bytes = blocks * (mem[2] + 1);
  logStr("IC_REF ");
  logHex(icRef);
  logStr("  UID ");
  for (int8_t i = 7; i >= 0; i--) logHex(uid[i]);
  logStr("  blocks ");
  logHex(blocks >> 8);
  logHex(blocks & 0xFF);
  logStr("\r\n");

  // 段 3：EH_MODE を 0 に（まだならパスワードを出して書く）
  step = 3;
  uint8_t ehMode;
  if (!stReadRetry(ADDR_SYS, 0x0002, &ehMode, 1)) { fail(step, "EH_MODE read"); return false; }
  logStr("EH_MODE ");
  logHex(ehMode);
  logStr("\r\n");
  if (ehMode != 0) {
    uint8_t pwd[17] = {0};  // パスワード 8 バイト、確認コード 0x09、もう一度パスワード 8 バイト
    pwd[8] = 0x09;
    if (!stWriteRetry(ADDR_SYS, 0x0900, pwd, 17)) { fail(step, "password write"); return false; }
    uint8_t sso;
    if (!stReadRetry(ADDR_USER, 0x2004, &sso, 1) || !(sso & 1)) { fail(step, "I2C session not open (password?)"); return false; }
    uint8_t zero = 0;
    if (!stWriteRetry(ADDR_SYS, 0x0002, &zero, 1)) { fail(step, "EH_MODE write"); return false; }
    if (!stReadRetry(ADDR_SYS, 0x0002, &ehMode, 1) || ehMode != 0) { fail(step, "EH_MODE verify"); return false; }
    logStr("EH_MODE -> 00\r\n");
  }

  // 段 4：CC が空なら NFC Forum Type 5 として初期化する
  step = 4;
  uint8_t cc[4];
  if (!stReadRetry(ADDR_USER, 0x0000, cc, 4)) { fail(step, "CC read"); return false; }
  logStr("CC ");
  for (uint8_t i = 0; i < 4; i++) logHex(cc[i]);
  logStr("\r\n");
  if (cc[0] != 0xE1) {
    // E1：NDEF あり、40：版 1.0・読み書き可、MLEN：容量 / 8、01：まとめ読みに対応
    uint8_t init[7] = {0xE1, 0x40, (uint8_t)(bytes / 8), 0x01, 0x03, 0x00, 0xFE};
    if (!stWriteRetry(ADDR_USER, 0x0000, init, 7)) { fail(step, "CC write"); return false; }
    logStr("CC formatted\r\n");
  }

  uint8_t ehDyn;
  if (stReadRetry(ADDR_USER, 0x2002, &ehDyn, 1)) {
    logStr("EH_CTRL_Dyn ");
    logHex(ehDyn);
    logStr(ehDyn & 0x04 ? "  field ON\r\n" : "  field off\r\n");
  }

  return true;
}

void setup() {
  // 使わないピンの入力バッファを止めて、受電だけで動くときの電流を減らす
  PORTA.PIN1CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTA.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTB.PIN1CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTB.PIN4CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTB.PIN5CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTC.PIN1CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTC.PIN2CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTC.PIN3CTRL = PORT_ISC_INPUT_DISABLE_gc;
  PORTB.PIN2CTRL = PORT_PULLUPEN_bm;  // GPO（オープンドレイン）

  PORTA.OUTSET = PIN7_bm;  // シリアルの待機は High
  PORTA.DIRSET = PIN3_bm | PIN4_bm | PIN5_bm | PIN6_bm | PIN7_bm;

  logStr("\r\nISHI-KI ishi demo\r\n");

  // 段 2〜4：ST25DV に給電して、準備する
  step = 2;
  PORTB.OUTSET = PIN3_bm;
  PORTB.DIRSET = PIN3_bm;
  delay(5);
  i2cInit();
  prepareTag();
  logStr("OK\r\n");
  // デモでは ST25DV の I2C 側を入れたまま、台が書いた値を読み続ける
  touchBegin();
}


// ---- 台が書いた色と光り方 ----

const uint16_t DEMO_ADDR = 504;
const uint16_t POLL_MS = 150;
struct StoneLook {
  uint8_t pat, spd, r, g, b;
  bool mirror;
};
StoneLook normal = {2, 128, 255, 255, 255, true};  // まだ何も書かれていない石は、白でゆっくり明滅
StoneLook touchLook;
bool haveTouchLook = false;
uint16_t touchHoldMs = 2000;
const uint16_t TOUCH_LOOK_ADDR = 492;
uint8_t lastSeq = 0;
bool haveSeq = false;
uint32_t lastPoll;

// ---- タッチ ----
const uint16_t TOUCH_ADDR = 500;
const uint16_t FLASH_MS = 700;
cap_sensor_t touchNode;
uint8_t touchCount = 0;
bool touching = false, touchDirty = false;
uint32_t touchAt = 0, touchUntil = 0;

void ptc_event_cb_touch(const ptc_cb_event_t eventType, cap_sensor_t *node) {
  (void)node;
  if (eventType == PTC_CB_EVENT_TOUCH_DETECT) {
    touching = true;
    touchCount++;
    touchAt = millis();
  } else if (eventType == PTC_CB_EVENT_TOUCH_RELEASE) {
    touching = false;
  }
  touchDirty = true;
}

void touchBegin() {
  PORTA.DIRCLR = PIN7_bm;  // シリアル出力をやめて、電極にする
  ptc_add_selfcap_node(&touchNode, 0, PIN_TO_PTC(PIN_PA7));
}

// 回数を ST25DV に書く。RF が使用中で NACK のときは、次の回にもう一度
void touchReport() {
  if (!touchDirty) return;
  uint8_t d[4] = {'T', touchCount, (uint8_t)touching, 0};
  if (stWrite(ADDR_USER, TOUCH_ADDR, d, 4)) touchDirty = false;
}

// RF が書き込み中は I2C が NACK になるので、そのときは次の回に読む
void pollTag() {
  uint8_t d[8];
  if (!stRead(ADDR_USER, DEMO_ADDR, d, 8)) return;
  if (d[0] != 'I' || d[1] != 'K') return;
  if (haveSeq && d[2] == lastSeq) return;
  // タッチ用も読む。読めなければ seq を覚えずに、次の回にもう一度
  uint8_t t[8];
  if (!stRead(ADDR_USER, TOUCH_LOOK_ADDR, t, 8)) return;
  haveSeq = true;
  lastSeq = d[2];
  normal = {(uint8_t)(d[3] & 0x7F), d[4], d[5], d[6], d[7], (bool)(d[3] & 0x80)};
  haveTouchLook = t[6] == 'T' && t[7] == 'K';
  if (haveTouchLook) {
    touchLook = {(uint8_t)(t[0] & 0x7F), t[1], t[2], t[3], t[4], (bool)(t[0] & 0x80)};
    touchHoldMs = (uint16_t)(t[5] ? t[5] : 1) * 100;
  }
}

void loop() {
  uint32_t now = millis();
  ptc_process(now);
  if (now - lastPoll >= POLL_MS) {
    lastPoll = now;
    pollTag();
    touchReport();
  }
  // 光り方の計算（割り算が多く 1MHz では重い）は 8ms ごとにして、その合間は青 LED の点け消しだけを速く回す。
  // 毎回計算していたらループが 1 周 3ms ほどになり、暗いときの青 LED がちらついた（2026-11-15）
  static PatternOut o;
  static uint32_t lastCalc;
  // タッチ用の光り方：触れている間はずっと、離してから touchHoldMs だけ
  if (touching) touchUntil = now + touchHoldMs;
  bool inTouch = haveTouchLook && touchAt && (int32_t)(touchUntil - now) > 0;
  const StoneLook &L = inTouch ? touchLook : normal;
  if (now - lastCalc >= 8) {
    lastCalc = now;
    o = patternAt(now, L.pat, L.spd, L.r, L.g, L.b);
    // タッチ用の光り方がない石は、触られた直後にいちばん明るく光らせてから元の光り方へ戻す
    if (!haveTouchLook && touchAt && now - touchAt < FLASH_MS) {
      uint8_t burst = 255 - (uint32_t)(now - touchAt) * 255 / FLASH_MS;
      if (o.r < burst) o.r = burst;
      if (o.g < burst) o.g = burst;
      if (o.b < burst) o.b = burst;
    }
    analogWrite(PIN_PA3, o.r);
    analogWrite(PIN_PA4, o.g);
    analogWrite(PIN_PA5, o.b);
  }
  if (!L.mirror) PORTA.OUTCLR = PIN6_bm;
  else {
    // PA6 にはハードの PWM がないので、ループのたびに明るさを貯めて、あふれたときだけ点ける（シグマデルタ）。
    // 時刻で比べるやり方は、タッチの処理でループが遅くなるとちらついた（2026-11-15）
    static uint8_t acc;
    uint8_t lvl = o.r > o.g ? (o.r > o.b ? o.r : o.b) : (o.g > o.b ? o.g : o.b);
    uint8_t before = acc;
    acc += lvl;
    if (acc < before || lvl == 255) PORTA.OUTSET = PIN6_bm;
    else PORTA.OUTCLR = PIN6_bm;
  }
}
