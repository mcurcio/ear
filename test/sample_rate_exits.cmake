# cmake -DROOT=<folder> [-DGLOB=<pattern>] [-DALLOWED=<paths>] -P sample_rate_exits.cmake
#
# Where a rate leaves its type (docs/engine.md#dating): SampleRate (src/core/include/ear/core/sample_rate.hpp) has no
# arithmetic, so frames and time convert only through FrameClock, and its number comes out only through hz(), for a
# library's or the wire's edge. Every file under ROOT matching GLOB (default: C and C++ sources and headers), except
# tests (anything under a folder named tests), is refused for each line holding the word `hz` (an identifier, however
# reached: .hz(), ->hz(), &SampleRate::hz, hz (...), or a name of its own) unless ALLOWED names the file, relative to
# ROOT. One word and a list a reviewer reads, no parsing: what an allowed file does with the number is the list's
# limit. ALLOWED's default names, exactly:
#   core/include/ear/core/sample_rate.hpp   the type itself;
#   core/include/ear/core/latency.hpp       FrameClock, the one conversion between frames and time;
#   pipewire/engine.cpp                     default.clock.rate and allowed-rates;
#   pipewire/sound_cards.cpp                a card's audio.rate and its SPA format's rate;
#   pipewire/rtp_receivers.cpp              the RTP receiver's audio.rate, the sender's own rate;
#   nodes/file/file.cpp                     libsndfile's samplerate;
#   nodes/sendspin/worker.cpp               sendspin-cpp's formats, its source's sample_rate and the says notes;
#   control/events.cpp                      the wire's graphFrequency.
# It fails, too, when a listed file is not there, and when ROOT and GLOB leave it no file to check.
cmake_policy(VERSION 3.28)
if(NOT DEFINED ROOT)
	message(FATAL_ERROR "sample_rate_exits: ROOT is not set")
endif()
if(NOT DEFINED GLOB)
	set(GLOB "*.cpp;*.hpp;*.c;*.h")
endif()
if(NOT DEFINED ALLOWED)
	set(ALLOWED
		core/include/ear/core/sample_rate.hpp
		core/include/ear/core/latency.hpp
		pipewire/engine.cpp
		pipewire/sound_cards.cpp
		pipewire/rtp_receivers.cpp
		nodes/file/file.cpp
		nodes/sendspin/worker.cpp
		control/events.cpp)
endif()

set(patterns)
foreach(pattern IN LISTS GLOB)
	list(APPEND patterns "${ROOT}/${pattern}")
endforeach()
file(GLOB_RECURSE found LIST_DIRECTORIES false ${patterns})
set(files)
set(allowedSeen 0)
foreach(file IN LISTS found)
	file(RELATIVE_PATH shown "${ROOT}" "${file}")
	if(shown MATCHES "(^|/)tests/")
		continue()
	endif()
	list(APPEND files "${file}")
endforeach()
list(SORT files)
# A listed file that is gone (renamed, moved) is a stale list: refused, so the list stays what is true.
foreach(path IN LISTS ALLOWED)
	if(NOT EXISTS "${ROOT}/${path}")
		message(FATAL_ERROR "sample_rate_exits: ${path} is listed but not found under ${ROOT}")
	endif()
endforeach()

set(offences 0)
foreach(file IN LISTS files)
	file(RELATIVE_PATH shown "${ROOT}" "${file}")
	list(FIND ALLOWED "${shown}" allowed)
	if(NOT allowed EQUAL -1)
		math(EXPR allowedSeen "${allowedSeen} + 1")
		continue()
	endif()
	file(STRINGS "${file}" lines)
	set(number 0)
	foreach(line IN LISTS lines)
		math(EXPR number "${number} + 1")
		if(line MATCHES "(^|[^A-Za-z0-9_])hz([^A-Za-z0-9_]|$)")
			message(STATUS "${shown}:${number}: a rate's number taken outside the listed edges: ${line}")
			math(EXPR offences "${offences} + 1")
		endif()
	endforeach()
endforeach()

list(LENGTH files count)
if(count EQUAL 0)
	message(FATAL_ERROR "sample_rate_exits: the check would check nothing: no file under ${ROOT} matches ${GLOB} "
		"outside tests")
endif()
if(offences GREATER 0)
	message(FATAL_ERROR "sample_rate_exits: ${offences} lines under ${ROOT} take a rate's number outside the listed "
		"edges; convert frames and time through FrameClock (src/core/include/ear/core/latency.hpp, "
		"docs/engine.md#dating), or, at a new library's or the wire's edge, list the file in "
		"test/sample_rate_exits.cmake")
endif()
message(STATUS "sample_rate_exits: ${count} files under ${ROOT}, ${allowedSeen} listed, none takes a rate's number "
	"elsewhere")
