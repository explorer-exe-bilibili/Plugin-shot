# plugin-shot

[中文](README.md) | **English**

> A screenshot tool where everything is a plugin. The host does only what a host
> should — load, validate, dispatch; screenshots, image processing, screen
> recording and every other capability come from plugins.

## Design

Keep the core tiny and stable, grow the product through plugins (the same idea as
DeepSeek Harness).

The **host** (currently built as `Plugin-shot.exe`) is responsible for:

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
| Multiple plugins per DLL (`plugin_get_count()` + `plugin_create(index)`) | ✅ working |
| `ImageProcessor` plugin type | ✅ working (`plugins/example_processor` packs two plugins into one DLL) |
| Plugin metadata appended to the DLL tail (`metadata.json`, CMake only) | ✅ working (the host does not read it yet — see roadmap) |
| Shared plugin build rules (`shot_add_plugin()`) | ✅ working (host and offline SDK share one module) |
| Offline plugin SDK export (`export_sdk` target) | ✅ working |
| `Window` / `Tray` plugin types | 🚧 enum only, headers still empty |
| Real screen capture pipeline | 🚧 not implemented (`main.cpp` smoke-tests with a 2×2 fake image) |
| Screen-recording plugin type | 📋 planned |

> The binary name comes from `PROJECT_NAME` in `CMakeLists.txt` (currently `Plugin-shot`):
> the host is `Plugin-shot.exe` and plugins land in `bin/plugins/`. When the icon file is missing,
> the generated `.rc` simply drops its ICON line, so renaming can never break the build.

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
├── plugins/                # every subdirectory is a standalone plugin project
│   └── example_processor/  # complete example: CMakeLists + metadata.json + include/ + src/
├── cmake/
│   ├── ShotPlugin.cmake        # shot_add_plugin(): shared plugin build rules
│   ├── AppendPluginMeta.cmake  # appends metadata.json to the DLL tail
│   ├── prune_output_dir.cmake  # prunes output dirs (no pdb/ilk/exp/lib)
│   └── ...                     # runtime dependency layout, SDK export, pre-link kill
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
cd build/Windows/bin && ./Plugin-shot.exe
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
export four C functions:

| Symbol | Purpose |
| --- | --- |
| `plugin_abi_version()` | returns `PLUGIN_ABI_VERSION`; the host refuses the plugin on mismatch |
| `plugin_get_count()` | how many plugins this module holds; `<= 0` means an empty package |
| `plugin_create(int index)` | allocates the `index`-th `CPluginHandle` on the plugin's own heap; out of range yields `nullptr` |
| `plugin_destroy(handle)` | destroys the handle — **plugin-side only**, the host never `delete`s it |

One DLL is one **plugin package**: the indexed factory means the host never has to know
the contents in advance, and `plugins[]` in `metadata.json` maps index for index onto
`plugin_create(index)`.

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

### Metadata in the file tail

`cmake/AppendPluginMeta.cmake` appends `metadata.json` to the DLL **after linking**:

```text
<original binary>
---PLUGIN_META_START---
{ "package": "example_processor", "plugins": [ ... ] }
---PLUGIN_META_END---
```

- Plain-text markers: no Python, no extra tool, and the host can just `rfind` them.
- Trailing data is outside the PE/ELF section table, so the loader ignores it entirely.
- The metadata travels with the file: copying or renaming a plugin can never separate a
  manifest from its DLL.
- The marker literals exist once, in `plugin_api.h`; the build script reads those macros at
  build time instead of duplicating them.

### Writing a plugin

The complete sample is [`plugins/example_processor/`](plugins/example_processor/README.md): one DLL
holding two plugins, laid out as `CMakeLists.txt` + `metadata.json` + `include/` + `src/`.
SDK layout and full build constraints: [`document/README_SDK.md`](document/README_SDK.md)
(currently written in Chinese).

```cpp
#include "plugin_api.h"

// Index table: the order must match plugins[] in metadata.json
constexpr Entry kEntries[] = {
    {::ImageProcessor, "acme.blur",    &process_blur},
    {::ImageProcessor, "acme.sharpen", &process_sharpen},
};

extern "C" {

SCREENSHOT_API int plugin_abi_version(void) { return PLUGIN_ABI_VERSION; }

SCREENSHOT_API int plugin_get_count(void) {
    return static_cast<int>(std::size(kEntries));
}

SCREENSHOT_API CPluginHandle* plugin_create(int index) {
    if (index < 0 || index >= static_cast<int>(std::size(kEntries))) return nullptr;
    auto* handle = new CPluginHandle{};
    handle->type = kEntries[index].type;
    handle->name = kEntries[index].name;
    handle->internal_data = nullptr;
    handle->process = kEntries[index].process;
    return handle;
}

SCREENSHOT_API void plugin_destroy(CPluginHandle* handle) { delete handle; }

}
```

A plugin project CMakeLists does three things — find the module, list sources, point at metadata:

```cmake
list(APPEND CMAKE_MODULE_PATH "${HOST}/cmake")   # or <SDK>/cmake
include(ShotPlugin)

shot_add_plugin(acme_tools
    METADATA     metadata.json
    SOURCES      src/plugin_entry.cpp src/blur.cpp src/sharpen.cpp
    INCLUDE_DIRS include)
```

`shot_add_plugin()` owns `/MD`, `PREFIX ""`, hidden visibility, output directories, pdb
redirection, metadata appending and output pruning — rules that used to be copy-pasted into
the root `CMakeLists.txt` and drifted away from the docs.

## Roadmap

- [x] Plugin host skeleton: scanning, ABI check, type index, lifecycle
- [x] `ImageProcessor` contract + example plugin + offline SDK export
- [ ] Real screen capture (full screen / region / window) and the `Window` / `Tray` plugin types
- [ ] Screen-recording plugins (new `PluginType` and contract; `PLUGIN_ABI_VERSION` bumps then)
- [x] Metadata appended to the plugin binary (CMake only) and multiple plugins per DLL
- [ ] Host reads the metadata (validate id/type/dependencies before `LoadLibrary`)
- [ ] Plugin hot reload and inter-plugin dependency declarations

## Platforms

Windows is the primary development and verification platform (MSVC + vcpkg `x64-windows`).
`CMakeLists.txt` already carries macOS / Linux / WSL branches and `dllHander.h` supports
`dlopen`, but **neither is verified**.

## License

Not specified yet.
