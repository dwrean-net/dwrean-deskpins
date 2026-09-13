cmake_minimum_required(VERSION 3.21)

project(dwrean_deskpins VERSION 2.0.0 LANGUAGES CXX)

add_executable(dwrean-deskpins WIN32
    src/modern/main.cpp
)

target_compile_features(dwrean-deskpins PRIVATE cxx_std_20)

target_compile_definitions(dwrean-deskpins PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(dwrean-deskpins PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
    )
endif()

target_link_libraries(dwrean-deskpins PRIVATE
    user32
    shell32
    advapi32
)

set_target_properties(dwrean-deskpins PROPERTIES
    OUTPUT_NAME "dwrean-deskpins"
)
