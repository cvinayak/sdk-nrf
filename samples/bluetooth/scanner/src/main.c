/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 *  SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include <zephyr/sys/__assert.h>

#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/reboot.h>

#include <zephyr/console/console.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/direction.h>

LOG_MODULE_REGISTER(u_df_scan, LOG_LEVEL_DBG);

// Uncomment below if CTE sampling shall be enabled after successful sync.
//#define SAMPLE_CTE

// Select either or both for tag state debugging

// +UUDFDBG:5293.2,16052.2,6514.2,3986.2,12064.2,1862.2,1627.2,4139.2,819.2,11194.2,7473.2,9394.2,9492.2,3799.2,15619.2,5571.2,11080.2,5432.2,5672.2,14029.2,
//#define PRINT_STATS_FOR_PLOT

// TAGS:0,0,sync...,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,-,
#define PRINT_STATS_FOR_READING

#define SYNCED_DEVICE_QUEUE_SIZE CONFIG_BT_CTLR_SYNC_PERIODIC_ADV_LIST_SIZE

#define LIST_INDEX_NONE 0xFF
#define NAME_LEN        30
#define DEVICE_LOOK_FOR_NAME "u-blox AoA 1M"
#define SYNC_LOST_TIMEOUT_10MS_UNIT	300

// Set to the number of tags set up in the test
#define NUM_TAGS_TO_LOOK_FOR 5

BUILD_ASSERT(CONFIG_BT_CTLR_SYNC_PERIODIC_ADV_LIST_SIZE >= NUM_TAGS_TO_LOOK_FOR, "Periodic advertising list size must be greater than NUM_TAGS_TO_LOOK_FOR");
BUILD_ASSERT(CONFIG_BT_PER_ADV_SYNC_MAX >= NUM_TAGS_TO_LOOK_FOR, "Maximum number of periodic advertising syncs must be greater than NUM_TAGS_TO_LOOK_FOR");

typedef struct device_t {
	struct bt_le_per_adv_sync *sync;
	bt_addr_le_t per_addr;
	int8_t per_sid;
	bool is_synced;
	bool is_used;
	uint32_t num_cte;
	uint16_t sync_index;
	uint32_t num_per_data;
	uint32_t num_per_data_error;
} device_t;

static int enable_cte_rx(device_t *device);

static device_t sync_devices[SYNCED_DEVICE_QUEUE_SIZE];

static struct bt_le_per_adv_sync   *syncForList;

static bool scan_enabled;
static int scan_enable(void);

#if defined(CONFIG_BT_CTLR_DF_ANT_SWITCH_RX)
const static uint8_t ant_patterns[] = { 0x0c, 0x0c, 0x0e, 0x0f, 0x04, 0x06, 0x0c, 0x0e, 0x0f, 0x04, 0x06, 0x0a, 0x08, 0x09, 0x02, 0x00, 0x0a, 0x08, 0x09, 0x02, 0x00 };
#endif /* CONFIG_BT_CTLR_DF_ANT_SWITCH_RX */

static bool data_cb(struct bt_data *data, void *user_data);
static void scan_recv(const struct bt_le_scan_recv_info *info,
			struct net_buf_simple *buf);
static void sync_cb(struct bt_le_per_adv_sync *sync,
			struct bt_le_per_adv_sync_synced_info *info);
static void term_cb(struct bt_le_per_adv_sync *sync,
			const struct bt_le_per_adv_sync_term_info *info);
static void recv_cb(struct bt_le_per_adv_sync *sync,
			const struct bt_le_per_adv_sync_recv_info *info,
			struct net_buf_simple *buf);
static void scan_recv(const struct bt_le_scan_recv_info *info,
			struct net_buf_simple *buf);
static void cte_recv_cb(struct bt_le_per_adv_sync *sync,
			struct bt_df_per_adv_sync_iq_samples_report const *report);
static void reset(struct k_work *work);

static struct bt_le_per_adv_sync_cb sync_callbacks = {
	.synced = sync_cb,
	.term = term_cb,
	.recv = recv_cb,
	.cte_report_cb = cte_recv_cb,
};

static struct bt_le_scan_cb scan_callbacks = {
	.recv = scan_recv,
};

K_WORK_DELAYABLE_DEFINE(restart_timeout_work, reset);

static void reset(struct k_work *work)
{
	bool error_detected = false;
	char error_log[512];
	memset(error_log, 0, sizeof(error_log));
	uint32_t index = 0;

	for (int i = 0; i < SYNCED_DEVICE_QUEUE_SIZE; i++) {
		if (sync_devices[i].is_synced) {
			if (sync_devices[i].num_per_data_error > 0) {
				error_detected = true;
			}
			index += snprintf(&error_log[index], sizeof(error_log) - index, "Sync index: %d, Total data packets: %d, Num error packets: %d (%d%%)\n", bt_le_per_adv_sync_get_index(sync_devices[i].sync), sync_devices[i].num_per_data, sync_devices[i].num_per_data_error, (sync_devices[i].num_per_data_error / sync_devices[i].num_per_data) * 100);
		}
	}
	printk("%s\n", error_log);
	k_msleep(1000);
	if (!error_detected) {
		LOG_INF("No Error detected, rebooting");
		sys_reboot(SYS_REBOOT_COLD);
	} else {
		__ASSERT(0, "Error detected, just assert so logs can be studied easier");
	}
}

static const char *phy2str(uint8_t phy)
{
	switch (phy) {
	case 0: return "No packets";
	case BT_GAP_LE_PHY_1M: return "LE 1M";
	case BT_GAP_LE_PHY_2M: return "LE 2M";
	case BT_GAP_LE_PHY_CODED: return "LE Coded";
	default: return "Unknown";
	}
}

static bool data_cb(struct bt_data *data, void *user_data)
{
	char *name = user_data;
	uint8_t len;

	switch (data->type) {
	case BT_DATA_NAME_SHORTENED:
	case BT_DATA_NAME_COMPLETE:
		len = MIN(data->data_len, NAME_LEN - 1);
		memcpy(name, data->data, len);
		name[len] = '\0';
		return false;
	default:
		return true;
	}
}

static int findTagIndexInSyncedListByAddr(const bt_addr_le_t *addr, int *const freeIndex)
{
	if (freeIndex) {
        *freeIndex = LIST_INDEX_NONE;
    }
    for (int i = 0; i < SYNCED_DEVICE_QUEUE_SIZE; i++) {
        if (sync_devices[i].is_used &&
            bt_addr_le_cmp(addr, &sync_devices[i].per_addr) == 0) {
            return i;
        } else if (freeIndex != NULL && !sync_devices[i].is_used && *freeIndex == LIST_INDEX_NONE) {
            *freeIndex = i;
        }
    }

    return LIST_INDEX_NONE;
}

static void restart_per_adv_list_syncing(void)
{
    int err;
    struct bt_le_per_adv_sync_param sync_create_param;
    // Check if we need to create new sync
	LOG_INF("Creating Sync for tags in list");
	sync_create_param.options = BT_LE_PER_ADV_SYNC_OPT_USE_PER_ADV_LIST;
	sync_create_param.skip = 0;
	sync_create_param.sid = 0; // Not used but needs to be 0 otherwise error
	sync_create_param.timeout = SYNC_LOST_TIMEOUT_10MS_UNIT;
	err = bt_le_per_adv_sync_create(&sync_create_param, &syncForList);
	__ASSERT(err == 0 || err == -EBUSY || err == -ENOMEM, "Failed enable sync: %d", err);
	if (err == -EBUSY) {
		LOG_WRN("Per syncing already enabled!");
	} else if (err == -ENOMEM) {
		LOG_WRN("Max synced tags reached!");
	}
}

static void sync_cb(struct bt_le_per_adv_sync *sync,
			struct bt_le_per_adv_sync_synced_info *info)
{
	char le_addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	LOG_DBG("(%d) PER_ADV_SYNC[%u]: [DEVICE]: %s synced, "
	       "Interval 0x%04x (%u ms), PHY %s\n",
		   k_uptime_get_32(),
	       bt_le_per_adv_sync_get_index(sync), le_addr,
	       info->interval, info->interval * 5 / 4, phy2str(info->phy));

	uint8_t device_index = findTagIndexInSyncedListByAddr(info->addr, NULL);
	if (device_index == LIST_INDEX_NONE) {
		LOG_ERR("Got sync from none queued tag!");
		restart_per_adv_list_syncing();
		return;
	}
	sync_devices[device_index].sync = sync;
	sync_devices[device_index].sync_index = bt_le_per_adv_sync_get_index(sync);

	sync_devices[device_index].is_synced = true;

	int err = bt_le_per_adv_list_remove(info->addr, info->sid);
	LOG_DBG("Removed tag from per adv list");
	__ASSERT(err == 0, "Expected tag to be in controller adv_list. Is there a race?");
	restart_per_adv_list_syncing();


#ifdef SAMPLE_CTE
	err = enable_cte_rx(&sync_devices[device_index]);
	if (err) {
		err = bt_le_per_adv_sync_delete(sync_devices[device_index].sync);
		if (err) {
			printk("sync_cb: Failed cancel sync\n");
		}
		printk("Clear device index: %d\n", device_index);
		memset(&sync_devices[device_index].per_addr, 0, sizeof(bt_addr_le_t));
		sync_devices[device_index].sync = NULL;
		sync_devices[device_index].is_used = false;
		sync_devices[device_index].is_synced = false;
	 }
#endif

	int synced_count = 0;
	for (int i = 0; i < SYNCED_DEVICE_QUEUE_SIZE; i++) {
		if (sync_devices[i].is_synced) {
			synced_count++;
		}
	}
	if (synced_count == NUM_TAGS_TO_LOOK_FOR) {
		LOG_WRN("All tags synced, if no errors restart in 1s");
		k_work_schedule(&restart_timeout_work, K_MSEC(1000));
	}
}

static void term_cb(struct bt_le_per_adv_sync *sync,
			const struct bt_le_per_adv_sync_term_info *info)
{
	char le_addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	LOG_DBG("(%d) PER_ADV_SYNC[%u]: [DEVICE]: %s sync terminated reason: %d\n",
			k_uptime_get_32(),
	       bt_le_per_adv_sync_get_index(sync), le_addr, info->reason);

	uint8_t device_index = findTagIndexInSyncedListByAddr(info->addr, NULL);
	if (device_index == LIST_INDEX_NONE) {
		LOG_ERR("Got sync_term from none queued tag!");
		__ASSERT(false, "Got sync_term from none queued tag!");
		restart_per_adv_list_syncing();
		return;
	}

	if (sync_devices[device_index].is_synced) {
		LOG_DBG("Device was synced: ");
	} else if (sync_devices[device_index].is_used) {
		LOG_ERR("term_cb for NONE synced tag! info->reason: %d", info->reason);
		LOG_DBG("Remove tag from per adv list");
		int err = bt_le_per_adv_list_remove(info->addr, info->sid);
        if (err) {
            LOG_ERR("bt_le_per_adv_list_remove failed (err %d)\n", err);
        }
	} else {
		LOG_ERR("Device unknown state: ");
	}

	memset(&sync_devices[device_index], 0, sizeof(device_t));

	restart_per_adv_list_syncing();
}

static void recv_cb(struct bt_le_per_adv_sync *sync,
			const struct bt_le_per_adv_sync_recv_info *info,
			struct net_buf_simple *buf)
{
	char le_addr_data[BT_ADDR_LE_STR_LEN];
	char le_addr_payload[BT_ADDR_LE_STR_LEN];
	uint8_t tagIndex;

	if (buf->len == 0) {
        return;
    }

    bt_addr_le_to_str(info->addr, le_addr_data, sizeof(le_addr_data));
    tagIndex = findTagIndexInSyncedListByAddr(info->addr, NULL);
    if (tagIndex != LIST_INDEX_NONE && sync_devices[tagIndex].is_synced) {
		__ASSERT(sync_devices[tagIndex].sync_index == bt_le_per_adv_sync_get_index(sync), "recv_cb wrong sync index");
        if (buf->len == 8) {
			bool error_detected = false;
            uint8_t *instanceId = &buf->data[2];
			bt_addr_le_t payloadAddr;
			payloadAddr.type = BT_ADDR_LE_PUBLIC;
			for (int i = 0; i < BT_ADDR_SIZE; i++) {
				payloadAddr.a.val[i] = instanceId[BT_ADDR_SIZE - 1 - i];
			}
			bt_addr_le_to_str(&payloadAddr, le_addr_payload, sizeof(le_addr_payload));

			if (sync_devices[tagIndex].sync_index != bt_le_per_adv_sync_get_index(sync)) {
				error_detected = true;
				LOG_ERR("Miss match in sync index, expected: %d, but was %d", sync_devices[tagIndex].sync_index, bt_le_per_adv_sync_get_index(sync));
			}

			if (bt_addr_le_cmp(&payloadAddr, info->addr) != 0) {
				error_detected = true;
                LOG_ERR("Data miss match: %d, sync_index: %d, MAC Addr : %s BUT data is: %s", tagIndex,  bt_le_per_adv_sync_get_index(sync), le_addr_data, le_addr_payload);
            }
			sync_devices[tagIndex].num_per_data++;
			if (error_detected) {
				sync_devices[tagIndex].num_per_data_error++;
			}
		}
        //uBleCommandUUDFPEvent(tag->tag, buf->data, buf->len, info->rssi);
    } else if (tagIndex == LIST_INDEX_NONE) {
        __ASSERT(false, "Got per. adv. data from tag not known in BT stack, index: %d", tagIndex);
    } else {
        // Got data from terminated tag, but BT stack have not yet removed it.
    }

    //__ASSERT(bt_addr_le_cmp(info->addr, &sync->addr) == 0, "recv_cb wrong addr");
}

static void cte_recv_cb(struct bt_le_per_adv_sync *sync,
			struct bt_df_per_adv_sync_iq_samples_report const *report)
{
	if (report->packet_status != 0) {
		return;
	}

	struct bt_le_per_adv_sync_info info;
	int err = bt_le_per_adv_sync_get_info(sync, &info);
	__ASSERT(err == 0, "Must find the sync here, otherwise something wrong");
	uint8_t device_index = findTagIndexInSyncedListByAddr(&info.addr, NULL);
	if (device_index == LIST_INDEX_NONE) {
		LOG_ERR("Got cte_recv_cb from none queued tag!");
		restart_per_adv_list_syncing();
		return;
	}

	if (report->sample_count != 82) {
		printk("_______MISSING IQ DATA %d/82 device index: %d, slot_durations: %d\n", report->sample_count, device_index, report->slot_durations);
		if (report->slot_durations != 1) {
			printk("ERR\n");
		}
		return;
	}

	__ASSERT(sync_devices[device_index].is_synced && sync_devices[device_index].is_used, "CTE from tag which is not synced!");

	sync_devices[device_index].num_cte++;
}

static void scan_recv(const struct bt_le_scan_recv_info *info,
			  struct net_buf_simple *buf)
{
	char le_addr[BT_ADDR_LE_STR_LEN];
	char name[NAME_LEN];
	int device_index;
	(void)memset(name, 0, sizeof(name));
	static int no_spam_counter = 0;

	bt_data_parse(buf, data_cb, name);

	bt_addr_le_to_str(info->addr, le_addr, sizeof(le_addr));

	if (no_spam_counter % 500 == 0) {
		LOG_DBG("Scan is running...\n");
	}
	no_spam_counter++;

	if ((info->interval != 0) && (strncmp(DEVICE_LOOK_FOR_NAME, name, strlen(DEVICE_LOOK_FOR_NAME)) == 0)) {
		// Do not try to sync with device we already synced with.
		// Could probably also use bt_le_per_adv_sync_lookup_addr(...) or similar
		// Check if already handled
		if (findTagIndexInSyncedListByAddr(info->addr, &device_index) != LIST_INDEX_NONE) {
			return;
		}
		int err = bt_le_per_adv_list_add(info->addr, info->sid);
		if (err) {
			LOG_ERR("Failed adding to bt_le_per_adv_list_add err= %d", err);
			return;
		} else {
			LOG_DBG("Added tag to per adv list");
			sync_devices[device_index].is_used = true;
			sync_devices[device_index].is_synced = false;
			sync_devices[device_index].per_sid = info->sid;
			bt_addr_le_copy(&sync_devices[device_index].per_addr, info->addr);
		}
	}
}

static int enable_cte_rx(device_t *device)
{
	int err;

	const struct bt_df_per_adv_sync_cte_rx_param cte_rx_params = {
		.max_cte_count = 1,
#if defined(CONFIG_BT_CTLR_DF_ANT_SWITCH_RX)
		.cte_types = BT_DF_CTE_TYPE_ALL,
		.slot_durations = 0x1,
		.num_ant_ids = ARRAY_SIZE(ant_patterns),
		.ant_ids = ant_patterns,
#else
		.cte_types = BT_DF_CTE_TYPE_ALL,
#endif /* CONFIG_BT_CTLR_DF_ANT_SWITCH_RX */
	};

	LOG_DBG("Enable receiving of CTE...\n");
	err = bt_df_per_adv_sync_cte_rx_enable(device->sync, &cte_rx_params);
	if (err != 0) {
		printk("failed (err %d)\n", err);
		return err;
	}
	LOG_DBG("(%d) success. CTE receive enabled.\n", k_uptime_get_32());
	return 0;
}

static int scan_init(void)
{
	printk("Scan callbacks register...");
	bt_le_scan_cb_register(&scan_callbacks);
	printk("success.\n");

	printk("Periodic Advertising callbacks register...");
	bt_le_per_adv_sync_cb_register(&sync_callbacks);
	printk("success.\n");

	return 0;
}

static int scan_enable(void)
{
	int err;
	struct bt_le_scan_param param = {
		.type       = BT_LE_SCAN_TYPE_PASSIVE,
		.options    = BT_LE_SCAN_OPT_NONE,
		.interval   = 0x10,
		.window     = 0x10,
		.timeout    = 0U,
	};

	if (!scan_enabled) {
		printk("Start scanning...");
		err = bt_le_scan_start(&param, NULL);
		if (err != 0) {
			printk("failed (err %d)\n", err);
			return err;
		}
		printk("success\n");
		scan_enabled = true;
	}

	return 0;
}

#define DEBUG_PRINT_LEN 500
static char debugBuf[DEBUG_PRINT_LEN];
typedef enum {
    TAG_IN_QUEUED,
    TAG_IN_PER_ADV_LIST,
    TAG_SYNCED
} tagState_t;

static int state_from_device(device_t *tag)
{
    if (!tag->is_synced) {
        return TAG_IN_PER_ADV_LIST;
    } else if (tag->is_synced) {
        return TAG_SYNCED;
    } else {
        return TAG_IN_QUEUED;
    }
}

static void print_stats_for_plot(void)
{
    int numQueued = 0;
    memset(debugBuf, 0, sizeof(debugBuf));
    int index = 0;
    index += snprintf(debugBuf + index, sizeof(debugBuf) - index, "+UUDFDBG:");
    for (int i = 0; i < SYNCED_DEVICE_QUEUE_SIZE; i++) {
        if (sync_devices[i].is_used) {
			numQueued++;
			// Use last 2 bytes from addr as the identifier.
			index += snprintf(debugBuf + index, sizeof(debugBuf) - index, "%d.%d,",
							*((uint16_t *)&sync_devices[i].per_addr.a.val[4]), state_from_device(&sync_devices[i]));
		}
    }
    index += snprintf(debugBuf + index, sizeof(debugBuf) - index, "\r\n");
    // Output an event so that we can plot the state of all tags.
    printk("%sQueued: %d\n", debugBuf, numQueued);
}

// This data is input to python scripts that collects and plots per.adv.sync and CTE statistics.
static void print_stats_readable(void)
{
	int numQueued = 0;
    memset(debugBuf, 0, sizeof(debugBuf));
    int index = 0;
    index += snprintf(debugBuf + index, sizeof(debugBuf) - index, "TAGS:");
	for (int i = 0; i < SYNCED_DEVICE_QUEUE_SIZE; i++) {
		if (sync_devices[i].is_synced) {
			index += snprintf(debugBuf + index, sizeof(debugBuf) - index, "%d,", sync_devices[i].num_cte);
		} else if (sync_devices[i].is_used) {
			index += snprintf(debugBuf + index, sizeof(debugBuf) - index, "sync...,");
		} else {
			index += snprintf(debugBuf + index, sizeof(debugBuf) - index, "-,");
		}
		sync_devices[i].num_cte = 0;
	}
	printk("%s, %d\n", debugBuf, numQueued);
}

int main(void)
{
	int err;
	printk("Starting Connectionless Locator Demo\n");

	memset(sync_devices, 0, sizeof(sync_devices));

	printk("Bluetooth initialization...");

	err = bt_enable(NULL);
	if (err != 0) {
		printk("failed (err %d)\n", err);
	}
	printk("success\n");

	scan_init();
	scan_enable();

	restart_per_adv_list_syncing();

	printk("Waiting for periodic advertising...\n");
	while (true) {
		k_msleep(1000);
#ifdef PRINT_STATS_FOR_PLOT
		print_stats_for_plot();
#endif
#ifdef PRINT_STATS_FOR_READING
		print_stats_readable();
#endif
	}

	return 0;
}
