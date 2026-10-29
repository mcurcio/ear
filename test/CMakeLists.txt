# Integration tests: Catch2 (vendored, single header) driving the audio graph through a
# virtual audio device. No OS audio device is touched.

juce_add_console_app(ear_tests
    PRODUCT_NAME "EAR Tests")
target_sources(ear_tests PRIVATE
    main.cpp
    audio_graph_test.cpp)
target_include_directories(ear_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/vendor/catch/single_include")
target_compile_definitions(ear_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
# The same Boost and JUCE targets as the ear executable (src/CMakeLists.txt).
target_link_libraries(ear_tests PRIVATE
    Boost::program_options
    Boost::uuid
    Boost::endian
    Boost::asio
    Boost::beast
    nlohmann_json::nlohmann_json
    juce::juce_core
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_audio_formats
    juce::juce_dsp)

# One CTest test per Catch test case, listed from the built binary.
include("${PROJECT_SOURCE_DIR}/vendor/catch/contrib/Catch.cmake")
catch_discover_tests(ear_tests)
