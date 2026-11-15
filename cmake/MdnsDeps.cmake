# What ear_mdns fetches (docs/architecture.md#packages): mjansson/mdns, one header, so nothing of the project is added
# (its example would be built). Public domain. Kept in .cache/deps across "make clean", as Boost's is; SYSTEM keeps its
# warnings out of EAR's. ear_mdns is the only library that sees it (its tests read answers with it too).
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

# Its licence, beside the binary for the run image (docker/Dockerfile) and any other copy.
if(NOT EXISTS "${mdns_SOURCE_DIR}/LICENSE")
	message(FATAL_ERROR "mdns: its licence is not at ${mdns_SOURCE_DIR}/LICENSE")
endif()
configure_file("${mdns_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/licenses/mdns-LICENSE" COPYONLY)
