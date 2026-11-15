# EAR's images, one file, three targets (AGENTS.md#commits-ci-and-releases):
#
# - dev: the build and test image, for development (scripts/dev.sh) and CI
#   (.github/workflows/test.yml). Debian 13 (trixie): PipeWire 1.4, GCC 14 (C++23), Node.js 24.
# - shairport-sync (throwaway): the AirPlay receiver the airplay node runs, built from a pinned commit.
# - nqptp (throwaway): AirPlay 2's timing service, built from a pinned commit, for the airplay node's tests only.
# - build: dev plus this checkout, built: "make release" and the graph editor's "npm run build".
# - run: what ships (test.yml's publish job): the ear binary, its engine.conf, the built editor
#   and the fetched dependencies' licences on debian:trixie-slim with the libraries they load at run time, no compiler.
#
# Every base is pinned by the digest of its multi-platform index, so the same base serves
# linux/amd64 (CI, the published image) and linux/arm64 (Docker on Apple Silicon). Bump one by
# replacing its digest with the one "docker buildx imagetools inspect <image>" prints. The
# packages are whatever trixie's archive holds when the image is built; the digest does not pin
# them.

# Node.js 24 LTS from the official image (also built on trixie, so its binary runs on dev's
# glibc). Only node and npm are copied out of it (dev, below).
FROM node:24-trixie-slim@sha256:54f84b4cf8432b21aaffd5d3ce8d41e12cea0d10d33c09d2f3c9879d49d6b446 AS node

# shairport-sync, the AirPlay receiver every airplay node runs (docs/node-types.md#airplay), built from source: its
# "metadata" mDNS backend, which hands its listing to ear instead of advertising it, is first in 5.6.3, which no
# Debian release carries. A throwaway stage, so the image keeps only its binary (and run, its licences). Built with
# the pipe output, the metadata feed and AirPlay 2 only: no Avahi, no D-Bus, no ALSA.
# - autoconf, automake, libtool, pkg-config, g++, make: its autotools build.
# - git, ca-certificates: the clone over HTTPS.
# - libpopt-dev, libconfig-dev, libssl-dev: its command line, its config file and its AirPlay 1 crypto (OpenSSL).
# - libavcodec-dev, libavformat-dev, libavutil-dev, libswresample-dev: FFmpeg, its ALAC decoder (configure's default,
#   and upstream's recommendation: the other two decoders are deprecated "for security reasons").
# - libplist-dev, libplist-utils, libsodium-dev, libgcrypt20-dev, uuid-dev, xxd: AirPlay 2 (--with-airplay-2): its
#   property lists, its pairing crypto, its device's UUID, and the plist its build embeds.
FROM debian:trixie@sha256:2dc419c6d7fe3dad0a2f9fd469c34cc90b0f9ba642d8fde837fe3d7d34a0e7f1 AS shairport-sync
# The pin's one home: tag 5.6.3 (2026-11-10). Bump it by replacing the commit with a newer tag's
# ("git ls-remote https://github.com/mikebrady/shairport-sync refs/tags/<tag>"); scripts/dev.sh rebuilds the images.
ARG SHAIRPORT_SYNC_COMMIT=7ce90d89529dd760488f20592a7cbff626672f6c
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		autoconf \
		automake \
		ca-certificates \
		g++ \
		git \
		libavcodec-dev \
		libavformat-dev \
		libavutil-dev \
		libconfig-dev \
		libgcrypt20-dev \
		libplist-dev \
		libplist-utils \
		libpopt-dev \
		libsodium-dev \
		libssl-dev \
		libswresample-dev \
		libtool \
		make \
		pkg-config \
		uuid-dev \
		xxd \
	&& rm -rf /var/lib/apt/lists/*
WORKDIR /src/shairport-sync
# The proof at the end: -V names the pipe backend, the metadata feed and AirPlay 2, and -h lists "metadata" among the
# mDNS backends, which is how the airplay node's config (src/nodes/airplay/airplay_spec.hpp) reaches its listing.
RUN git init --quiet . \
	&& git remote add origin https://github.com/mikebrady/shairport-sync.git \
	&& git fetch --quiet --depth 1 origin "$SHAIRPORT_SYNC_COMMIT" \
	&& git checkout --quiet FETCH_HEAD \
	&& autoreconf -fi \
	&& ./configure --prefix=/usr/local --sysconfdir=/etc --with-pipe --with-metadata --with-ssl=openssl \
		--with-airplay-2 \
	&& make -j"$(nproc)" \
	&& ./shairport-sync -V | tee /dev/stderr | grep -q pipe \
	&& ./shairport-sync -V | grep -q metadata \
	&& ./shairport-sync -V | grep -q AirPlay2 \
	&& ./shairport-sync -h | sed -n '/mDNS backends/,$p' | grep -qw metadata

# nqptp, AirPlay 2's timing service (docs/node-types.md#airplay), which the operator runs beside ear and ear never
# starts: built here only so the airplay node's tests can run one (test/system/nqptp.hpp). Copied into dev alone, so
# build inherits it and run never has it (scripts/image-smoke.sh checks). Its pin must speak the shared-memory version
# the pinned shairport-sync reads (11).
# - autoconf, automake, g++, make: its autotools build.
# - git, ca-certificates: the clone over HTTPS.
FROM debian:trixie@sha256:2dc419c6d7fe3dad0a2f9fd469c34cc90b0f9ba642d8fde837fe3d7d34a0e7f1 AS nqptp
# The pin's one home: tag 1.2.9, shared-memory version 11. Bump it with shairport-sync's, to a tag whose shared-memory
# version that shairport-sync reads ("git ls-remote https://github.com/mikebrady/nqptp refs/tags/<tag>").
ARG NQPTP_COMMIT=ab8fb67483411640fd7f1e193889482f2421c37c
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		autoconf \
		automake \
		ca-certificates \
		g++ \
		git \
		make \
	&& rm -rf /var/lib/apt/lists/*
WORKDIR /src/nqptp
# The proof at the end: -V names shared-memory version 11.
RUN git init --quiet . \
	&& git remote add origin https://github.com/mikebrady/nqptp.git \
	&& git fetch --quiet --depth 1 origin "$NQPTP_COMMIT" \
	&& git checkout --quiet FETCH_HEAD \
	&& autoreconf -fi \
	&& ./configure \
	&& make -j"$(nproc)" \
	&& ./nqptp -V | tee /dev/stderr | grep -q smi11

FROM debian:trixie@sha256:2dc419c6d7fe3dad0a2f9fd469c34cc90b0f9ba642d8fde837fe3d7d34a0e7f1 AS dev

# - g++ and make: the compiler and the build tool; cmake (trixie: 3.31) runs the build.
# - ccache: compiler cache; scripts/dev.sh points CCACHE_DIR at the checkout's .cache/ccache.
# - pkg-config: how CMakeLists.txt finds PipeWire and libsndfile.
# - ca-certificates, xz-utils: FetchContent downloads Boost (a .tar.xz), knobly and mDNS over HTTPS.
# - libpipewire-0.3-dev, libspa-0.2-dev: PipeWire's headers and pkg-config files.
# - libpipewire-0.3-modules, libspa-0.2-modules: the modules and SPA plugins EAR's in-process
#   engine loads at run time (protocol, client-node, link-factory, the dummy driver).
# - libsndfile1-dev: the file node writes WAV files with libsndfile.
# - clang-format: make format and make format-check (.clang-format), the one C++ style CI enforces.
# - git: FetchContent clones the sendspin node's dependencies (micro-flac's submodule needs a clone).
# - python3, python3-venv: the real Sendspin server the sendspin node's tests run against, and the AirPlay sender
#   the airplay node's run against (below).
# - libpopt0, libconfig11, libssl3t64, libavcodec61, libavformat61, libavutil59, libswresample5, libplist-2.0-4,
#   libsodium23, libgcrypt20, libuuid1: shairport-sync's run-time libraries (its stage, above).
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		ca-certificates \
		ccache \
		clang-format \
		cmake \
		g++ \
		git \
		libavcodec61 \
		libavformat61 \
		libavutil59 \
		libconfig11 \
		libgcrypt20 \
		libpipewire-0.3-dev \
		libpipewire-0.3-modules \
		libplist-2.0-4 \
		libpopt0 \
		libsndfile1-dev \
		libsodium23 \
		libspa-0.2-dev \
		libspa-0.2-modules \
		libssl3t64 \
		libswresample5 \
		libuuid1 \
		make \
		pkg-config \
		python3 \
		python3-venv \
		xz-utils \
	&& rm -rf /var/lib/apt/lists/*

# The real Sendspin server (docs/node-types.md#library-facts-and-carried-patches), every package pinned by hash for both
# architectures. The run image gets none of it.
COPY test/sendspin/requirements.txt /opt/sendspin-server/requirements.txt
RUN python3 -m venv /opt/sendspin-server \
	&& /opt/sendspin-server/bin/pip install --quiet --no-cache-dir --no-compile --require-hashes \
		--only-binary :all: --requirement /opt/sendspin-server/requirements.txt

# The scripted AirPlay 1 sender the airplay node's system test plays through (test/support/airplay_sender.py,
# docs/testing.md#the-airplay-sender): pyatv and zeroconf, every package pinned by hash for both architectures, built
# as the Sendspin server's venv is. The run image gets none of it.
COPY test/airplay/requirements.txt /opt/airplay-sender/requirements.txt
RUN python3 -m venv /opt/airplay-sender \
	&& /opt/airplay-sender/bin/pip install --quiet --no-cache-dir --no-compile --require-hashes \
		--only-binary :all: --requirement /opt/airplay-sender/requirements.txt

# shairport-sync, which the airplay node runs (its stage, above): the same binary in dev and run.
COPY --from=shairport-sync /src/shairport-sync/shairport-sync /usr/local/bin/shairport-sync
# nqptp, which the airplay node's AirPlay 2 tests start (test/system/nqptp.hpp; CMakeLists.txt's EAR_NQPTP): here only,
# never in run, since the operator runs it beside ear (docs/node-types.md#airplay).
COPY --from=nqptp /src/nqptp/nqptp /usr/local/bin/nqptp

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

# The sources .dockerignore lets in. The cache mounts keep the fetched trees, ccache and
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
# - libpopt0, libconfig11, libssl3t64, libavcodec61, libavformat61, libavutil59, libswresample5, libplist-2.0-4,
#   libsodium23, libgcrypt20, libuuid1: shairport-sync, which the airplay node runs (its stage, above).
# Boost and the sendspin node's protocol library are linked statically (program_options is the only compiled Boost
# library).
RUN apt-get update \
	&& apt-get install -y --no-install-recommends \
		libasound2t64 \
		libavcodec61 \
		libavformat61 \
		libavutil59 \
		libconfig11 \
		libgcrypt20 \
		libpipewire-0.3-0t64 \
		libpipewire-0.3-modules \
		libplist-2.0-4 \
		libpopt0 \
		libsndfile1 \
		libsodium23 \
		libspa-0.2-modules \
		libssl3t64 \
		libswresample5 \
		libuuid1 \
	&& rm -rf /var/lib/apt/lists/*

# engine.conf sits beside the binary, where main.cpp looks for it.
COPY --from=build /src/out/ear /src/out/engine.conf /opt/ear/
# The licences of the fetched code ear compiles in: knobly (CMakeLists.txt) and the sendspin node's
# (cmake/SendspinDeps.cmake).
COPY --from=build /src/out/licenses /opt/ear/licenses
COPY --from=build /src/web/dist /opt/ear/web
# shairport-sync, which the airplay node runs (its stage, above), and its licences.
COPY --from=shairport-sync /src/shairport-sync/shairport-sync /usr/local/bin/shairport-sync
COPY --from=shairport-sync /src/shairport-sync/LICENSES /opt/ear/licenses/shairport-sync/

# Realtime priority for an ordinary user: a capability a pod adds does not reach a non-root
# process (execve drops it without file or ambient capabilities), so the binary carries
# CAP_SYS_NICE as a file capability. Permitted only ("=p"): a binary marked effective ("=ep")
# fails to start with EPERM wherever SYS_NICE is not allowed (a plain "docker run", CI);
# Engine::create makes it effective at boot when it is allowed. setcap's package is installed
# and purged in this one step, so it is in no layer. The recordings folder (the only place file
# nodes write) and the graph file folder (ear.conf, which ear writes back as the editor edits
# it) belong to the ear user; each is a volume, so recordings and the graph outlive the container.
# The graph file folder must be mounted writable: a read-only mount (a ConfigMap) cannot be edited.
RUN apt-get update \
	&& apt-get install -y --no-install-recommends libcap2-bin \
	&& setcap cap_sys_nice=p /opt/ear/ear \
	&& apt-get purge -y --auto-remove libcap2-bin \
	&& rm -rf /var/lib/apt/lists/* \
	&& useradd --uid 10001 --user-group --no-create-home --shell /usr/sbin/nologin ear \
	&& install -d -o ear -g ear -m 0755 /var/lib/ear/recordings /var/lib/ear/config

USER 10001:10001
VOLUME ["/var/lib/ear/config", "/var/lib/ear/recordings"]
EXPOSE 7777 8928
ENTRYPOINT ["/opt/ear/ear"]
CMD ["--config-dir", "/var/lib/ear/config", "--recordings-dir", "/var/lib/ear/recordings"]
