idf_component_register(
    SRCS "test_diagnostics.cpp" "test_filament_change_trigger.cpp" "test_printer_flow.cpp"
    INCLUDE_DIRS ".."
    REQUIRES unity esp_timer bblanchon__arduinojson
)
