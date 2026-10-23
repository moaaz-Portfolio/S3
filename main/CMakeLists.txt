set(reqs esp_wifi_sensing
        protocol_examples_common
        driver
        esp_driver_ledc
        esp_http_server
        esp_netif
        esp_event
        esp_wifi
        esp_system
        esp_timer
        nvs_flash
        led_strip
        mdns
        dns_server)

idf_component_register(SRCS "app_main.c"
                            "led_control.c"
                            "web_serial_monitor.c"
                            "wifi_web_server.c"
                            "drwalls_wifi.c"
                       INCLUDE_DIRS "."
                       REQUIRES ${reqs}
                       PRIV_REQUIRES esp_driver_usb_serial_jtag
                       EMBED_TXTFILES "dashboard.html")
