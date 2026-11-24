/*
 * PSN50 - passthrough de audio por blocos, RX e TX simultaneos.
 * Zybo Z7-10 / Vitis 2025.1 / AXI DMA Simple Mode.
 *
 * Substituir o conteudo de src/demo.c por este arquivo DEPOIS de fazer backup.
 * Utiliza os modulos audio/, dma/, iic/, intc/, userio/ ja adaptados.
 *
 * IMPORTANTE: prototipo de streaming por blocos, nao garante ausencia de
 * descontinuidades na borda dos blocos com DMA em Simple Mode.
 */
#include "demo.h"
#include "audio/audio.h"
#include "dma/dma.h"
#include "iic/iic.h"
#include "intc/intc.h"
#include "userio/userio.h"

#include "xaxidma.h"
#include "xgpio.h"
#include "xscugic.h"
#include "xparameters.h"
#include "xil_cache.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xstatus.h"
#include "sleep.h"

#define BLOCK_WORDS 1024U             /* palavras AXI de 32 bits */
#define BLOCK_BYTES (4U * BLOCK_WORDS)
#define I2S_S2MM_EN 0x00000001U       /* stream bit 0: I2S -> DDR */
#define I2S_MM2S_EN 0x00000002U       /* stream bit 1: DDR -> I2S */

/* Alocado automaticamente pelo linker na DDR, 64-byte aligned. */
static u32 audio_buffers[2][BLOCK_WORDS] __attribute__((aligned(64)));

volatile sDemo_t Demo;
static XIic sIic;
static XAxiDma sAxiDma;
static XGpio sUserIO;
static XScuGic sIntc;

/* Mapeamento confirmado no xparameters.h da plataforma PSN50. */
static const ivt_t ivt[] = {
    {XPAR_FABRIC_AXI_DMA_0_S2MM_INTROUT_INTR, (XInterruptHandler)fnS2MMInterruptHandler, &sAxiDma},
    {XPAR_FABRIC_AXI_DMA_0_MM2S_INTROUT_INTR,   (XInterruptHandler)fnMM2SInterruptHandler, &sAxiDma},
    {XPAR_FABRIC_AXI_GPIO_0_IP2INTC_IRPT_INTR,  (XInterruptHandler)fnUserIOIsr, &sUserIO}
};

static UINTPTR buffer_addr(unsigned int index)
{
    return (UINTPTR)&audio_buffers[index][0];
}

static int start_rx(unsigned int index)
{
    UINTPTR addr = buffer_addr(index);

    /* Evita linhas sujas na cache durante uma escrita do DMA. */
    Xil_DCacheFlushRange((INTPTR)addr, BLOCK_BYTES);
    Xil_DCacheInvalidateRange((INTPTR)addr, BLOCK_BYTES);

    return XAxiDma_SimpleTransfer(&sAxiDma, addr, BLOCK_BYTES,
                                  XAXIDMA_DEVICE_TO_DMA);
}

static int start_tx(unsigned int index)
{
    UINTPTR addr = buffer_addr(index);

    /* Necessario caso o ARM tenha alterado amostras na cache. */
    Xil_DCacheFlushRange((INTPTR)addr, BLOCK_BYTES);

    return XAxiDma_SimpleTransfer(&sAxiDma, addr, BLOCK_BYTES,
                                  XAXIDMA_DMA_TO_DEVICE);
}

/* Niveaux de volume en virgule fixe Q8 (256 = 1.0 = 100%, 0 = muet) */
static const u16 volume_levels[] = {
    0U,    /* Muet (0%) */
    32U,   /* Tres faible (~12%) */
    64U,   /* Faible (25%) */
    128U,  /* Moyen (50%) */
    192U,  /* Fort (75%) */
    256U   /* Max (100%) */
};
#define NUM_VOLUMES (sizeof(volume_levels) / sizeof(volume_levels[0]))
static unsigned int vol_idx = 3U; /* Demarre a 50% (index 3) */

#define SAMPLE24_MIN (-8388608)
#define SAMPLE24_MAX (8388607)

/* Seuils d'ecretage (plus le seuil est bas, plus la saturation/distorsion est forte) */
static const int32_t clip_thresholds[] = {
    8388607,   /* 0: Aucune saturation (seuil max 24 bits) */
    4194303,   /* 1: Legere (~50% du max) */
    2097151,   /* 2: Moderee (~25% du max) */
    1048575,   /* 3: Forte (~12.5% du max) */
    524287     /* 4: Tres forte / drive (~6.25% du max) */
};
#define NUM_CLIP_LEVELS (sizeof(clip_thresholds) / sizeof(clip_thresholds[0]))
static unsigned int clip_idx = 0U; /* Demarre sans saturation (index 0) */

static void apply_volume_to_block(u32 *buf, unsigned int num_words, u16 gain_q8)
{
	int32_t limit = clip_thresholds[clip_idx];
    for (unsigned int i = 0U; i < num_words; i++) {
        /* Echantillon signe 24 bits (etendu sur 32 bits) */
        int32_t sample = (int32_t)(buf[i] << 8) >> 8;

        /* Multiplication en virgule fixe Q8 (division par 256) */
        sample = (sample * (int32_t)gain_q8) >> 8;

        /* Ecretage (saturation) sur la plage 24 bits signee */
        if (sample > limit) {  // or SAMPLE24_MAX
            sample = limit;
        } else if (sample < -limit) { // or SAMPLE24_MIN
        	sample = -limit;
        }

        /* Re-masquage sur 24 bits pour le format I2S / DMA */
        buf[i] = ((uint32_t)sample) & 0x00FFFFFFU;
    }
}



int main(void)
{
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
    if (status != XST_SUCCESS) goto fail;

    status = fnInitIic(&sIic);
    if (status != XST_SUCCESS) goto fail;

    status = fnInitUserIO(&sUserIO);
    if (status != XST_SUCCESS) goto fail;

    status = fnConfigDma(&sAxiDma);
    if (status != XST_SUCCESS) goto fail;

    status = fnInitAudio();
    if (status != XST_SUCCESS) goto fail;
    /* Preserva a segunda inicializacao usada na demo Digilent. */
    sleep(2);
    status = fnInitAudio();
    if (status != XST_SUCCESS) goto fail;

    /* LINE IN -> DAC/headphones. Evita chamar fnSetHpOutput(), que
     * reescreve R4_ANALOG_PATH e altera a selecao da entrada.
     */
    fnSetLineInput();

    fnEnableInterrupts(&sIntc, ivt, sizeof(ivt) / sizeof(ivt[0]));

    /* O IP Digilent tem UM contador de amostras compartilhado e dois
     * contadores internos independentes: um por direcao.
     * 1024 palavras x 4 bytes por palavra = 4096 bytes por bloco.
     */
    Xil_Out32(I2S_STREAM_CONTROL_REG, 0U);
    Xil_Out32(I2S_TRANSFER_CONTROL_REG, 0U);
    Xil_Out32(I2S_PERIOD_COUNT_REG, BLOCK_WORDS);
    Xil_Out32(I2S_TRANSFER_CONTROL_REG, 3U); /* habilitar RX e TX I2S */

    /* Primeiro gravamos A uma unica vez para haver audio valido. */
    status = start_rx(0U);
    if (status != XST_SUCCESS) goto fail;
    stream_mask = I2S_S2MM_EN;
    Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);

    while (1) {
        if (Demo.fDmaError) {
            xil_printf("\r\nErro de DMA: fluxo interrompido.\r\n");
            goto fail;
        }

        /* Primeiro bloco: A gravado -> TX A + RX B simultaneamente. */
        if (first_block) {
            if (!Demo.fDmaS2MMEvent) continue;
            Demo.fDmaS2MMEvent = 0;

            stream_mask &= ~I2S_S2MM_EN;
            Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
            Xil_DCacheInvalidateRange((INTPTR)buffer_addr(0U), BLOCK_BYTES);

            status = start_tx(0U);
            if (status != XST_SUCCESS) goto fail;
            status = start_rx(1U);
            if (status != XST_SUCCESS) goto fail;

            tx_index = 0U;
            rx_index = 1U;
            first_block = 0U;
            stream_mask = I2S_S2MM_EN | I2S_MM2S_EN;
            Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
            continue;
        }

        /* Cada evento desabilita SOMENTE seu proprio sentido de stream.
         * Desabilitar permite que o contador interno seja recarregado.
         */
        if (Demo.fDmaS2MMEvent && !rx_done) {
            Demo.fDmaS2MMEvent = 0;
            stream_mask &= ~I2S_S2MM_EN;
            Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
            Xil_DCacheInvalidateRange((INTPTR)buffer_addr(rx_index), BLOCK_BYTES);

            /* Appliquer le niveau de volume actuel au bloc recu */
            apply_volume_to_block(audio_buffers[rx_index], BLOCK_WORDS, volume_levels[vol_idx]);

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

            /* O buffer que terminou a reproducao fica livre para gravar.
             * O buffer que terminou a gravacao fica pronto para reproduzir.
             */
            rx_index = old_tx;
            tx_index = old_rx;

            status = start_rx(rx_index);
            if (status != XST_SUCCESS) goto fail;
            status = start_tx(tx_index);
            if (status != XST_SUCCESS) goto fail;

            rx_done = 0U;
            tx_done = 0U;
            stream_mask = I2S_S2MM_EN | I2S_MM2S_EN;
            Xil_Out32(I2S_STREAM_CONTROL_REG, stream_mask);
        }

        /* 2) Verifier BTN0 pour passer au niveau de volume suivant */
        if (Demo.fUserIOEvent) {
        	Demo.fUserIOEvent = 0;

        	if (Demo.chBtn & 0x01U) { /* BTN0 */
        		vol_idx = (vol_idx + 1U) % NUM_VOLUMES;
                xil_printf("Volume index: %u/%u (gain=%u/256)\r\n",
                vol_idx, (unsigned int)(NUM_VOLUMES - 1U), volume_levels[vol_idx]);
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
    xil_printf("\r\nPassthrough interrompido (status=%d).\r\n", status);
    return XST_FAILURE;
}
