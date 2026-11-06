# FTXUI (vendored)

- Upstream: https://github.com/ArthurSonzogni/FTXUI
- Version: v5.0.0 (MIT, see LICENSE)
- Why vendored: the build host is offline; S9 must build with cmake -B build && cmake --build build
  without fetching anything.
- What was removed: examples/, doc/, .github/, tools/ (not needed to build the library).
- Local patch: CMakeLists.txt no longer calls add_subdirectory(examples) / add_subdirectory(doc)
  because those directories are stripped. The library targets (screen, dom, component) are untouched.
