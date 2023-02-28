#ifndef _BLINK_LEDS
#define _BLINK_LEDS

#include <zephyr/device.h>

#include <stdint.h>

__subsystem struct blink_led_api {
	int (*start)(const struct device*, uint32_t idx);
    int (*stop)(const struct device*, uint32_t idx);
    int (*start_all)(const struct device*);
    int (*stop_all)(const struct device*);
};

#endif // !_BLINK_LEDS
