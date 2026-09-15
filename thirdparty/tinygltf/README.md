# thirdparty/tinygltf

Pinned: syoyo/tinygltf **v2.9.7**
Source: https://github.com/syoyo/tinygltf/tree/v2.9.7
Patched: `#include "json.hpp"` -> `#include <nlohmann/json.hpp>` (same as vcpkg port)
Vendored to avoid flaky GitHub archive downloads in CI (vcpkg unexpected hash).
