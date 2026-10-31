# ear_core: the desired graph and its edits, the node catalogue contract, the node framework, the
# reconciler, levels, the graph read and the executor contract (docs/design/batch2.md section 1).
# It never calls PipeWire, opens a socket, starts a thread or touches a file: nlohmann/json is its
# only dependency, and the engine and the control port reach it through the interfaces its
# headers declare.
add_library(ear_core STATIC
	command_error.cpp
	desired_graph.cpp
	graph_status.cpp
	levels.cpp
	node_catalogue.cpp
	node_host.cpp
	node_lifecycle.cpp
	reconciler.cpp
	rtp_receiver.cpp
	sound_card.cpp)
# The public headers as a header set: its base directory is the include path of ear_core and
# of everything that links it, and VERIFY_INTERFACE_HEADER_SETS adds a target that compiles
# each header on its own, so none depends on what an includer happened to include first. The
# test executable depends on that target, so "make test" builds it.
target_sources(ear_core PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
	include/ear/core/command_error.hpp
	include/ear/core/desired_graph.hpp
	include/ear/core/executor.hpp
	include/ear/core/graph_spec.hpp
	include/ear/core/graph_view.hpp
	include/ear/core/levels.hpp
	include/ear/core/node_catalogue.hpp
	include/ear/core/node_host.hpp
	include/ear/core/node_lifecycle.hpp
	include/ear/core/node_type.hpp
	include/ear/core/reconciler.hpp
	include/ear/core/rtp_receiver.hpp
	include/ear/core/sound_card.hpp)
set_target_properties(ear_core PROPERTIES VERIFY_INTERFACE_HEADER_SETS ON)
target_link_libraries(ear_core PUBLIC nlohmann_json::nlohmann_json)

if(BUILD_TESTS)
	add_executable(ear_core_tests
		tests/desired_graph_test.cpp
		tests/graph_spec_test.cpp
		tests/graph_status_test.cpp
		tests/levels_test.cpp
		tests/node_host_test.cpp
		tests/node_lifecycle_test.cpp
		tests/reconciler_test.cpp
		tests/rtp_receiver_test.cpp
		tests/sound_card_test.cpp)
	target_link_libraries(ear_core_tests PRIVATE ear_core ear_catch_main)
	add_dependencies(ear_core_tests ear_core_verify_interface_header_sets)
	ear_catch_tests(ear_core_tests)
endif()
