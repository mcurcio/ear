# JUCE's system dependencies in this build

Checked against the JUCE 9.0.4 source; JUCE refs are under `.cache/deps/juce-src/`. JUCE's own
list for every module is `docs/Linux Dependencies.md`.

## Linux

- Each module header declares its system packages as `linuxPackages` (pkg-config names). For
  each module that has some, JUCE runs `pkg_check_modules(<module>_LINUX_DEPS <packages>)` and
  links the result into the module as `juce::pkgconfig_<module>_LINUX_DEPS`
  (`extras/Build/CMake/JUCEModuleSupport.cmake:633-637`, `_juce_create_pkgconfig_target`
  at `:367`). It is not `REQUIRED`: a missing package prints FindPkgConfig's
  `Checking for module` lines and leaves the target empty, and the build then fails on a
  missing header. `src/CMakeLists.txt` walks `ear_dependencies` to those targets and stops
  configure when one's `<module>_LINUX_DEPS_FOUND` is not true. JUCE keeps no package list on
  a failed lookup, so that message names the module and its target, not the package.
- The modules EAR links need `alsa` (`juce_audio_devices`). Of the rest, `juce_graphics` needs
  `freetype2 fontconfig` and `juce_opengl` needs `egl gl`.
- juceaide, JUCE's build tool, is configured and built inside `add_subdirectory(JUCE)` by a
  nested CMake run (`extras/Build/juceaide/CMakeLists.txt:129-160`). It links
  `juce_build_tools`, which depends on `juce_graphics`
  (`extras/Build/juce_build_tools/juce_build_tools.h:52`), so FreeType and Fontconfig are
  needed although EAR links no GUI module. Without them configure stops at JUCE's own
  `Failed to build juceaide` error (the nested build's output: `ft2build.h: No such file or
  directory`) before EAR's check runs; FindPkgConfig's `Package 'freetype2' ... not found`
  line is printed above it.
- `scripts/install-build-deps.sh` installs the Debian and Ubuntu packages for all three.

## macOS

Modules declare `OSXFrameworks` and `WeakOSXFrameworks`, which JUCE links from the SDK
(`JUCEModuleSupport.cmake:600-614`); the SDK comes with the Xcode Command Line Tools, which the
top of `CMakeLists.txt` requires (`xcrun --show-sdk-path`).
