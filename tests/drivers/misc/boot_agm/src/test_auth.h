/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test helpers for the signed-command gate, shared by the two files of the API
 * suite (main.c defines
 * them, an3155.c uses them).
 *
 * The locked profile (CONFIG_BOOT_AGM_LOCK_PRODUCTION) refuses to publish an
 * upload or erase a target until that command has been authorized with a
 * signature over the nonce the device handed out. The suite runs in all three
 * profiles, so the cases that exercise the *storage* paths ask for the grant
 * first and then do the same call they always did; only the three cases at the
 * end of main.c test the gate itself.
 */

#ifndef BOOT_AGM_TEST_AUTH_H_
#define BOOT_AGM_TEST_AUTH_H_

#include <stdint.h>
#include <zephyr/sys/util.h>

#include <zephyr/drivers/misc/boot_agm.h>

#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
/**
 * @brief Authorize one command with the test key (fails the test if it is
 *        refused).
 *
 * The grant it leaves behind is device-wide and one shot: the next publish or
 * erase -- whichever comes first, from the console, AN3155 or SMP -- spends
 * it. That is why a caller that wants a *specific* call authorized has to ask
 * right before it, and why an AN3155 session (whose GO publishes) needs it
 * too.
 */
void boot_agm_test_authorize(uint8_t cmd);

/**
 * @brief Same, but with arguments the signature has to cover.
 *
 * The device rebuilds `nonce || cmd || args_len || args` from what it is
 * handed, so signing these and presenting others has to fail (the host side's
 * packing is pinned in tools/tests/test_smp_cli.py; this is the device half).
 */
void boot_agm_test_authorize_args(uint8_t cmd, const uint8_t *args, uint32_t args_len);
#endif

/**
 * @brief Get the grant a publish needs, in the profile that wants one.
 *
 * A no-op in the two unlocked profiles: there the call is allowed without any
 * authorization, which is what "the default profile is unchanged" means.
 */
static inline void auth_before_publish(void)
{
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_PUBLISH);
#endif
}

/**
 * @brief Get the grant opening an upload session needs, in the profile that
 *        wants one.
 *
 * The same command that guards an erase: opening the session is what makes the
 * first write erase the target . A no-op where there is no
 * gate.
 */
static inline void auth_before_write(void)
{
#if IS_ENABLED(CONFIG_BOOT_AGM_LOCK_PRODUCTION)
	boot_agm_test_authorize(AGM_BOOT_AUTH_CMD_ERASE);
#endif
}

#endif /* BOOT_AGM_TEST_AUTH_H_ */
