/* Wiretouch: an open capacitive multi-touch tracker
   Copyright (C) 2011-2013 Georg Kaindl and Armin Wagner

   This file is part of WireTouch

   Wiretouch is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Wiretouch is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with WireTouch. If not, see <http://www.gnu.org/licenses/>.
*/

#include <SPI.h>

#define FIRMWARE_VERSION    "1.0b6"

#define SERIAL_BAUD         500000  // patched serial baud rate
#define SER_BUF_SIZE        256     // serial buffer for sending
#define IBUF_LEN            12      // serial buffer for incoming commands

#define DEFAULT_MEASURE_DELAY       14
#define HALFWAVE_POT_VALUE          195
#define OUTPUT_AMP_POT_VALUE        22
#define OUTPUT_AMP_POT_TUNE_DEFAULT 8
#define WAVE_FREQUENCY              18

#define CALIB_PASSES_BASE               128
#define CALIB_PASSES_AT_EACH_CROSSPOINT 16
#define CALIB_THRESHOLD                 500
#define CALIB_THRESHOLD2                1005
#define CALIB_OUTPUT_BLIND_DELTA        4

#define ORDER_MEASURE_UNORDERED   0
#define PRINT_BINARY              1 // set to 0 if you want to debug via serial monitor
#define measure                   measure_with_atmega_adc

#ifndef cbi
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#endif
#ifndef sbi
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))
#endif

const byte signalboardWires = 32;
const byte sensorboardWires = 22;

static uint8_t sbuf[SER_BUF_SIZE + 1];
static uint16_t sbufpos = 0;

static byte halfwavePotBase   = HALFWAVE_POT_VALUE;
static byte outputAmpPotBase  = OUTPUT_AMP_POT_VALUE;
static byte waveFrequency     = WAVE_FREQUENCY;
static uint16_t measureDelay  = DEFAULT_MEASURE_DELAY;

static byte outputAmpPotTune[((signalboardWires * sensorboardWires) >> 1) + 1];

static byte isRunning;

void setup()
{
  Serial.begin(SERIAL_BAUD);

  // set prescale to 16
  cbi(ADCSRA, ADPS2);
  sbi(ADCSRA, ADPS1);
  cbi(ADCSRA, ADPS0);

  pinMode(2, OUTPUT);     // CS for SPI pot 1 (halfwave splitting)        PD2
  pinMode(3, OUTPUT);     // reference signal OUT                         PD3
  pinMode(4, OUTPUT);     // CS for SPI pot 2 (output amplifier)          PD4
  pinMode(6, OUTPUT);     // horizontal lines (sensor) shift reg latch    PD6
  pinMode(9, OUTPUT);     // vertical lines (signal) shift reg latch      PB1
  pinMode(A1, OUTPUT);    // maximum "sample and hold" RESET switch       PC1

  SPI.setBitOrder(MSBFIRST);
  SPI.setClockDivider(SPI_CLOCK_DIV2);
  SPI.setDataMode(SPI_MODE0);
  SPI.begin();

  PORTD |= (1 << 2) | (1 << 4);

  set_wave_frequency(waveFrequency);
  set_halfwave_pot(halfwavePotBase);
  reset_output_amp_tuning();
}

void reset_output_amp_tuning()
{
  for (int i = 0; i < sizeof(outputAmpPotTune); i++)
    outputAmpPotTune[i] = (OUTPUT_AMP_POT_TUNE_DEFAULT) | (OUTPUT_AMP_POT_TUNE_DEFAULT << 4);
}

byte output_amp_tuning_for_point(byte x, byte y)
{
  uint16_t pt = y * signalboardWires + x;
  return (outputAmpPotTune[pt >> 1] >> (4 * (pt & 1))) & 0xf;
}

void set_output_amp_tuning_for_point(byte x, byte y, byte val)
{
  uint16_t pt = y * signalboardWires + x;
  outputAmpPotTune[pt >> 1] =
    (outputAmpPotTune[pt >> 1] & ((pt & 1) ? 0x0f : 0xf0)) |
    ((val & 0x0f) << (4 * (pt & 1)));
}

void set_wave_frequency(byte freq)
{
  TCCR2A = B00100011;
  TCCR2B = B11001;
  OCR2A = freq << 1;
  OCR2B = freq;
}

inline void muxSPI(byte output, byte vertical, byte off)
{
  if (vertical)
    PORTB &= ~(1 << 1);
  else
    PORTD &= ~(1 << 6);

  byte bits = 0;

  if (vertical) {
    if (off)
      bits = 0xff;
    else {
      if (output < 16) {
        bits = (1 << 4);
      }
      else {
        bits = (1 << 5);
      }
      bits |= output % 16;
    }
  } else {
    if (off)
      bits = 0xff;
    else
      bits = ((~(1 << ((output / 8)))) << 3) | (output % 8);
  }

  SPDR = bits;
  while (!(SPSR & _BV(SPIF)));

  if (vertical)
    PORTB |= (1 << 1);
  else
    PORTD |= (1 << 6);
}

inline void set_halfwave_pot(uint16_t value)
{
  PORTD &= ~(1 << 2);
  SPDR = (value >> 8) & 0xff;
  while (!(SPSR & _BV(SPIF)));
  SPDR = (value & 0xff);
  while (!(SPSR & _BV(SPIF)));
  PORTD |= (1 << 2);
}

inline void set_output_amp_pot(uint16_t value)
{
  PORTD &= ~(1 << 4);
  SPDR = (value >> 8) & 0xff;
  while (!(SPSR & _BV(SPIF)));
  SPDR = (value & 0xff);
  while (!(SPSR & _BV(SPIF)));
  PORTD |= (1 << 4);
}

inline unsigned int measure_with_atmega_adc()
{
  return analogRead(0);
}

inline void send_packed10(uint16_t w16, byte flush_all)
{
  static byte p = 0;
  static uint16_t d16 = 0;

  d16 |= (w16 << p);
  p += 10;

  while (p >= 8) {
    sbuf[sbufpos++] = (d16 & 0xff);
    p -= 8;
    d16 >>= 8;
  }

  if (flush_all && p > 0) {
    sbuf[sbufpos++] = (d16 & 0xff);
    p = 0;
    d16 = 0;
  }

  if (flush_all || sbufpos >= SER_BUF_SIZE) {
    Serial.write(sbuf, sbufpos);
    Serial.flush();
    sbufpos = 0;
  }
}

inline void map_coords(uint16_t x, uint16_t y, uint16_t* mx, uint16_t* my)
{
    #if ORDER_MEASURE_UNORDERED
    uint16_t a = x * sensorboardWires + y;
    uint16_t b = (59 * a + 13) % (sensorboardWires * signalboardWires);

    *mx = b / sensorboardWires;
    *my = b - (*mx) * ((uint16_t)sensorboardWires);
    #else
    *mx = x;
    *my = y;
    #endif
}

uint16_t measure_one(uint16_t x, uint16_t y)
{
  uint16_t xx, yy, sample;

  map_coords(x, y, &xx, &yy);

  set_output_amp_pot(outputAmpPotBase + output_amp_tuning_for_point(xx, yy));

  muxSPI(xx, 1, 0);
  muxSPI(yy, 0, 0);

  PORTC &= ~(1 << 1);

  delayMicroseconds(measureDelay);

  sample = measure_with_atmega_adc();

  PORTC |= 1 << 1;

  return sample;
}

uint16_t measure_one_avg(uint16_t x, uint16_t y, uint16_t passes)
{
  uint16_t p = passes;
  uint32_t avg_sample = 0;

  while (p-- > 0)
    avg_sample += measure_one(x, y);

  return (avg_sample / passes);
}

void auto_tune_output_amp()
{
  uint16_t targetValue = 0;
  byte mid_x = signalboardWires >> 1, mid_y = sensorboardWires >> 1;
  uint16_t xx, yy;

  for (uint16_t oabase = 255; oabase > 0; oabase--) {
    outputAmpPotBase = oabase;

    set_output_amp_pot(oabase);

    map_coords(mid_x, mid_y, &xx, &yy);

    // Sample the circuit output at a cross point roughly at the middle of the sensor panel.
    // Find the hightest possible value at which the measurement still exhibits some noise.
    // We will use this value for the base output amplification
    if ((targetValue = measure_one_avg(xx, yy, CALIB_PASSES_BASE)) > CALIB_THRESHOLD)
      break;
  }

  for (uint16_t k = 0; k < signalboardWires; k++) {
    for (uint16_t l = 0; l < sensorboardWires; l++) {
      uint16_t minDiff = 0xffff, tune_val = 0;

      map_coords(k, l, &xx, &yy);

      for (byte amp_tune = 0; amp_tune < 16; amp_tune++) {
        set_output_amp_tuning_for_point(xx, yy, amp_tune);

        uint16_t val = measure_one_avg(k, l, CALIB_PASSES_AT_EACH_CROSSPOINT);
        uint16_t diff = (uint16_t)abs((int)val - (int)targetValue);

        if (diff < minDiff) {
          minDiff = diff;
          tune_val = amp_tune;
        }
      }
      set_output_amp_tuning_for_point(xx, yy, tune_val);
    }
  }
  for (uint16_t oabase = outputAmpPotBase; oabase > 0; oabase--) {
    outputAmpPotBase = oabase;

    set_output_amp_pot(oabase);

    map_coords(mid_x, mid_y, &xx, &yy);

    if ((targetValue = measure_one_avg(xx, yy, CALIB_PASSES_BASE)) > CALIB_THRESHOLD2) {
      outputAmpPotBase -= CALIB_OUTPUT_BLIND_DELTA;
      break;
    }
  }
}

void print_configuration_info()
{
  char buf[128] = {0};
  sprintf(buf, "{ \"halfwave_amp\":\"%d\", \"output_amp\":\"%d\", \"version\":\"%s\"}",
          halfwavePotBase, outputAmpPotBase, FIRMWARE_VERSION);
  Serial.println(buf);
  /* 
    for (uint16_t k = 0; k < signalboardWires; k++) {
      for (uint16_t l = 0; l < sensorboardWires; l++) {
        sprintf(buf, "\"tune_%d_%d\":\"%d\",",
        k, l, output_amp_tuning_for_point(k, l));
        Serial.print(buf);
      }
    }
    Serial.print(buf);
  */
  Serial.println("}");
}

void process_cmd(char* cmd)
{
  switch (*cmd++) {
    case 'e': {
        byte x = 0, y = 0, value = 0;

        while (',' != *cmd)
          x = x * 10 + (*cmd++ - '0');

        cmd++;    // skip the ,

        while (',' != *cmd)
          y = y * 10 + (*cmd++ - '0');

        cmd++;    // skip the ,

        while ('\n' != *cmd)
          value = value * 10 + (*cmd++ - '0');

        set_output_amp_tuning_for_point(x, y, value);

        break;
      }

    case 'o': {
        byte value = 0;

        while ('\n' != *cmd)
          value = value * 10 + (*cmd++ - '0');

        outputAmpPotBase = value;

        break;
      }

    case 'h': {
        byte value = 0;

        while ('\n' != *cmd)
          value = value * 10 + (*cmd++ - '0');

        set_halfwave_pot((halfwavePotBase = value));

        break;
      }

    case 'd': {
        uint16_t value = 0;

        while ('\n' != *cmd)
          value = value * 10 + (*cmd++ - '0');

        measureDelay = value;

        break;
      }

    case 'f': {
        uint16_t value = 0;

        while ('\n' != *cmd)
          value = value * 10 + (*cmd++ - '0');

        set_wave_frequency(waveFrequency = value);

        break;
      }

    case 's': {
        isRunning = 1;
        break;
      }

    case 'x': {
        isRunning = 0;  // stop sending measurement data
        Serial.flush(); // wait for the transmission of the outgoing data to complete
        break;
      }

    case 'c': {
        reset_output_amp_tuning();
        auto_tune_output_amp();
        break;
      }

    case 'r': {
        reset_output_amp_tuning();
        break;
      }

    case 'i': {
        print_configuration_info();
        break;
      }

    default:
      break;
  }
}

void loop()
{
  static byte ibuf_pos = 0;
  static char ibuf[IBUF_LEN];
  uint16_t sample;

  while (Serial.available()) {
    byte c = Serial.read();

    if (ibuf_pos < IBUF_LEN) {
      ibuf[ibuf_pos++] = c;
    } else
      ibuf_pos = 0;

    if ('\n' == c) {
      process_cmd(ibuf);
      ibuf_pos = 0;
    }
  }

  if (!isRunning)
    return;

  int cnt = 0;
  for (uint16_t k = 0; k < signalboardWires; k++) {
    for (uint16_t l = 0; l < sensorboardWires; l++) {
      sample = measure_one(k, l);

      cnt++;
#if PRINT_BINARY
      send_packed10(sample, (cnt >= signalboardWires * sensorboardWires));
      if (cnt >= signalboardWires * sensorboardWires) cnt = 0;
#else
      Serial.print(sample, DEC);
      Serial.print(",");
#endif
    }
  }

#if !PRINT_BINARY
  Serial.print(" ");
#endif
}
