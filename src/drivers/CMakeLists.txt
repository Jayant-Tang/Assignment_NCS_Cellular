zephyr_include_directories(.)
zephyr_library()
zephyr_library_sources_ifdef(CONFIG_JAYANT_VOLTAGE_SENSOR  voltage-sensor.c)
zephyr_library_sources_ifdef(CONFIG_JAYANT_BLINK_LED       blink-led.c)