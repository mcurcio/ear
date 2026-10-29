#set(msgpack_SOURCES
#    ${msgpack_SOURCE_DIR}/objectc
#    ${msgpack_SOURCE_DIR}/unpack
#    ${msgpack_SOURCE_DIR}/version
#    ${msgpack_SOURCE_DIR}/vrefbuffer
#    ${msgpack_SOURCE_DIR}/zone
#)

#add_library(msgpack_o OBJECT ${msgpack_SOURCES})

set(ear_SOURCES
    main.cpp
)

# What ear and ear_tests (test/CMakeLists.txt) build against, in one place. Only what the code
# uses is linked, so a build compiles nothing else:
# - Boost: program_options is the only compiled library. asio_core instead of the Boost::asio
#   umbrella, which adds Boost::asio_deadline_timer (compiled date_time) and Boost::asio_spawn
#   (compiled context). Beast is header-only, but Boost::beast links Boost::asio and
#   Boost::container, so it is taken for compilation only (its headers and their dependencies'
#   headers, nothing linked).
# - JUCE: AudioProcessor and AudioProcessorGraph are in juce_audio_processors_headless;
#   juce_audio_processors and juce_audio_utils would add the GUI modules (juce_gui_extra,
#   juce_gui_basics, juce_graphics) and their image, font and SVG libraries.
add_library(ear_dependencies INTERFACE)
target_compile_definitions(ear_dependencies INTERFACE
    # JUCE_WEB_BROWSER and JUCE_USE_CURL would be on by default, but you might not need them.
    JUCE_WEB_BROWSER=0  # If you remove this, add `NEEDS_WEB_BROWSER TRUE` to the `juce_add_console_app` call
    JUCE_USE_CURL=0)
target_link_libraries(ear_dependencies INTERFACE
    Boost::program_options
    Boost::uuid
    Boost::endian
    Boost::asio_core
    $<COMPILE_ONLY:Boost::beast>
    nlohmann_json::nlohmann_json
    juce::juce_core
    juce::juce_events
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_processors_headless
    juce::juce_audio_formats
    juce::juce_dsp)
# Linux: JUCE looks up each module's linuxPackages with pkg_check_modules but does not fail when
# one is missing (_juce_create_pkgconfig_target in JUCEModuleSupport.cmake), so stop here,
# naming every juce::pkgconfig_<module>_LINUX_DEPS target ear_dependencies reaches whose lookup
# failed. FindPkgConfig caches a found result and skips the lookup on later configures, so a
# package removed after a successful configure is reported only once out/CMakeCache.txt is
# deleted (make clean); a failed lookup is retried on every configure.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    block()
        set(pending ear_dependencies)
        set(seen "")
        set(missing "")
        while(pending)
            list(POP_FRONT pending target)
            get_target_property(aliased ${target} ALIASED_TARGET)
            if(aliased)
                set(target ${aliased})
            endif()
            if(target IN_LIST seen)
                continue()
            endif()
            list(APPEND seen ${target})
            # pkg_check_modules' prefix, so its <prefix>_FOUND, is <module>_LINUX_DEPS.
            if(target MATCHES "^pkgconfig_((.+)_LINUX_DEPS)$")
                if(NOT ${CMAKE_MATCH_1}_FOUND)
                    list(APPEND missing "${CMAKE_MATCH_2} (target juce::${target})")
                endif()
            endif()
            get_target_property(links ${target} INTERFACE_LINK_LIBRARIES)
            foreach(link IN LISTS links)
                if(NOT link MATCHES "\\$<" AND TARGET "${link}")
                    list(APPEND pending "${link}")
                endif()
            endforeach()
        endwhile()
        if(missing)
            list(JOIN missing "\n  " missing)
            message(FATAL_ERROR "System packages are missing for these JUCE modules' "
                "linuxPackages (the pkg-config lines above name them):\n  ${missing}\n"
                "Install them with ${PROJECT_SOURCE_DIR}/scripts/install-build-deps.sh, "
                "then configure again.")
        endif()
    endblock()
endif()

juce_add_console_app(ear
    PRODUCT_NAME "Einstein Audio Router")
target_sources(ear PRIVATE
    ${ear_SOURCES})
juce_generate_juce_header(ear)
target_link_libraries(ear PRIVATE ear_dependencies)

#add_executable(ear ${ear_SOURCES})
#target_link_libraries(ear ${Boost_LIBRARIES})

add_custom_command(TARGET ear POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:ear> ${PROJECT_BINARY_DIR}/ear)


#add_library(mlog_o OBJECT ${mlog_SOURCES})
#add_library(mlog_file_o OBJECT ${mlog_file_SOURCES} $<TARGET_OBJECTS:msgpack_o>)

#add_library(mlog SHARED $<TARGET_OBJECTS:mlog_o> $<TARGET_OBJECTS:msgpack_o>)
#add_library(mlog_file SHARED $<TARGET_OBJECTS:mlog_file_o> $<TARGET_OBJECTS:msgpack_o>)
#set_target_properties(mlog PROPERTIES OUTPUT_NAME "mlog-${MLOG_VERSION}")
#set_target_properties(mlog_file PROPERTIES OUTPUT_NAME "mlog_file-${MLOG_VERSION}")

#target_link_libraries(mlog ${Boost_LIBRARIES})
#target_link_libraries(mlog_file mlog ${Boost_LIBRARIES} ${RTTL_LIBRARIES})

#install(TARGETS mlog mlog_file
#    LIBRARY DESTINATION "lib"
#    ARCHIVE DESTINATION "lib"
#)
