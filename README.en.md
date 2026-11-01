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
- resolving exactly **one** exported symbol, `shot_module_entry`, and reading the self-describing module table
- checking `struct_size` before touching any field, then comparing `abi_major` — mismatched plugins are rejected on the spot
- asking for capabilities with `query_interface("shot.image_processor", 1)` and silently skipping the ones it does not know
- wrapping instances into C++ objects and indexing them by `PluginType`; plugin objects are destroyed by the plugin itself, the host never `delete`s them

A **plugin** (a shared library) provides the actual capability. Screenshots are the
current main line; screen recording and friends will follow.

## Status

| Capability | State |
| --- | --- |
| Plugin loading / length and version checks / type index | ✅ working |
| Single entry point `shot_module_entry` + self-describing module table | ✅ working |
| Multiple plugins per DLL (`plugin_count` + `create(index)` + `get_info`) | ✅ working |
| Capability query `query_interface("shot.image_processor", 1)` | ✅ working |
| Runtime provider `shot.lib_provider` (`PluginType::Lib`) | ✅ working (`plugins/opencv_provider` brings OpenCV into the process) |
| Plugin configuration `shot.configurable` (plugin declares, host renders and stores) | ✅ contract and sample working (host UI not built yet) |
| Plugin metadata appended to the DLL tail (`metadata.json`, CMake only) | ✅ working (the host does not read it yet — see roadmap) |
| Build-time metadata validation (unique ids / registered interfaces / abi matches the header / dependency syntax) | ✅ working (`cmake/ValidatePluginMeta.cmake`) |
| Plugin-level dependencies (`requires` / `provides` with version ranges, `core` = the host) | ✅ working (`document/METADATA.md`) |
| Dependency-driven load order (topological sort; an unsatisfied plugin is skipped on its own) | ✅ working |
| Shared plugin build rules (`shot_add_plugin()`) | ✅ working (host and offline SDK share one module) |
| Offline plugin SDK export (`export_sdk` target) | ✅ working |
| `Window` / `Tray` capabilities | 📋 only needs a new interface id plus a struct — the contract stays put |
| Real screen capture pipeline | 🚧 not implemented (`main.cpp` smoke-tests with a 2×2 fake image) |
| Screen recording and other capabilities | 📋 only needs a new interface id — the contract stays put |

> The binary name comes from `PROJECT_NAME` in `CMakeLists.txt` (currently `Plugin-shot`):
> the host is `Plugin-shot.exe` and plugins land in `bin/plugins/`. When the icon file is missing,
> the generated `.rc` simply drops its ICON line, so renaming can never break the build.

## Layout

```
.
├── CMakeLists.txt          # cross-platform build (vcpkg manifest mode + OpenCV)
├── vcpkg.json              # dependency manifest
├── include/
│   ├── plugin_api.h          # ★ the host ⇄ plugin ABI contract (only cross-boundary header, plain C)
│   ├── plugin_api_export.h   # export macro: dllexport for plugins, SCREENSHOT_HOST_EXPORTS for the host
│   ├── shot_plugin_helpers.h # plugin-side helpers: capability list + shot_query_interface + SHOT_OPS
│   ├── dllHander.h           # LoadLibrary / dlopen wrapper (with a non-throwing tryGetFunction)
│   ├── pluginManager.h       # plugin scanning, length/version checks, type index
│   └── PluginInstances/      # host-side wrappers (base / ImageProcessor / LibProvider …)
├── src/                      # host implementation and entry point
├── plugins/                  # every subdirectory is a standalone plugin project
│   ├── example_processor/    # complete example: one DLL, two plugins, both configurable
│   └── opencv_provider/      # a valid package with zero capabilities (brings OpenCV into the process)
├── document/
│   ├── README_SDK.md         # SDK layout and every build constraint (written in Chinese)
│   ├── ABI_POLICY.md         # append-only rules and the interface id registry (Chinese)
│   └── METADATA.md           # metadata.json fields, authority rules, what stays out (Chinese)
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

There is exactly one cross-boundary header: `include/plugin_api.h` (plain C, no STL).
A plugin package exports **one** C function:

```c
SCREENSHOT_API const ShotModule* shot_module_entry(void);
```

It returns a self-describing module table, `ShotModule`; everything else lives inside it:

| Field | Purpose |
| --- | --- |
| `struct_size` / `abi_major` / `abi_minor` | self-described length and version: the host reads the length first, then compares the major |
| `plugin_count` | how many plugins this DLL holds |
| `get_info(index, info)` | id / name / version without creating an instance |
| `create(index, host)` | allocates the instance on the plugin's own heap and returns an opaque pointer |
| `destroy(instance)` | destroys the instance — **plugin-side only**, the host never `delete`s it |
| `query_interface(instance, id, version)` | asks "do you implement this capability?" |
| `init(host, flags)` / `shutdown()` | optional module-level setup and teardown; `nullptr` means not needed |

One DLL is one **plugin package**: `plugin_count` + `create(index)` mean the host never has to
know the contents in advance, and `plugins[]` in `metadata.json` maps index for index onto it.

### Three rules that make the ABI stop changing

1. **Add fields, never symbols.** The package exports `shot_module_entry` and nothing else. New capabilities are new fields appended to a struct, not new exports.
2. **Ask for the length before reading a field.** Every cross-boundary struct starts with `struct_size`, and readers go through `SHOT_HAS_FIELD(ptr, type, field)`. Old plugins (short structs) and new hosts (wanting new fields) coexist safely — **appending a field never needs a version bump**.
3. **Ask for capabilities by string id.** `query_interface(instance, "shot.image_processor", 1)` returns a function table or `NULL`, and the host quietly skips what it does not get. A new capability kind is a new id plus a new struct; no existing struct changes.

`plugin_api.h` ends with `static_assert`s pinning the offset and size of every struct: reordering or retyping fails the build, while appending a field forces you to update that line by hand — making an ABI break a deliberate act.

### Registered capabilities

| interface id | Struct | Purpose |
| --- | --- | --- |
| `shot.image_processor` | `ShotImageProcessorV1` | processes one image: `process(self, ShotImage*)`, 0 means success |
| `shot.configurable` | `ShotConfigurableV1` | declares and reads/writes configuration items, see below |
| `shot.lib_provider` | `ShotLibProviderV1` | says "I am a runtime provider" and reports library name/version; the host registers it as `PluginType::Lib` |

`ShotImage` describes an 8-bit pixel buffer:

- `pixel_format` states the layout explicitly (`SHOT_PIXEL_FORMAT_BGRA8` today) instead of guessing from `channels`
- `stride` is in bytes and **may be negative** (bottom-up storage) or larger than `width * channels` (row padding)
- therefore always address rows explicitly; never assume `data` is contiguous

### Configuration: the plugin declares, the host renders and stores

`shot.configurable` lets a plugin list its own configuration items; the host renders a UI from
that list, stores the values and feeds them back — **the host hard-codes no key name**:

```c
typedef struct ShotConfigurableV1 {
    uint32_t struct_size;
    uint32_t version;
    uint32_t (*get_config_count)(void* self);
    int32_t  (*get_config_desc)(void* self, uint32_t index, ShotConfigDesc* out);
    int32_t  (*get_config)(void* self, const char* key, char* buf, uint32_t buf_size);
    int32_t  (*set_config)(void* self, const char* key, const char* value);
} ShotConfigurableV1;
```

- Each item is described by `ShotConfigDesc`: `kind` (BOOL / INT / FLOAT / STRING / ENUM / COLOR / PATH), `key`, label, default, range and enum options.
- **Values are always strings**: no width or alignment problem across the boundary, trivially storable as JSON, and no STL needed inside the plugin.
- A plugin must ignore unknown keys safely (return a negative value), so a newer host can send extra items to an older plugin.
- `set_config` may be called at run time (a slider drag): either be thread-safe or document "takes effect on the next `process`".

**Host-level settings do not belong in a plugin**: output format, save directory and hotkeys are the host's; a plugin only owns its own algorithm parameters.

### ABI rules you must follow

1. **Use the /MD runtime (`MultiThreadedDLL`)** on both sides; `/MT` gives you two CRTs.
2. **Match the host's Debug/Release configuration** — a Debug plugin only loads into a Debug host.
3. **Never bundle base dependencies** (`opencv_*.dll` etc.). One copy already sits in `bin/` next to the host exe; a second copy in the plugin directory means two OpenCV instances in one process — split type info, global state and allocators.

   > The host itself **does not link** OpenCV (`CMakeLists.txt` only links `nlohmann_json`).
   > The `opencv_*.dll` files in `bin/` are placed there by `cmake/copy_runtime_deps.cmake`,
   > which copies the whole vcpkg runtime directory — they are not pulled in by linking.
   > A plugin that needs OpenCV calls `find_package(OpenCV QUIET)` in its **own**
   > `CMakeLists.txt` (see `plugins/opencv_provider`) and resolves the same DLL from `bin/` at run time.
4. **Do not pass STL containers / `std::string` / `cv::Mat` across the boundary**, and no C++ objects with virtual functions either: only POD structs and function pointers cross.
5. **Use fixed-width types only** (`uint32_t` / `int32_t` / `double`), never `int` / `long` / `size_t`; no C enums inside structs, use fixed-width constants.
6. **Never let an exception cross a DLL boundary**: catch inside every `extern "C"` function and report failure through the return value.
7. **`abi_major` must match the host.** Bump `abi_minor` for append-only additions; the host accepts plugins whose minor is ≤ its own.
8. **Structs grow at the end only**: never reorder, retype or delete; rename retired fields to `retired_xxx` and keep the slot.

### Metadata in the file tail

`cmake/AppendPluginMeta.cmake` appends `metadata.json` to the DLL **after linking**:

```text
<original binary>
---PLUGIN_META_START---
{ "schema": 1, "abi": { ... }, "plugins": [ ... ] }
---PLUGIN_META_END---
```

- Plain-text markers: no Python, no extra tool, and the host can just `rfind` them.
- Trailing data is outside the PE/ELF section table, so the loader ignores it entirely.
- The metadata travels with the file: copying or renaming a plugin can never separate a
  manifest from its DLL.
- The marker literals exist once, in `plugin_api.h`; the build script reads those macros at
  build time instead of duplicating them.
- Before appending, the metadata is validated: `plugins[]` non-empty, ids unique, every
  `interfaces` entry defined in the contract header, `abi` matching `SHOT_ABI_*`, and
  `package` matching the target name. A mistake fails the build instead of surfacing in the host.
- Field reference and authority rules: [`document/METADATA.md`](document/METADATA.md) (Chinese).
- Dependencies are **plugin-level**: `"requires": ["core>=0.1.0", "opencv>=4.12"]`, where `core`
  is the host's own version (from the CMake-generated `version.h`). The host reads every DLL's
  metadata first, topologically sorts, then loads; an unsatisfied plugin is skipped on its own
  while the rest of its package and the host keep working.

### Writing a plugin

One plugin per `.cpp`: the state, `process`, optional configuration accessors, and finally the
capability tables:

```cpp
// plugins/acme_tools/src/blur.cpp
static int32_t blur_process(void* self, ShotImage* image) {
    auto* state = static_cast<BlurState*>(state_of(self));   // note: self is the opaque instance
    // … walk the image row by row …
    return 0;                                                 // 0 means success
}

const ShotImageProcessorV1 kImageOps =
    SHOT_OPS(ShotImageProcessorV1, .process = &blur_process);

const ShotIfaceEntry kIfaces[] = {                            // capability list
    { SHOT_IFACE_IMAGE_PROCESSOR, 1, &kImageOps },
    { nullptr, 0, nullptr },                                  // terminator
};
```

`plugin_entry.cpp` does three things: register the plugins as `kEntries[]`, forward
`query_interface`, and return the module table:

```cpp
extern "C" {

SCREENSHOT_API const ShotModule* shot_module_entry(void) {
    static const ShotModule module = {
        .struct_size     = sizeof(ShotModule),
        .abi_major       = SHOT_ABI_MAJOR,
        .abi_minor       = SHOT_ABI_MINOR,
        .plugin_count    = kEntryCount,
        .get_info        = &get_info,
        .create          = &create_instance,
        .destroy         = &destroy_instance,
        .query_interface = &query_instance,   // forwards to shot_query_interface(ifaces, id, ver)
    };
    return &module;
}

}
```

The complete, buildable sample is [`plugins/example_processor/`](plugins/example_processor/README.md):
one DLL holding two plugins, both configurable, laid out as `CMakeLists.txt` + `metadata.json` +
`include/` + `src/`. SDK layout: [`document/README_SDK.md`](document/README_SDK.md); append-only
rules and the interface id registry: [`document/ABI_POLICY.md`](document/ABI_POLICY.md) (both in Chinese).

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

- [x] Plugin host skeleton: scanning, length/version checks, type index, lifecycle
- [x] Single entry point `shot_module_entry` + self-describing module table + `query_interface`
- [x] `shot.image_processor` and `shot.configurable` contracts, sample plugin, offline SDK export
- [x] Metadata appended to the plugin binary (CMake only) and multiple plugins per DLL
- [ ] Generic configuration UI on the host side (rendered from `ShotConfigDesc`) plus config storage
- [x] Host reads the metadata (pre-filter + dependency topological sort + per-plugin skipping)
- [ ] Cross-check metadata against `get_info` (warn when id / name / version disagree)
- [ ] Real screen capture (full screen / region / window) and the `Window` / `Tray` capabilities
- [ ] Screen recording (a new interface id; the contract itself does not change)
- [ ] Plugin hot reload and inter-plugin dependency declarations

## Platforms

Windows is the primary development and verification platform (MSVC + vcpkg `x64-windows`).
`CMakeLists.txt` already carries macOS / Linux / WSL branches and `dllHander.h` supports
`dlopen`, but **neither is verified**.

## License

Not specified yet.
