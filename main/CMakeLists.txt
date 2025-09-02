set(DEVICE_ID "falldetector" CACHE STRING "Device ID for this build")
set(CERT_FILE "dev.crt" CACHE STRING "Path to the certificate file")

idf_component_register(
    SRCS "main.c" "camera_app.c" "mqtt_app.c"
    INCLUDE_DIRS "."
    REQUIRES wifiManager esp32-camera
    PRIV_REQUIRES json mqtt esp_https_ota spi_flash esp_netif lwip
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_ID=\"${DEVICE_ID}\")

if(NOT DEFINED CERT_FILE)
    set(CERT_FILE "dev.crt")
endif()

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CERT_FILE}")
    message(FATAL_ERROR "Certificate file not found: ${CMAKE_CURRENT_SOURCE_DIR}/${CERT_FILE}")
endif()

target_add_binary_data(${COMPONENT_TARGET} "${CERT_FILE}" TEXT)
