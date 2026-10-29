# Integration tests: Catch2 (vendored, single header) driving the audio graph through a
# virtual audio device. No OS audio device is touched.

juce_add_console_app(ear_tests
    PRODUCT_NAME "EAR Tests")
target_sources(ear_tests PRIVATE
    main.cpp
    audio_graph_test.cpp)
target_include_directories(ear_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/vendor/catch/single_include")
target_link_libraries(ear_tests PRIVATE ear_dependencies)

# One CTest test per Catch test case, listed from the built binary.
include("${PROJECT_SOURCE_DIR}/vendor/catch/contrib/Catch.cmake")
catch_discover_tests(ear_tests)
