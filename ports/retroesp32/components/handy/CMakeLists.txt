idf_component_register(
    SRCS
        "handy_run.cpp"
        "cart.cpp"
        "eeprom.cpp"
        "lynxdec.cpp"
        "memmap.cpp"
        "mikie.cpp"
        "ram.cpp"
        "rom.cpp"
        "susie.cpp"
        "system.cpp"
    INCLUDE_DIRS "."
    REQUIRES odroid esp_timer
)

# C++ flags: no exceptions, no RTTI, optimise for size
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-exceptions
    -fno-rtti
    -Os
    -Wno-narrowing
    -Wno-write-strings
    -Wno-unused-variable
    -Wno-sign-compare
    -Wno-parentheses
    -Wno-format
    -Wno-unused-but-set-variable
    -Wno-comment
    -Wno-sequence-point
    -Wno-sizeof-pointer-memaccess
    -Wno-maybe-uninitialized
    -Wno-error=overloaded-virtual
)

# Defines matching the original RetroESP32 build
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    GIT_VERSION=""
)
