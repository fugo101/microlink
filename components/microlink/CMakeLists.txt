set(SRCS
    "src/microlink.c"
    "src/ml_coord.c"
    "src/ml_derp.c"
    "src/ml_net_io.c"
    "src/ml_wg_mgr.c"
    "src/ml_stun.c"
    "src/ml_noise.c"
    "src/ml_h2.c"
    "src/ml_udp.c"
    "src/ml_tcp.c"
    "src/ml_peer_nvs.c"
    "src/nacl_box.c"
    "src/x25519.c"
    "src/ml_zerocopy.c"
    "src/ml_net_switch.c"
    "src/ml_config_httpd.c"
)

if(CONFIG_ML_ENABLE_CELLULAR)
    list(APPEND SRCS
        "src/ml_cellular.c"
        "src/ml_at_socket.c"
    )
endif()

set(REQUIRES
    nvs_flash
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    lwip
    mbedtls
    espressif__cjson
    driver
    esp_driver_tsens
    esp_http_server
    # ml_cellular.c / ml_at_socket.c include driver/uart.h and driver/gpio.h. On
    # ESP-IDF 6.x those headers live in esp_driver_uart / esp_driver_gpio, not in
    # the umbrella `driver` component above -- `driver` only PRIV_REQUIRES
    # esp_driver_gpio (private, doesn't propagate) and doesn't require
    # esp_driver_uart at all. Unconditional, not gated on CONFIG_ML_ENABLE_CELLULAR:
    # REQUIRES/PRIV_REQUIRES are resolved during ESP-IDF's component-requirements
    # expansion pass, which runs before Kconfig is available -- REQUIRES can never
    # be conditioned on a CONFIG_* value (only SRCS can, in the later registration
    # pass). Confirmed empirically: a Kconfig-gated REQUIRES here silently no-ops
    # in every environment tried (PlatformIO and real ESP-IDF Docker CI alike).
    esp_driver_uart
    esp_driver_gpio
)

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS
        "include"
        "src"
    REQUIRES ${REQUIRES}
    PRIV_REQUIRES
        wireguard_lwip
)

set_source_files_properties(src/nacl_box.c
    PROPERTIES COMPILE_FLAGS "-Wno-unterminated-string-initialization"
)
