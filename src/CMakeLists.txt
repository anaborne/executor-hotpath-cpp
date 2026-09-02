add_library(hotpath_core STATIC
    clock.cpp
    executor.cpp
    json.cpp
    killswitch.cpp
    orders.cpp
    pricing.cpp
    protocol.cpp
    signer.cpp
    telemetry.cpp
)
add_library(hotpath::core ALIAS hotpath_core)

target_include_directories(hotpath_core
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

# The system SQLite rather than a vendored amalgamation. Every row this writes lands off the hot
# path, after the span being measured has closed, so the library's version cannot move a benchmark
# number, and building 250k lines of C on every clean checkout to pin one that cannot would buy
# nothing.
# OpenSSL::Crypto only. libssl is the TLS record layer and nothing here opens a socket to Kalshi;
# the signer needs EVP, PEM and the base64 encoder, all of which live in libcrypto.
target_link_libraries(hotpath_core PRIVATE hotpath::warnings SQLite::SQLite3 OpenSSL::Crypto)

add_executable(executor_hotpath main.cpp)
target_link_libraries(executor_hotpath PRIVATE hotpath::core hotpath::warnings)
