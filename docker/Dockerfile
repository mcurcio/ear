# EAR's images, one file, three targets (docs/design/batch2.md section 7):
#
# - dev: the build and test image, for development (scripts/dev.sh) and CI
#   (.github/workflows/test.yml). Debian 13 (trixie): PipeWire 1.4, GCC 14 (C++23), Node.js 24.
# - build: dev plus this checkout, built: "make release" and the graph editor's "npm run build".
# - run: what ships (.github/workflows/image.yml): the ear binary, its engine.conf and the built
#   editor on debian:trixie-slim with the libraries they load at run time, no compiler.
#
# Every base is pinned by the digest of its multi-platform index, so the same base serves
# linux/amd64 (CI, the published image) and linux/arm64 (Docker on Apple Silicon). Bump one by
# replacing its digest with the one "docker buildx imagetools inspect <image>" prints. The
# packages are whatever trixie's archive holds when the image is built; the digest does not pin
# them.

# Node.js 24 LTS from the official image (also built on trixie, so its binary runs on dev's
# glibc). Only node and npm are copied out of it (dev, below).
FROM node:24-trixie-slim@sha256:54f84b4cf8432b21aaffd5d3ce8d41e12cea0d10d33c09d2f3c9879d49d6b446 AS node

FROM debian:trixie@sha256:2dc419c6d7fe3dad0a2f9fd469c34cc90b0f9ba642d8fde837fe3d7d34a0e7f1 AS dev

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

# Node.js and npm for web/ (the graph editor's toolchain: web/package.json; web/.nvmrc names the
# same major version).
COPY --from=node /usr/local/bin/node /usr/local/bin/node
COPY --from=node /usr/local/lib/node_modules/npm /usr/local/lib/node_modules/npm
RUN ln -s ../lib/node_modules/npm/bin/npm-cli.js /usr/local/bin/npm \
	&& ln -s ../lib/node_modules/npm/bin/npx-cli.js /usr/local/bin/npx

# CMake (3.17+) takes these as its compiler launcher defaults, so every build here uses ccache.
ENV CMAKE_C_COMPILER_LAUNCHER=ccache \
	CMAKE_CXX_COMPILER_LAUNCHER=ccache

FROM dev AS build

# The sources .dockerignore lets in. The cache mounts keep the fetched Boost tree, ccache and
# npm's cache between local builds; they are not part of any image (in CI, which keeps only
# layers, the build stage compiles from scratch whenever a source changed).
WORKDIR /src
COPY . .
RUN --mount=type=cache,target=/src/.cache/deps \
	--mount=type=cache,target=/ccache \
	CCACHE_DIR=/ccache make release
RUN --mount=type=cache,target=/root/.npm \
	cd web && npm ci && npm run build

FROM debian:trixie-slim@sha256:f6b08b09a05725f9431502d3b5714f5c4576e82c1a84fa1debe0c24a7e0c3ec4 AS run

# What ear loads at run time, and nothing to build with:
# - libpipewire-0.3-0t64, libpipewire-0.3-modules, libspa-0.2-modules: the in-process engine,
#   its modules and SPA plugins (the dummy driver, and the ALSA plugin a sound card node uses).
# - libasound2t64: alsa-lib, which the ALSA plugin opens sound cards through.
# - libsndfile1: the file node.
# Boost is linked statically (program_options is the only compiled Boost library).
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		libasound2t64 \
		libpipewire-0.3-0t64 \
		libpipewire-0.3-modules \
		libsndfile1 \
		libspa-0.2-modules \
	&& rm -rf /var/lib/apt/lists/*

# engine.conf sits beside the binary, where main.cpp looks for it.
COPY --from=build /src/out/ear /src/out/engine.conf /opt/ear/
COPY --from=build /src/web/dist /opt/ear/web

# Realtime priority for an ordinary user: a capability a pod adds does not reach a non-root
# process (execve drops it without file or ambient capabilities), so the binary carries
# CAP_SYS_NICE as a file capability. Permitted only ("=p"): a binary marked effective ("=ep")
# fails to start with EPERM wherever SYS_NICE is not allowed (a plain "docker run", CI);
# Engine::create makes it effective at boot when it is allowed. setcap's package is installed
# and purged in this one step, so it is in no layer.
RUN apt-get update \
	&& apt-get install -y --no-install-recommends libcap2-bin \
	&& setcap cap_sys_nice=p /opt/ear/ear \
	&& apt-get purge -y --auto-remove libcap2-bin \
	&& rm -rf /var/lib/apt/lists/* \
	&& useradd --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin ear

USER 10001:10001
EXPOSE 7777
ENTRYPOINT ["/opt/ear/ear"]
CMD ["--config", "/etc/ear/ear.json"]
