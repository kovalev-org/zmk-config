if(CONFIG_SHIELD_KEYBALL39_RIGHT)
  zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
  zephyr_library_sources(totp.c)
  zephyr_library_sources(behavior_totp.c)
  # The OLED layer-or-TOTP widget + custom status screen are temporarily
  # disabled — they prevented the right half from booting (uninitialized
  # SSD1306 framebuffer + BLE never came up). Pending isolation.
  # if(CONFIG_ZMK_DISPLAY)
  #   zephyr_library_sources(widget_layer_or_totp.c)
  #   zephyr_library_sources(status_screen_custom.c)
  # endif()
endif()
