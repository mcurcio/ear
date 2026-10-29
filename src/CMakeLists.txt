#set(msgpack_SOURCES
#    ${msgpack_SOURCE_DIR}/objectc
#    ${msgpack_SOURCE_DIR}/unpack
#    ${msgpack_SOURCE_DIR}/version
#    ${msgpack_SOURCE_DIR}/vrefbuffer
#    ${msgpack_SOURCE_DIR}/zone
#)

#add_library(msgpack_o OBJECT ${msgpack_SOURCES})

set(ear_SOURCES
    main
)

juce_add_console_app(ear
    PRODUCT_NAME "Einstein Audio Router")
target_sources(ear PRIVATE
    ${ear_SOURCES})
target_compile_definitions(ear PRIVATE
    # JUCE_WEB_BROWSER and JUCE_USE_CURL would be on by default, but you might not need them.
    JUCE_WEB_BROWSER=0  # If you remove this, add `NEEDS_WEB_BROWSER TRUE` to the `juce_add_console_app` call
    JUCE_USE_CURL=0)
juce_generate_juce_header(ear)
target_link_libraries(ear PRIVATE
	Boost::headers
	Boost::program_options
	Boost::filesystem
	nlohmann_json::nlohmann_json
    # ConsoleAppData            # If you'd created a binary data target, you'd link to it here
    juce::juce_core
	juce::juce_audio_devices
	juce::juce_audio_utils
	juce::juce_audio_processors
	juce::juce_audio_formats
	juce::juce_dsp
)

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
