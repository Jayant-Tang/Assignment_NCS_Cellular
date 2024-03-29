/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <app_event_manager.h>
#if defined(CONFIG_NRF_MODEM_LIB)
#include <modem/nrf_modem_lib.h>
#endif /* CONFIG_NRF_MODEM_LIB */
#include <zephyr/sys/reboot.h>
#if defined(CONFIG_LWM2M_INTEGRATION)
#include <net/lwm2m_client_utils.h>
#endif /* CONFIG_LWM2M_INTEGRATION */
#include <net/nrf_cloud.h>

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>

#include <zephyr/drivers/gpio.h>                                                                                                                                                     
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/i2c.h>

/* Module name is used by the Application Event Manager macros in this file */
#define MODULE main
#include <caf/events/module_state_event.h>

#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/cloud_module_event.h"
#include "events/data_module_event.h"
#include "events/sensor_module_event.h"
#include "events/util_module_event.h"
#include "events/modem_module_event.h"

// added by jayant
#include "events/ui_module_event.h"

#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <zephyr/pm/device.h>

#include "voltage-sensor.h"

// 判断电压表device tree node是否存在
#if !DT_NODE_EXISTS(DT_PATH(my_voltage_sensor))|| \
    !DT_NODE_HAS_PROP(DT_PATH(my_voltage_sensor), io_channels)
#error "vdd sensor does not exist!"
#endif

const struct device *voltage_sensor = DEVICE_DT_GET(DT_PATH(my_voltage_sensor));

#include <zephyr/net/socket.h>
static int client_fd;
static struct sockaddr_storage host_addr;
static bool connected_3rd_party = false;

LOG_MODULE_REGISTER(MODULE, CONFIG_APPLICATION_MODULE_LOG_LEVEL);

/* Message structure. Events from other modules are converted to messages
 * in the Application Event Manager handler, and then queued up in the message queue
 * for processing in the main thread.
 */
struct app_msg_data {
	union {
		struct cloud_module_event cloud;
		struct sensor_module_event sensor;
		struct data_module_event data;
		struct util_module_event util;
		struct modem_module_event modem;
		struct app_module_event app;
        struct ui_module_event ui; // added by jayant
	} module;
};

/* Application module super states. */
static enum state_type {
	STATE_INIT,
	STATE_RUNNING,
	STATE_SHUTDOWN
} state;

/* Application sub states. The application can be in either active or passive
 * mode.
 *
 * Active mode: Sensor data and GNSS position is acquired at a configured
 *		interval and sent to cloud.
 *
 * Passive mode: Sensor data and GNSS position is acquired when movement is
 *		 detected, or after the configured movement timeout occurs.
 */
static enum sub_state_type {
	SUB_STATE_ACTIVE_MODE,
	SUB_STATE_PASSIVE_MODE,
} sub_state;

/* Internal copy of the device configuration. */
static struct cloud_data_cfg app_cfg;

/* Variable that keeps track whether modem static data has been successfully sampled by the
 * modem module. Modem static data does not change and only needs to be sampled and sent to cloud
 * once.
 */
static bool modem_static_sampled;

/* Variable that is set high whenever a sample request is ongoing. */
static bool sample_request_ongoing;

/* Variable that is set high whenever the device is considered active (under movement). */
static bool activity;

/* Timer callback used to signal when timeout has occurred both in active
 * and passive mode.
 */
static void data_sample_timer_handler(struct k_timer *timer);

/* Application module message queue. */
#define APP_QUEUE_ENTRY_COUNT		10
#define APP_QUEUE_BYTE_ALIGNMENT	4

/* Data fetching timeouts */
#define DATA_FETCH_TIMEOUT_DEFAULT 2

K_MSGQ_DEFINE(msgq_app, sizeof(struct app_msg_data), APP_QUEUE_ENTRY_COUNT,
	      APP_QUEUE_BYTE_ALIGNMENT);

/* Data sample timer used in active mode. */
K_TIMER_DEFINE(data_sample_timer, data_sample_timer_handler, NULL);

/* Movement timer used to detect movement timeouts in passive mode. */
K_TIMER_DEFINE(movement_timeout_timer, data_sample_timer_handler, NULL);

/* Movement resolution timer decides the period after movement that consecutive
 * movements are ignored and do not cause data collection. This is used to
 * lower power consumption by limiting how often GNSS search is performed and
 * data is sent on air.
 */
K_TIMER_DEFINE(movement_resolution_timer, NULL, NULL);

/* Module data structure to hold information of the application module, which
 * opens up for using convenience functions available for modules.
 */
static struct module_data self = {
	.name = "app",
	.msg_q = &msgq_app,
	.supports_shutdown = true,
};

#if defined(CONFIG_NRF_MODEM_LIB)
NRF_MODEM_LIB_ON_INIT(asset_tracker_init_hook, on_modem_lib_init, NULL);

/* Initialized to value different than success (0) */
static int modem_lib_init_result = -1;

static void on_modem_lib_init(int ret, void *ctx)
{
	modem_lib_init_result = ret;
}
#endif /* CONFIG_NRF_MODEM_LIB */

/* Convenience functions used in internal state handling. */
static char *state2str(enum state_type new_state)
{
	switch (new_state) {
	case STATE_INIT:
		return "STATE_INIT";
	case STATE_RUNNING:
		return "STATE_RUNNING";
	case STATE_SHUTDOWN:
		return "STATE_SHUTDOWN";
	default:
		return "Unknown";
	}
}

static char *sub_state2str(enum sub_state_type new_state)
{
	switch (new_state) {
	case SUB_STATE_ACTIVE_MODE:
		return "SUB_STATE_ACTIVE_MODE";
	case SUB_STATE_PASSIVE_MODE:
		return "SUB_STATE_PASSIVE_MODE";
	default:
		return "Unknown";
	}
}

static void state_set(enum state_type new_state)
{
	if (new_state == state) {
		LOG_DBG("State: %s", state2str(state));
		return;
	}

	LOG_DBG("State transition %s --> %s",
		state2str(state),
		state2str(new_state));

	state = new_state;
}

static void sub_state_set(enum sub_state_type new_state)
{
	if (new_state == sub_state) {
		LOG_DBG("Sub state: %s", sub_state2str(sub_state));
		return;
	}

	LOG_DBG("Sub state transition %s --> %s",
		sub_state2str(sub_state),
		sub_state2str(new_state));

	sub_state = new_state;
}

/* Check the return code from nRF modem library initialization to ensure that
 * the modem is rebooted if a modem firmware update is ready to be applied or
 * an error condition occurred during firmware update or library initialization.
 */
static void handle_nrf_modem_lib_init_ret(void)
{
#if defined(CONFIG_NRF_MODEM_LIB)
	int ret = modem_lib_init_result;

	/* Handle return values relating to modem firmware update */
	switch (ret) {
	case 0:
		/* Initialization successful, no action required. */
		return;
	case MODEM_DFU_RESULT_OK:
		LOG_DBG("MODEM UPDATE OK. Will run new modem firmware after reboot");
		break;
	case MODEM_DFU_RESULT_UUID_ERROR:
	case MODEM_DFU_RESULT_AUTH_ERROR:
		LOG_ERR("MODEM UPDATE ERROR %d. Will run old firmware", ret);
		break;
	case MODEM_DFU_RESULT_HARDWARE_ERROR:
	case MODEM_DFU_RESULT_INTERNAL_ERROR:
		LOG_ERR("MODEM UPDATE FATAL ERROR %d. Modem failure", ret);
		break;
	default:
		/* All non-zero return codes other than DFU result codes are
		 * considered irrecoverable and a reboot is needed.
		 */
		LOG_ERR("nRF modem lib initialization failed, error: %d", ret);
		break;
	}

#if defined(CONFIG_NRF_CLOUD_FOTA)
	/* Ignore return value, rebooting below */
	(void)nrf_cloud_fota_pending_job_validate(NULL);
#elif defined(CONFIG_LWM2M_INTEGRATION)
	lwm2m_verify_modem_fw_update();
#endif
	LOG_DBG("Rebooting...");
	LOG_PANIC();
	sys_reboot(SYS_REBOOT_COLD);
#endif /* CONFIG_NRF_MODEM_LIB */
}

/* Application Event Manager handler. Puts event data into messages and adds them to the
 * application message queue.
 */
static bool app_event_handler(const struct app_event_header *aeh)
{
	struct app_msg_data msg = {0};
	bool enqueue_msg = false;

	if (is_cloud_module_event(aeh)) {
		struct cloud_module_event *evt = cast_cloud_module_event(aeh);

		msg.module.cloud = *evt;
		enqueue_msg = true;
	}

	if (is_app_module_event(aeh)) {
		struct app_module_event *evt = cast_app_module_event(aeh);

		msg.module.app = *evt;
		enqueue_msg = true;
	}

	if (is_data_module_event(aeh)) {
		struct data_module_event *evt = cast_data_module_event(aeh);

		msg.module.data = *evt;
		enqueue_msg = true;
	}

	if (is_sensor_module_event(aeh)) {
		struct sensor_module_event *evt = cast_sensor_module_event(aeh);

		msg.module.sensor = *evt;
		enqueue_msg = true;
	}

	if (is_util_module_event(aeh)) {
		struct util_module_event *evt = cast_util_module_event(aeh);

		msg.module.util = *evt;
		enqueue_msg = true;
	}

	if (is_modem_module_event(aeh)) {
		struct modem_module_event *evt = cast_modem_module_event(aeh);

		msg.module.modem = *evt;
		enqueue_msg = true;
	}

    // added by jayant
    if (is_ui_module_event(aeh)) {
		struct ui_module_event *evt = cast_ui_module_event(aeh);

		msg.module.ui = *evt;
		enqueue_msg = true;
	}

	if (enqueue_msg) {
		int err = module_enqueue_msg(&self, &msg);

		if (err) {
			LOG_ERR("Message could not be enqueued");
			SEND_ERROR(app, APP_EVT_ERROR, err);
		}
	}

	return false;
}

static void data_sample_timer_handler(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	/* Cancel if a previous sample request has not completed or the device is not under
	 * activity in passive mode.
	 */
	if (sample_request_ongoing || ((sub_state == SUB_STATE_PASSIVE_MODE) && !activity)) {
		return;
	}

	SEND_EVENT(app, APP_EVT_DATA_GET_ALL);
}

/* Static module functions. */
static void passive_mode_timers_start_all(void)
{
	LOG_DBG("Device mode: Passive");
	LOG_DBG("Start movement timeout: %d seconds interval", app_cfg.movement_timeout);

	LOG_DBG("%d seconds until movement can trigger a new data sample/publication",
		app_cfg.movement_resolution);

	k_timer_start(&data_sample_timer,
		      K_SECONDS(app_cfg.movement_resolution),
		      K_SECONDS(app_cfg.movement_resolution));

	k_timer_start(&movement_resolution_timer,
		      K_SECONDS(app_cfg.movement_resolution),
		      K_SECONDS(0));

	k_timer_start(&movement_timeout_timer,
		      K_SECONDS(app_cfg.movement_timeout),
		      K_SECONDS(app_cfg.movement_timeout));
}

static void active_mode_timers_start_all(void)
{
	LOG_DBG("Device mode: Active");
	LOG_DBG("Start data sample timer: %d seconds interval", app_cfg.active_wait_timeout);

	k_timer_start(&data_sample_timer,
		      K_SECONDS(app_cfg.active_wait_timeout),
		      K_SECONDS(app_cfg.active_wait_timeout));

	k_timer_stop(&movement_resolution_timer);
	k_timer_stop(&movement_timeout_timer);
}

static void activity_event_handle(enum sensor_module_event_type sensor_event)
{
	__ASSERT(((sensor_event == SENSOR_EVT_MOVEMENT_ACTIVITY_DETECTED) ||
		  (sensor_event == SENSOR_EVT_MOVEMENT_INACTIVITY_DETECTED)),
		  "Unknown event");

	activity = (sensor_event == SENSOR_EVT_MOVEMENT_ACTIVITY_DETECTED) ? true : false;

	if (sample_request_ongoing) {
		LOG_DBG("Sample request ongoing, abort request.");
		return;
	}

	if ((sensor_event == SENSOR_EVT_MOVEMENT_ACTIVITY_DETECTED) &&
	    (k_timer_remaining_get(&movement_resolution_timer) != 0)) {
		LOG_DBG("Movement resolution timer has not expired, abort request.");
		return;
	}

	SEND_EVENT(app, APP_EVT_DATA_GET_ALL);
	passive_mode_timers_start_all();
}

static void data_get(void)
{
	struct app_module_event *app_module_event = new_app_module_event();

	__ASSERT(app_module_event, "Not enough heap left to allocate event");

	size_t count = 0;

	/* Set a low sample timeout. If location is requested, the sample timeout
	 * will be increased to accommodate the location request.
	 */
	app_module_event->timeout = DATA_FETCH_TIMEOUT_DEFAULT;

	/* Specify which data that is to be included in the transmission. */
	app_module_event->data_list[count++] = APP_DATA_MODEM_DYNAMIC;
	app_module_event->data_list[count++] = APP_DATA_BATTERY;
	app_module_event->data_list[count++] = APP_DATA_ENVIRONMENTAL;

	if (!modem_static_sampled) {
		app_module_event->data_list[count++] = APP_DATA_MODEM_STATIC;
	}

	if (!app_cfg.no_data.neighbor_cell || !app_cfg.no_data.gnss) {
		app_module_event->data_list[count++] = APP_DATA_LOCATION;

		/* Set application module timeout when location sampling is requested.
		 * This is selected to be long enough so that most of the GNSS would
		 * have enough time to run to get a fix. We also want it to be smaller than
		 * the sampling interval (120s). So, 110s was selected but we take
		 * minimum of sampling interval minus 5 (just some selected number) and 110.
		 * And mode (active or passive) is taken into account.
		 * If the timeout would become smaller than 5s, we want to ensure some time for
		 * the modules so the minimum value for application module timeout is 5s.
		 */
		app_module_event->timeout = (app_cfg.active_mode) ?
			MIN(app_cfg.active_wait_timeout - 5, 110) :
			MIN(app_cfg.movement_resolution - 5, 110);
		app_module_event->timeout = MAX(app_module_event->timeout, 5);
	}

	/* Set list count to number of data types passed in app_module_event. */
	app_module_event->count = count;
	app_module_event->type = APP_EVT_DATA_GET;

	APP_EVENT_SUBMIT(app_module_event);
}

/* Message handler for STATE_INIT. */
static void on_state_init(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_INIT)) {
		/* Keep a copy of the new configuration. */
		app_cfg = msg->module.data.data.cfg;

		if (app_cfg.active_mode) {
			active_mode_timers_start_all();
		} else {
			passive_mode_timers_start_all();
		}

		state_set(STATE_RUNNING);
		sub_state_set(app_cfg.active_mode ? SUB_STATE_ACTIVE_MODE :
						    SUB_STATE_PASSIVE_MODE);
	}
}

#define NVS_PARTITION_DEVICE DEVICE_DT_GET(DT_NODELABEL(flash_controller))
#define NVS_PARTITION_OFFSET PM_MY_NVS_STORAGE_ADDRESS
#define NVS_PARTITION_SIZE   PM_MY_NVS_STORAGE_SIZE

static struct nvs_fs fs;

void my_nvs_init()
{
    int rc = 0;
	struct flash_pages_info info;

    fs.flash_device = NVS_PARTITION_DEVICE;
    if(!device_is_ready(fs.flash_device)){
        LOG_ERR("flash device is not ready!");
        return;
    }
    fs.offset = NVS_PARTITION_OFFSET;
    rc = flash_get_page_info_by_offs(fs.flash_device, fs.offset, &info);
    if (rc) {
		LOG_ERR("Unable to get page info\n");
		return;
	}
    fs.sector_size = info.size;
    LOG_INF("partition_size = 0x%x, sector_size = 0x%x", NVS_PARTITION_SIZE, fs.sector_size);
	fs.sector_count = NVS_PARTITION_SIZE / fs.sector_size;

    rc = nvs_mount(&fs);
    if (rc) {
		LOG_ERR("Flash init failed\n");
		return;
	}
}

const struct spi_cs_control spi_cs = {
    .gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(spi1), cs_gpios),
    .delay = 0,
};

static struct spi_config spi_cfg = {
    .frequency = 125000,
    .operation = SPI_OP_MODE_MASTER | SPI_TRANSFER_MSB | SPI_WORD_SET(8),
    .slave = 0,
    .cs =  &spi_cs,
};

static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));

void my_spi_init()
{ 
    if(!device_is_ready(spi_dev)){
        LOG_ERR("spi device is not ready!");
        return;
    }
    enum pm_device_state state;
    pm_device_state_get(spi_dev, &state);
    LOG_INF("spi_dev pm status:%s", pm_device_state_str(state));
}

static const struct device *twi_dev = DEVICE_DT_GET(DT_NODELABEL(i2c2));

#define I2C_ADDR 0xbeef

void my_twi_init()
{
    if(!device_is_ready(twi_dev)){
        LOG_ERR("twi device is not ready!");
        return;
    }
    enum pm_device_state state;
    pm_device_state_get(twi_dev, &state);
    LOG_INF("spi_dev pm status:%s", pm_device_state_str(state));    
}

enum custom_cmd_t {
    FLASH_WRITE = 0x01,
    FLASH_READ = 0x02,
    SPIM_WRITE = 0x03,
    SPIM_READ = 0x04,
    TWI_WRITE = 0x05,
    TWI_READ = 0x06,
};

#define NVS_CUSTOM_CLOUD_DATA_ID 0x01

/* 自定义云端命令处理函数*/
static void on_cloud_custom_cmd(struct app_msg_data *msg)
{
    struct cloud_module_custom_cmd *command = &(msg->module.cloud.data.custom_cmd);

    uint8_t cmd = command->ptr[0];
    uint8_t len = command->ptr[1];
    uint8_t *data = command->ptr + 2;

    switch(cmd) {
    case FLASH_WRITE:
    case SPIM_WRITE:
    case TWI_WRITE:
        if (len != command->len - 2) {
            LOG_WRN("write len is not match! paylod_len=%d, len=%d", command->len - 2, len);
        }
        break;
    }

    switch(cmd) {
    case FLASH_WRITE:{
        LOG_HEXDUMP_INF(data, len, "flash write: ");
        int rc = nvs_write(&fs, NVS_CUSTOM_CLOUD_DATA_ID, data, len);
        if (rc >= 0){
            LOG_INF("write flash success!");
        } else {
            LOG_INF("write flash failed!, rc=%d", rc);
        }
        goto free_ptr;
    }
    
    case FLASH_READ:{
        struct app_module_event *evt = new_app_module_event();
        if (evt == NULL) {
            LOG_ERR("Failed to allocate memory for event");
            goto free_ptr;
        }

        if( len <= 0 ) {
            LOG_WRN("read flash len is less than 0!");
            evt->data.custom_cmd.buf = NULL;
            evt->data.custom_cmd.len = 0;
            evt->data.custom_cmd.is_allocated = false;
        } else {
            uint8_t *buf = k_malloc(len);
            if ( buf == NULL){
                LOG_WRN("read flash malloc failed!");
                goto free_ptr;
            }
            int rc = nvs_read(&fs, NVS_CUSTOM_CLOUD_DATA_ID, buf, len);
            if (rc < 0) {
                LOG_WRN("read flash failed!, rc = %d", rc);
                k_free(buf);
                evt->data.custom_cmd.buf = NULL;
                evt->data.custom_cmd.len = 0;
                evt->data.custom_cmd.is_allocated = false;
            } else {
                LOG_HEXDUMP_INF(buf, len, "flash read success:");
                evt->data.custom_cmd.buf = buf;
                evt->data.custom_cmd.len = len;
                evt->data.custom_cmd.is_allocated = true;
            }
        }
        evt->type = APP_EVT_CUSTOM_CLOUD_CMD_READY;
        APP_EVENT_SUBMIT(evt);

        if(connected_3rd_party) {
            struct app_module_event *evt2 = new_app_module_event();
            uint8_t *buf = k_malloc(len);
            if ( buf == NULL){
                LOG_WRN("read flash malloc failed!");
                goto free_ptr;
            }
            memcpy(buf, evt->data.custom_cmd.buf, len);
            evt2->data.custom_cmd.buf = buf;
            evt2->data.custom_cmd.len = len;
            evt2->data.custom_cmd.is_allocated = true;
            evt2->type = APP_EVT_SEND_TO_WILLIAMS_SERVER;
            APP_EVENT_SUBMIT(evt2);
        }

        goto free_ptr;
    }
    case SPIM_WRITE:{
        LOG_HEXDUMP_INF(data, len, "spim write: ");
        // 直接把完整指令+数据发送到从机，由从机处理写入或读出
        struct spi_buf spi_buffer = {
            .buf = command->ptr,
            .len = command->len,
        };
        struct spi_buf_set tx = {
            .buffers = &spi_buffer,
            .count = 1,
        };
        int rc = spi_write(spi_dev, &spi_cfg, &tx);
        if (rc >= 0){
            LOG_INF("write spi success!");
        } else {
            LOG_INF("write spi failed!, rc=%d", rc);
        }
        goto free_ptr;
    }
    case SPIM_READ:{
        struct app_module_event *evt = new_app_module_event();
        if (evt == NULL) {
            LOG_ERR("Failed to allocate memory for event");
            goto free_ptr;
        }
        if( len <= 0 ) {
            LOG_WRN("read spi len is less than 0!");
            evt->data.custom_cmd.buf = NULL;
            evt->data.custom_cmd.len = 0;
            evt->data.custom_cmd.is_allocated = false;
        } else {
             // 直接把完整指令+数据发送到从机，由从机处理写入或读出
            struct spi_buf spi_buffer = {
                .buf = command->ptr,
                .len = command->len,
            };
            struct spi_buf_set txrx = {
                .buffers = &spi_buffer,
                .count = 1,
            };
            spi_write(spi_dev, &spi_cfg, &txrx);
            k_sleep(K_MSEC(1000));
            
            spi_buffer.len = len; // 读取数据长度
            int rc = spi_read(spi_dev, &spi_cfg, &txrx);
            if ((rc < 0) ) {
                LOG_INF("read spi failed!, rc=%d", rc);
                evt->data.custom_cmd.buf = NULL;
                evt->data.custom_cmd.len = 0;
                evt->data.custom_cmd.is_allocated = false;
            } else {
                LOG_HEXDUMP_INF(spi_buffer.buf, spi_buffer.len, "spi read success:");
                evt->data.custom_cmd.buf = k_malloc(len);
                if ( evt->data.custom_cmd.buf == NULL){
                    LOG_WRN("read flash malloc failed!");
                    goto free_ptr;
                }
                memcpy(evt->data.custom_cmd.buf, (uint8_t*)(spi_buffer.buf), len);
                evt->data.custom_cmd.len = len;
                evt->data.custom_cmd.is_allocated = true;
            }
            evt->type = APP_EVT_CUSTOM_CLOUD_CMD_READY;
            APP_EVENT_SUBMIT(evt);

            if(connected_3rd_party) {
                struct app_module_event *evt2 = new_app_module_event();
                uint8_t *buf = k_malloc(len);
                if ( buf == NULL){
                    LOG_WRN("read flash malloc failed!");
                    goto free_ptr;
                }
                memcpy(buf, evt->data.custom_cmd.buf, len);
                evt2->data.custom_cmd.buf = buf;
                evt2->data.custom_cmd.len = len;
                evt2->data.custom_cmd.is_allocated = true;
                evt2->type = APP_EVT_SEND_TO_WILLIAMS_SERVER;
                APP_EVENT_SUBMIT(evt2);
            }

            goto free_ptr; 
        }
        goto free_ptr; 
    }
    case TWI_WRITE:{
        LOG_HEXDUMP_INF(data, len, "twi write: ");
        int rc = i2c_write(twi_dev, data, len, I2C_ADDR);
        if (rc >= 0){
            LOG_INF("write i2c success!");
        } else {
            LOG_INF("write i2c failed!, rc=%d", rc);
        }
        goto free_ptr; 
    }
    case TWI_READ:{
        struct app_module_event *evt = new_app_module_event();
        if (evt == NULL) {
            LOG_ERR("Failed to allocate memory for event");
            goto free_ptr;
        }
        if( len <= 0 ) {
            LOG_WRN("read i2c len is less than 0!");
            evt->data.custom_cmd.buf = NULL;
            evt->data.custom_cmd.len = 0;
            evt->data.custom_cmd.is_allocated = false;
        } else {
            uint8_t* buf = k_malloc(len);
            if ( buf == NULL){
                LOG_WRN("read flash malloc failed!");
                goto free_ptr;
            }
            int rc = i2c_read(twi_dev, buf, len, I2C_ADDR);
            if (rc < 0) {
                LOG_WRN("read i2c failed!, rc=%d", rc);
                k_free(buf);
                evt->data.custom_cmd.buf = NULL;
                evt->data.custom_cmd.len = 0;
                evt->data.custom_cmd.is_allocated = false;
            } else {
                LOG_HEXDUMP_INF(buf, len, "i2c read success:");
                evt->data.custom_cmd.buf = buf;
                evt->data.custom_cmd.len = len;
                evt->data.custom_cmd.is_allocated = true;
            }
        }
        evt->type = APP_EVT_CUSTOM_CLOUD_CMD_READY;
        APP_EVENT_SUBMIT(evt);

        if(connected_3rd_party) {
            struct app_module_event *evt2 = new_app_module_event();
            uint8_t *buf = k_malloc(len);
            if ( buf == NULL){
                LOG_WRN("read flash malloc failed!");
                goto free_ptr;
            }
            memcpy(buf, evt->data.custom_cmd.buf, len);
            evt2->data.custom_cmd.buf = buf;
            evt2->data.custom_cmd.len = len;
            evt2->data.custom_cmd.is_allocated = true;
            evt2->type = APP_EVT_SEND_TO_WILLIAMS_SERVER;
            APP_EVENT_SUBMIT(evt2);
        }

        goto free_ptr; 
    }
    default:
        goto free_ptr; 
    }

free_ptr:         
    if(command->is_allocated){
        k_free(command->ptr);
    }
}

// added by jayant
#include <zephyr/dfu/mcuboot.h>
#include <dfu/dfu_target_mcuboot.h>
#include <net/fota_download.h>

static struct k_work fota_work;
static bool fota_lock = false;

#define SEG_TAG -1

static void fota_dl_handler(const struct fota_download_evt *evt)
{
	switch (evt->id) {
	case FOTA_DOWNLOAD_EVT_ERROR:
		LOG_ERR("Received error from fota_download\n");
		fota_lock = false;
		break;

	case FOTA_DOWNLOAD_EVT_FINISHED:
		LOG_INF("Press 'Reset' button to apply new firmware\n");
        fota_lock = false;
		break;

	default:
		break;
	}
}

static void fota_work_cb(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = fota_download_start("182.61.144.86:50002", "app_update_jayant.bin", SEG_TAG, 0, 0);
	if (err != 0) {
		LOG_ERR("fota_download_start() failed, err %d\n", err);
	}
}

static void recv_packet(void)
{
    int received_len;
    int buf[20] = {0};
	while (1) {
        LOG_INF("Waiting for packets ...");
		received_len = recv(client_fd, buf, sizeof(buf), 0);
		if (received_len < 0) {
            LOG_ERR("Failed to recv packets");
			return;
		}
        LOG_HEXDUMP_INF(buf, received_len, "Received from William's Server:");

        struct cloud_module_event *cloud_evt = new_cloud_module_event(); 
        __ASSERT(cloud_evt, "Not enough heap left to allocate event");
        cloud_evt->data.custom_cmd.ptr = k_malloc(received_len);
        if (NULL == cloud_evt->data.custom_cmd.ptr) {
            LOG_WRN("k_malloc failed");
            return;
        }
        memcpy(cloud_evt->data.custom_cmd.ptr, buf, received_len);
        cloud_evt->data.custom_cmd.len = received_len;
        cloud_evt->data.custom_cmd.is_allocated = true; // 用于指示此内存是需要释放的

        cloud_evt->type = CLOUD_EVT_CUSTOM_CMD;
        APP_EVENT_SUBMIT(cloud_evt);
	}
}

K_THREAD_DEFINE(receiver_thread_id, 1024,
		recv_packet, NULL, NULL, NULL,
		K_PRIO_PREEMPT(8), 0, -1);

// FOTA triggered by button 1
// UDP socket opened by button 2
static void on_my_button_pressed(struct app_msg_data *msg)
{
    struct ui_module_data *evt_data = &(msg->module.ui.data.ui);
    
    // refuse button event when already in FOTA progress
    if (fota_lock) {
        LOG_INF("FOTA is in progress, please wait!");
        return;
    }
    
    // Button 1
    if(evt_data->button_number == 1) {
        // start FOTA
        fota_lock = true;
        int err = fota_download_init(fota_dl_handler);
        if (err != 0) {
            LOG_ERR("fota_download_init() failed, err %d\n", err);
            fota_lock = false;
            return;
        }
        
        // add a new thread to start fota,
        // prevent blocking the main thread
        k_work_init(&fota_work, fota_work_cb);
        k_work_submit(&fota_work);

    } 
    
    // Button 2; enable socket
    else if (evt_data->button_number == 2) {

        static bool first_press = true;
        if(!first_press){
            LOG_INF("Press Button 2 more than once is not supported, Socket already opened!");
            return;
        }
        first_press = false;

        struct sockaddr_in *server4 = ((struct sockaddr_in *)&host_addr);

#define USE_UDP 1 // set 0 to use tcp

        server4->sin_family = AF_INET;
        server4->sin_port = htons( USE_UDP ? 50001 : 50000);

        inet_pton(AF_INET, "182.61.144.86",&server4->sin_addr);

#ifdef USE_UDP
        client_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#else
        client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif

        if (client_fd < 0) {
            LOG_ERR("Failed to create %s socket", USE_UDP ? "UDP" : "TCP");
            return;
	    }
        
        int err = connect(client_fd, (struct sockaddr *)&host_addr,
		      sizeof(struct sockaddr_in));
        if (err < 0) {
		    LOG_ERR("Connect to server failed : %d\n", errno);
            return;
	    }

        connected_3rd_party = true;

        send(client_fd, "Hello from Jayant\n", sizeof("Hello from Jayant\n"), 0);
        LOG_INF("Send Hello from Jayant to William's Server");
        k_thread_start(receiver_thread_id);

    }
}


/* Message handler for STATE_RUNNING. */
static void on_state_running(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, cloud, CLOUD_EVT_CONNECTED)) {
		data_get();
	}

	if (IS_EVENT(msg, app, APP_EVT_DATA_GET_ALL)) {
		data_get();
	}

    // custom mqtt cmd handler
    if(IS_EVENT(msg, cloud, CLOUD_EVT_CUSTOM_CMD)) {
        on_cloud_custom_cmd(msg);
    }

    // custom fota handler, triggerd by button
    if(IS_EVENT(msg, ui, UI_EVT_BUTTON_DATA_READY)){
        on_my_button_pressed(msg);
    }

    // send data to william's server
    if (IS_EVENT(msg, app, APP_EVT_SEND_TO_WILLIAMS_SERVER)) {
        struct app_module_custom_cloud_cmd_data *cmd = &(msg->module.app.data.custom_cmd);
        send(client_fd, cmd->buf, cmd->len, 0);
        if (cmd->is_allocated) {
             k_free(cmd->buf);
        }
    }
}

/* Message handler for SUB_STATE_PASSIVE_MODE. */
void on_sub_state_passive(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {
		/* Keep a copy of the new configuration. */
		app_cfg = msg->module.data.data.cfg;

		if (app_cfg.active_mode) {
			active_mode_timers_start_all();
			sub_state_set(SUB_STATE_ACTIVE_MODE);
			return;
		}

		passive_mode_timers_start_all();
	}

	if ((IS_EVENT(msg, sensor, SENSOR_EVT_MOVEMENT_ACTIVITY_DETECTED)) ||
	    (IS_EVENT(msg, sensor, SENSOR_EVT_MOVEMENT_INACTIVITY_DETECTED))) {
		activity_event_handle(msg->module.sensor.type);
	}
}

/* Message handler for SUB_STATE_ACTIVE_MODE. */
static void on_sub_state_active(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, data, DATA_EVT_CONFIG_READY)) {
		/* Keep a copy of the new configuration. */
		app_cfg = msg->module.data.data.cfg;

		if (!app_cfg.active_mode) {
			passive_mode_timers_start_all();
			sub_state_set(SUB_STATE_PASSIVE_MODE);
			return;
		}

		active_mode_timers_start_all();
	}
}

/* Message handler for all states. */
static void on_all_events(struct app_msg_data *msg)
{
	if (IS_EVENT(msg, util, UTIL_EVT_SHUTDOWN_REQUEST)) {
		k_timer_stop(&data_sample_timer);
		k_timer_stop(&movement_timeout_timer);
		k_timer_stop(&movement_resolution_timer);

		SEND_SHUTDOWN_ACK(app, APP_EVT_SHUTDOWN_READY, self.id);
		state_set(STATE_SHUTDOWN);
	}

	if (IS_EVENT(msg, modem, MODEM_EVT_MODEM_STATIC_DATA_READY)) {
		modem_static_sampled = true;
	}

	if (IS_EVENT(msg, app, APP_EVT_DATA_GET)) {
		sample_request_ongoing = true;
	}

    if (IS_EVENT(msg, app, APP_EVT_DATA_GET)) {
		for (int i = 0; i < msg->module.app.count; i++){
            if (APP_DATA_BATTERY == msg->module.app.data_list[i]) {
                
                const struct voltage_sensor_api *api = (const struct voltage_sensor_api*)voltage_sensor->api;
                int32_t volt_vdd = api->voltage_get();
                
                if (-1 == volt_vdd) {
                    SEND_EVENT(app, APP_EVT_BATTERY_DATA_NOT_READY);
                } else {
                    struct app_module_event *evt = new_app_module_event();
                    __ASSERT(evt, "Not enough heap left to allocate event");
                    evt->type = APP_EVT_BATTERY_DATA_READY;
                    evt->data.bat.vdd_mv = volt_vdd;
                    evt->data.bat.timestamp = k_uptime_get();
                    APP_EVENT_SUBMIT(evt);
                }
            }
        }
	}

	if (IS_EVENT(msg, data, DATA_EVT_DATA_READY)) {
		sample_request_ongoing = false;
	}

	if (IS_EVENT(msg, sensor, SENSOR_EVT_MOVEMENT_IMPACT_DETECTED)) {
		SEND_EVENT(app, APP_EVT_DATA_GET_ALL);
	}
}

void main(void)
{
	int err;
	struct app_msg_data msg = { 0 };
	if (!IS_ENABLED(CONFIG_LWM2M_CARRIER)) {
		handle_nrf_modem_lib_init_ret();
	}

	if (app_event_manager_init()) {
		/* Without the Application Event Manager, the application will not work
		 * as intended. A reboot is required in an attempt to recover.
		 */
		LOG_ERR("Application Event Manager could not be initialized, rebooting...");
		k_sleep(K_SECONDS(5));
		sys_reboot(SYS_REBOOT_COLD);
	} else {
		module_set_state(MODULE_STATE_READY);
		SEND_EVENT(app, APP_EVT_START);
	}

	self.thread_id = k_current_get();

    my_nvs_init();
    my_spi_init();
    my_twi_init();

	err = module_start(&self);
	if (err) {
		LOG_ERR("Failed starting module, error: %d", err);
		SEND_ERROR(app, APP_EVT_ERROR, err);
	}

	while (true) {
		module_get_next_msg(&self, &msg);

		switch (state) {
		case STATE_INIT:
			on_state_init(&msg);
			break;
		case STATE_RUNNING:
			switch (sub_state) {
			case SUB_STATE_ACTIVE_MODE:
				on_sub_state_active(&msg);
				break;
			case SUB_STATE_PASSIVE_MODE:
				on_sub_state_passive(&msg);
				break;
			default:
				LOG_ERR("Unknown sub state");
				break;
			}

			on_state_running(&msg);
			break;
		case STATE_SHUTDOWN:
			/* The shutdown state has no transition. */
			break;
		default:
			LOG_ERR("Unknown state");
			break;
		}

		on_all_events(&msg);
	}
}

APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE_EARLY(MODULE, cloud_module_event);
APP_EVENT_SUBSCRIBE(MODULE, app_module_event);
APP_EVENT_SUBSCRIBE(MODULE, data_module_event);
APP_EVENT_SUBSCRIBE(MODULE, util_module_event);
APP_EVENT_SUBSCRIBE(MODULE, ui_module_event);
APP_EVENT_SUBSCRIBE_FINAL(MODULE, sensor_module_event);
APP_EVENT_SUBSCRIBE_FINAL(MODULE, modem_module_event);
