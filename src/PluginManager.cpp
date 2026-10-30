#include "plugin/PluginManager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <utility>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace screenshot::plugin {
namespace {

#ifdef _WIN32

std::string to_utf8(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                          utf8.data(), size, nullptr, nullptr);
    return utf8;
}

std::string trim_trailing_whitespace(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
        text.pop_back();
    }
    return text;
}

std::string last_error_text() {
    const DWORD code = ::GetLastError();
    std::string text = "GetLastError=" + std::to_string(static_cast<std::uint32_t>(code));
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    if (length != 0 && buffer != nullptr) {
        text += " " + trim_trailing_whitespace(to_utf8(std::wstring_view(buffer, length)));
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    return text;
}

#endif  // _WIN32

std::string display_path(const std::filesystem::path& path) {
#ifdef _WIN32
    return to_utf8(std::wstring_view(path.native()));
#else
    return path.native();
#endif
}

bool has_plugin_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
#ifdef _WIN32
    return extension == ".dll";
#else
    return extension == ".so" || extension == ".dylib";
#endif
}

}  // namespace

void ProcessorDeleter::operator()(IImageProcessor* processor) const noexcept {
    if (processor == nullptr || destroy == nullptr) {
        return;
    }
    try {
        destroy(processor);
    } catch (...) {
        // 插件销毁器抛异常时无法重试，也绝不允许它击穿宿主。
    }
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

DynamicLibrary::~DynamicLibrary() { close(); }

bool DynamicLibrary::open(const std::filesystem::path& path, std::string& error) {
    close();
#ifdef _WIN32
    const HMODULE module = ::LoadLibraryW(path.c_str());
    if (module == nullptr) {
        error = "LoadLibraryW failed: " + last_error_text();
        return false;
    }
    handle_ = module;
#else
    handle_ = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle_ == nullptr) {
        const char* message = ::dlerror();
        error = std::string("dlopen failed: ") + ((message != nullptr) ? message : "unknown error");
        return false;
    }
#endif
    return true;
}

void DynamicLibrary::close() noexcept {
    if (handle_ == nullptr) {
        return;
    }
#ifdef _WIN32
    ::FreeLibrary(static_cast<HMODULE>(handle_));
#else
    ::dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* DynamicLibrary::symbol(const char* name) const noexcept {
    if (handle_ == nullptr) {
        return nullptr;
    }
#ifdef _WIN32
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    return ::dlsym(handle_, name);
#endif
}

PluginManager::PluginManager(std::filesystem::path plugin_directory) : directory_(std::move(plugin_directory)) {}

PluginManager::~PluginManager() { unload_all(); }

void PluginManager::log(std::string_view first, std::string_view second, std::string_view third) noexcept {
    const std::string_view parts[3] = {first, second, third};
    std::fputs("[plugins] ", stderr);
    bool written = false;
    for (const std::string_view part : parts) {
        if (part.empty()) {
            continue;
        }
        if (written) {
            std::fputs(": ", stderr);
        }
        std::fwrite(part.data(), 1, part.size(), stderr);
        written = true;
    }
    std::fputc('\n', stderr);
    std::fflush(stderr);

    try {
        std::string line;
        for (const std::string_view part : parts) {
            if (part.empty()) {
                continue;
            }
            if (!line.empty()) {
                line += ": ";
            }
            line.append(part);
        }
        diagnostics_.push_back(std::move(line));
    } catch (...) {
        // 诊断记录失败不影响加载/执行流程。
    }
}

void PluginManager::load_all() {
    unload_all();
    diagnostics_.clear();

    std::vector<std::filesystem::path> candidates;
    std::error_code ec;
    if (!std::filesystem::is_directory(directory_, ec)) {
        log("plugin directory not found", display_path(directory_));
        return;
    }

    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory_, ec)) {
        if (!entry.is_regular_file(ec) || !has_plugin_extension(entry.path())) {
            continue;
        }
        candidates.push_back(entry.path());
    }
    if (ec) {
        log("plugin directory scan incomplete", display_path(directory_), ec.message());
    }

    std::sort(candidates.begin(), candidates.end());
    plugins_.reserve(candidates.size());
    for (const std::filesystem::path& candidate : candidates) {
        load_one(candidate);
    }
}

void PluginManager::load_one(const std::filesystem::path& path) {
    const std::string display = display_path(path);

    DynamicLibrary library;
    std::string error;
    if (!library.open(path, error)) {
        log(display, "load failed", error);
        return;
    }

    const auto abi_version_fn = reinterpret_cast<AbiVersionFn>(library.symbol("plugin_abi_version"));
    const auto create_fn = reinterpret_cast<CreateProcessorFn>(library.symbol("plugin_create_processor"));
    const auto destroy_fn = reinterpret_cast<DestroyProcessorFn>(library.symbol("plugin_destroy_processor"));
    if (abi_version_fn == nullptr || create_fn == nullptr || destroy_fn == nullptr) {
        log(display, "missing exported symbol",
            "expected plugin_abi_version / plugin_create_processor / plugin_destroy_processor");
        return;
    }

    int plugin_version = 0;
    try {
        plugin_version = abi_version_fn();
    } catch (...) {
        log(display, "plugin_abi_version threw");
        return;
    }
    if (plugin_version != PLUGIN_ABI_VERSION) {
        log(display, "ABI mismatch",
            "plugin=" + std::to_string(plugin_version) + " host=" + std::to_string(PLUGIN_ABI_VERSION) + ", unloading");
        return;
    }

    IImageProcessor* raw_processor = nullptr;
    try {
        raw_processor = create_fn();
    } catch (...) {
        log(display, "plugin_create_processor threw");
        return;
    }
    if (raw_processor == nullptr) {
        log(display, "plugin_create_processor returned null");
        return;
    }

    // 对象一交给 ProcessorPtr 就由插件销毁器接管：之后任意一步失败，
    // 局部 processor 先销毁对象，再轮到局部 library 卸载动态库。
    ProcessorPtr processor(raw_processor, ProcessorDeleter{destroy_fn});

    std::string name;
    try {
        const char* raw_name = processor->name();
        name = (raw_name != nullptr) ? raw_name : display;
    } catch (...) {
        log(display, "name() threw");
        return;
    }

    const std::string loaded_name = name;
    plugins_.push_back(LoadedPlugin{std::move(library), std::move(processor), std::move(name), path});
    log(display, "loaded", loaded_name);
}

void PluginManager::unload_all() noexcept {
    // 显式逆序释放：后加载的先卸载，且每个插件都严格「先销毁对象、再归还动态库」。
    // 与 LoadedPlugin 的成员声明顺序叠加后，任何退出路径的次序都一致。
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        it->instance.reset();
        it->library.close();
    }
    plugins_.clear();
}

void PluginManager::process_all(Image& image) noexcept {
    for (const LoadedPlugin& plugin : plugins_) {
        if (!plugin.instance) {
            continue;
        }
        try {
            plugin.instance->process(image);
        } catch (const std::exception& error) {
            log(plugin.name, "process() threw", error.what());
        } catch (...) {
            log(plugin.name, "process() threw an unknown exception");
        }
    }
}

std::vector<std::string> PluginManager::loaded_names() const {
    std::vector<std::string> names;
    names.reserve(plugins_.size());
    for (const LoadedPlugin& plugin : plugins_) {
        names.push_back(plugin.name);
    }
    return names;
}

}  // namespace screenshot::plugin
