#!/bin/sh
# Installs the system packages EAR's build needs on this platform: the system dependencies the
# JUCE modules it builds declare, which configure checks
# (ear_require_juce_system_dependencies in cmake/JuceSystemDependencies.cmake). CMake, a C++17
# compiler and git are not installed here (AGENTS.md, Build and run).
set -eu

case "$(uname -s)" in
Linux)
	# The apt-get line below maps the pkg-config packages configure requires to their Debian and
	# Ubuntu names; configure's error names any it finds missing.
	if ! command -v apt-get >/dev/null 2>&1; then
		echo "install-build-deps: only Debian and Ubuntu (apt-get) are supported; install the" \
			"pkg-config packages that configure reports missing" >&2
		exit 1
	fi
	sudo=""
	if [ "$(id -u)" -ne 0 ]; then
		sudo=sudo
	fi
	$sudo apt-get update
	$sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		pkg-config libasound2-dev libfreetype-dev libfontconfig1-dev
	;;
Darwin)
	# The frameworks JUCE's OSXFrameworks name ship in the macOS SDK.
	if ! xcode-select -p >/dev/null 2>&1; then
		echo "install-build-deps: run xcode-select --install (the Xcode Command Line Tools" \
			"hold the macOS SDK)" >&2
		exit 1
	fi
	echo "install-build-deps: nothing to install; the Xcode Command Line Tools at" \
		"$(xcode-select -p) hold the macOS SDK"
	;;
*)
	echo "install-build-deps: nothing known to install on $(uname -s); CMakeLists.txt names the" \
		"platforms EAR builds on" >&2
	exit 1
	;;
esac
