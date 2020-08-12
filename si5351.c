/*
 * Copyright (c) 2019-2020, Dmitry (DiSlord) dislordlive@gmail.com
 * Based on TAKAHASHI Tomohiro (TTRFTECH) edy555@gmail.com
 * All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * The software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Radio; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */
#include "hal.h"
#include "nanovna.h"
#include "si5351.h"

// XTAL frequency on si5351
#define XTALFREQ 26000000U
// audio codec frequency clock
#define CLK2_FREQUENCY AUDIO_CLOCK_REF

// Fixed PLL mode multiplier (used in band 1 for frequency 800-10k)
#define PLL_N_1  8
// Fixed PLL mode multiplier (used in band 2 for frequency 10k-100M)
#define PLL_N_2 32

// I2C address on bus (only 0x60 for Si5351A in 10-Pin MSOP)
#define SI5351_I2C_ADDR     0x60

static uint8_t  current_band   = 0;
static uint32_t current_freq   = 0;
static int32_t  current_offset = FREQUENCY_OFFSET;
// Use cache for this reg, not update if not change
static uint8_t  clk_cache[3] = {0, 0, 0};

// Generator ready delays, values in x100 us
#if 1
//uint16_t timings[16]={  4,  3, 20,    0, 6000,  0,  4, 25}; // For H  device timings
  uint16_t timings[16]={  2,  2, 20,    0, 6000,  0,  3, 25}; // For H4 device timings
void si5351_set_timing(int i, int v) {timings[i]=v;}
#define DELAY_BAND_1_2           timings[0]   // Delay for bands
#define DELAY_BAND_3_4           timings[1]   // Delay for bands
#define DELAY_BANDCHANGE         timings[2]   // Band changes need set additional delay after reset PLL
#define DELAY_RESET_PLL_BEFORE   timings[3]   // Delay after set new PLL values
#define DELAY_RESET_PLL_AFTER    timings[4]   // Delay after set new PLL values
#define DELAY_GAIN_CHANGE        timings[5]   // Delay for gain change
//#define DELAY_CHANNEL_CHANGE   timings[6]   // defined in main.c switch channel delay
//#define DELAY_SWEEP_START      timings[7]   // defined in main.c delay at sweep start

#else
#define DELAY_BAND_1_2            2    // Delay for bands 1-2
#define DELAY_BAND_3_4            2    // Delay for bands 3-4
#define DELAY_BANDCHANGE         20    // Band changes need set additional delay after reset PLL
// Delay after set new PLL values, and send reset
#define DELAY_RESET_PLL_BEFORE    0    // 1000 possibly not need it if align freq
#define DELAY_RESET_PLL_AFTER  6000    // 3500 possibly not need it if align freq
#define DELAY_GAIN_CHANGE         0    // Delay for gain change
#endif

uint32_t si5351_get_frequency(void)
{
  return current_freq;
}

void si5351_set_frequency_offset(int32_t offset)
{
  current_offset = offset;
  current_freq = 0; // reset freq, for
}

void si5351_bulk_write(const uint8_t *buf, int len)
{
//  i2cAcquireBus(&I2CD1);
  (void)i2cMasterTransmitTimeout(&I2CD1, SI5351_I2C_ADDR, buf, len, NULL, 0, 1000);
//  i2cReleaseBus(&I2CD1);
}

#if 0
static bool si5351_bulk_read(uint8_t reg, uint8_t* buf, int len)
{
  i2cAcquireBus(&I2CD1);
  msg_t mr = i2cMasterTransmitTimeout(&I2CD1, SI5351_I2C_ADDR, &reg, 1, buf, len, 1000);
  i2cReleaseBus(&I2CD1);
  return mr == MSG_OK;
}

static void si5351_wait_pll_lock(void)
{
  uint8_t status;
  int count = 100;
  do{
    status=0xFF;
    si5351_bulk_read(0, &status, 1);
    if ((status & 0x60) == 0) // PLLA and PLLB locked
      return;
  }while (--count);
}
#endif

static inline void
si5351_write(uint8_t reg, uint8_t dat)
{
  uint8_t buf[] = { reg, dat };
  si5351_bulk_write(buf, 2);
}

// register addr, length, data, ...
const uint8_t si5351_configs[] = {
  2, SI5351_REG_3_OUTPUT_ENABLE_CONTROL, 0xff,
  4, SI5351_REG_16_CLK0_CONTROL, SI5351_CLK_POWERDOWN, SI5351_CLK_POWERDOWN, SI5351_CLK_POWERDOWN,
  2, SI5351_REG_183_CRYSTAL_LOAD, SI5351_CRYSTAL_LOAD_8PF|(0<<3)|(0<<0),
// All of this init code run late on sweep
#if 0
  // setup PLL (26MHz * 32 = 832MHz, 32/2-2=14)
  9, SI5351_REG_PLL_A, /*P3*/0, 1, /*P1*/0, 14, 0, /*P3/P2*/0, 0, 0,
  9, SI5351_REG_PLL_B, /*P3*/0, 1, /*P1*/0, 14, 0, /*P3/P2*/0, 0, 0,
  // RESET PLL
  2, SI5351_REG_177_PLL_RESET, SI5351_PLL_RESET_A | SI5351_PLL_RESET_B | 0x0C, //
  // setup multisynth (832MHz / 104 = 8MHz, 104/2-2=50)
  9, SI5351_REG_58_MULTISYNTH2, /*P3*/0, 1, /*P1*/0, 50, 0, /*P2|P3*/0, 0, 0,
  2, SI5351_REG_18_CLK2_CONTROL, SI5351_CLK_DRIVE_STRENGTH_2MA | SI5351_CLK_INPUT_MULTISYNTH_N | SI5351_CLK_INTEGER_MODE,
#endif
  2, SI5351_REG_3_OUTPUT_ENABLE_CONTROL, ~(SI5351_CLK0_EN|SI5351_CLK1_EN|SI5351_CLK2_EN),
  0 // sentinel
};

void
si5351_init(void)
{
  const uint8_t *p = si5351_configs;
  while (*p) {
    uint8_t len = *p++;
    si5351_bulk_write(p, len);
    p += len;
  }
  // Set any (let it be 32MHz) frequency for AIC can run
  si5351_set_frequency(32000000U, 0);
}

static const uint8_t disable_output[] = {
  SI5351_REG_16_CLK0_CONTROL,
  SI5351_CLK_POWERDOWN,  // CLK 0
  SI5351_CLK_POWERDOWN,  // CLK 1
  SI5351_CLK_POWERDOWN   // CLK 2
};

/* Get the appropriate starting point for the PLL registers */
static const uint8_t msreg_base[] = {
  SI5351_REG_42_MULTISYNTH0,
  SI5351_REG_50_MULTISYNTH1,
  SI5351_REG_58_MULTISYNTH2,
};

// Reset PLL need then band changes
static void si5351_reset_pll(uint8_t mask)
{
  // Writing a 1<<5 will reset PLLA, 1<<7 reset PLLB, this is a self clearing bits.
  si5351_write(SI5351_REG_177_PLL_RESET, mask | 0x0C);
}

void si5351_disable_output(void)
{
  si5351_write(SI5351_REG_3_OUTPUT_ENABLE_CONTROL, SI5351_CLK0_EN|SI5351_CLK1_EN|SI5351_CLK2_EN);
  si5351_bulk_write(disable_output, sizeof(disable_output));
  current_band = 0;
}

void si5351_enable_output(void)
{
  si5351_write(SI5351_REG_3_OUTPUT_ENABLE_CONTROL, ~(SI5351_CLK0_EN|SI5351_CLK1_EN|SI5351_CLK2_EN));
//si5351_reset_pll(SI5351_PLL_RESET_A | SI5351_PLL_RESET_B);
  current_freq = 0;
  current_band = 0;
}

// Set PLL freq = XTALFREQ * (mult + num/denom)
static void si5351_setupPLL(uint8_t   pllSource,  /* SI5351_REG_PLL_A or SI5351_REG_PLL_B */
                            uint32_t  mult,
                            uint32_t  num,
                            uint32_t  denom)
{
  /* Feedback Multisynth Divider Equation
   * where: a = mult, b = num and c = denom
   * P1 register is an 18-bit value using following formula:
   *    P1[17:0] = 128 * mult + int((128*num)/denom) - 512
   * P2 register is a 20-bit value using the following formula:
   *    P2[19:0] = (128 * num) % denom
   * P3 register is a 20-bit value using the following formula:
   *    P3[19:0] = denom
   */
  /* Set the main PLL config registers */
  mult <<= 7;
  num <<= 7;
  uint32_t P1 = mult - 512;  // Integer mode
  uint32_t P2 = 0;
  uint32_t P3 = 1;
  if (num) {                 // Fractional mode
    P1+= num / denom;
    P2 = num % denom;
    P3 = denom;
  }
  // Pll MSN(A|B) registers Datasheet
  uint8_t reg[9];
  reg[0] = pllSource;                                       // SI5351_REG_PLL_A or SI5351_REG_PLL_B
  reg[1] = (P3 & 0x0FF00) >> 8;                             // MSN_P3[15: 8]
  reg[2] = (P3 & 0x000FF);                                  // MSN_P3[ 7: 0]
  reg[3] = (P1 & 0x30000) >> 16;                            // MSN_P1[17:16]
  reg[4] = (P1 & 0x0FF00) >> 8;                             // MSN_P1[15: 8]
  reg[5] = (P1 & 0x000FF);                                  // MSN_P1[ 7: 0]
  reg[6] = ((P3 & 0xF0000) >> 12) | ((P2 & 0xF0000) >> 16); // MSN_P3[19:16] | MSN_P2[19:16]
  reg[7] = (P2 & 0x0FF00) >> 8;                             // MSN_P2[15: 8]
  reg[8] = (P2 & 0x000FF);                                  // MSN_P2[ 7: 0]
  si5351_bulk_write(reg, 9);
}

// Set Multisynth divider = (div + num/denom) * rdiv
static void
si5351_setupMultisynth(uint8_t   channel,
                       uint32_t  div,    // 4,6,8, 8+ ~ 900
                       uint32_t  num,
                       uint32_t  denom,
                       uint32_t  rdiv,   // SI5351_R_DIV_1~128
                       uint8_t   chctrl) // SI5351_REG_16_CLKX_CONTROL settings
{
  /* Output Multisynth Divider Equations
   * where: a = div, b = num and c = denom
   * P1 register is an 18-bit value using following formula:
   *   P1[17:0] = 128 * a + int((128*b)/c) - 512
   * P2 register is a 20-bit value using the following formula:
   *   P2[19:0] = (128 * b) % c
   * P3 register is a 20-bit value using the following formula:
   *   P3[19:0] = c
   */
  /* Set the main PLL config registers */
  uint32_t P1 = 0;
  uint32_t P2 = 0;
  uint32_t P3 = 1;
  if (div == 4)
    rdiv|= SI5351_DIVBY4;
  else {
    num<<=7;
    div<<=7;
    P1 = div - 512; // Integer mode
    if (num) {       // Fractional mode
      P1+= num / denom;
      P2 = num % denom;
      P3 = denom;
    }
  }
  /* Set the MSx config registers */
  uint8_t reg[9];
  reg[0] = msreg_base[channel];                       // SI5351_REG_42_MULTISYNTH0, SI5351_REG_50_MULTISYNTH1, SI5351_REG_58_MULTISYNTH2
  reg[1] = (P3 & 0x0FF00)>>8;                         // MSx_P3[15: 8]
  reg[2] = (P3 & 0x000FF);                            // MSx_P3[ 7: 0]
  reg[3] = ((P1 & 0x30000)>>16)| rdiv;                // Rx_DIV[2:0] | MSx_DIVBY4[1:0] | MSx_P1[17:16]
  reg[4] = (P1 & 0x0FF00)>> 8;                        // MSx_P1[15: 8]
  reg[5] = (P1 & 0x000FF);                            // MSx_P1[ 7: 0]
  reg[6] = ((P3 & 0xF0000)>>12)|((P2 & 0xF0000)>>16); // MSx_P3[19:16] | MSx_P2[19:16]
  reg[7] = (P2 & 0x0FF00)>>8;                         // MSx_P2[15: 8]
  reg[8] = (P2 & 0x000FF);                            // MSx_P2[ 7: 0]
  si5351_bulk_write(reg, 9);

  /* Configure the clk control and enable the output */
  uint8_t dat = chctrl | SI5351_CLK_INPUT_MULTISYNTH_N;
  if (num == 0)
    dat |= SI5351_CLK_INTEGER_MODE;
  if (clk_cache[channel]!=dat) {
    si5351_write(SI5351_REG_16_CLK0_CONTROL+channel, dat);
    clk_cache[channel]=dat;
  }
}

// Find better approximate values for n/d
#define MAX_DENOMINATOR ((1 << 20) - 1)
static inline void approximate_fraction(uint32_t *n, uint32_t *d)
{
  // cf. https://github.com/python/cpython/blob/master/Lib/fractions.py#L227
  uint32_t denom = *d;
  if (denom > MAX_DENOMINATOR) {
    uint32_t num = *n;
    uint32_t p0 = 0, q0 = 1, p1 = 1, q1 = 0;
    while (denom != 0) {
      uint32_t a = num / denom;
      uint32_t b = num % denom;
      uint32_t q2 = q0 + a*q1;
      if (q2 > MAX_DENOMINATOR)
        break;
      uint32_t p2 = p0 + a*p1;
      p0 = p1; q0 = q1; p1 = p2; q1 = q2;
      num = denom; denom = b;
    }
    *n = p1;
    *d = q1;
  }
}

// Setup Multisynth divider for get correct output freq if fixed PLL = pllfreq
static void
si5351_set_frequency_fixedpll(uint8_t channel, uint64_t pllfreq, uint32_t freq, uint32_t rdiv, uint8_t chctrl)
{
  uint32_t denom = freq;
  uint32_t div = pllfreq / denom; // range: 8 ~ 1800
  uint32_t num = pllfreq % denom;
  approximate_fraction(&num, &denom);
  si5351_setupMultisynth(channel, div, num, denom, rdiv, chctrl);
}

// Setup PLL freq if Multisynth divider fixed = div (need get output =  freq/mul)
static void
si5351_setupPLL_freq(uint32_t pllSource, uint32_t freq, uint32_t div, uint32_t mul)
{
  uint32_t denom = XTALFREQ * mul;
  uint64_t pllfreq = (uint64_t)freq * div;
  uint32_t multi = pllfreq / denom;
  uint32_t num   = pllfreq % denom;
  approximate_fraction(&num, &denom);
  si5351_setupPLL(pllSource, multi, num, denom);
}

#if 0
static void
si5351_set_frequency_fixeddiv(uint8_t channel, uint32_t pll, uint32_t freq, uint32_t div,
                              uint8_t chctrl, uint32_t mul)
{
  si5351_setupPLL_freq(pll, freq, div, mul);
  si5351_setupMultisynth(channel, div, 0, 1, SI5351_R_DIV_1, chctrl);
}

void
si5351_set_frequency(int channel, uint32_t freq, uint8_t drive_strength)
{
  if (freq <= 100000000) {
    si5351_setupPLL(SI5351_PLL_B, 32, 0, 1);
    si5351_set_frequency_fixedpll(channel, SI5351_PLL_B, PLLFREQ, freq, SI5351_R_DIV_1, drive_strength, 1);
  } else if (freq < 150000000) {
    si5351_set_frequency_fixeddiv(channel, SI5351_PLL_B, freq, 6, drive_strength, 1);
  } else {
    si5351_set_frequency_fixeddiv(channel, SI5351_PLL_B, freq, 4, drive_strength, 1);
  }
}
#endif

typedef struct {
 uint32_t mode;
 uint32_t freq;
 uint8_t pow;
 union {
   uint8_t div;
   uint8_t pll_n;
 };
 uint8_t mul;
 uint8_t omul;
 uint8_t l_gain;
 uint8_t r_gain;
 uint16_t freq_align;
} band_strategy_t;

#define SI5351_FIXED_PLL   1
#define SI5351_FIXED_MULT  2
#define SI5351_MIXED       3

#define CONST_BAND
/*
 * Frequency generation divide on band
 */
#define THRESHOLD 300000100U
static
#ifdef CONST_BAND
const
#endif
band_strategy_t band_s[] = {

  {               0 ,            0, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 0}, 0, 0,  0,  0,       1}, // 0
  {SI5351_FIXED_PLL ,       10000U, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 8}, 1, 1,  0,  0,       1}, // 1
  {SI5351_FIXED_PLL ,   100000000U, SI5351_CLK_DRIVE_STRENGTH_8MA,{32}, 1, 1,  0,  0,       1}, // 2

  {SI5351_FIXED_MULT,   130000000U, SI5351_CLK_DRIVE_STRENGTH_4MA,{ 8}, 1, 1,  0,  0,       1}, // 3
  {SI5351_FIXED_MULT,   180000000U, SI5351_CLK_DRIVE_STRENGTH_4MA,{ 6}, 1, 1,  0,  0,       1}, // 4
  {SI5351_FIXED_MULT,    THRESHOLD, SI5351_CLK_DRIVE_STRENGTH_4MA,{ 4}, 1, 1,  0,  0,       1}, // 5

  {SI5351_FIXED_MULT,   450000000U, SI5351_CLK_DRIVE_STRENGTH_4MA,{ 6}, 3, 5, 50, 50,   3*5*6}, // 6
  {SI5351_FIXED_MULT,   600000000U, SI5351_CLK_DRIVE_STRENGTH_4MA,{ 4}, 3, 5, 50, 50,   3*5*4}, // 7
  {SI5351_FIXED_MULT,  3*THRESHOLD, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4}, 3, 5, 50, 50,   3*5*4}, // 8

  {SI5351_FIXED_MULT,  1200000000U, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4}, 5, 7, 75, 75,   5*7*4}, // 9
  {SI5351_FIXED_MULT,  5*THRESHOLD, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4}, 5, 7, 75, 75,   5*7*4}, //10

  {SI5351_FIXED_MULT,  1800000000U, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4}, 7, 9, 85, 85,   7*9*4}, //11
  {SI5351_FIXED_MULT,  7*THRESHOLD, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4}, 7, 9, 85, 85,   7*9*4}, //12

  {SI5351_FIXED_MULT,  2400000000U, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4}, 9,11, 95, 95,  9*11*4}, //13
  {SI5351_FIXED_MULT,  9*THRESHOLD, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4}, 9,11, 95, 95,  9*11*4}, //14

  {SI5351_FIXED_MULT, 11*THRESHOLD, SI5351_CLK_DRIVE_STRENGTH_8MA,{ 4},11,12, 95, 95, 11*12*4}  //15
};

void si5351_update_band_config(int idx, uint32_t pidx, uint32_t v){
#ifdef CONST_BAND
  (void)idx;
  (void)pidx;
  (void)v;
#else
  band_strategy_t *b = &band_s[idx];
  switch(pidx){
    case 0:b->mode   = v;break;
    case 1:b->freq   = v;break;
    case 2:b->pow    = v;break;
    case 3:b->div    = v;break;
    case 4:b->mul    = v;break;
    case 5:b->omul   = v;break;
    case 6:b->l_gain = v;break;
    case 7:b->r_gain = v;break;
    case 8:b->l_gain = b->r_gain = v;break;
    case 9:b->freq_align = v;break;
  }
#endif
}

uint32_t
si5351_get_harmonic_lvl(uint32_t freq){
  uint16_t i;
  for (i=0;i<sizeof(band_s)/sizeof(*band_s);i++)
    if (freq <= band_s[i].freq)
      return i;
  return i;
}

/*
 * Maximum supported frequency = FREQ_HARMONICS * 9U
 * configure output as follows:
 * CLK0: frequency + offset
 * CLK1: frequency
 * CLK2: fixed 8MHz
 */
int
si5351_set_frequency(uint32_t freq, uint8_t drive_strength)
{
  uint8_t band;
  int delay = 0;

  uint32_t rdiv = SI5351_R_DIV_1;
  uint32_t fdiv, pll_n;
  uint32_t ofreq = freq + current_offset;

  // Select optimal band for prepared freq
  if (freq <  10000U) {
     rdiv = SI5351_R_DIV_128;
     freq<<= 7;
    ofreq<<= 7;
    band = 1;
  } else if (freq <= 500000U) {
    rdiv = SI5351_R_DIV_64;
     freq<<= 6;
    ofreq<<= 6;
    band = 2;
  } else if (freq <= 4000000U) {
    rdiv = SI5351_R_DIV_8;
     freq<<= 3;
    ofreq<<= 3;
    band = 2;
  }
  else
    band = si5351_get_harmonic_lvl(freq);

  uint32_t align = band_s[band].freq_align;
  if (align > 1){
    freq/=align;
    freq*=align;
    ofreq = freq + current_offset;
  }

  if (freq == current_freq)
    return 0;

  if (current_band != band) {
    si5351_write(SI5351_REG_3_OUTPUT_ENABLE_CONTROL, SI5351_CLK0_EN|SI5351_CLK1_EN|SI5351_CLK2_EN);
    si5351_reset_pll(SI5351_PLL_RESET_A | SI5351_PLL_RESET_B);
    // Possibly not need add delay now
    if (DELAY_RESET_PLL_BEFORE)
      chThdSleepMicroseconds(DELAY_RESET_PLL_BEFORE);
  }
  uint32_t mul  = band_s[band].mul;
  uint32_t omul = band_s[band].omul;
  if (drive_strength > SI5351_CLK_DRIVE_STRENGTH_8MA) drive_strength = band_s[band].pow;
  switch (band_s[band].mode) {
                           // 800Hz to 10kHz   PLLN =  8
    case SI5351_FIXED_PLL: // 10kHz to 100MHz  PLLN = 32
      pll_n = band_s[band].pll_n;
      // Setup CH0 and CH1 constant PLLA freq at band change, and set CH2 freq = CLK2_FREQUENCY
      if (current_band != band) {
        si5351_setupPLL(SI5351_REG_PLL_A,   pll_n, 0, 1);
        si5351_setupPLL(SI5351_REG_PLL_B, PLL_N_2, 0, 1);
        si5351_set_frequency_fixedpll(2, XTALFREQ * PLL_N_2, CLK2_FREQUENCY, SI5351_R_DIV_1, SI5351_CLK_DRIVE_STRENGTH_2MA | SI5351_CLK_PLL_SELECT_B);
      }
      delay = DELAY_BAND_1_2;
      // Calculate and set CH0 and CH1 divider
      si5351_set_frequency_fixedpll(0, (uint64_t)omul * XTALFREQ * pll_n, ofreq, rdiv, drive_strength | SI5351_CLK_PLL_SELECT_A);
      si5351_set_frequency_fixedpll(1, (uint64_t) mul * XTALFREQ * pll_n,  freq, rdiv, drive_strength | SI5351_CLK_PLL_SELECT_A);
      break;
#if 0
    case SI5351_MIXED:
      fdiv  = band_s[band].div;
      pll_n = 32;
      // Calculate and set fixed PLL frequency for CH0 freq+offset
      if (band_s[current_band].div != band_s[band].div)
        si5351_setupPLL(SI5351_REG_PLL_A, pll_n, 0, 1);
      // Calculate and set variable PLL frequency for CH1 freq
      si5351_setupPLL_freq(SI5351_REG_PLL_B,  freq, fdiv,  mul);  // set PLLB freq = ( freq/ mul)*fdiv

      // Setup CH1 constant fdiv divider at change
      if (band_s[current_band].div != band_s[band].div)
        si5351_setupMultisynth(1, fdiv, 0, 1, SI5351_R_DIV_1, drive_strength | SI5351_CLK_PLL_SELECT_B);

      // Set CH0 divider
      si5351_set_frequency_fixedpll(0, (uint64_t)omul * XTALFREQ * pll_n, ofreq, rdiv, drive_strength | SI5351_CLK_PLL_SELECT_A);
      // Calculate CH2 freq = CLK2_FREQUENCY, depend from calculated before CH1 PLLB = (freq/mul)*fdiv
      si5351_set_frequency_fixedpll(2, (uint64_t)freq * fdiv, CLK2_FREQUENCY * mul, SI5351_R_DIV_1, SI5351_CLK_DRIVE_STRENGTH_2MA | SI5351_CLK_PLL_SELECT_B);
      delay= DELAY_BAND_3_4;
    break;
#endif
                             // fdiv = 8, f 100-130   PLL 800-1040
                             // fdiv = 6, f 130-170   PLL 780-1050
    case SI5351_FIXED_MULT:  // fdiv = 4, f 170-270   PLL 680-1080
      fdiv = band_s[band].div;
      // Calculate and set CH0 and CH1 PLL freq
      si5351_setupPLL_freq(SI5351_REG_PLL_A, ofreq, fdiv, omul);  // set PLLA freq = (ofreq/omul)*fdiv
      si5351_setupPLL_freq(SI5351_REG_PLL_B,  freq, fdiv,  mul);  // set PLLB freq = ( freq/ mul)*fdiv
      // Setup CH0 and CH1 constant fdiv divider at change
      if (band_s[current_band].div != band_s[band].div) {
        si5351_setupMultisynth(0, fdiv, 0, 1, SI5351_R_DIV_1, drive_strength | SI5351_CLK_PLL_SELECT_A);
        si5351_setupMultisynth(1, fdiv, 0, 1, SI5351_R_DIV_1, drive_strength | SI5351_CLK_PLL_SELECT_B);
      }
      // Calculate CH2 freq = CLK2_FREQUENCY, depend from calculated before CH1 PLLB = (freq/mul)*fdiv
      si5351_set_frequency_fixedpll(2, (uint64_t)freq * fdiv, CLK2_FREQUENCY * mul, SI5351_R_DIV_1, SI5351_CLK_DRIVE_STRENGTH_2MA | SI5351_CLK_PLL_SELECT_B);
      delay= DELAY_BAND_3_4;
      break;
  }
  if (band_s[current_band].l_gain != band_s[band].l_gain || band_s[current_band].r_gain != band_s[band].r_gain){
    tlv320aic3204_set_gain(band_s[band].l_gain, band_s[band].r_gain);
    delay = DELAY_GAIN_CHANGE;
  }

  if (current_band != band) {
    si5351_write(SI5351_REG_3_OUTPUT_ENABLE_CONTROL, ~(SI5351_CLK0_EN|SI5351_CLK1_EN|SI5351_CLK2_EN));
    // Possibly not need add delay now
    if (DELAY_RESET_PLL_AFTER)
      chThdSleepMicroseconds(DELAY_RESET_PLL_AFTER);
    si5351_reset_pll(SI5351_PLL_RESET_A|SI5351_PLL_RESET_B);
    current_band = band;
    delay = DELAY_BANDCHANGE;
  }
  current_freq = freq;
  return delay;
}
