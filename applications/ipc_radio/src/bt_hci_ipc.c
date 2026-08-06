/*
 * Copyright (c) 2023 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/net_buf.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/buf.h>
#include <zephyr/bluetooth/hci_raw.h>
#include <zephyr/bluetooth/hci_vs.h>

#include <zephyr/ipc/ipc_service.h>

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/atomic.h>
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

#include <fatal_error.h>

#include <zephyr/logging/log.h>

#include "ipc_bt.h"

#if DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_bt_hci_ipc), zephyr_ipc_openamp_static_vrings)
#include <openamp/rpmsg_virtio.h>
#define IPC_BUF_SIZE DT_PROP_OR(DT_CHOSEN(zephyr_bt_hci_ipc), zephyr_buffer_size, RPMSG_BUFFER_SIZE)
#define IPC_MEM_SIZE (DT_REG_SIZE(DT_PHANDLE(DT_CHOSEN(zephyr_bt_hci_ipc), memory_region)) / 2)
#define MAX_IPC_BLOCKS DIV_ROUND_UP(IPC_MEM_SIZE, IPC_BUF_SIZE)
#elif DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_bt_hci_ipc), zephyr_ipc_icbmsg)
#define MAX_IPC_BLOCKS DT_PROP(DT_CHOSEN(zephyr_bt_hci_ipc), rx_blocks)
#else
#error "IPC backends other than rpmsg or icbmsg are not supported."
#endif

LOG_MODULE_DECLARE(ipc_radio, CONFIG_IPC_RADIO_LOG_LEVEL);

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
static bool ipc_ept_ready;
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

static K_SEM_DEFINE(ipc_bound_sem, 0, 1);

static struct ipc_ept hci_ept;

struct ipc_block_item {
	const void *ptr;
	size_t len;
};

K_MSGQ_DEFINE(ipc_block_queue, sizeof(struct ipc_block_item), MAX_IPC_BLOCKS, sizeof(void *));

static K_FIFO_DEFINE(tx_queue);
static K_FIFO_DEFINE(rx_queue);

enum hci_h4_type {
	HCI_H4_CMD = 0x01, /* rx */
	HCI_H4_ACL = 0x02, /* rx */
	HCI_H4_EVT = 0x04, /* tx */
	HCI_H4_ISO = 0x05  /* rx */
};

static struct net_buf *recv_cmd(const uint8_t *data, size_t len)
{
	const struct bt_hci_cmd_hdr *hdr = (const struct bt_hci_cmd_hdr *)data;
	struct net_buf *buf;

	if (len < sizeof(*hdr)) {
		LOG_ERR("Not enough data for command header.");
		return NULL;
	}

	if ((len - sizeof(*hdr)) != hdr->param_len) {
		LOG_ERR("Command param_len does not match the remaining data length.");
		return NULL;
	}

	buf = bt_buf_get_tx(BT_BUF_CMD, K_FOREVER, hdr, sizeof(*hdr));
	data += sizeof(*hdr);
	len -= sizeof(*hdr);

	if (len > net_buf_tailroom(buf)) {
		LOG_ERR("Not enough space in buffer.");
		net_buf_unref(buf);
		return NULL;
	}

	LOG_DBG("Received HCI CMD packet (opcode: %#x, len: %u).",
						sys_le16_to_cpu(hdr->opcode), hdr->param_len);

	net_buf_add_mem(buf, data, len);
	return buf;
}

static struct net_buf *recv_acl(const uint8_t *data, size_t len)
{
	const struct bt_hci_acl_hdr *hdr = (const struct bt_hci_acl_hdr *)data;
	struct net_buf *buf;

	if (len < sizeof(*hdr)) {
		LOG_ERR("Not enough data for ACL header.");
		return NULL;
	}

	if ((len - sizeof(*hdr)) != sys_le16_to_cpu(hdr->len)) {
		LOG_ERR("ACL payload length does not match the remaining data length.");
		return NULL;
	}

	buf = bt_buf_get_tx(BT_BUF_ACL_OUT, K_FOREVER, hdr, sizeof(*hdr));
	data += sizeof(*hdr);
	len -= sizeof(*hdr);

	if (len > net_buf_tailroom(buf)) {
		LOG_ERR("Not enough space in buffer.");
		net_buf_unref(buf);
		return NULL;
	}

	LOG_DBG("Received HCI ACL packet (handle: %u, len: %u).",
					sys_le16_to_cpu(hdr->handle), sys_le16_to_cpu(hdr->len));

	net_buf_add_mem(buf, data, len);
	return buf;
}

static struct net_buf *recv_iso(const uint8_t *data, size_t len)
{
	const struct bt_hci_iso_hdr *hdr = (const struct bt_hci_iso_hdr *)data;
	struct net_buf *buf;

	if (len < sizeof(*hdr)) {
		LOG_ERR("Not enough data for ISO header.");
		return NULL;
	}

	if ((len - sizeof(*hdr)) != bt_iso_hdr_len(sys_le16_to_cpu(hdr->len))) {
		LOG_ERR("ISO payload length does not match the remaining data length.");
		return NULL;
	}

	buf = bt_buf_get_tx(BT_BUF_ISO_OUT, K_FOREVER, hdr, sizeof(*hdr));
	data += sizeof(*hdr);
	len -= sizeof(*hdr);

	if (len > net_buf_tailroom(buf)) {
		LOG_ERR("Not enough space in buffer.");
		net_buf_unref(buf);
		return NULL;
	}

	LOG_DBG("Received HCI ISO packet (handle: %u, len: %u).",
					sys_le16_to_cpu(hdr->handle), sys_le16_to_cpu(hdr->len));

	net_buf_add_mem(buf, data, len);
	return buf;
}

static void send(struct net_buf *buf)
{
	uint8_t retries = 0;
	int ret;

	LOG_DBG("buf %p type %u len %u", buf, buf->data[0], buf->len);
	LOG_HEXDUMP_DBG(buf->data, buf->len, "Controller buffer:");

	do {
		ret = ipc_service_send(&hci_ept, buf->data, buf->len);
		if (ret < 0) {
			retries++;
			if (retries > 10) {
				LOG_WRN("IPC send has been blocked during 10 retires.");
				retries = 0;
			}

			k_yield();
		}
	} while (ret < 0);

	LOG_INF("Sent message of %d bytes.", ret);

	net_buf_unref(buf);
}

static void bound(void *priv)
{
#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
	ipc_ept_ready = true;
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

	k_sem_give(&ipc_bound_sem);
}

static void recv(const void *data, size_t len, void *priv)
{
	struct ipc_block_item block;
	int err;

	LOG_INF("Received hci message of %u bytes.", len);
	LOG_HEXDUMP_DBG(data, len, "HCI data:");

	block.ptr = data;
	block.len = len;

	err = ipc_service_hold_rx_buffer(&hci_ept, (void *)data);
	if (err) {
		LOG_ERR("Failed to hold rx buffer: %d.", err);
	}

	err = k_msgq_put(&ipc_block_queue, &block, K_NO_WAIT);
	__ASSERT(err == 0, "Failed to put data into msgq: %d.", err);
}

static void queue_thread(void)
{
	struct ipc_block_item block;
	struct net_buf *buf;
	const uint8_t *tmp;
	enum hci_h4_type type;
	int err;

	while (1) {
		err = k_msgq_get(&ipc_block_queue, &block, K_FOREVER);
		__ASSERT(err == 0, "Failed to get data from msgq: %d.", err);

		tmp = (const uint8_t *)block.ptr;
		type = (enum hci_h4_type)*tmp++;
		block.len -= sizeof(type);

		switch (type) {
		case HCI_H4_CMD:
			buf = recv_cmd(tmp, block.len);
			break;

		case HCI_H4_ACL:
			buf = recv_acl(tmp, block.len);
			break;

		case HCI_H4_ISO:
			buf = recv_iso(tmp, block.len);
			break;

		default:
			LOG_ERR("Unknown HCI type %u.", type);
			buf = NULL;
			break;
		}

		err = ipc_service_release_rx_buffer(&hci_ept, (void *)block.ptr);
		if (err < 0) {
			LOG_ERR("Failed to release rx buffer: %d.", err);
		} else {
			LOG_DBG("Released rx buffer with ret %d.", err);
		}

		if (buf) {
			k_fifo_put(&tx_queue, buf);
		}
	}
}

K_THREAD_DEFINE(queue_thread_id, CONFIG_QUEUE_THREAD_STACK_SIZE, queue_thread,
		NULL, NULL, NULL,
		CONFIG_QUEUE_THREAD_PRIO, 0, 0);

static void tx_thread(void)
{
	struct net_buf *buf;
	int err;

	while (1) {
		buf = k_fifo_get(&tx_queue, K_FOREVER);
		err = bt_send(buf);
		if (err) {
			LOG_ERR("bt_send failed err: %d.", err);
			net_buf_unref(buf);
		}

		k_yield();
	}
}

K_THREAD_DEFINE(tx_thread_id, CONFIG_BT_HCI_TX_STACK_SIZE, tx_thread,
		NULL, NULL, NULL,
		CONFIG_BT_HCI_TX_PRIO, 0, 0);

static struct ipc_ept_cfg hci_ept_cfg = {
	.name = "nrf_bt_hci",
	.cb = {
		.bound    = bound,
		.received = recv,
	},
};

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
/* Guard against re-entrancy of the fatal error path.
 *
 * Zero-latency interrupts are not masked by irq_lock(), so a fatal error raised from a
 * zero-latency interrupt can preempt an in-progress fatal error report. The buffer pool used
 * to create the Vendor Specific event is a dedicated single buffer pool, hence the second
 * entry must go straight to the reset instead of corrupting the ongoing report.
 */
static atomic_t fatal_error_reported;

/* Send a Vendor Specific fatal error event from any context.
 *
 * The fatal error can be raised from a thread, an interrupt or a zero-latency interrupt
 * context, with interrupts locked. Blocking is therefore not permitted, and the number of
 * retries is bounded so that the network core is reset instead of hanging when the IPC
 * endpoint does not accept the message.
 */
static void fatal_error_send(struct net_buf *buf)
{
	for (uint32_t i = 0U; i < CONFIG_IPC_RADIO_BT_FATAL_ERROR_SEND_RETRIES; i++) {
		int ret;

		ret = ipc_service_send(&hci_ept, buf->data, buf->len);
		if (ret >= 0) {
			return;
		}

		k_busy_wait(CONFIG_IPC_RADIO_BT_FATAL_ERROR_SEND_RETRY_US);
	}

	LOG_ERR("Can't send Fatal Error HCI event.");
}

static void fatal_error_report(struct net_buf *buf)
{
	if (!atomic_cas(&fatal_error_reported, 0, 1)) {
		LOG_ERR("Fatal Error HCI event already in progress.");
		return;
	}

	if (!ipc_ept_ready) {
		LOG_ERR("Fatal error before IPC endpoint is ready.");
		return;
	}

	if (buf == NULL) {
		LOG_ERR("Can't create Fatal Error HCI event.");
		return;
	}

	fatal_error_send(buf);
}

static void fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	if (esf == NULL) {
		LOG_ERR("Fatal error %u without an exception stack frame.", reason);
		return;
	}

	fatal_error_report(hci_vs_err_stack_frame(reason, esf));
}

FATAL_ERROR_HANDLER_DEFINE(hci_vs_fatal_error_handler, fatal_error_handler);
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

#if defined(CONFIG_BT_CTLR_ASSERT_HANDLER)
__weak void bt_ctlr_assert_handle(char *file, uint32_t line)
{
	LOG_PANIC();

	(void)irq_lock();

	LOG_ERR("Controller assert in: %s at %d.", file, line);

#if defined(CONFIG_BT_HCI_VS_FATAL_ERROR)
	fatal_error_report(hci_vs_err_assert(file, line));
#endif /* CONFIG_BT_HCI_VS_FATAL_ERROR */

	fatal_error_reset();
}
#endif /* CONFIG_BT_CTLR_ASSERT_HANDLER */

int ipc_bt_init(void)
{
	int err;
	const struct device *hci_ipc_instance = DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci_ipc));

	LOG_INF("Initializing ipc_radio bt_hci.");

	err = bt_enable_raw(&rx_queue);
	if (err) {
		LOG_ERR("bt_enable_raw failed: %d.", err);
		return err;
	}

	err = ipc_service_open_instance(hci_ipc_instance);
	if ((err < 0) && (err != -EALREADY)) {
		LOG_ERR("IPC service instance initialization failed: %d.", err);
		return err;
	}

	err = ipc_service_register_endpoint(hci_ipc_instance, &hci_ept, &hci_ept_cfg);
	if (err) {
		LOG_ERR("Registering endpoint failed: %d.", err);
		return err;
	}

	return 0;
}

int ipc_bt_process(void)
{
	struct net_buf *buf;

	k_sem_take(&ipc_bound_sem, K_FOREVER);

	while (1) {
		buf = k_fifo_get(&rx_queue, K_FOREVER);
		send(buf);
	}

	return 0;
}
