/*
 * Copyright 2021 u-blox
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/controller.h>

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include "bt_adv.h"
#include <zephyr/random/random.h>
#include <zephyr/sys/__assert.h>
#include "bt_util.h"
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app, LOG_LEVEL_DBG);

static uint16_t advIntervals[] = {50, 100, 250, 1000};
static uint8_t advIntervalIndex = 0;

static uint8_t uuid[EDDYSTONE_INSTANCE_ID_LEN];

int main(void)
{
    uint8_t randDelayMs;
    bt_addr_le_t addr;

    // If all tags are powered on at once their advertisements may collide.
    // Use a random delay in order to give them some random offset.
    randDelayMs = (uint8_t)(sys_rand32_get() & 0xFF);
    k_msleep(randDelayMs);
    LOG_DBG("Slept %dms", randDelayMs);
    utilGetBtAddr(&addr);

    for (uint8_t i = 0; i < EDDYSTONE_INSTANCE_ID_LEN; i++) {
        uuid[i] = addr.a.val[(EDDYSTONE_INSTANCE_ID_LEN - 1) - i];
    }

    // EDDYSTONE UID is the MAC for easy identification in this bug hunting session.
    LOG_HEXDUMP_INF(uuid, EDDYSTONE_INSTANCE_ID_LEN, "InstanceId (MAC)");

    bt_ctlr_set_public_addr(addr.a.val);
    __ASSERT(bt_enable(NULL) == 0, "Bluetooth init failed");
    btAdvInit(advIntervals[advIntervalIndex], advIntervals[advIntervalIndex], NULL, uuid, 0);

    struct bt_data adData;
    adData.type = BT_DATA_MANUFACTURER_DATA;
    adData.data = (uint8_t *)uuid;
    adData.data_len = EDDYSTONE_INSTANCE_ID_LEN;
    btAdvSetPerAdvData(&adData, 1);
    btAdvStart();

    return 0;
}
