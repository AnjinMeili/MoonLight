/* hub75_pio_c_api.c -- see hub75_pio_c_api.h. Compiled as C (piolib.h is C-only).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Pin/config facts: the OUT range GPIO 2..27 (26 pins), side-set on
 * GPIO 17 (CLK), shift right, no autopull, TX FIFO join -- see hub75.pio
 * and hub75_pio_pins.h. The numbers below (2, 26, 17) are those facts, kept
 * in sync with hub75_pio_pins.h (kOutBaseGpio, kOutPinCount, kClkGpio).
 */

#include "platform/rpi/hub75_pio_c_api.h"

#include <stddef.h>

#include "piolib.h"

/* Keep in sync with hub75_pio_pins.h. */
#define HUB75_OUT_BASE_GPIO 2
#define HUB75_OUT_PIN_COUNT 26
#define HUB75_CLK_GPIO 17

void* hub75_pio_open(void) {
    PIO pio = pio0;
    pio_select(pio);
    pio_enable_fatal_errors(pio, false);
    pio_clear_error(pio);
    return (void*)pio;
}

int hub75_pio_claim_sm(void* handle) {
    PIO pio = (PIO)handle;
    return pio_claim_unused_sm(pio, false);
}

int hub75_pio_add_program(void* handle, const uint16_t* instructions,
                          int length) {
    PIO pio = (PIO)handle;
    pio_program_t program;
    uint offset;
    program.instructions = instructions;
    program.length = (uint8_t)length;
    program.origin = -1;
    program.pio_version = 0;
    pio_clear_error(pio);
    offset = pio_add_program(pio, &program);
    if (offset == PIO_ORIGIN_INVALID || pio_get_error(pio))
        return -1;
    return (int)offset;
}

int hub75_pio_config_xfer(void* handle, int sm, uint32_t buf_size,
                          uint32_t buf_count) {
    PIO pio = (PIO)handle;
    return pio_sm_config_xfer(pio, (uint)sm, PIO_DIR_TO_SM, buf_size,
                              buf_count);
}

int hub75_pio_init_sm(void* handle, int sm, int offset, int wrap_target,
                      int wrap, uint16_t clkdiv_int, uint8_t clkdiv_frac,
                      uint32_t idle_wide_word) {
    PIO pio = (PIO)handle;
    pio_sm_config config;
    uint gpio;
    const uint32_t out_mask = ((1u << HUB75_OUT_PIN_COUNT) - 1u)
                              << HUB75_OUT_BASE_GPIO;
    /* Values, then directions, then the mux: the pins come up holding the
     * idle word. Output values start at 0, which on this board is OE low
     * (lit) on row 0, so switching first lights one row until the first
     * frame. */
    pio_sm_set_pins_with_mask(pio, (uint)sm,
                              idle_wide_word << HUB75_OUT_BASE_GPIO, out_mask);
    pio_sm_set_consecutive_pindirs(pio, (uint)sm, HUB75_OUT_BASE_GPIO,
                                   HUB75_OUT_PIN_COUNT, true);
    for (gpio = HUB75_OUT_BASE_GPIO;
         gpio < HUB75_OUT_BASE_GPIO + HUB75_OUT_PIN_COUNT; gpio++)
        pio_gpio_init(pio, gpio);
    config = pio_get_default_sm_config();
    sm_config_set_wrap(&config, (uint)(offset + wrap_target),
                       (uint)(offset + wrap));
    sm_config_set_sideset(&config, 1, false, false);
    sm_config_set_out_pins(&config, HUB75_OUT_BASE_GPIO, HUB75_OUT_PIN_COUNT);
    sm_config_set_sideset_pins(&config, HUB75_CLK_GPIO);
    /* Shift right (word bit 0 -> OUT base pin), NO autopull: the program
     * pulls one word per pixel clock explicitly. */
    sm_config_set_out_shift(&config, true, false, 0);
    /* TX FIFO join: an 8-word-deep TX FIFO (RP1_PIO_FIFO_DEPTH). */
    sm_config_set_fifo_join(&config, PIO_FIFO_JOIN_TX);
    sm_config_set_clkdiv_int_frac(&config, clkdiv_int, clkdiv_frac);
    pio_clear_error(pio);
    pio_sm_init(pio, (uint)sm, (uint)offset, &config);
    return pio_get_error(pio) ? -1 : 0;
}

void hub75_pio_clear_fifos(void* handle, int sm) {
    pio_sm_clear_fifos((PIO)handle, (uint)sm);
}

void hub75_pio_set_enabled(void* handle, int sm, int enable) {
    pio_sm_set_enabled((PIO)handle, (uint)sm, enable ? true : false);
}

int hub75_pio_xfer(void* handle, int sm, void* data, uint32_t bytes) {
    return pio_sm_xfer_data((PIO)handle, (uint)sm, PIO_DIR_TO_SM, bytes,
                            data);
}

int hub75_pio_tx_fifo_empty(void* handle, int sm) {
    return pio_sm_is_tx_fifo_empty((PIO)handle, (uint)sm) ? 1 : 0;
}

void hub75_pio_remove_program(void* handle, const uint16_t* instructions,
                              int length, int offset) {
    PIO pio = (PIO)handle;
    pio_program_t program;
    program.instructions = instructions;
    program.length = (uint8_t)length;
    program.origin = -1;
    program.pio_version = 0;
    pio_clear_error(pio);
    pio_remove_program(pio, &program, (uint)offset);
}

/* One kernel client per process. piolib's pio_open() starts an IRQ wait
 * thread blocked in PIO_IOC_IRQ_WAIT on the instance's fd, and pio_close()
 * closes the fd without stopping it, so the blocked ioctl keeps the file
 * alive and the kernel's release, the one place it frees the client's DMA
 * claims, waits for process exit. A second client's pio_sm_config_xfer then
 * gets EBUSY. Keeping the instance open makes a re-init the same client,
 * which re-configures its own DMA. The kernel frees everything at exit. */
void hub75_pio_release(void* handle, int sm) {
    PIO pio = (PIO)handle;
    if (sm >= 0)
        pio_sm_unclaim(pio, (uint)sm);
}
