idf_component_register(
    SRCS "test_diagnostics.cpp" "test_filament_change_trigger.cpp"
    INCLUDE_DIRS ".."
    REQUIRES unity esp_timer
)
