#ifndef _VOLTAGE_SENSOR
#define _VOLTAGE_SENSOR

#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <stdint.h>

__subsystem struct voltage_sensor_api {
    int32_t (*voltage_get)(void);
};


#endif