cmake_minimum_required(VERSION 3.25)

project(endstone_exchange VERSION 1.4.4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Endstone's Linux ABI is libc++. Its top-level project applies this itself,
# but those directory options do not propagate when Endstone is consumed with
# FetchContent, so plugin consumers must opt in explicitly.
if(UNIX AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(-stdlib=libc++)
    add_link_options(-stdlib=libc++)
endif()

set(ENDSTONE_VERSION "0.11.6" CACHE STRING "Exact Endstone release to target")
option(EXCHANGE_BUILD_TESTS "Build the exchange test executable" ON)

include(FetchContent)
FetchContent_Declare(
    endstone
    GIT_REPOSITORY https://github.com/EndstoneMC/endstone.git
    GIT_TAG v${ENDSTONE_VERSION}
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(endstone)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(MARIADB REQUIRED IMPORTED_TARGET libmariadb)

configure_file(include/endstone_exchange/version.hpp.in
               ${CMAKE_CURRENT_BINARY_DIR}/generated/endstone_exchange/version.hpp @ONLY)

set(EXCHANGE_CORE_SOURCES
    src/config.cpp
    src/database.cpp
    src/exchange_service.cpp
    src/hologram_packet.cpp
    src/interaction_gate.cpp
    src/item_identity.cpp
    src/market_display.cpp
    src/trade_form.cpp
)

endstone_add_plugin(exchange
    src/plugin.cpp
    src/inventory_escrow.cpp
    src/nbt_codec.cpp
    src/structure_reader.cpp
    ${EXCHANGE_CORE_SOURCES}
)
target_include_directories(exchange PRIVATE
    include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)
target_link_libraries(exchange PRIVATE PkgConfig::MARIADB Threads::Threads)
target_compile_options(exchange PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(exchange PROPERTIES SKIP_BUILD_RPATH TRUE)

if(EXCHANGE_BUILD_TESTS)
    enable_testing()
    add_executable(exchange_tests
        tests/test_main.cpp
        src/nbt_codec.cpp
        src/structure_reader.cpp
        ${EXCHANGE_CORE_SOURCES}
    )
    target_include_directories(exchange_tests PRIVATE
        include
        ${CMAKE_CURRENT_BINARY_DIR}/generated
    )
    target_link_libraries(exchange_tests PRIVATE endstone::endstone PkgConfig::MARIADB)
    target_compile_options(exchange_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(UNIX AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_link_libraries(exchange_tests PRIVATE -static-libgcc -static-libstdc++ libc++.a libc++abi.a)
    endif()
    add_test(NAME exchange_tests COMMAND exchange_tests)
endif()
