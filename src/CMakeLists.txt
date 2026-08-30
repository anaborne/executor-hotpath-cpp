add_library(hotpath_core STATIC
    clock.cpp
    executor.cpp
    json.cpp
    killswitch.cpp
    orders.cpp
    pricing.cpp
    protocol.cpp
    version.cpp
)
add_library(hotpath::core ALIAS hotpath_core)

target_include_directories(hotpath_core
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hotpath_core PRIVATE hotpath::warnings)

add_executable(executor_hotpath main.cpp)
target_link_libraries(executor_hotpath PRIVATE hotpath::core hotpath::warnings)
