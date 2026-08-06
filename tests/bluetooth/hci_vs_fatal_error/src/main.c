/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Test of the Vendor Specific fatal error reporting of the ipc_radio application.
 *
 * The test runs on the application core and drives the network core, running the ipc_radio
 * application with the fault injection hook enabled, over the HCI IPC transport. For every
 * combination of an execution context (thread, interrupt, zero-latency interrupt) and a fault
 * type (kernel panic, controller assert), the test:
 *
 *  - injects the fault with a Vendor Specific HCI command,
 *  - waits for the Vendor Specific fatal error HCI event and validates its content,
 *  - waits for the network core to reset and to accept HCI commands again.
 */

#include <string.h>

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/bluetooth/hci_vs.h>

#include <zephyr/logging/log.h>

#include "ipc_bt_fatal_error_test.h"

LOG_MODULE_REGISTER(hci_vs_fatal_error, LOG_LEVEL_INF);

/* Time in milliseconds given to the network core to report the fatal error. */
#define FATAL_ERROR_TIMEOUT_MS 5000
/* Time in milliseconds given to the network core to reset and bind the IPC endpoint again. */
#define CTLR_ALIVE_TIMEOUT_MS  10000
/* Interval in milliseconds between two HCI Reset commands used to probe the network core. */
#define CTLR_PROBE_INTERVAL_MS 250

static K_FIFO_DEFINE(rx_queue);

static void cmd_send(uint16_t opcode, const void *param, uint8_t param_len)
{
	struct bt_hci_cmd_hdr hdr;
	struct net_buf *buf;
	int err;

	hdr.opcode = sys_cpu_to_le16(opcode);
	hdr.param_len = param_len;

	buf = bt_buf_get_tx(BT_BUF_CMD, K_SECONDS(1), &hdr, sizeof(hdr));
	zassert_not_null(buf, "Failed to allocate a command buffer.");

	if (param_len > 0U) {
		net_buf_add_mem(buf, param, param_len);
	}

	err = bt_send(buf);
	zassert_ok(err, "Failed to send the HCI command %#06x: %d.", opcode, err);
}

/* Get the next event of the given type, or NULL when the timeout expires. */
static struct net_buf *evt_get(uint8_t evt_code, int64_t timeout_ms)
{
	const int64_t end = k_uptime_get() + timeout_ms;

	while (true) {
		struct bt_hci_evt_hdr *hdr;
		struct net_buf *buf;
		int64_t remaining;

		remaining = end - k_uptime_get();
		if (remaining <= 0) {
			return NULL;
		}

		buf = k_fifo_get(&rx_queue, K_MSEC(remaining));
		if (buf == NULL) {
			return NULL;
		}

		if (bt_buf_get_type(buf) != BT_BUF_EVT) {
			net_buf_unref(buf);
			continue;
		}

		hdr = (void *)buf->data;
		if (hdr->evt == evt_code) {
			net_buf_pull(buf, sizeof(*hdr));
			return buf;
		}

		net_buf_unref(buf);
	}
}

/* Probe the network core with HCI Reset commands until it answers. */
static bool ctlr_alive_wait(int64_t timeout_ms)
{
	const int64_t end = k_uptime_get() + timeout_ms;

	while (k_uptime_get() < end) {
		struct bt_hci_evt_cmd_complete *cc;
		struct net_buf *buf;

		cmd_send(BT_HCI_OP_RESET, NULL, 0U);

		buf = evt_get(BT_HCI_EVT_CMD_COMPLETE, CTLR_PROBE_INTERVAL_MS);
		if (buf == NULL) {
			continue;
		}

		cc = (void *)buf->data;
		if (sys_le16_to_cpu(cc->opcode) == BT_HCI_OP_RESET) {
			net_buf_unref(buf);
			return true;
		}

		net_buf_unref(buf);
	}

	return false;
}

static void fault_inject(uint8_t context, uint8_t fault)
{
	const struct bt_hci_vs_fatal_error_test_inject cmd = {
		.context = context,
		.fault = fault,
	};

	cmd_send(BT_HCI_OP_VS_FATAL_ERROR_TEST_INJECT, &cmd, sizeof(cmd));
}

static void stack_frame_validate(struct net_buf *buf)
{
	const struct bt_hci_vs_fatal_error_stack_frame *sf;
	const struct bt_hci_vs_fatal_error_cpu_data_cortex_m *cpu_data;

	zassert_true(buf->len >= (sizeof(*sf) + sizeof(*cpu_data)),
		     "Stack frame event is too short: %u.", buf->len);

	sf = (const void *)buf->data;
	zassert_equal(sf->cpu_type, BT_HCI_EVT_VS_ERROR_CPU_TYPE_CORTEX_M,
		      "Unexpected CPU type: %u.", sf->cpu_type);
	zassert_equal(sys_le32_to_cpu(sf->reason), K_ERR_KERNEL_PANIC,
		      "Unexpected fatal error reason: %u.", sys_le32_to_cpu(sf->reason));

	cpu_data = (const void *)sf->cpu_data;
	zassert_not_equal(sys_le32_to_cpu(cpu_data->pc), 0U, "Program counter is not reported.");
	zassert_not_equal(sys_le32_to_cpu(cpu_data->lr), 0U, "Link register is not reported.");
}

static void ctrl_assert_validate(struct net_buf *buf)
{
	const char *file = (const char *)buf->data;
	size_t file_len;
	uint32_t line;

	zassert_true(buf->len > sizeof(line), "Controller assert event is too short: %u.",
		     buf->len);

	file_len = strnlen(file, buf->len);
	zassert_true(file_len > 0U, "File name is not reported.");
	zassert_true(file_len < buf->len, "File name is not null terminated.");

	line = sys_get_le32(&buf->data[file_len + 1U]);
	zassert_not_equal(line, 0U, "Line number is not reported.");

	LOG_INF("Controller assert reported in %s at %u.", file, line);
}

static void fatal_error_test(uint8_t context, uint8_t fault)
{
	uint8_t data_type = (fault == BT_HCI_VS_FATAL_ERROR_TEST_FAULT_ASSERT)
				    ? BT_HCI_EVT_VS_ERROR_DATA_TYPE_CTRL_ASSERT
				    : BT_HCI_EVT_VS_ERROR_DATA_TYPE_STACK_FRAME;
	struct bt_hci_evt_vs *vs;
	struct net_buf *buf;

	fault_inject(context, fault);

	buf = evt_get(BT_HCI_EVT_VENDOR, FATAL_ERROR_TIMEOUT_MS);
	zassert_not_null(buf, "Fatal error event was not reported.");

	vs = (void *)buf->data;
	zassert_equal(vs->subevent, data_type, "Unexpected fatal error data type: %u.",
		      vs->subevent);
	net_buf_pull(buf, sizeof(*vs));

	if (data_type == BT_HCI_EVT_VS_ERROR_DATA_TYPE_STACK_FRAME) {
		stack_frame_validate(buf);
	} else {
		ctrl_assert_validate(buf);
	}

	net_buf_unref(buf);

	/* The network core resets itself after the report, which also validates the
	 * fatal_error_reset() path.
	 */
	zassert_true(ctlr_alive_wait(CTLR_ALIVE_TIMEOUT_MS),
		     "The network core did not reset and bind the IPC endpoint again.");
}

ZTEST(hci_vs_fatal_error, test_thread_panic)
{
	fatal_error_test(BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_THREAD,
			 BT_HCI_VS_FATAL_ERROR_TEST_FAULT_PANIC);
}

ZTEST(hci_vs_fatal_error, test_thread_assert)
{
	fatal_error_test(BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_THREAD,
			 BT_HCI_VS_FATAL_ERROR_TEST_FAULT_ASSERT);
}

ZTEST(hci_vs_fatal_error, test_isr_panic)
{
	fatal_error_test(BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ISR,
			 BT_HCI_VS_FATAL_ERROR_TEST_FAULT_PANIC);
}

ZTEST(hci_vs_fatal_error, test_isr_assert)
{
	fatal_error_test(BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ISR,
			 BT_HCI_VS_FATAL_ERROR_TEST_FAULT_ASSERT);
}

ZTEST(hci_vs_fatal_error, test_zli_panic)
{
	fatal_error_test(BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ZLI,
			 BT_HCI_VS_FATAL_ERROR_TEST_FAULT_PANIC);
}

ZTEST(hci_vs_fatal_error, test_zli_assert)
{
	fatal_error_test(BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ZLI,
			 BT_HCI_VS_FATAL_ERROR_TEST_FAULT_ASSERT);
}

static void *suite_setup(void)
{
	int err;

	err = bt_enable_raw(&rx_queue);
	zassert_ok(err, "Failed to enable the raw HCI transport: %d.", err);

	return NULL;
}

static void test_before(void *fixture)
{
	ARG_UNUSED(fixture);

	/* Drop the events left over by the previous test case. */
	while (true) {
		struct net_buf *buf = k_fifo_get(&rx_queue, K_NO_WAIT);

		if (buf == NULL) {
			break;
		}

		net_buf_unref(buf);
	}

	zassert_true(ctlr_alive_wait(CTLR_ALIVE_TIMEOUT_MS),
		     "The network core does not accept HCI commands.");
}

ZTEST_SUITE(hci_vs_fatal_error, NULL, suite_setup, test_before, NULL, NULL);
