/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* Test-only fault injection interface of the ipc_radio Bluetooth HCI serialization.
 *
 * The Vendor Specific HCI command defined here is only implemented when the
 * CONFIG_IPC_RADIO_BT_FATAL_ERROR_TEST_HOOK Kconfig option is enabled. It is shared with the
 * test application running on the application core.
 */

#ifndef IPC_BT_FATAL_ERROR_TEST_H_
#define IPC_BT_FATAL_ERROR_TEST_H_

#include <zephyr/bluetooth/hci.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Vendor Specific HCI command injecting a fatal error. */
#define BT_HCI_OP_VS_FATAL_ERROR_TEST_INJECT BT_OP(BT_OGF_VS, 0x03ffu)

/** Raise the fatal error from the HCI command handling thread. */
#define BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_THREAD 0x00u
/** Raise the fatal error from an interrupt context. */
#define BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ISR    0x01u
/** Raise the fatal error from a zero-latency interrupt context. */
#define BT_HCI_VS_FATAL_ERROR_TEST_CONTEXT_ZLI    0x02u

/** Raise the fatal error with k_panic(), reported as a stack frame. */
#define BT_HCI_VS_FATAL_ERROR_TEST_FAULT_PANIC  0x00u
/** Raise the fatal error with bt_ctlr_assert_handle(), reported as a controller assert. */
#define BT_HCI_VS_FATAL_ERROR_TEST_FAULT_ASSERT 0x01u

struct bt_hci_vs_fatal_error_test_inject {
	uint8_t context;
	uint8_t fault;
} __packed;

#ifdef __cplusplus
}
#endif

#endif /* IPC_BT_FATAL_ERROR_TEST_H_ */
