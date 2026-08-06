/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/arch/cpu.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
#include <zephyr/fatal.h>

#include <fatal_error.h>

LOG_MODULE_REGISTER(fatal_error, CONFIG_FATAL_ERROR_LOG_LEVEL);

#if defined(CONFIG_RESET_ON_FATAL_ERROR)
extern void sys_arch_reboot(int type);
#endif /* CONFIG_RESET_ON_FATAL_ERROR */

void fatal_error_handlers_run(unsigned int reason, const struct arch_esf *esf)
{
	STRUCT_SECTION_FOREACH(fatal_error_handler, entry) {
		if (entry->handler != NULL) {
			entry->handler(reason, esf);
		}
	}
}

FUNC_NORETURN void fatal_error_reset(void)
{
#if defined(CONFIG_RESET_ON_FATAL_ERROR)
	LOG_ERR("Resetting system");
	sys_arch_reboot(0);
#else /* !CONFIG_RESET_ON_FATAL_ERROR */
	LOG_ERR("Halting system");
#endif /* !CONFIG_RESET_ON_FATAL_ERROR */

	for (;;) {
		/* Wait for a watchdog or an external reset. */
	}

	CODE_UNREACHABLE;
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	LOG_PANIC();

	(void)irq_lock();

	fatal_error_handlers_run(reason, esf);

	fatal_error_reset();
}
