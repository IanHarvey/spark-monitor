#include <math.h>
#include "application.h"

#ifndef ADC1_DR_ADDRESS
#define ADC1_DR_ADDRESS   ((uint32_t)0x4001244C)
#endif

static const int SAMPLE_COUNT = 1024;

static const float cal_Scale = 0.0121F;
static const int   cal_PhaseTrim = 20;

// Use TX pin for serial debugging
#define DBG Serial1

// Data processing ----------------------------------------------------

// Table below was:
// NSTEPS=64
// [ int(65535 * math.sin(w * math.pi / (NSTEPS*2)) + 0.5) for w in range(NSTEPS+2) ]

static const uint16_t s_table[66] = {
0x0000, 0x0648, 0x0C90, 0x12D5, 0x1918, 0x1F56, 0x2590, 0x2BC4,
0x31F1, 0x3817, 0x3E34, 0x4447, 0x4A50, 0x504D, 0x563E, 0x5C22,
0x61F7, 0x67BD, 0x6D74, 0x7319, 0x78AD, 0x7E2E, 0x839C, 0x88F5,
0x8E39, 0x9368, 0x987F, 0x9D7F, 0xA267, 0xA736, 0xABEB, 0xB085,
0xB504, 0xB968, 0xBDAE, 0xC1D8, 0xC5E3, 0xC9D0, 0xCD9E, 0xD14C,
0xD4DA, 0xD847, 0xDB93, 0xDEBD, 0xE1C5, 0xE4A9, 0xE76B, 0xEA09,
0xEC82, 0xEED8, 0xF108, 0xF313, 0xF4F9, 0xF6B9, 0xF853, 0xF9C7,
0xFB14, 0xFC3A, 0xFD3A, 0xFE12, 0xFEC3, 0xFF4D, 0xFFB0, 0xFFEB,
0xFFFF, 0xFFEB
};

static int i_sin(int w)
{
  // w is 0..FFFF for value 0..2*pi
  // returns 65535 * sin(w)
  w &= 0xFFFF;
  int sgn = (w & 0x8000);
  w &= 0x7FFF;
  if (w >= 0x4000)
    w = 0x8000-w;
  int wi = (w >> 8);
  int wr = (w & 0xFF);
  int res = (s_table[wi]*(256-wr) + s_table[wi+1]*wr)/256;
  return sgn ? -res : res;
}
  
static int i_cos(int w)
{
  return i_sin(w + 0x4000);
}

struct FComplex
{
  float re;
  float im;
};

static uint16_t vmin,vmax;

bool processData(volatile uint16_t *raw, int count, int zerosample, struct FComplex *res)
{
    // 'zerosample' is sample at which AC voltage is a positive maximum
    // Should be in the middle of the sync pulse. 
    int i;
    int dc;

    if ( zerosample >= count )
      return false;

    zerosample -= cal_PhaseTrim;

    dc=0;
    vmin=vmax=raw[0];
    for (i=0; i<count; i++)
    {
        uint16_t sample = raw[i];
        dc += (int)sample;
        if (vmin > sample) vmin=sample;
        if (vmax < sample) vmax=sample;
    }

    dc = dc/count;

    int s_sum=0, c_sum=0;

    for (i=0; i<count; i++)
    {
       int w = ((i-zerosample)*65536) / count;
       int v = (int)raw[i] - dc;  // Range -2K to +2K
       s_sum += (v * i_sin(w)) >> 6; // Guarantee sum < 64K * 2K * 1K / 64  = 2G
       c_sum += (v * i_cos(w)) >> 6; 
    }

    res->re += (cal_Scale * c_sum) / count;
    res->im += (cal_Scale * s_sum) / count;
    return true;
}

// Hardware control ---------------------------------------------------

void dmaInit(volatile uint16_t *addr, uint32_t len)
{
    DMA_InitTypeDef dmac;

    dmac.DMA_PeripheralBaseAddr = ADC1_DR_ADDRESS;
    dmac.DMA_MemoryBaseAddr = (uint32_t) addr;
    dmac.DMA_DIR = DMA_DIR_PeripheralSRC;
    dmac.DMA_BufferSize = len;
    dmac.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dmac.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dmac.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dmac.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dmac.DMA_Mode = DMA_Mode_Normal;
    dmac.DMA_Priority = DMA_Priority_High;
    dmac.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel1, &dmac);

    DMA_Cmd(DMA1_Channel1, ENABLE);
}

void adcSetup()
{
    ADC_InitTypeDef adc1;

    // Clock config
    RCC_ADCCLKConfig(RCC_PCLK2_Div6);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);

    //ADC1 configuration
    adc1.ADC_Mode = ADC_Mode_Independent;
    adc1.ADC_ScanConvMode = DISABLE;
    adc1.ADC_ContinuousConvMode = ENABLE;
    adc1.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    adc1.ADC_DataAlign = ADC_DataAlign_Right;
    adc1.ADC_NbrOfChannel = 1;
    ADC_Init(ADC1, &adc1);

    // This gives ~50Khz sampling rate, 1 mains cycle should fit in 1024 samples
    ADC_RegularChannelConfig(ADC1, ADC_Channel_0, 1, ADC_SampleTime_239Cycles5);
    ADC_DMACmd(ADC1, ENABLE);

    // Enable ADC & await calibration
    ADC_Cmd(ADC1, ENABLE);

    ADC_ResetCalibration(ADC1);
    while (ADC_GetResetCalibrationStatus(ADC1))
        ;

    ADC_StartCalibration(ADC1);
    while (ADC_GetCalibrationStatus(ADC1))
        ;
}


// Sync pin interrupt service routine ---------------------------------

volatile uint16_t adcBuf[SAMPLE_COUNT];
volatile int nSamples;
volatile int syncWidthSamples;
volatile unsigned tickCount;

volatile int state;
#define IDLE     0
#define START    1
#define RUNNING  2
#define COMPLETE 3

void isr_sync_pin()
{
    static unsigned long lastTime;
    int fallingEdge = (digitalRead(D2)==LOW);

    switch (state)
    {
      case START:
        if (fallingEdge)
        {
          lastTime = micros();
          state = RUNNING;
          dmaInit(adcBuf, SAMPLE_COUNT);
          ADC_SoftwareStartConvCmd(ADC1, ENABLE);
        }
        break;

     case RUNNING:
        if (!fallingEdge)
        {
          // Measure width of sync pulse
          syncWidthSamples = SAMPLE_COUNT - DMA_GetCurrDataCounter(DMA1_Channel1);
        }
        else
        {
          tickCount = micros() - lastTime;
          ADC_SoftwareStartConvCmd(ADC1, DISABLE);
          nSamples = SAMPLE_COUNT - DMA_GetCurrDataCounter(DMA1_Channel1);
          DMA_ClearFlag(DMA1_FLAG_TC1);
          DMA_DeInit(DMA1_Channel1);
          state = COMPLETE;
        }
        break;
     
     default:
        break;
    }

}

// Main setup() and loop() ============================================

static int startTime;
static int cycleCount;
static int totalTickCount;
static FComplex totals;

// Spark variables:
static int upTime;
static int connectTime;
static int wifiRSSI;

static double powerWatts;
static double powerVA;
static double sinPhi;
static double mainsFreq;
static double totalWh;
 
void setup()
{
    state = IDLE;
    upTime = connectTime = 0;
    wifiRSSI = -128;
    powerWatts = powerVA = mainsFreq = totalWh = 0.0;

    pinMode(A0, AN_INPUT);
    adcSetup();

    pinMode(D2, INPUT);
    attachInterrupt(D2, isr_sync_pin, CHANGE);

    DBG.begin(115200);

    DBG.println("Starting...");

    Spark.variable("powerWatts", &powerWatts, DOUBLE);
    Spark.variable("powerVA",    &powerVA, DOUBLE);
    Spark.variable("mainsFreq",  &mainsFreq, DOUBLE);
    Spark.variable("totalWh",    &totalWh, DOUBLE);
    Spark.variable("sinPhi",     &sinPhi, DOUBLE);

    Spark.variable("upTime",     &upTime, INT);
    Spark.variable("connectTime", &connectTime, INT);
    Spark.variable("wifiRSSI",   &wifiRSSI, INT);
}

static void oneSecondUpdate();

void loop()
{
    switch ( state )
    {
        case IDLE:
          cycleCount = 0;
          totals.re = totals.im = 0.0F;
          totalTickCount = 0;
          startTime = Time.now();
          state = START;
          break;

        case START:
        case RUNNING:
          break;

        case COMPLETE:
          if (nSamples < (SAMPLE_COUNT/2) || !processData(adcBuf, nSamples, syncWidthSamples/2, &totals))
          {
              DBG.println("Warning, not enough data");
          }
          else
          {
              totalTickCount += tickCount;
              cycleCount++;
          }

          if (Time.now()==startTime) // Keep accumulating for 1 sec
          {
              state = START;
          }
          else
          {
              double dblCycles = (float)cycleCount;
              mainsFreq = dblCycles * 1.0e6 / ((double)totalTickCount);
              powerWatts = totals.re/dblCycles;
              double rms = sqrt(totals.re*totals.re + totals.im*totals.im);
              powerVA = rms / dblCycles;
              sinPhi = totals.im / rms;
              totalWh += (powerWatts / 3600.0);

              oneSecondUpdate();
              state = IDLE;
          }
          break;

        default:
          DBG.println("Warning, invalid state");
          state = IDLE;
          break;
    }
}

static void oneSecondUpdate()
{
  upTime++;
  if (!Spark.connected())
      connectTime=0;
  else
      connectTime++;
  wifiRSSI = WiFi.RSSI();
 
  DBG.print("t="); DBG.print(startTime); DBG.print(" ");
  DBG.print(powerWatts); DBG.print("W ");
  DBG.print(powerVA); DBG.print("VA phi=");
  DBG.print(sinPhi); DBG.print(" ");
  DBG.print(mainsFreq); DBG.println("Hz");
  DBG.print("Vmin="); DBG.print(vmin*(3.30/4096.0));
  DBG.print(" Vmax="); DBG.println(vmax*(3.30/4096.0));
}
