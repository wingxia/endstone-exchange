cmake_minimum_required(VERSION 3.22)

project(endstone_exchange VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(EXCHANGE_BUILD_TESTS "Build core tests" ON)
option(EXCHANGE_BUILD_ENDSTONE_PLUGIN "Build Endstone plugin target" OFF)

add_library(exchange_core
    cpp/src/core/ExchangeService.cpp
    cpp/src/core/HttpBridgeEconomy.cpp
    cpp/src/core/InMemoryRepository.cpp
    cpp/src/core/ItemIdentity.cpp
    cpp/src/ui/ExchangeUiModel.cpp
)
set_target_properties(exchange_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(exchange_core PUBLIC cpp/include)

if(EXCHANGE_BUILD_TESTS)
    enable_testing()
    add_executable(exchange_core_tests
        tests/cpp/test_exchange_core.cpp
    )
    target_link_libraries(exchange_core_tests PRIVATE exchange_core)
    add_test(NAME exchange_core_tests COMMAND exchange_core_tests)
endif()

if(EXCHANGE_BUILD_ENDSTONE_PLUGIN)
    include(FetchContent)
    set(ENDSTONE_API_VERSION "0.11.4" CACHE STRING "Endstone API version")
    FetchContent_Declare(
        endstone
        GIT_REPOSITORY https://github.com/EndstoneMC/endstone.git
        GIT_TAG v${ENDSTONE_API_VERSION}
    )
    FetchContent_MakeAvailable(endstone)

    endstone_add_plugin(endstone_exchange
        cpp/src/plugin/ExchangePlugin.cpp
    )
    target_link_libraries(endstone_exchange PRIVATE exchange_core)
    target_include_directories(endstone_exchange PRIVATE cpp/include)
endif()
