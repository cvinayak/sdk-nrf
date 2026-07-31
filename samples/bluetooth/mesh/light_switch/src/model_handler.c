/*
 * Copyright (c) 2019 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @file
 * @brief Model handler for the light switch.
 *
 * Instantiates a Generic OnOff Client model for each button on the devkit, as
 * well as the standard Config and Health Server models. Handles all application
 * behavior related to the models.
 */
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/mesh/access.h>
#include <zephyr/bluetooth/mesh/proxy.h>
#include <zephyr/sys/atomic.h>
#include <errno.h>
#include <bluetooth/mesh/models.h>
#include <dk_buttons_and_leds.h>
#include "model_handler.h"

/* Light switch behavior */

/** Context for a single light switch. */
struct button {
	/** Current light status of the corresponding server. */
	bool status;
	/** Generic OnOff client instance for this switch. */
	struct bt_mesh_onoff_cli client;
};

static void status_handler(struct bt_mesh_onoff_cli *cli,
			   struct bt_mesh_msg_ctx *ctx,
			   const struct bt_mesh_onoff_status *status);

static struct button buttons[] = {
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
	{ .client = BT_MESH_ONOFF_CLI_INIT(&status_handler) },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw1))
	{ .client = BT_MESH_ONOFF_CLI_INIT(&status_handler) },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw2))
	{ .client = BT_MESH_ONOFF_CLI_INIT(&status_handler) },
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw3)) && !defined(CONFIG_BT_MESH_LOW_POWER)
	{ .client = BT_MESH_ONOFF_CLI_INIT(&status_handler) },
#endif
};

/* Periodic unacknowledged OnOff publish (button 1 start/stop). */
#define POC_GROUP_ADDR 0xC000
#define ADV_EVENT_HISTOGRAM_SIZE 8

static struct k_work_delayable periodic_tx_work;
static struct k_work_delayable stats_print_work;
static bool periodic_tx_active;
static bool periodic_onoff;
static atomic_t unack_tx_count;
static atomic_t adv_event_histogram[ADV_EVENT_HISTOGRAM_SIZE];

static void periodic_tx_end(int err, uint8_t num_sent, void *cb_data)
{
	const struct bt_mesh_model *model = cb_data;
	struct bt_mesh_onoff_cli *cli = model->rt->user_data;
	struct button *button = CONTAINER_OF(cli, struct button, client);
	int index = button - &buttons[0];

	dk_set_led(3, false);

	if (num_sent < ARRAY_SIZE(adv_event_histogram)) {
		atomic_inc(&adv_event_histogram[num_sent]);
	} else {
		printk("Unexpected num_sent value: %u\n", num_sent);
	}

	if (err) {
		printk("Periodic OnOff %d send failed: %d\n", index + 1, err);
	}
}

static void periodic_tx_send_start(uint16_t duration, int err, void *cb_data)
{
	ARG_UNUSED(duration);

	if (err) {
		periodic_tx_end(err, 0, cb_data);
	}
}

static const struct bt_mesh_send_cb periodic_tx_cb = {
	.start = periodic_tx_send_start,
	.end = periodic_tx_end,
};

static int periodic_tx_get_ctx(struct bt_mesh_onoff_cli *cli,
			       struct bt_mesh_msg_ctx *ctx)
{
	const struct bt_mesh_model *model = cli->model;
	uint16_t app_idx = BT_MESH_KEY_UNUSED;

	if (!model || !model->pub) {
		return -EADDRNOTAVAIL;
	}

	if (model->pub->addr == BT_MESH_ADDR_UNASSIGNED) {
		return -EADDRNOTAVAIL;
	}

	for (int i = 0; i < model->keys_cnt; i++) {
		if (model->keys[i] != BT_MESH_KEY_DEV_ANY &&
		    model->keys[i] != BT_MESH_KEY_UNUSED) {
			app_idx = model->keys[i];
			break;
		}
	}

	if (app_idx == BT_MESH_KEY_UNUSED) {
		return -EINVAL;
	}

	ctx->addr = model->pub->addr;
	ctx->app_idx = app_idx;
	ctx->send_ttl = model->pub->ttl ? model->pub->ttl : BT_MESH_TTL_DEFAULT;
	ctx->send_rel = false;
	ctx->cb = &periodic_tx_cb;

	return 0;
}

static void periodic_tx_log_config(const struct bt_mesh_onoff_cli *cli)
{
	const struct bt_mesh_model *model = cli->model;
	uint16_t pub_addr = BT_MESH_ADDR_UNASSIGNED;
	bool has_app_key;

	has_app_key = false;
	if (model && model->pub) {
		pub_addr = model->pub->addr;
	}

	if (model) {
		for (int i = 0; i < model->keys_cnt; i++) {
			if (model->keys[i] != BT_MESH_KEY_DEV_ANY &&
			    model->keys[i] != BT_MESH_KEY_UNUSED) {
				has_app_key = true;
				break;
			}
		}
	}

	if (pub_addr == BT_MESH_ADDR_UNASSIGNED) {
		printk("Element 1 OnOff Client: publish=unassigned app_key=%s\n",
		       has_app_key ? "bound" : "NOT bound");
	} else if (pub_addr == POC_GROUP_ADDR) {
		printk("Element 1 OnOff Client: publish=0x%04x (group) app_key=%s\n",
		       pub_addr, has_app_key ? "bound" : "NOT bound");
	} else if (bt_mesh_model_pub_is_unicast(model)) {
		printk("Element 1 OnOff Client: publish=0x%04x (unicast) app_key=%s\n",
		       pub_addr, has_app_key ? "bound" : "NOT bound");
		printk("POC: expected group publish address 0x%04x\n", POC_GROUP_ADDR);
	} else {
		printk("Element 1 OnOff Client: publish=0x%04x (group) app_key=%s\n",
		       pub_addr, has_app_key ? "bound" : "NOT bound");
		printk("POC: expected group publish address 0x%04x\n", POC_GROUP_ADDR);
	}
}

static bool periodic_tx_start(void)
{
	struct bt_mesh_msg_ctx ctx = {0};
	int err;

	err = periodic_tx_get_ctx(&buttons[0].client, &ctx);
	if (err) {
		periodic_tx_log_config(&buttons[0].client);
		printk("Periodic TX not started: model publish not ready (%d)\n",
		       err);
		printk("Configure Element 1 Generic OnOff Client in nRF Mesh:\n");
		printk("  - Bind Application Key 1\n");
		printk("  - Set Publish Address to group 0x%04x\n", POC_GROUP_ADDR);
		return false;
	}

	if (ctx.addr != POC_GROUP_ADDR) {
		periodic_tx_log_config(&buttons[0].client);
		printk("Periodic TX not started: publish address must be 0x%04x\n",
		       POC_GROUP_ADDR);
		return false;
	}

	periodic_tx_log_config(&buttons[0].client);
	printk("Periodic TX started (100 ms)\n");
	return true;
}

static void stats_print_handler(struct k_work *work)
{
	atomic_val_t bins[ARRAY_SIZE(adv_event_histogram)];
	atomic_val_t total = 0;

	for (int i = 0; i < ARRAY_SIZE(bins); i++) {
		bins[i] = atomic_get(&adv_event_histogram[i]);
		total += bins[i];
	}

	printk("TX accepted: %d, num_sent samples: %d: %d %d %d %d %d %d %d %d\n",
	       (int)atomic_get(&unack_tx_count), (int)total, (int)bins[0], (int)bins[1],
	       (int)bins[2], (int)bins[3], (int)bins[4], (int)bins[5], (int)bins[6],
	       (int)bins[7]);
	k_work_reschedule(&stats_print_work, K_SECONDS(5));
}

static void periodic_tx_handler(struct k_work *work)
{
	if (!periodic_tx_active) {
		return;
	}

	struct bt_mesh_onoff_set set = {
		.on_off = periodic_onoff,
	};
	struct bt_mesh_msg_ctx ctx = {0};
	int err;

	err = periodic_tx_get_ctx(&buttons[0].client, &ctx);
	if (err) {
		printk("Periodic TX stopped: publish not configured (%d)\n", err);
		periodic_tx_active = false;
		k_work_cancel_delayable(&periodic_tx_work);
		return;
	}

	/* LED3 serves as a digital signal indicating when the model message API is
	 * invoked and when the message is sent.
	 */
	dk_set_led(3, true);
	err = bt_mesh_onoff_cli_set_unack(&buttons[0].client, &ctx, &set);
	if (!err) {
		atomic_inc(&unack_tx_count);
		periodic_onoff = !periodic_onoff;
		buttons[0].status = set.on_off;
		dk_set_led(0, set.on_off);
	} else {
		dk_set_led(3, false);
		printk("Periodic OnOff set failed: %d\n", err);
		if (err == -EADDRNOTAVAIL || err == -EINVAL) {
			periodic_tx_active = false;
			k_work_cancel_delayable(&periodic_tx_work);
			return;
		}
	}

	k_work_reschedule(&periodic_tx_work, K_MSEC(100));
}

static void status_handler(struct bt_mesh_onoff_cli *cli,
			   struct bt_mesh_msg_ctx *ctx,
			   const struct bt_mesh_onoff_status *status)
{
	struct button *button =
		CONTAINER_OF(cli, struct button, client);
	int index = button - &buttons[0];

	button->status = status->present_on_off;
	dk_set_led(index, status->present_on_off);

	printk("Button %d: Received response: %s\n", index + 1,
	       status->present_on_off ? "on" : "off");
}

static void button_handler_cb(uint32_t pressed, uint32_t changed)
{
	if (!bt_mesh_is_provisioned()) {
		return;
	}

	if (IS_ENABLED(CONFIG_BT_MESH_LOW_POWER) && (pressed & changed & BIT(3))) {
		bt_mesh_proxy_identity_enable();
		return;
	}

	for (int i = 0; i < ARRAY_SIZE(buttons); ++i) {
		if (!(pressed & changed & BIT(i))) {
			continue;
		}

#if DT_NODE_EXISTS(DT_ALIAS(sw0))
		if (i == 0) {
			if (!periodic_tx_active) {
				if (!periodic_tx_start()) {
					continue;
				}
				periodic_tx_active = true;
				k_work_reschedule(&periodic_tx_work, K_NO_WAIT);
				k_work_reschedule(&stats_print_work, K_SECONDS(5));
			} else {
				periodic_tx_active = false;
				k_work_cancel_delayable(&periodic_tx_work);
				printk("Periodic TX stopped\n");
			}
			continue;
		}
#endif

		struct bt_mesh_onoff_set set = {
			.on_off = !buttons[i].status,
		};
		int err;

		/* As we can't know how many nodes are in a group, it doesn't
		 * make sense to send acknowledged messages to group addresses -
		 * we won't be able to make use of the responses anyway. This also
		 * applies in LPN mode, since we can't expect to receive a response
		 * in appropriate time.
		 */
		if (bt_mesh_model_pub_is_unicast(buttons[i].client.model) &&
		    !IS_ENABLED(CONFIG_BT_MESH_LOW_POWER)) {
			err = bt_mesh_onoff_cli_set(&buttons[i].client, NULL, &set, NULL);
		} else {
			err = bt_mesh_onoff_cli_set_unack(&buttons[i].client,
							  NULL, &set);
			if (!err) {
				/* There'll be no response status for the
				 * unacked message. Set the state immediately.
				 */
				buttons[i].status = set.on_off;
				dk_set_led(i, set.on_off);
			}
		}

		if (err) {
			printk("OnOff %d set failed: %d\n", i + 1, err);
		}
	}
}

/* Set up a repeating delayed work to blink the DK's LEDs when attention is
 * requested.
 */
static struct k_work_delayable attention_blink_work;
static bool attention;

static void attention_blink(struct k_work *work)
{
	static int idx;
	const uint8_t pattern[] = {
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
		BIT(0),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw1))
		BIT(1),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw2))
		BIT(2),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw3))
		BIT(3),
#endif
	};

	if (attention) {
		dk_set_leds(pattern[idx++ % ARRAY_SIZE(pattern)]);
		k_work_reschedule(&attention_blink_work, K_MSEC(30));
	} else {
		dk_set_leds(DK_NO_LEDS_MSK);
	}
}

static void attention_on(const struct bt_mesh_model *mod)
{
	attention = true;
	k_work_reschedule(&attention_blink_work, K_NO_WAIT);
}

static void attention_off(const struct bt_mesh_model *mod)
{
	/* Will stop rescheduling blink timer */
	attention = false;
}

static const struct bt_mesh_health_srv_cb health_srv_cb = {
	.attn_on = attention_on,
	.attn_off = attention_off,
};

static struct bt_mesh_health_srv health_srv = {
	.cb = &health_srv_cb,
};

BT_MESH_HEALTH_PUB_DEFINE(health_pub, 0);

static struct bt_mesh_elem elements[] = {
#if DT_NODE_EXISTS(DT_ALIAS(sw0))
	BT_MESH_ELEM(1,
		     BT_MESH_MODEL_LIST(
			     BT_MESH_MODEL_CFG_SRV,
			     BT_MESH_MODEL_HEALTH_SRV(&health_srv, &health_pub),
			     BT_MESH_MODEL_ONOFF_CLI(&buttons[0].client)),
		     BT_MESH_MODEL_NONE),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw1))
	BT_MESH_ELEM(2,
		     BT_MESH_MODEL_LIST(
			     BT_MESH_MODEL_ONOFF_CLI(&buttons[1].client)),
		     BT_MESH_MODEL_NONE),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw2))
	BT_MESH_ELEM(3,
		     BT_MESH_MODEL_LIST(
			     BT_MESH_MODEL_ONOFF_CLI(&buttons[2].client)),
		     BT_MESH_MODEL_NONE),
#endif
#if DT_NODE_EXISTS(DT_ALIAS(sw3)) && !defined(CONFIG_BT_MESH_LOW_POWER)
	BT_MESH_ELEM(4,
		     BT_MESH_MODEL_LIST(
			     BT_MESH_MODEL_ONOFF_CLI(&buttons[3].client)),
		     BT_MESH_MODEL_NONE),
#endif

};

static const struct bt_mesh_comp comp = {
	.cid = CONFIG_BT_COMPANY_ID,
	.elem = elements,
	.elem_count = ARRAY_SIZE(elements),
};

const struct bt_mesh_comp *model_handler_init(void)
{
	static struct button_handler button_handler = {
		.cb = button_handler_cb,
	};

	dk_button_handler_add(&button_handler);
	k_work_init_delayable(&attention_blink_work, attention_blink);
	k_work_init_delayable(&periodic_tx_work, periodic_tx_handler);
	k_work_init_delayable(&stats_print_work, stats_print_handler);

	return &comp;
}
