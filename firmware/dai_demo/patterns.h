// 光り方（台と石で共通。石のファームにも同じものをコピーして使う）
// pattern: 0 消灯 / 1 点灯 / 2 呼吸 / 3 点滅 / 4 鼓動 / 5 ろうそく / 6 虹（色相が回る）/ 7 きらめき
// speed 0〜255 で 1 周期が 4 秒〜0.4 秒。戻り値は、色 (r,g,b) に掛ける明るさと、虹のときの色
#pragma once
#include <stdint.h>

struct PatternOut {
  uint8_t r, g, b;
};

static inline uint16_t patternPeriodMs(uint8_t speed) {
  return 4000 - (uint32_t)speed * 3600 / 255;
}

// 0〜255 の三角波
static inline uint8_t tri8(uint32_t t, uint16_t period) {
  uint32_t x = (t % period) * 510 / period;
  return x < 256 ? x : 510 - x;
}

// 目に自然に見えるよう 2 乗で暗い側を広げる
static inline uint8_t gamma8(uint8_t x) {
  return (uint16_t)x * x / 255;
}

static inline uint32_t rand32(uint32_t &s) {
  s ^= s << 13;
  s ^= s >> 17;
  s ^= s << 5;
  return s;
}

static inline void hueToRgb(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = h / 43, rem = (h - region * 43) * 6;
  uint8_t q = 255 - rem, t = rem;
  switch (region) {
    case 0: r = 255; g = t; b = 0; break;
    case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break;
    case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break;
    default: r = 255; g = 0; b = q; break;
  }
}

static inline PatternOut patternAt(uint32_t t, uint8_t pattern, uint8_t speed, uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t seed = 0x1234567;
  static uint8_t flick = 255;
  static uint32_t nextFlick = 0;
  uint16_t period = patternPeriodMs(speed);
  uint8_t k = 255;
  switch (pattern) {
    case 0: k = 0; break;
    case 1: k = 255; break;
    case 2: k = gamma8(tri8(t, period)); break;
    case 3: k = (t % period) < period / 2 ? 255 : 0; break;
    case 4: {  // ドクン、ドクン、休み
      uint32_t p = t % period;
      uint16_t beat = period / 8;
      if (p < beat) k = gamma8(255 - p * 255 / beat);
      else if (p >= beat * 2 && p < beat * 3) k = gamma8(255 - (p - beat * 2) * 255 / beat);
      else k = 0;
      break;
    }
    case 5:  // ろうそく：ときどき明るさが揺れる
      if ((int32_t)(t - nextFlick) >= 0) {
        flick = 120 + rand32(seed) % 136;
        nextFlick = t + 30 + rand32(seed) % (period / 10 + 1);
      }
      k = flick;
      break;
    case 6: {  // 虹：色相を回し、明るさは r,g,b の一番大きいもの
      uint8_t lvl = r > g ? (r > b ? r : b) : (g > b ? g : b);
      uint8_t hr, hg, hb;
      hueToRgb((t % period) * 255 / period, hr, hg, hb);
      return {(uint8_t)((uint16_t)hr * lvl / 255), (uint8_t)((uint16_t)hg * lvl / 255), (uint8_t)((uint16_t)hb * lvl / 255)};
    }
    case 7:  // きらめき：ふだんは暗く、たまに強く光る
      if ((int32_t)(t - nextFlick) >= 0) {
        flick = (rand32(seed) % 6 == 0) ? 255 : 40;
        nextFlick = t + 40 + rand32(seed) % (period / 4 + 1);
      }
      k = flick;
      break;
  }
  return {(uint8_t)((uint16_t)r * k / 255), (uint8_t)((uint16_t)g * k / 255), (uint8_t)((uint16_t)b * k / 255)};
}
