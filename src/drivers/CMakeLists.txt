target_include_directories(app PRIVATE .)
target_sources_ifdef(CONFIG_JAYANT_VOLTAGE_SENSOR app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/voltage-sensor.c)
