cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ot_node_v6)

target_include_directories(app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/sensors
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_sources(app PRIVATE
    src/main.c
    src/app_core.c
    src/app_antenna.c
    src/thread_mgr.c
    src/util/topic.c
    src/transport/transport.c
    src/transport/coap_client.c
)

if(CONFIG_APP_USE_SHT41_SENSOR)
    target_sources(app PRIVATE src/sensors/sht41.c)
endif()

if(CONFIG_APP_USE_SCD41_SENSOR)
    target_sources(app PRIVATE src/sensors/scd41.c)
endif()