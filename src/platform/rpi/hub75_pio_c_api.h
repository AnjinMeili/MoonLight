/* hub75_pio_c_api.h -- minimal C wrapper over the piolib calls this engine uses.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * piolib.h is C-only: it does not compile as C++ (its
 * irq_get_exclusive_handler() converts void* to a function pointer, which
 * C++ rejects). Rather than compiling the backend with -fpermissive, the
 * piolib calls live behind this small C interface (hub75_pio_c_api.c,
 * compiled as C), and the C++ backend (hub75_pio_backend_piollib.cpp)
 * talks to it. Only the dozen calls the engine needs are wrapped; nothing
 * else from piolib leaks into the C++ code.
 *
 * Handles: the PIO instance is opaque (void*). The state machine is an
 * int. All functions return 0 on success and -1 on failure unless noted.
 */

#ifndef MM_HUB75_PIO_C_API_H
#define MM_HUB75_PIO_C_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Open pio0, select it as current, and make piolib errors non-fatal
 * (reported via return values instead of aborting). Returns the opaque
 * PIO handle, or NULL on failure. Callers must check /dev/pio0 exists
 * first: pio_open_helper() exits the process if the device is missing. */
void* hub75_pio_open(void);

/* Claim any free state machine. Returns the SM number, or -1. */
int hub75_pio_claim_sm(void* pio);

/* Load a program (instructions, length in words, .pio_version 0).
 * Returns the load offset, or -1 if no instruction memory is free. */
int hub75_pio_add_program(void* pio, const uint16_t* instructions, int length);

/* Configure the DMA transfer ring for one SM (buf_size multiple of 4,
 * buf_count <= 4). */
int hub75_pio_config_xfer(void* pio, int sm, uint32_t buf_size,
                          uint32_t buf_count);

/* Initialise the SM for the hub75_wide program: GPIO 2..27 as PIO
 * outputs, OUT base 2 count 26, side-set base 17 (CLK), shift right with
 * no autopull, TX FIFO join, clock divider int+frac/256, wrap bounds,
 * initial PC at the program offset. Does NOT enable the SM. */
int hub75_pio_init_sm(void* pio, int sm, int offset, int wrap_target,
                      int wrap, uint16_t clkdiv_int, uint8_t clkdiv_frac);

/* Clear the SM's FIFOs. */
void hub75_pio_clear_fifos(void* pio, int sm);

/* Enable (1) or disable (0) the SM. */
void hub75_pio_set_enabled(void* pio, int sm, int enable);

/* Blocking DMA transfer of one frame into the SM's TX FIFO. Returns 0 on
 * success, non-zero (negative errno-style) on failure. */
int hub75_pio_xfer(void* pio, int sm, void* data, uint32_t bytes);

/* True (1) when the TX FIFO is empty. */
int hub75_pio_tx_fifo_empty(void* pio, int sm);

/* Remove a previously loaded program. */
void hub75_pio_remove_program(void* pio, const uint16_t* instructions,
                              int length, int offset);

/* Release the SM and close the PIO instance. */
void hub75_pio_close(void* pio, int sm);

#ifdef __cplusplus
}
#endif

#endif /* MM_HUB75_PIO_C_API_H */
