# EAR's build and test image, one for development (scripts/dev.sh) and CI
# (.github/workflows/test.yml). Debian 13 (trixie): PipeWire 1.4 and GCC 14 (C++23).
#
# The base is pinned by the digest of its multi-platform index, so the same base serves
# linux/amd64 (CI) and linux/arm64 (Docker on Apple Silicon). Bump it by replacing the digest
# with the one "docker buildx imagetools inspect debian:trixie" prints. The packages are
# whatever trixie's archive holds when the image is built; the digest does not pin them.
FROM debian:trixie@sha256:2dc419c6d7fe3dad0a2f9fd469c34cc90b0f9ba642d8fde837fe3d7d34a0e7f1

# - g++ and make: the compiler and the build tool; cmake (trixie: 3.31) runs the build.
# - ccache: compiler cache; scripts/dev.sh points CCACHE_DIR at the checkout's .cache/ccache.
# - pkg-config: how CMakeLists.txt finds PipeWire and libsndfile.
# - ca-certificates, xz-utils: FetchContent downloads Boost over HTTPS as a .tar.xz.
# - libpipewire-0.3-dev, libspa-0.2-dev: PipeWire's headers and pkg-config files.
# - libpipewire-0.3-modules, libspa-0.2-modules: the modules and SPA plugins EAR's in-process
#   engine loads at run time (protocol, client-node, link-factory, the dummy driver).
# - libsndfile1-dev: the file node writes WAV files with libsndfile.
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		ca-certificates \
		ccache \
		cmake \
		g++ \
		libpipewire-0.3-dev \
		libpipewire-0.3-modules \
		libsndfile1-dev \
		libspa-0.2-dev \
		libspa-0.2-modules \
		make \
		pkg-config \
		xz-utils \
	&& rm -rf /var/lib/apt/lists/*

# CMake (3.17+) takes these as its compiler launcher defaults, so every build here uses ccache.
ENV CMAKE_C_COMPILER_LAUNCHER=ccache \
	CMAKE_CXX_COMPILER_LAUNCHER=ccache
