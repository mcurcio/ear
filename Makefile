BUILDDIR=out

debug: cmake_debug
	cd ${BUILDDIR}; make
.PHONY: debug

release: cmake_release
	cd ${BUILDDIR}; make
.PHONY: release

test: ${BUILDDIR}
	cd ${BUILDDIR}; cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=ON ..
	cd ${BUILDDIR}; make ear_tests
	cd ${BUILDDIR}; ctest --output-on-failure
.PHONY: test

cmake_debug: ${BUILDDIR}
	cd ${BUILDDIR}; cmake -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTS=OFF ..
.PHONY: cmake_debug

cmake_release: ${BUILDDIR}
	cd ${BUILDDIR}; cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF ..
.PHONY: cmake_release

clean:
	rm -rf ${BUILDDIR}
.PHONY: clean

${BUILDDIR}:
	mkdir -p ${BUILDDIR}
