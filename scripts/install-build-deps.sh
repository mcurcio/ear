#!/bin/sh
# Installs the system packages EAR's build needs (Linux only; macOS needs only the Xcode Command
# Line Tools, which configure checks for). CMake, a C++17 compiler and git are not installed
# here (AGENTS.md, Build and run).
#
# Debian package       pkg-config name   JUCE module that asks for it (linuxPackages)
# libasound2-dev       alsa              juce_audio_devices
# libfreetype-dev      freetype2         juce_graphics (linked by juceaide, JUCE's build tool)
# libfontconfig1-dev   fontconfig        juce_graphics (linked by juceaide)
set -eu

case "$(uname -s)" in
Linux) ;;
*)
	echo "install-build-deps: nothing to install on $(uname -s)"
	exit 0
	;;
esac

if ! command -v apt-get >/dev/null 2>&1; then
	echo "install-build-deps: only Debian and Ubuntu (apt-get) are supported; install" \
		"pkg-config and the alsa, freetype2 and fontconfig development packages" >&2
	exit 1
fi
sudo=""
if [ "$(id -u)" -ne 0 ]; then
	sudo=sudo
fi
$sudo apt-get update
$sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
	pkg-config libasound2-dev libfreetype-dev libfontconfig1-dev
