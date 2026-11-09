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
- enabling/disabling plugins at run time: disabling is a **hot unload** (destroy instances → unsubscribe
  from the event bus → `FreeLibrary`), enabling reloads the DLL and feeds the stored configuration back
- aggregating UI data contributed by plugins into read-only JSON snapshots keyed by **list id**
  (`shot.host.plugin_lists`), with `lists.changed` as the change notification; a new data shape means a
  new list id + JSON schema, never a new ABI struct

A **plugin** (a shared library) provides the actual capability. Screenshots are the
current main line.

## Status

| Capability | State |
| --- | --- |
| Plugin loading / length and version checks / type index | ✅ working |
| Single entry point `shot_module_entry` + self-describing module table | ✅ working |
| Multiple plugins per DLL (`plugin_count` + `create(index)` + `get_info`) | ✅ working |
| Capability query `query_interface("shot.image_processor", 1)` | ✅ working |
| Screen capture `shot.screen_capture` (`plugins/screen_capture`, GDI over the virtual desktop; Windows only) | ✅ working (`document/CAPTURE.md`) |
| Image saving `shot.image_saver` (`plugins/image_saver`, PNG/JPEG plus four config items) | ✅ working (`document/CAPTURE.md`) |
| Capture → process → save pipeline (`capture.request` / `capture.done` / `capture.failed`) | ✅ working (`document/CAPTURE.md`) |
| Host event bus `shot.host.event_bus` (topic publish / subscribe) | ✅ working (`document/EVENT_BUS.md`) |
| Generic list service `shot.host.plugin_lists` (JSON snapshot per list id: `config_list` / `tray_command_list`) | ✅ working (`document/LISTS.md`) |
| List event protocol `lists.*` (`add` / `remove` / `update` / `commit` / `updated` / `changed`, event-driven writes) | ✅ working (`document/LISTS.md`) |
| Host plugin registry `shot.host.plugin_registry` (enable/disable, hot unload) | ✅ working (`document/SETTINGS.md`) |
| Runtime provider `shot.lib_provider` (`PluginType::Lib`) | ✅ working (`plugins/opencv_provider` brings OpenCV into the process) |
| UI provider `shot.ui_provider` (windows/tray owned by the plugin itself) | ✅ working (`plugins/settings_ui`, `plugins/tray`) |
| Plugin configuration `shot.configurable` (plugin declares; the host aggregates into `config_list`, event-driven writes / persistence / feed-back) | ✅ working (`document/SETTINGS.md`) |
| Settings window plugin `plugins/settings_ui` (reads `config_list`; ImGui + GLFW + OpenGL3, descriptor-driven) | ✅ working (cross-platform; verified on Windows) |
| Tray entry `plugins/tray` (menu data from `tray_command_list`; capture / open settings / quit; Windows only) | ✅ working |
| Command line `--capture` / `--settings` / `--help` | ✅ working |
| Host settings (`core` pseudo-plugin: log level / console / file log) | ✅ working |
| Plugin metadata appended to the DLL tail (`metadata.json`, CMake only) | ✅ working (the host reads it before loading: prefilter + dependency topological sort) |
| Build-time metadata validation (unique ids / registered interfaces / abi matches the header / dependency syntax) | ✅ working (`cmake/ValidatePluginMeta.cmake`) |
| Plugin-level dependencies (`requires` / `provides` with version ranges, `core` = the host) | ✅ working (`document/METADATA.md`) |
| Dependency-driven load order (topological sort; an unsatisfied plugin is skipped on its own) | ✅ working |
| Shared plugin build rules (`shot_add_plugin()`) | ✅ working (host and offline SDK share one module) |
| Offline plugin SDK export (`export_sdk` target) | ✅ working |

> The binary name comes from `PROJECT_NAME` in `CMakeLists.txt` (currently `Plugin-shot`):
> the host is `Plugin-shot.exe` and plugins land in `bin/plugins/`. When the icon file is missing,
> the generated `.rc` simply drops its ICON line, so renaming can never break the build.

## Layout

```
.
├── CMakeLists.txt          # cross-platform build (vcpkg manifest mode; plugins auto-discovered by GLOB)
├── vcpkg.json              # dependency manifest
├── include/
│   ├── plugin_api.h          # ★ the host ⇄ plugin ABI contract (only cross-boundary header, plain C)
│   ├── plugin_api_export.h   # export macro: dllexport for plugins, SCREENSHOT_HOST_EXPORTS for the host
│   ├── shot_plugin_helpers.h # umbrella header only includes; one header per helper class
│   ├── shot_plugin_helpers/  # iface_entry / logger / config / event_bus /
│   │                         # plugin_lists / plugin_registry, one header each
│   ├── dllHandler.h           # LoadLibrary / dlopen wrapper (with a non-throwing tryGetFunction)
│   ├── pluginManager.h       # plugin scanning, dependency order, loading and hot unload
│   ├── hostServices.h        # host service tables: logging, config, event bus, list service, plugin registry
│   ├── hostListCatalog.h     # generic list catalog: generates config_list + implements the lists.* protocol
│   ├── hostRuntime.h         # host wiring: PluginManager + list catalog + plugin registry + capture pipeline
│   ├── capturePipeline.h     # capture pipeline: screen capture -> processing -> image save wiring
│   ├── config.h              # core::Config: JSON configuration file
│   └── PluginInstances/      # host-side wrappers (base / ImageProcessor / LibProvider / ScreenCapture / ImageSaver …)
├── src/                      # host implementation and entry point
├── plugins/                  # every subdirectory is a standalone plugin project
│   ├── example_processor/    # complete example: one DLL, two plugins, both configurable
│   ├── opencv_provider/      # a valid package with zero capabilities (brings OpenCV into the process)
│   ├── screen_capture/       # screen capture: Windows GDI over the virtual desktop (skipped off Windows)
│   ├── image_saver/          # image saving: PNG/JPEG with directory/format/quality/filename config
│   ├── settings_ui/          # settings window: ImGui + GLFW + OpenGL3 (cross-platform, Windows verified)
│   └── tray/                 # tray entry: capture / open settings / quit (skipped off Windows)
├── document/
│   ├── README_SDK.md         # SDK layout and every build constraint (written in Chinese)
│   ├── ABI_POLICY.md         # append-only rules and the interface id registry (Chinese)
│   ├── SETTINGS.md           # settings window: config_list, event-driven writes/commit, plugin enable/disable and hot unload (Chinese)
│   ├── LISTS.md              # generic lists + event-driven design: login of list ids, lists.* protocol, refresh timing (Chinese)
│   ├── CAPTURE.md            # capture pipeline: capture.* events, failure semantics and --capture (Chinese)
│   ├── EVENT_BUS.md          # host event bus: topics, threading, lifetime (Chinese)
│   └── METADATA.md           # metadata.json fields, authority rules, what stays out (Chinese)
├── cmake/
│   ├── ShotPlugin.cmake        # shot_add_plugin(): shared plugin build rules
│   ├── AppendPluginMeta.cmake  # appends metadata.json to the DLL tail
│   ├── prune_output_dir.cmake  # prunes output dirs (no pdb/ilk/exp/lib)
│   └── ...                     # runtime dependency layout, SDK export, pre-link kill
└── sdk/                    # offline SDK produced by export_sdk (not committed, see .gitignore)
```

## Build

Requirements: CMake ≥ 3.20 (≥ 3.21 when using `CMakePresets.json`), a C++23 compiler (MSVC on Windows), and [vcpkg](https://github.com/microsoft/vcpkg).

```bash
# toolchain lookup order: $VCPKG_ROOT, then $VCPKG_INSTALLATION_ROOT, then /opt/vcpkg
export VCPKG_ROOT=/path/to/vcpkg      # Windows: set VCPKG_ROOT=D:\vcpkg

cmake -S . -B build/Windows -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Windows

# export the offline SDK so third-party plugins build without a local OpenCV install
cmake --build build/Windows --target export_sdk
```

The first configure installs the manifest dependencies (`opencv4` with jpeg/png/tiff/webp
features, `nlohmann-json`, `spdlog`, `glfw3`, `nativefiledialog-extended`, `stb`, and `imgui`
— a static library with `glfw-binding` + `opengl3-binding` for the settings window), which
takes a while. Like OpenCV, the settings window's runtime DLLs (`glfw3.dll`, `nfd.dll`) are
laid out into `bin/` by `cmake/copy_runtime_deps.cmake`, not stored in the plugin directory.
The generic list protocol adds no new dependency: a list snapshot is just JSON serialized with
`nlohmann-json`.

### Run

```bash
cd build/Windows/bin && ./Plugin-shot.exe            # no arguments: tray-resident
cd build/Windows/bin && ./Plugin-shot.exe --capture  # capture once, print the path, exit
cd build/Windows/bin && ./Plugin-shot.exe --settings # open settings, then stay resident (debug)
cd build/Windows/bin && ./Plugin-shot.exe --help     # print usage
```

The plugin directory is resolved as the relative path `./plugins`, so **launch from `bin/`**.

Without arguments the host does not open the settings window on its own while the tray is loaded:
the tray menu (capture / open settings / quit) publishes `capture.request` / `settings.open` /
`app.quit`. If `tray.main` is not loaded (disabled or failed), startup publishes `settings.open`
automatically as a fallback entry point.
Screenshots go to `%USERPROFILE%\Pictures` by default, with the `shot_%Y%m%d_%H%M%S` filename
template and PNG format; directory/format/quality/filename are `plugins/image_saver` config
items editable in the settings window. `--capture` logs `Capture saved: <path>` on success and
returns non-zero on failure (exit codes: `document/CAPTURE.md` §6).
Logging goes through spdlog: console plus `%LOCALAPPDATA%\screen-shot\logs\host.log` (appended).
Configuration lives in `%LOCALAPPDATA%\screen-shot\config.json` (host settings and plugin items,
keyed as `<owner>/<id>`, matching the `owner` / `id` of a `config_list` entry).

### Output layout (hard constraints)

| Path | Contents |
| --- | --- |
| `bin/` | host exe plus this configuration's runtime DLLs (**no `*.pdb` / `*.ilk`**) |
| `bin/plugins/` | plugin artifacts only (**no OpenCV or other base dependencies**) |
| `build/<platform>/pdb/` | every debug symbol |
| `sdk/` | offline SDK: `include/`, `lib/` (release), `lib/debug/`, `bin/`, `bin/debug/` |

These rules are enforced at build time by `cmake/copy_runtime_deps.cmake` and
`cmake/prune_output_dir.cmake`. Plugin artifacts use the platform extension
(`.dll` / `.dylib` / `.so`); on non-Windows platforms runtime libraries are not copied —
RPATH points into vcpkg's `lib/`. See contract rule 3 below: a plugin that ships its
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

### Four rules that make the ABI stop changing

1. **Add fields, never symbols.** The package exports `shot_module_entry` and nothing else. New capabilities are new fields appended to a struct, not new exports.
2. **Ask for the length before reading a field.** Every cross-boundary struct starts with `struct_size`, and readers go through `SHOT_HAS_FIELD(ptr, type, field)`. Old plugins (short structs) and new hosts (wanting new fields) coexist safely — **appending a field never needs a version bump**.
3. **Ask for capabilities by string id.** `query_interface(instance, "shot.image_processor", 1)` returns a function table or `NULL`, and the host quietly skips what it does not get. A new capability kind is a new id plus a new struct; no existing struct changes.
4. **Data shapes never enter the ABI.** A new kind of UI/host data (menu items, config items, target lists, …) is a new list id + JSON schema + `lists.*` events; the ABI describes capabilities and behaviour only, never data structures — see `document/LISTS.md`.

`plugin_api.h` ends with `static_assert`s pinning the offset and size of every struct: reordering or retyping fails the build, while appending a field forces you to update that line by hand — making an ABI break a deliberate act.

### Registered capabilities

| interface id | Struct | Purpose |
| --- | --- | --- |
| `shot.image_processor` | `ShotImageProcessorV1` | processes one image: `process(self, ShotImage*)`, 0 means success |
| `shot.screen_capture` | `ShotScreenCaptureV1` | screen capture: `describe` reports the virtual-desktop geometry, `capture` writes BGRA8 pixels into the host-allocated `ShotImage`; see `document/CAPTURE.md` |
| `shot.image_saver` | `ShotImageSaverV1` | image saving: writes a `ShotImage` to disk and returns the final path; format/quality/directory come from the plugin's config items; see `document/CAPTURE.md` |
| `shot.configurable` | `ShotConfigurableV1` | declares and reads/writes configuration items, see below |
| `shot.lib_provider` | `ShotLibProviderV1` | says "I am a runtime provider" and reports library name/version; the host registers it as `PluginType::Lib` |
| `shot.ui_provider` | `ShotUIProviderV1` | the plugin creates and owns its window/tray; the host only calls `init` / `deinit` / `post_to_plugin` |
| `shot.host.event_bus` | `ShotHostEventBusV1` | **host service**: in-process topic publish/subscribe |
| `shot.host.config` | `ShotHostConfigV1` | **host service**: host config read/write (legacy; new code uses `plugin_lists`' `config_list`) |
| `shot.host.board_msg` | `ShotHostBoardEventV1` | **host service**: legacy broadcast queue (new code uses event_bus) |
| `shot.host.plugin_lists` | `ShotHostPluginListsV1` | **host service**: generic lists, returns a read-only JSON snapshot per list id (`config_list` / `tray_command_list`), see `document/LISTS.md` |
| `shot.host.plugin_registry` | `ShotHostPluginRegistryV1` | **host service**: installed plugin states and asynchronous enable/disable |

Current contract version `SHOT_ABI_MAJOR = 2` / `SHOT_ABI_MINOR = 0`: 2.0 removed the old config
catalog service and replaced it with the generic list service `shot.host.plugin_lists` (list data is
a JSON snapshot; registration, value writes and committing all go through `lists.*` events).
Removing a published service is a breaking change, hence major 2;
**since 2.0 a new kind of UI/host data no longer changes the ABI** — a new list id + JSON schema is enough.

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

**Host settings use the same descriptors**: the host implements `shot.configurable` as a `core`
pseudo-plugin (log level, console/file log switches), and the host aggregates every configurable
instance into the `config_list` JSON snapshot, so the settings window renders one set of code.

- Storage keys are `<owner>/<id>`; the **host** persists them to
  `%LOCALAPPDATA%\screen-shot\config.json` — plugins only get/set values and never write config files.
- On startup the host feeds stored values back into plugins (using `default_value` when nothing is
  stored yet), so a plugin never has to read configuration itself.
- Value writes go through `lists.update` (event-driven, per-item results) and persistence through
  `lists.commit`; on success the host publishes the existing `config.applied` (empty payload):
  plugins that cache derived state re-read the current values; there is **no separate reload call**.
- Output/save settings do not belong to the host: whoever implements saving declares those items
  in its own `kConfig[]`.

**Lists: a new data shape never changes the ABI.** `config_list` and the tray menu's
`tray_command_list` come from the same generic list service. To give the UI a new batch of data
(menu items, target lists, …), define a new **list id + JSON schema**, register it at run time with
`lists.add`, and let the host notify consumers with `lists.changed`; **do not add ABI structs or host
services for the data shape**. The full protocol, the list-id registry and the performance rules are
in [`document/LISTS.md`](document/LISTS.md).

The full pipeline (`config_list`, settings plugin, plugin enable/disable and hot unload) is
documented in [`document/SETTINGS.md`](document/SETTINGS.md) (Chinese).

### Settings window and plugin enable/disable

The settings window is a plugin too (`plugins/settings_ui`, implementing `shot.ui_provider`); the
host has no main window:

- it reads the `config_list` JSON snapshot through `shot.host.plugin_lists` and renders the seven
  control kinds (BOOL / INT / FLOAT / STRING / ENUM / COLOR / PATH) from the entry schema; value
  writes go through `lists.update` and persistence through `lists.commit` (tagged event round trips,
  per-item results via `lists.updated` / `config.applied`);
- it manages enable/disable through `shot.host.plugin_registry`. **Disabling is a hot unload**:
  destroy the instances, send `host.shutdown` + `deinit` to UI plugins, unsubscribe from the event
  bus, and `FreeLibrary` once the last instance of the package is gone; enabling reloads the DLL and
  feeds the stored configuration back. A plugin that another loaded plugin `requires` cannot be
  disabled on its own;
- **enabling a plugin cascade-enables its dependencies**: the host first recursively enables every
  provider it `requires`; if a provider fails to enable, the whole request fails with the offending
  dependency named in `reason`;
- the tray (`plugins/tray`) can be disabled and re-enabled: its context menu is driven by
  `tray_command_list` (the host pre-seeds the three core items, plugins contribute via `lists.add`)
  and clicking an item only publishes the event declared by that item; when `tray.main` is not loaded
  (disabled or failed), a resident startup publishes `settings.open` automatically so the settings
  window serves as the fallback entry point;
- enable/disable requests are asynchronous: the settings window only registers them, and the host
  main thread publishes `plugins.changed` after applying them. One enable may cascade several
  dependencies, so the settings window can receive several `plugins.changed` events in a row.

### ABI rules you must follow

1. **Use the /MD runtime (`MultiThreadedDLL`)** on both sides; `/MT` gives you two CRTs.
2. **Match the host's Debug/Release configuration** — a Debug plugin only loads into a Debug host.
3. **Never bundle base dependencies** (`opencv_*.dll` etc.). One copy already sits in `bin/` next to the host exe; a second copy in the plugin directory means two OpenCV instances in one process — split type info, global state and allocators.

   > The host itself **does not link** OpenCV (`CMakeLists.txt` only links `nlohmann_json` and `spdlog`).
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

## Platforms

Windows is the primary development and verification platform (MSVC + vcpkg `x64-windows`).
macOS / Linux branches pick the artifact extension (`SHOT_DYN_EXT`), runtime library lookup
(RPATH) and pruning rules per platform; `plugins/screen_capture` and `plugins/tray` are
platform-specific (GDI capture / Win32 tray) and are skipped by CMake on non-Windows hosts.
`plugins/settings_ui` uses GLFW for a cross-platform window, but only Windows is verified so
far; the unverified POSIX points are listed in `plugins/settings_ui/README.md`.

## License

Not specified yet.
