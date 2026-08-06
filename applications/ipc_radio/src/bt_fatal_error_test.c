/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Test-only fault injection hook.
 *
 * The hook registers a Vendor Specific HCI command that raises a fatal error in the requested
 * execution context. It is used by the tests validating that the Vendor Specific fatal error
 * event reaches the host from a thread, an interrupt and a zero-latency interrupt context.
 *
 * This file is only compiled when the CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK Kconfig option
 * is enabled, which must never be the case in a production image.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/irq_offload.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_raw.h>

#include <zephyr/logging/log.h>

#include <cmsis_core.h>

#include "ipc_bt.h"
#include "ipc_bt_fatal_error_test.h"

LOG_MODULE_DECLARE(ipc_radio, CONFIG_IPC_RADIO_LOG_LEVEL);

static void fault_raise(uint8_t fault);

static uint8_t pending_fault;

#if defined(CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK_ZLI)
static void zli_isr(const void *arg)
{
	ARG_UNUSED(arg);

	fault_raise(pending_fault);
}

static void zli_trigger(void)
{
	IRQ_CONNECT(CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK_ZLI_IRQN, 0, zli_isr, NULL,
		    IRQ_ZERO_LATENCY);
	irq_enable(CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK_ZLI_IRQN);

	NVIC_SetPendingIRQ(CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK_ZLI_IRQN);

	/* The zero-latency interrupt is not masked by irq_lock() and preempts this context
	 * immediately, hence this point is never reached.
	 */
	for (;;) {
	}
}
#endif /* CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK_ZLI */

static void isr_offload(const void *arg)
{
	ARG_UNUSED(arg);

	fault_raise(pending_fault);
}

static void fault_raise(uint8_t fault)
{
	switch (fault) {
	case BT_HCI_VS_FATAL_ERROR_TEST_FAULT_ASSERT:
		bt_ctlr_assert_handle(__FILE__, __LINE__);
		break;

	case BT_HCI_VS_FATAL_ERROR_TEST_FAULT_PANIC:
	default:
		k_panic();
		break;
	}
}

static uint8_t cmd_inject_fatal_error(struct net_buf *buf)
{
	const struct bt_hci_vs_fatal_error_test_inject *cmd = (void *)buf->data;

	LOG_WRN("Injecting a fatal error, context %u fault %u.", cmd->context, cmd->fault);

	pending_fault = cmd->fault;

	switch (cmd->context) {
	case BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ISR:
		irq_offload(isr_offload, NULL);
		break;

#if defined(CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK_ZLI)
	case BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ZLI:
		zli_trigger();
		break;
#endif /* CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK_ZLI */

	case BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_THREAD:
		fault_raise(cmd->fault);
		break;

	default:
		return BT_HCI_ERR_INVALID_PARAM;
	}

	return BT_HCI_ERR_UNSPECIFIED;
}

static struct bt_hci_raw_cmd_ext cmd_ext[] = {
	{
		.op = BT_HCI_OP_VS_FATAL_ERROR_TEST_INJECT,
		.min_len = sizeof(struct bt_hci_vs_fatal_error_test_inject),
		.func = cmd_inject_fatal_error,
	},
};

static int fatal_error_test_hook_init(void)
{
	bt_hci_raw_cmd_ext_register(cmd_ext, ARRAY_SIZE(cmd_ext));

	return 0;
}

SYS_INIT(fatal_error_test_hook_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
