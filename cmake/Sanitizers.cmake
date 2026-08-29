if(HOTPATH_SANITIZE)
    # -fno-sanitize-recover turns a UBSan diagnostic into a failing exit. UBSan is recoverable by
    # default: it prints, continues, and exits zero, so ctest passes on undefined behaviour and the
    # report scrolls past in a log nobody reads.
    add_compile_options(
        -fsanitize=${HOTPATH_SANITIZE}
        -fno-sanitize-recover=all
        -fno-omit-frame-pointer
        -g
    )
    add_link_options(-fsanitize=${HOTPATH_SANITIZE})
    message(STATUS "Sanitizers enabled: ${HOTPATH_SANITIZE}")
endif()
