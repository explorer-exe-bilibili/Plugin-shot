# plugin-shot

[中文](README.md) | **English**

> A screenshot tool where everything is a plugin. The host does only what a host
> should — load, validate, dispatch; screenshots, image processing, screen
> recording and every other capability come from plugins.

## Design

Keep the core tiny and stable, grow the product through plugins (the same idea as
DeepSeek Harness).

The **host** (currently built as `my-project.exe`) is responsible for:

- scanning the plugin directory and loading `.dll` / `.so` / `.dylib` files
- validating ABI via `plugin_abi_version()` — mismatched plugins are rejected on the spot
- wrapping each `CPluginHandle` into a C++ object and indexing it by `PluginType`
- lifecycle management only — plugin objects are destroyed by the plugin itself, the host never `delete`s them

A **plugin** (a shared library) provides the actual capability. Screenshots are the
current main line; screen recording and friends will follow.

## Status

| Capability | State |
| --- | --- |
| Plugin loading / ABI check / type index | ✅ working |
| `ImageProcessor` plugin type | ✅ working (`plugins/example_processor` is the minimal example) |
| Offline plugin SDK export (`export_sdk` target) | ✅ working |
| `Window` / `Tray` plugin types | 🚧 enum only, headers still empty |
| Real screen capture pipeline | 🚧 not implemented (`main.cpp` smoke-tests with a 2×2 fake image) |
| Screen-recording plugin type | 📋 planned |

> ⚠️ `PROJECT_NAME` in `CMakeLists.txt` is still the template placeholder
> `my-project`, so the binary is called `my-project.exe`. Renaming is a one-variable change.

## Layout

```
.
├── CMakeLists.txt          # cross-platform build (vcpkg manifest mode + OpenCV)
├── vcpkg.json              # dependency manifest
├── include/
│   ├── plugin_api.h        # ★ the host ⇄ plugin ABI contract (only cross-boundary header)
│   ├── plugin_api_export.h # export macro: dllexport for plugins, SCREENSHOT_HOST_EXPORTS for the host
│   ├── dllHander.h         # LoadLibrary / dlopen wrapper
│   ├── pluginManager.h     # plugin scanning, ABI check, type index
│   └── PluginInstances/    # host-side wrappers (base / ImageProcessor / window / tray)
├── src/                    # host implementation and entry point
├── plugins/
│   └── example_processor/  # minimal example plugin: invert RGB, keep alpha
├── cmake/                  # runtime dependency layout, output pruning, SDK export, pre-link kill
└── sdk/                    # offline SDK produced by export_sdk (not committed, see .gitignore)
```

## Build

Requirements: CMake ≥ 3.15, a C++23 compiler (MSVC on Windows), and [vcpkg](https://github.com/microsoft/vcpkg).

```bash
# toolchain lookup order: $VCPKG_ROOT, then $VCPKG_INSTALLATION_ROOT, then /opt/vcpkg
export VCPKG_ROOT=/path/to/vcpkg      # Windows: set VCPKG_ROOT=D:\vcpkg

cmake -S . -B build/Windows -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows

# export the offline SDK so third-party plugins build without a local OpenCV install
cmake --build build/Windows --target export_sdk
```

The first configure installs OpenCV through the manifest (`opencv4` with
jpeg/png/tiff/webp features), which takes a while.

### Run

```bash
cd build/Windows/bin && ./my-project.exe
```

The plugin directory is resolved as the relative path `./plugins`, so **launch from `bin/`**.

### Output layout (hard constraints)

| Path | Contents |
| --- | --- |
| `bin/` | host exe plus this configuration's runtime DLLs (**no `*.pdb` / `*.ilk`**) |
| `bin/plugins/` | plugin artifacts only (**no OpenCV or other base dependencies**) |
| `build/<platform>/pdb/` | every debug symbol |
| `sdk/` | offline SDK: `include/`, `lib/` (release), `lib/debug/`, `bin/`, `bin/debug/` |

These rules are enforced at build time by `cmake/copy_runtime_deps.cmake` and
`cmake/prune_output_dir.cmake`. See contract rule 3 below: a plugin that ships its
own OpenCV puts two instances into one process.

## Plugin contract

There is exactly one cross-boundary header: `include/plugin_api.h`. A plugin must
export three C functions:

| Symbol | Purpose |
| --- | --- |
| `plugin_abi_version()` | returns `PLUGIN_ABI_VERSION`; the host refuses the plugin on mismatch |
| `plugin_create_processor()` | allocates a `CPluginHandle` on the plugin's own heap and returns it |
| `plugin_destroy(handle)` | destroys the handle — **plugin-side only**, the host never `delete`s it |

`CPluginHandle` carries `type` (`Window` / `Tray` / `ImageProcessor`), `name`,
`internal_data` and `process(internal_data, Image*) -> int` (non-zero means success).

`Image` describes an 8-bit pixel buffer:

- `width` / `height` are pixel dimensions; `channels` is bytes per pixel (always 4 for screenshots, i.e. BGRA)
- `stride` is in bytes and **may be negative** (bottom-up storage) or larger than `width * channels` (row padding)
- therefore always address rows explicitly; never assume `data` is contiguous

### ABI rules you must follow

1. **Use the /MD runtime (`MultiThreadedDLL`)** on both sides; `/MT` gives you two CRTs.
2. **Match the host's Debug/Release configuration** — a Debug plugin only loads into a Debug host.
3. **Never bundle base dependencies** (`opencv_*.dll` etc.). The host already loaded its copy; a second copy in the plugin directory means two OpenCV instances in one process — split type info, global state and allocators.
4. **Do not pass STL containers / `std::string` / `cv::Mat` across the boundary** unless both CRTs are provably identical; what crosses the boundary is a C++ vtable, not a plain C struct.
5. **`PLUGIN_ABI_VERSION` must match the host.** Replace `include/` wholesale when upgrading the SDK; never mix old and new.

### Writing a plugin

The minimal working implementation is [`plugins/example_processor/main.cpp`](plugins/example_processor/main.cpp)
(under 60 lines). SDK layout and full build constraints: [`sdk/README.md`](sdk/README.md)
(currently written in Chinese).

```cpp
#include "plugin_api.h"

// int process_blur(void* internal_data, Image* image);  // non-zero means success

extern "C" {

SCREENSHOT_API int plugin_abi_version(void) { return PLUGIN_ABI_VERSION; }

SCREENSHOT_API CPluginHandle* plugin_create_processor(void) {
    auto* handle = new CPluginHandle{};
    handle->type = PluginType::ImageProcessor;
    handle->name = "acme.blur";
    handle->internal_data = nullptr;
    handle->process = &process_blur;
    return handle;
}

SCREENSHOT_API void plugin_destroy(CPluginHandle* handle) { delete handle; }

}
```

On the CMake side, point the output at `bin/plugins/` and mind `PREFIX ""` plus the runtime flag:

```cmake
add_library(my_plugin SHARED main.cpp)
set_target_properties(my_plugin PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    RUNTIME_OUTPUT_DIRECTORY "${HOST_BIN}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${HOST_BIN}/plugins"
    MSVC_RUNTIME_LIBRARY "MultiThreaded<$<$<CONFIG:Debug>:Debug>DLL")
target_include_directories(my_plugin PRIVATE "${SDK}/include" "${SDK}/include/opencv4")
```

## Roadmap

- [x] Plugin host skeleton: scanning, ABI check, type index, lifecycle
- [x] `ImageProcessor` contract + example plugin + offline SDK export
- [ ] Real screen capture (full screen / region / window) and the `Window` / `Tray` plugin types
- [ ] Screen-recording plugins (new `PluginType` and contract; `PLUGIN_ABI_VERSION` bumps then)
- [ ] Plugin metadata (version, author, declared dependencies) and hot reload

## Platforms

Windows is the primary development and verification platform (MSVC + vcpkg `x64-windows`).
`CMakeLists.txt` already carries macOS / Linux / WSL branches and `dllHander.h` supports
`dlopen`, but **neither is verified**.

## License

Not specified yet.
