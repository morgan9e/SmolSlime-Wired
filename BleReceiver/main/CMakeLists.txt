# The PC link: USB HID like the nRF receiver where the chip has USB OTG (ESP32-S3, TinyUSB from
# idf_component.yml), else USB Serial/JTAG text lines (ESP32-C6)
idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32s3")
    set(link_src "link_usb.c")
else()
    set(link_src "link_jtag.c")
endif()

idf_component_register(SRCS "main.c" ${link_src}
                       PRIV_REQUIRES bt nvs_flash esp_driver_usb_serial_jtag vfs)
