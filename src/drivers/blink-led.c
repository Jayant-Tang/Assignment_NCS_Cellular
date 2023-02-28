#include "blink-led.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/counter.h>

// zephyr驱动未提供的nordic独有功能，需要使用nrfxlib实现
#include <hal/nrf_timer.h>

#include <debug/ppi_trace.h>
#include <helpers/nrfx_gppi.h>
#if defined(DPPI_PRESENT)
#include <nrfx_dppi.h>
#else
#include <nrfx_ppi.h>
#endif

#include <nrfx_gpiote.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(jayant_blink_led, 3);

// 在驱动文件内，定义 `DT_DRV_COMPAT` 宏，就可开启实例编号（Instance）功能。
// 
// 宏的内容必须与dts中某个节点的compatible相同（字母变为小写、特殊符号变为下划线）
//
// 定义此功能后，会使dvice tree中所有符合此compatible的设备节点都被编入一个列表，
// 之后在驱动代码里，就可以方便地用编号获取节点的node id。而不需要节点的别名或完整路径
//
#define DT_DRV_COMPAT jayant_led_blink // compatible = "jayant,led-blink"

// 本驱动程序所维护的一个blink-leds设备的config结构体
struct blink_led_config {
    // gpios
	size_t num_leds;
    const struct gpio_dt_spec *led;

    // timer
    const struct device *dev_timer;
    int blink_period;
    NRF_TIMER_Type *timer_base;

    // ppi
    const struct device *dev_dppi;
};


/**
 * @brief 初始化函数 
 */
static int blink_led_init(const struct device *dev)
{
    const struct blink_led_config *config = (struct blink_led_config*)(dev->config);
    int err = 0;

    // 配置GPIO
    // zephyr驱动只能初始化GPIO输入输出，或中断输入。无法配置PPI输出
    // 故在device tree中只配置普通gpio，zephyr gpio驱动会自动将GPIO初始化
    // 然后此处再用nrfx gpiote库为其赋能PPI输出能力
    // 此外，zephyr gpio驱动会初始化gpiote外设本身，此处无需再次初始化
 	if (!config->num_leds) {
		LOG_ERR("%s: no LEDs found (DT child nodes missing)", dev->name);
		return -ENODEV;
	}
	for (size_t i = 0; i < config->num_leds; i++) {
		const struct gpio_dt_spec *led = &config->led[i];

		if (!device_is_ready(led->port)) {
            LOG_ERR("%s: GPIO device not ready", dev->name);
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(led, GPIO_OUTPUT);
        if (err) {
            LOG_ERR("Cannot configure GPIO (err %d)", err);
        }
        // 此处开始使用nrfx api
        // zephyr驱动中已经调用nrfx_gpiote_init(0)，无需再次调用
        uint8_t led_channel;
        err = nrfx_gpiote_channel_alloc(&led_channel);
        if (err != NRFX_SUCCESS) {
            LOG_ERR("Failed to allocate in_channel, error: 0x%08X", err);
            return err;
        }
        static const nrfx_gpiote_output_config_t output_config = {
            .drive = NRF_GPIO_PIN_S0S1, // 标准驱动力
            .input_connect = NRF_GPIO_PIN_INPUT_DISCONNECT,
            .pull = NRF_GPIO_PIN_NOPULL,
        };
        const nrfx_gpiote_task_config_t task_config = {
            .task_ch = led_channel,
            .polarity = NRF_GPIOTE_POLARITY_TOGGLE,
            .init_val = 1,
        };
        err = nrfx_gpiote_output_configure(led->pin,
                    &output_config,
                    &task_config);
        if (err != NRFX_SUCCESS) {
            LOG_ERR("nrfx_gpiote_output_configure error: 0x%08X", err);
            return err;
        }
        nrfx_gpiote_out_task_enable(led->pin);
        nrfx_gpiote_clr_task_trigger(led->pin);
	}

    // 配置timer
    const struct device *timer_dev = config->dev_timer;
    NRF_TIMER_Type *timer_base = (NRF_TIMER_Type *)config->timer_base;
    if(device_is_ready(timer_dev)){

        // 由于prj.conf中启用了Timer0，所以timer0已经被counter驱动初始化完成
        // 直接使用counter驱动即可
        struct counter_alarm_cfg alarm_cfg = {
            .flags = 0,
            .ticks = counter_us_to_ticks(timer_dev, 1000 * config->blink_period / 2), // 用toggle模式，周期变一半
            .callback = NULL,
            .user_data = &alarm_cfg
        };
        err = counter_set_channel_alarm(timer_dev, 0, &alarm_cfg); // 此处channel id为0，底层驱动转为Compare时ID+2
        
        // zephyr counter驱动不包含nordic nrf short寄存器设置，
        // 故此处需要使用nrfx API来打补丁，启用short寄存器
        // 连接channel 0 compare event 和 定时器 clear task
        // Note
        uint32_t short_bit_mask = nrf_timer_short_compare_clear_get(2);
        nrf_timer_shorts_enable(timer_base, short_bit_mask); // CC为2

    } else {
        LOG_ERR("%s: Timer[%s] device not ready", dev->name, timer_dev->name);
		err = -ENODEV;
    }

    // 配置PPI
    // Zephyr没有ppi驱动，故只能使用nrfx驱动
    uint8_t ppi_channel;
    err = nrfx_gppi_channel_alloc(&ppi_channel);
	if (err != NRFX_SUCCESS) {
		LOG_ERR("nrfx_gppi_channel_alloc error: 0x%08X", err);
		return err;
	}
    // 配置PPI event端点
    nrfx_gppi_event_endpoint_setup(ppi_channel,
        nrf_timer_event_address_get(timer_base,NRF_TIMER_EVENT_COMPARE2)); // channl0是Compare2
    // 配置PPI task端点
    // 9160是分布式PPI，可分配多个端点
    for (size_t i = 0; i < config->num_leds; i++) {
        nrfx_gppi_task_endpoint_setup(ppi_channel,
            nrfx_gpiote_out_task_addr_get(config->led[i].pin));
    }
    nrfx_gppi_channels_enable(BIT(ppi_channel));

	return err;
}

static int blink_start_all(const struct device *dev)
{
    const struct blink_led_config *config = (struct blink_led_config*)(dev->config);
    const struct device *timer_dev = config->dev_timer;

    for (size_t i = 0; i < config->num_leds; i++) {
		const struct gpio_dt_spec *led = &config->led[i];
        nrfx_gpiote_out_task_enable(led->pin);
    }
    counter_start(timer_dev);
    return 0;
}

static int blink_stop_all(const struct device *dev)
{
    const struct blink_led_config *config = (struct blink_led_config*)(dev->config);
    const struct device *timer_dev = config->dev_timer;

    counter_stop(timer_dev);
    
    for (size_t i = 0; i < config->num_leds; i++) {
		const struct gpio_dt_spec *led = &config->led[i];
        nrfx_gpiote_out_task_disable(led->pin);
        nrfx_gpiote_clr_task_trigger(led->pin);
    }
    return 0;
}

static int blink_start(const struct device *dev, uint32_t idx)
{
    const struct blink_led_config *config = (struct blink_led_config*)(dev->config);
    const struct device *timer_dev = config->dev_timer;
    const struct gpio_dt_spec *led = &config->led[idx];
    nrfx_gpiote_out_task_enable(led->pin);
    counter_start(timer_dev);
    return 0;
}

static int blink_stop(const struct device *dev, uint32_t idx)
{
    const struct blink_led_config *config = (struct blink_led_config*)(dev->config);
    const struct device *timer_dev = config->dev_timer;
    const struct gpio_dt_spec *led = &config->led[idx];
    counter_stop(timer_dev);
    nrfx_gpiote_out_task_disable(led->pin);
    nrfx_gpiote_clr_task_trigger(led->pin);
    return 0;
}

static const struct blink_led_api blink_api= {
    .start_all = blink_start_all,
    .stop_all = blink_stop_all,
    .start = blink_start,
    .stop = blink_stop,
};                                                                                      

// 代码模板，用于批量定义device结构体并赋值config
//
// 此模板中，指针指向了counter的device结构体
// 因此，必须要在prj.conf中启用它的驱动，并且deviceTree中status="okay"
// 这个结构体才会被定义
// 然后此处的赋值才能成立
// 否则link将会失败 
//
// 一个反例是，ppi虽然在dts中有定义，但它不是zephyr标准设备
// 所以zephyr/driver中并不存在给ppi创建device结构体实例的代码
// 所以此处无论如何都无法用 DEVICE_DT_GET() 来获取到ppi的 device 结构体
#define BLINK_LED_DEVICE(i)                                                     \
                                                                                \
static const struct gpio_dt_spec gpios_dt_spec_##i[] = {		                \
    DT_INST_FOREACH_CHILD_SEP_VARGS(i, GPIO_DT_SPEC_GET, (,), my_gpios) \
};                                                                              \
                                                                                \
static const struct blink_led_config blink_led_config_##i = {                   \
    .num_leds	= ARRAY_SIZE(gpios_dt_spec_##i),                               \
    .led = gpios_dt_spec_##i,                                                  \
    .blink_period = DT_PROP(DT_DRV_INST(i), blink_period),                      \
    .dev_timer = DEVICE_DT_GET(DT_PHANDLE_BY_IDX(DT_DRV_INST(i), timer, 0)),    \
    .timer_base = (NRF_TIMER_Type *)                                            \
                DT_REG_ADDR(DT_PHANDLE_BY_IDX(DT_DRV_INST(i), timer,0)),        \
};                                                                              \
                                                                                \
DEVICE_DT_INST_DEFINE(i, &blink_led_init, NULL,			                        \
		      NULL, &blink_led_config_##i,		                                \
		      POST_KERNEL, CONFIG_JAYANT_BLINK_LED_PRIORITY,	                \
		      &blink_api);                               
                            
// 利用代码模板，批量真正定义device结构体，并把配置赋值给device
DT_INST_FOREACH_STATUS_OKAY(BLINK_LED_DEVICE)