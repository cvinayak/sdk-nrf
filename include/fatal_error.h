/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file
 * @brief Fatal error library.
 */

#ifndef FATAL_ERROR_H_
#define FATAL_ERROR_H_

#include <zephyr/kernel.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/toolchain.h>

/**
 * @defgroup fatal_error Fatal error
 * @brief Library handling fatal errors on Nordic Semiconductor devices.
 *
 * @{
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Reset the system.
 *
 * The system is reset when the @kconfig{CONFIG_RESET_ON_FATAL_ERROR} Kconfig option is enabled.
 * Otherwise, the function halts the current CPU in an endless loop.
 *
 * This function never returns and is safe to call from any context, including from a
 * zero-latency interrupt.
 */
FUNC_NORETURN void fatal_error_reset(void);

/**
 * @brief Fatal error pre-reset handler.
 *
 * Use the @ref FATAL_ERROR_HANDLER_DEFINE macro to register a handler.
 */
struct fatal_error_handler {
	/**
	 * @brief Handler called from @c k_sys_fatal_error_handler before the system is reset.
	 *
	 * The handler is called with interrupts locked, from the context in which the fatal
	 * error was raised. This can be a thread, an interrupt or a zero-latency interrupt
	 * context. The implementation must therefore not block, allocate with a timeout or
	 * yield.
	 *
	 * @param reason Fatal error reason, see @c k_fatal_error_reason.
	 * @param esf    Exception stack frame, may be NULL.
	 */
	void (*handler)(unsigned int reason, const struct arch_esf *esf);
};

/**
 * @brief Register a fatal error pre-reset handler.
 *
 * The handler is called by the fatal error library before the system is reset.
 * Requires the @kconfig{CONFIG_FATAL_ERROR_HANDLERS} Kconfig option to be enabled.
 *
 * @param _name    Name of the handler instance.
 * @param _handler Handler function, see @ref fatal_error_handler.
 */
#define FATAL_ERROR_HANDLER_DEFINE(_name, _handler)                                                \
	static const STRUCT_SECTION_ITERABLE(fatal_error_handler, _name) = {                       \
		.handler = (_handler),                                                             \
	}

/**
 * @brief Run all registered fatal error pre-reset handlers.
 *
 * This function is called by the fatal error library from @c k_sys_fatal_error_handler.
 * Applications that install their own @c k_sys_fatal_error_handler can call it directly.
 *
 * @param reason Fatal error reason, see @c k_fatal_error_reason.
 * @param esf    Exception stack frame, may be NULL.
 */
void fatal_error_handlers_run(unsigned int reason, const struct arch_esf *esf);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* FATAL_ERROR_H_ */
