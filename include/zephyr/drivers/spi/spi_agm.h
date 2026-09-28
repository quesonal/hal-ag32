/*
 * Copyright (c) 2026 AgRV Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_SPI_SPI_AGM_H_
#define ZEPHYR_INCLUDE_DRIVERS_SPI_SPI_AGM_H_

#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>

/**
 * @file
 * @brief Extensions of the AgRV2K phase-engine SPI driver.
 *
 * The IP can do things the generic SPI API has no way to express. They are
 * exposed here rather than through spi_config, so a caller has to ask for
 * them explicitly.
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Poll a status byte inside one chip-select window
 *
 * The engine's last phase can be a POLL phase (PHASE_ACTION = 3): it keeps
 * clocking @p cmd out and comparing the byte the device answers with until
 * `(value & mask) == expect` or it has tried @p limit times, all without
 * releasing CS. That is the flash "wait for WIP to clear" loop done in
 * hardware -- one transfer instead of one per attempt.
 *
 * @param dev SPI device (`agm,agrv2k-spi`)
 * @param spi_cfg Bus configuration (controller mode, mode 0, 8-bit, single
 *                line -- the same shape spi_transceive() accepts)
 * @param cmd Bytes clocked out before each attempt (1..4)
 * @param cmd_len Number of bytes in @p cmd
 * @param mask Mask applied to the byte read back
 * @param expect Value the masked byte has to equal
 * @param limit Maximum number of attempts (1..255; 0 means 255)
 *
 * @retval 0 The comparison succeeded within @p limit attempts
 * @retval -ETIMEDOUT The engine used up its attempts
 * @retval -ENODEV @p dev is not an AgRV2K SPI controller
 * @retval -EINVAL @p cmd or @p cmd_len is unusable
 * @retval -ENOTSUP The bus configuration is outside what the engine does
 * @retval -EIO The engine reported a different phase error
 *
 * @note @p limit counts attempts, not time, and POLL_LIMIT is 8 bits wide:
 * the longest wait one call can cover is 255 commands (well under a
 * millisecond at 25 MHz). Polling for something that takes longer -- a flash
 * sector erase, say -- means calling this again while it returns -ETIMEDOUT,
 * which is still one CS window per call instead of one per RDSR.
 *
 * @note The engine does not report the byte it last read back: POLL_READ
 * comes back 0x00 on this silicon even when the comparison clearly ran
 * against another value. Read the status normally if
 * you need to know why the poll failed.
 */
int spi_agm_poll_status(const struct device *dev, const struct spi_config *spi_cfg,
			const uint8_t *cmd, size_t cmd_len, uint8_t mask, uint8_t expect,
			uint8_t limit);

/**
 * @brief Clock filler bytes out without a buffer (PHASE_ACTION = DUMMY TX)
 *
 * The engine has a phase type that clocks N bytes of filler without reading
 * memory at all: no TX buffer, no DMA, just clocks inside one chip-select
 * window. That is what a device asking for "dummy cycles" (a flash's fast
 * read, a wake-up sequence) needs, and up to @ref SPI_AGM_MAX_PHASE_BYTES
 * bytes go in one phase.
 *
 * @param dev SPI device (`agm,agrv2k-spi`)
 * @param spi_cfg Bus configuration (controller mode, mode 0, 8-bit, single
 *                line)
 * @param bytes Number of filler bytes to clock (1..SPI_AGM_MAX_PHASE_BYTES)
 *
 * @retval 0 The filler was clocked
 * @retval -ENODEV @p dev is not an AgRV2K SPI controller
 * @retval -EINVAL @p bytes is 0 or above the phase limit
 * @retval -ENOTSUP The bus configuration is outside what the engine does
 * @retval -EIO The engine reported a phase error
 */
int spi_agm_clock_dummy(const struct device *dev, const struct spi_config *spi_cfg, size_t bytes);

/** Line mode of a phase: 1, 2 or 4 data lines (PHASE_CTRL.SPI_MODE). */
#define SPI_AGM_LINES_SINGLE 0U
#define SPI_AGM_LINES_DUAL   1U
#define SPI_AGM_LINES_QUAD   2U

/** Per-phase byte count ceiling: 12-bit field of PHASE_CTRL.
 *  Applies to every phase regardless of action (TX / RX / DUMMY / POLL).
 */
#define SPI_AGM_MAX_PHASE_BYTES    4095U

/** Bytes one *register-fed* TX phase can carry: the phase's data register is
 *  32 bits and, without DMA, that register is the phase's only source. A TX
 *  phase longer than this is rejected by spi_agm_transceive_phases().
 */
#define SPI_AGM_MAX_TX_PHASE_BYTES 4U

/** Phases one phase list may carry (the engine's PHASE_CNT is 3 bits). */
#define SPI_AGM_MAX_PHASES         8U

/** Total TX byte ceiling for one generic-API transceive() call.
 *  The DMA path moves one register-fed header (4 bytes) and one DMA-fed
 *  phase (up to SPI_AGM_MAX_PHASE_BYTES), so the total is the two added.
 */
#define SPI_AGM_MAX_TX_BYTES_TOTAL 4099U

/**
 * @brief Split one chunk of TX bytes into the register-fed phases that carry it.
 *
 * spi_agm_write_long() walks a buffer that is longer than one phase list can
 * carry. Each iteration arms up to SPI_AGM_MAX_PHASES phases of at most
 * SPI_AGM_MAX_TX_PHASE_BYTES bytes -- that is what the engine accepts from the
 * data register, and what spi_agm_transceive_phases() validates (an earlier
 * version sliced into SPI_AGM_MAX_PHASE_BYTES-sized chunks and
 * handed each to one phase, so every write longer than four bytes was rejected
 * with -EINVAL).
 *
 * @param chunk Bytes this iteration has to carry (clamped to the ceiling below).
 * @param lens  Receives the byte count of each phase, in order.
 *
 * @return Number of phases written to @p lens (0 for a zero-length chunk).
 */
static inline size_t spi_agm_tx_phase_split(size_t chunk,
					    uint16_t lens[SPI_AGM_MAX_PHASES])
{
	size_t count = 0U;
	size_t off = 0U;

	chunk = MIN(chunk, (size_t)SPI_AGM_MAX_PHASES * SPI_AGM_MAX_TX_PHASE_BYTES);

	while ((count < SPI_AGM_MAX_PHASES) && (off < chunk)) {
		lens[count] = (uint16_t)MIN((size_t)SPI_AGM_MAX_TX_PHASE_BYTES,
					    chunk - off);
		off += lens[count];
		count++;
	}

	return count;
}

/**
 * @brief One phase of a transfer the SPI API cannot describe
 *
 * The generic API has one TX list and one RX list for the whole frame; the
 * engine's phases each have their own line mode (a flash's 0x6B sends the
 * command and address on one line and reads the data on four) and its own
 * action. A phase list is how a caller says that.
 */
struct spi_agm_phase {
	/** SPI_AGM_LINES_SINGLE / _DUAL / _QUAD */
	uint8_t lines;
	/** Clock @ref len filler bytes: no TX data, no data register. */
	bool dummy;
	/** Bytes clocked in this phase (1..SPI_AGM_MAX_PHASE_BYTES). */
	uint16_t len;
	/** TX bytes; at most four (the phase's data register) and only when not dummy. */
	const uint8_t *tx;
	/** RX buffer; only the last phase may set it, and only when not dummy. */
	uint8_t *rx;
};

/**
 * @brief Run a caller-defined list of phases in one chip-select window
 *
 * Everything the generic API can express is a special case of this; use it
 * for the shapes it cannot, such as a quad flash read (command and address on
 * one line, dummy clocks, then the data phase on four lines).
 *
 * Constraints, all from the engine: the first phase must put something on
 * MOSI (a TX or DUMMY phase, never RX), RX can only be the last phase, there
 * are at most eight phases, and a phase's byte count is 12 bits wide.
 *
 * @param dev SPI device (`agm,agrv2k-spi`)
 * @param spi_cfg Bus configuration (controller mode, mode 0, 8-bit; the
 *                per-phase line modes come from @p phases)
 * @param phases Phase list
 * @param count Number of phases (1..8)
 *
 * @retval 0 All phases ran
 * @retval -ENODEV @p dev is not an AgRV2K SPI controller
 * @retval -EINVAL The phase list is malformed, or an RX phase does not fit
 *                 the driver's bounce buffer
 * @retval -ENOTSUP The bus configuration or a line mode is outside what the
 *                  engine does, or the RX phase needs DMA and the node has no
 *                  `dmas`
 * @retval -EIO The engine reported a phase error
 * @retval -ETIMEDOUT The engine did not finish in time
 */
int spi_agm_transceive_phases(const struct device *dev, const struct spi_config *spi_cfg,
			      const struct spi_agm_phase *phases, size_t count);

/**
 * @brief Run a TX-only transfer longer than one frame can carry
 *
 * The engine caps a phase's byte count at @ref SPI_AGM_MAX_PHASE_BYTES
 * (4095) and a generic-API transceive() at @ref SPI_AGM_MAX_TX_BYTES_TOTAL
 * (4099) bytes, so a 4 KB flash sector write cannot fit in one call. This
 * helper slices the caller's TX buffer into chunks of
 * @ref SPI_AGM_MAX_PHASE_BYTES and calls the extension phase list once per
 * chunk, all inside one chip-select window.
 *
 * The caller must use a GPIO-driven chip select (`spi_cfg->cs.cs_is_gpio`).
 * The engine has no way to hold CS across phase lists, so the helper drives
 * it around the loop.
 *
 * @param dev SPI device (`agm,agrv2k-spi`)
 * @param spi_cfg Bus configuration (controller mode, mode 0, 8-bit, single
 *                line; GPIO CS)
 * @param tx Bytes to clock out
 * @param tx_len Number of bytes in @p tx (may be any positive size)
 *
 * @retval 0 All chunks were clocked out
 * @retval -ENODEV @p dev is not an AgRV2K SPI controller
 * @retval -ENOTSUP The bus configuration is outside what the engine does,
 *                  or @p spi_cfg does not use a GPIO chip select
 * @retval -EINVAL @p tx is NULL or @p tx_len is 0
 * @retval -EIO / -ETIMEDOUT The engine reported a phase error or did not
 *                          finish a chunk in time
 */
int spi_agm_write_long(const struct device *dev, const struct spi_config *spi_cfg,
		       const uint8_t *tx, size_t tx_len);

/**
 * @brief Run an RX-only transfer longer than one frame can carry
 *
 * The RX phase counter is the same 12-bit field as the TX one, capped at
 * @ref SPI_AGM_MAX_PHASE_BYTES (4095), and the controller only refreshes the
 * data register of the last phase. An RX longer than
 * that has to come through here, in chunks, all inside one chip-select
 * window.
 *
 * The caller must use a GPIO-driven chip select. RX is the last phase, so
 * each chunk's phase list is [DUMMY 1 B, RX chunk bytes] -- the dummy phase
 * exists because the engine cannot start with RX.
 *
 * @param dev SPI device (`agm,agrv2k-spi`)
 * @param spi_cfg Bus configuration (controller mode, mode 0, 8-bit, single
 *                line; GPIO CS)
 * @param rx Buffer for the read-back bytes
 * @param rx_len Number of bytes to read (may be any positive size)
 *
 * @retval 0 All chunks were read in
 * @retval -ENODEV @p dev is not an AgRV2K SPI controller
 * @retval -ENOTSUP The bus configuration is outside what the engine does,
 *                  or @p spi_cfg does not use a GPIO chip select
 * @retval -EINVAL @p rx is NULL or @p rx_len is 0
 * @retval -EIO / -ETIMEDOUT The engine reported a phase error or did not
 *                          finish a chunk in time
 */
int spi_agm_read_long(const struct device *dev, const struct spi_config *spi_cfg, uint8_t *rx,
		      size_t rx_len);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_SPI_SPI_AGM_H_ */
