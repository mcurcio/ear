BUILDDIR=out

# Parallel jobs for the inner build and ctest: the CPU count by default, "make JOBS=4 debug"
# to override.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 1)

# The inner build runs through "cmake --build --parallel", which works for any generator.
# MAKEFLAGS is cleared for it so a "make -jN" here neither forces -j onto a jobserver nor
# hands down jobserver descriptors this recipe does not pass: JOBS alone sets the parallelism.
BUILD = MAKEFLAGS= cmake --build ${BUILDDIR} --parallel ${JOBS}

# $(call configure,<build type>,<BUILD_TESTS>) configures ${BUILDDIR} unless its cache already
# holds both values and a generate step has written the build system (cmake_install.cmake).
# Skipping is safe because the build re-runs CMake itself when a CMakeLists.txt or the cache
# changes; re-running it here cost about 1.7 s of a 2 s no-op build.
configure = cd ${BUILDDIR}; \
	if grep -qx 'CMAKE_BUILD_TYPE:STRING=$(1)' CMakeCache.txt 2>/dev/null \
		&& grep -qx 'BUILD_TESTS:BOOL=$(2)' CMakeCache.txt && test -f cmake_install.cmake; \
	then echo "${BUILDDIR} is configured for $(1) with BUILD_TESTS=$(2)"; \
	else cmake -DCMAKE_BUILD_TYPE=$(1) -DBUILD_TESTS=$(2) ..; fi

debug: cmake_debug
	${BUILD}
.PHONY: debug

release: cmake_release
	${BUILD}
.PHONY: release

test: ${BUILDDIR}
	$(call configure,Debug,ON)
	${BUILD} --target ear_tests
	ctest --test-dir ${BUILDDIR} --output-on-failure --parallel ${JOBS}
.PHONY: test

cmake_debug: ${BUILDDIR}
	$(call configure,Debug,OFF)
.PHONY: cmake_debug

cmake_release: ${BUILDDIR}
	$(call configure,Release,OFF)
.PHONY: cmake_release

clean:
	rm -rf ${BUILDDIR}
.PHONY: clean

${BUILDDIR}:
	mkdir -p ${BUILDDIR}
