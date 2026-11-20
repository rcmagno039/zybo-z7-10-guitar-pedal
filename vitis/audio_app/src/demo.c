/*
 * PSN50 - Guitar pedal using the following setup:
 *          Zybo Z7-10 / Vitis 2019.2 / AXI DMA Simple Mode.
 */

#include "demo.h"
#include "audio/audio.h"
#include "dma/dma.h"
#include "iic/iic.h"
#include "intc/intc.h"
#include "userio/userio.h"

#include "sleep.h"
#include "xaxidma.h"
#include "xgpio.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xparameters.h"
#include "xscugic.h"
#include "xstatus.h"

#define BLOCK_WORDS 1024U /* 32 bits AXI words*/
#define BLOCK_BYTES (4U * BLOCK_WORDS)
#define I2S_S2MM_EN 0x00000001U /* stream bit 0: I2S -> DDR */
#define I2S_MM2S_EN 0x00000002U /* stream bit 1: DDR -> I2S */

/* Automatically allocated by the linker in the DDR, 64-byte aligned. */
static u32 audio_buffers[2][BLOCK_WORDS] __attribute__((aligned(64)));

volatile sDemo_t Demo;
static XIic sIic;
static XAxiDma sAxiDma;
static XGpio sUserIO;
static XScuGic sIntc;

/* Imports mapping from xparameters.h. */
static const ivt_t ivt[] = {
    {XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR,
     (XInterruptHandler)fnS2MMInterruptHandler, &sAxiDma},
    {XPAR_FABRIC_AXI_DMA_0_MM2S_INTROUT_INTR,
     (XInterruptHandler)fnMM2SInterruptHandler, &sAxiDma},
    {XPAR_FABRIC_AXI_GPIO_0_IP2INTC_IRPT_INTR, (XInterruptHandler)fnUserIOIsr,
     &sUserIO}};

static UINTPTR buffer_addr(unsigned int index) {
  return (UINTPTR)&audio_buffers[index][0];
}

static int start_rx(unsigned int index) {
  UINTPTR addr = buffer_addr(index);

  Xil_DCacheFlushRange((INTPTR)addr, BLOCK_BYTES);
  Xil_DCacheInvalidateRange((INTPTR)addr, BLOCK_BYTES);

  return XAxiDma_SimpleTransfer(&sAxiDma, addr, BLOCK_BYTES,
                                XAXIDMA_DEVICE_TO_DMA);
}

static int start_tx(unsigned int index) {
  UINTPTR addr = buffer_addr(index);

  Xil_DCacheFlushRange((INTPTR)addr, BLOCK_BYTES);

  return XAxiDma_SimpleTransfer(&sAxiDma, addr, BLOCK_BYTES,
                                XAXIDMA_DMA_TO_DEVICE);
}

/* Volume levels in fixed point Q8 (256 = 1.0 = 100%, 0 = No sound) */
static const u16 volume_levels[] = {
    0U,   /* No sound(0%) */
    32U,  /* Very low (~12%) */
    64U,  /* Low (25%) */
    128U, /* Nedium (50%) */
    192U, /* Strong (75%) */
    256U  /* Max (100%) */
};
#define NUM_VOLUMES (sizeof(volume_levels) / sizeof(volume_levels[0]))
static unsigned int vol_idx = 3U; /* Defaults at 50% (index 3) */

#define SAMPLE24_MIN (-8388608)
#define SAMPLE24_MAX (8388607)

/* Cut-off threshold (the lower the threshold, the stronger the
 * saturation/distorsion) */
static const int32_t clip_thresholds[] = {
    8388607, /* 0: No saturation */
    4194303, /* 1: Light (~50% du max) */
    2097151, /* 2: Moderated (~25% du max) */
    1048575, /* 3: Strong (~12.5% du max) */
    524287   /* 4: Really strong / Drive (~6.25% du max) */
};
#define NUM_CLIP_LEVELS (sizeof(clip_thresholds) / sizeof(clip_thresholds[0]))
static unsigned int clip_idx = 0U; /* Defaults to no saturation (index 0) */

static void apply_volume_to_block(u32 *buf, unsigned int num_words,
                                  u16 gain_q8) {
  int32_t limit = clip_thresholds[clip_idx];
  for (unsigned int i = 0U; i < num_words; i++) {
    /* 24-bit signed sample (extended to 32 bits) */
    int32_t sample = (int32_t)(buf[i] << 8) >> 8;

    /* Q8 fixed-point multiplication (division by 256) */
    sample = (sample * (int32_t)gain_q8) >> 8;

    /* Saturation to the signed 24-bit range */
    if (sample > limit) {
      sample = limit;
    } else if (sample < -limit) {
      sample = -limit;
    }

    /* Re-mask to 24 bits for the I2S / DMA format */
    buf[i] = ((uint32_t)sample) & 0x00FFFFFFU;
  }
}

int main(void) {
  int status;
  u32 stream_mask = 0U;
  unsigned int rx_index = 0U;
  unsigned int tx_index = 0U;
  unsigned int rx_done = 0U;
  unsigned int tx_done = 0U;
  unsigned int first_block = 1U;

  xil_printf("\r\nPSN50: passthrough RX/TX initiated...\r\n");
  Demo.u8Verbose = 0;

  status = fnInitInterruptController(&sIntc);
  if (status != XST_SUCCESS)
    goto fail;

  status = fnInitIic(&sIic);
  if (status != XST_SUCCESS)
    goto fail;

  status = fnInitUserIO(&sUserIO);
  if (status != XST_SUCCESS)
    goto fail;

  status = fnConfigDma(&sAxiDma);
  if (status != XST_SUCCESS)
    goto fail;

  status = fnInitAudio();
  if (status != XST_SUCCESS)
    goto fail;
  sleep(2);
  status = fnInitAudio();
  if (status != XST_SUCCESS)
    goto fail;

  fnSetLineInput();

  fnEnableInterrupts(&sIntc, ivt, sizeof(ivt) / sizeof(ivt[0]));

  Xil_Out32(I2S_STREAM_CONTROL_REG, 0U);
  Xil_Out32(I2S_TRANSFER_CONTROL_REG, 0U);
  Xil_Out32(I2S_PERIOD_COUNT_REG, BLOCK_WORDS);
  Xil_Out32(I2S_TRANSFER_CONTROL_REG, 3U); /* enable I2S RX and TX */

  /* Record A once first so there is valid audio */
  status = start_rx(0U);
  if (status != XST_SUCCESS)
    goto fail;
  stream_mask = I2S_S2MM_EN;
  Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);

  while (1) {
    if (Demo.fDmaError) {
      xil_printf("\r\nErro de DMA: fluxo interrompido.\r\n");
      goto fail;
    }

    /* A recorded -> TX A + RX B simultaneously. */
    if (first_block) {
      if (!Demo.fDmaS2MMEvent)
        continue;
      Demo.fDmaS2MMEvent = 0;

      stream_mask &= ~I2S_S2MM_EN;
      Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
      Xil_DCacheInvalidateRange((INTPTR)buffer_addr(0U), BLOCK_BYTES);

      status = start_tx(0U);
      if (status != XST_SUCCESS)
        goto fail;
      status = start_rx(1U);
      if (status != XST_SUCCESS)
        goto fail;

      tx_index = 0U;
      rx_index = 1U;
      first_block = 0U;
      stream_mask = I2S_S2MM_EN | I2S_MM2S_EN;
      Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
      continue;
    }

    if (Demo.fDmaS2MMEvent && !rx_done) {
      Demo.fDmaS2MMEvent = 0;
      stream_mask &= ~I2S_S2MM_EN;
      Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
      Xil_DCacheInvalidateRange((INTPTR)buffer_addr(rx_index), BLOCK_BYTES);

      /* Apply the current volume level to the received block */
      apply_volume_to_block(audio_buffers[rx_index], BLOCK_WORDS,
                            volume_levels[vol_idx]);

      rx_done = 1U;
    }

    if (Demo.fDmaMM2SEvent && !tx_done) {
      Demo.fDmaMM2SEvent = 0;
      stream_mask &= ~I2S_MM2S_EN;
      Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
      tx_done = 1U;
    }

    if (rx_done && tx_done) {
      unsigned int old_rx = rx_index;
      unsigned int old_tx = tx_index;

      /* The buffer that finished playback becomes free for recording.
         The buffer that finished recording becomes ready for playback.
       */
      rx_index = old_tx;
      tx_index = old_rx;

      status = start_rx(rx_index);
      if (status != XST_SUCCESS)
        goto fail;
      status = start_tx(tx_index);
      if (status != XST_SUCCESS)
        goto fail;

      rx_done = 0U;
      tx_done = 0U;
      stream_mask = I2S_S2MM_EN | I2S_MM2S_EN;
      Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
    }

    /* Check BTN0 to switch to the next volume level */
    if (Demo.fUserIOEvent) {
      Demo.fUserIOEvent = 0;

      if (Demo.chBtn & 0x01U) { /* BTN0 */
        vol_idx = (vol_idx + 1U) % NUM_VOLUMES;
        xil_printf("Volume index: %u/%u (gain=%u/256)\r\n", vol_idx,
                   (unsigned int)(NUM_VOLUMES - 1U), volume_levels[vol_idx]);
      }

      /*if (Demo.chBtn & 0x02U) { // BTN1
          clip_idx = (clip_idx + 1U) % NUM_CLIP_LEVELS;
          xil_printf("Clip index: %u/%u (threshold=%ld)\r\n",
          clip_idx, (unsigned int)(NUM_CLIP_LEVELS - 1U),
                   (long)clip_thresholds[clip_idx]);
      }*/
    }
  }

fail:
  Xil_Out32(I2S_STREAM_CONTROL_REG, 0U);
  Xil_Out32(I2S_TRANSFER_CONTROL_REG, 0U);
  xil_printf("\r\nPassthrough interrupteda(status=%d).\r\n", status);
  return XST_FAILURE;
}
