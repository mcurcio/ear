# What the sendspin node fetches (docs/design/sendspin.md section 13): the protocol library, sendspin-cpp, with one
# carried patch; the four dependencies it would fetch itself at moving tags, each kept here at a full commit, IXWebSocket
# with a carried patch of its own; and the
# one header ear's mDNS uses. kept_fetchcontent declares
# each one first, and FetchContent's first declaration wins, so the library's own FetchContent_Declare of a name
# changes nothing and its FetchContent_MakeAvailable finds it already there. Every tree is kept in .cache/deps across
# "make clean", as Boost's is; only what ear_node_sendspin links is built (EXCLUDE_FROM_ALL), and SYSTEM keeps their
# headers' warnings out of EAR's. ear_node_sendspin is the only package that sees any of them. Bump one by changing its
# commit here, and sendspin-cpp's by reading its cmake/host.cmake for new dependencies.

include("${CMAKE_CURRENT_LIST_DIR}/CarriedPatch.cmake")

# TLS and zlib are off: `ws://` only (section 4). The library forces both off itself, later; the options must hold
# before IXWebSocket's own configure runs here.
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)

# Roles: the player and the source only. Opus is off because libsndfile pulls in libopus.so and
# micro-opus's static C symbols would interpose on it (section 5(a2)).
set(SENDSPIN_ENABLE_CONTROLLER OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_METADATA OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_ARTWORK OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_VISUALIZER OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_COLOR OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_OPUS OFF CACHE BOOL "" FORCE)

# sendspin_dep(<name> <repository> <commit> [FetchContent options...])
function(sendspin_dep name repository commit)
	kept_fetchcontent(${name} "${repository}\n${commit}"
		GIT_REPOSITORY ${repository}
		GIT_TAG ${commit}
		EXCLUDE_FROM_ALL
		SYSTEM
		${ARGN}
	)
	set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# sendspin_patched_dep(<name> <repository> <commit> <patch>): a dependency with one carried patch. Its SHA-256 is in the
# pin, so changing it fetches the tree again, and a failed apply stops configure. Configure then asks whether the tree holds
# the whole patch, so no node runs without what it carries, whichever tree an explicit FETCHCONTENT_SOURCE_DIR_<NAME> names
# (cmake/CarriedPatch.cmake). Rebase the patch at each bump of the commit.
function(sendspin_patched_dep name repository commit patch)
	file(SHA256 "${patch}" patch_sha256)
	kept_fetchcontent(${name} "${repository}\n${commit}\n${patch_sha256}"
		GIT_REPOSITORY ${repository}
		GIT_TAG ${commit}
		PATCH_COMMAND git apply "${patch}"
		EXCLUDE_FROM_ALL
		SYSTEM
	)
	get_filename_component(patch_name "${patch}" NAME)
	carried_patch_missing(missing "${${name}_SOURCE_DIR}" "${patch}")
	if(missing)
		message(FATAL_ERROR "${name}: ${${name}_SOURCE_DIR} lacks cmake/${patch_name}: ${missing}")
	endif()
	set(${name}_SOURCE_DIR "${${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

# The trees' folders are named by kept_fetchcontent in lower case, as FetchContent names its properties.
sendspin_dep(arduinojson https://github.com/bblanchon/ArduinoJson.git
	32520135092970120a5ac165cf45f48e658c421d)   # v7.4.1, MIT
# The carried patch bounds what an unproved peer can cost before any Sendspin message (section 11): frames, fragments, the
# transport's per-connection log lines, and the sockets it holds, which give way to newcomers. It also keeps a close racing a
# peer's reset from ending the process.
sendspin_patched_dep(ixwebsocket https://github.com/machinezone/IXWebSocket.git
	64fae7676bd8fe31f7cb4bcde7a6841892dad65e   # v12.0.1, BSD-3
	"${CMAKE_CURRENT_LIST_DIR}/ixwebsocket-limits.patch")
sendspin_dep(micro_flac https://github.com/esphome-libs/micro-flac.git
	f9c27d4d0166021db977d7fc37d205b21d81e9f2    # v0.3.0, Apache-2.0
	GIT_SUBMODULES "lib/micro-ogg-demuxer")
# Its CMakeLists.txt is for ESP-IDF only, so nothing of it is added here: the library builds noise_c from the tree.
sendspin_dep(noise_c https://github.com/esphome-libs/noise-c.git
	a1e08809a1b8f65cd91765ba7d68a6d00648ad61    # v0.1.13, MIT
	SOURCE_SUBDIR no-such-directory)

# The carried patch refuses an upgrade carrying Origin, and makes unproved connections give way to newcomers (section 11).
sendspin_patched_dep(sendspin https://github.com/Sendspin/sendspin-cpp.git
	7be2278a252dd8c71da55732e93fe28871a9021d   # master: accept_inbound_connections came after v0.9.5, Apache-2.0
	"${CMAKE_CURRENT_LIST_DIR}/sendspin-listener.patch")

# ear's mDNS: one header, so nothing of the project is added (its example would be built). Public domain.
set(mdns_url https://github.com/mjansson/mdns/archive/refs/tags/1.4.3.tar.gz)
set(mdns_sha256 be1fd8e35599cb7de179decbd0633c121d11a2dcb9cc193ff5c590bd0d480483)
kept_fetchcontent(mdns "${mdns_url}\n${mdns_sha256}"
	URL ${mdns_url}
	URL_HASH SHA256=${mdns_sha256}
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
	EXCLUDE_FROM_ALL
	SYSTEM
	SOURCE_SUBDIR no-such-directory
)

# Each licence, beside the binary for the run image (docker/Dockerfile) and any other copy: <name>-LICENSE.
foreach(entry IN ITEMS "arduinojson LICENSE.txt" "ixwebsocket LICENSE.txt" "micro_flac LICENSE"
		"noise_c LICENSE" "sendspin LICENSE" "mdns LICENSE")
	separate_arguments(entry)
	list(GET entry 0 name)
	list(GET entry 1 file)
	if(NOT EXISTS "${${name}_SOURCE_DIR}/${file}")
		message(FATAL_ERROR "${name}: its licence is not at ${${name}_SOURCE_DIR}/${file}")
	endif()
	configure_file("${${name}_SOURCE_DIR}/${file}" "${CMAKE_BINARY_DIR}/licenses/${name}-LICENSE" COPYONLY)
endforeach()
