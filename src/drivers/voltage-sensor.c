#include "voltage-sensor.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>

#include <zephyr/sys/util.h>

// 判断电压表device tree node是否存在
#if !DT_NODE_EXISTS(DT_PATH(my_voltage_sensor))|| \
    !DT_NODE_HAS_PROP(DT_PATH(my_voltage_sensor), io_channels)
#error "vdd sensor does not exist!"
#endif

// 使用Kernel提供的日志功能
//#define LOG_LEVEL 
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
LOG_MODULE_REGISTER(voltage_sensor,CONFIG_JAYANT_VOLTAGE_SENSOR_LOG_LEVEL);

// 获得 ADC io-channel 句柄
static const struct adc_dt_spec adc_channel_vdd = \
                ADC_DT_SPEC_GET_BY_IDX(DT_PATH(my_voltage_sensor), 0);

/**
 * @brief init voltage sensor
 */
static inline int voltage_sensor_setup()
{   
    // 判断adc controller是否已经初始化
    if (!device_is_ready(adc_channel_vdd.dev)) {
        LOG_ERR("ADC controller device [%s] is not ready", adc_channel_vdd.dev->name);
        return -1;
    }
     
    // 配置channel
    if (0 > adc_channel_setup_dt(&adc_channel_vdd)) {
        LOG_ERR("Could not setup channel [%d] for [%s]", adc_channel_vdd.channel_id, adc_channel_vdd.dev->name);
        return -1;
    }

    LOG_INF("Init success.");
    return 0;
}

/**
 * @brief start ADC sampling
 */
static inline int32_t voltage_sensor_sampling()
{   
    int16_t buf;

    LOG_INF("Start sampling.");

    // 配置一次采样序列
	struct adc_sequence sequence = {
		.buffer = &buf,
		.buffer_size = sizeof(buf), // buf单元的大小
	};
    (void)adc_sequence_init_dt(&adc_channel_vdd, &sequence);

    // 执行采样序列
    int err = adc_read(adc_channel_vdd.dev, &sequence);
    if (err < 0) {
        LOG_ERR("Could not read ADC Channel, reason: (%d)\n", err);
        return -1;
    } 

    // 自动根据device tree中配置的基准电压进行转换
    int32_t volt_mv = buf;
    err = adc_raw_to_millivolts_dt(&adc_channel_vdd, &volt_mv);
    if (err < 0) {
        LOG_ERR("ADC raw value in mV is not available");
        return -1;
    }

    LOG_INF("Sampling Finished.");

    return volt_mv;
}

static const struct voltage_sensor_api voltage_sensor_api = {
    .voltage_get = voltage_sensor_sampling,
};

DEVICE_DT_DEFINE(DT_PATH(my_voltage_sensor),         \
                voltage_sensor_setup,                \
                NULL,                                \
                NULL,                                \
                NULL,                                \
                POST_KERNEL,                         \
                CONFIG_JAYANT_VOLTAGE_SENSOR_INIT_PRIORITY,\
                &voltage_sensor_api);
