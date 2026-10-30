#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "plugin_api.h"

namespace screenshot::plugin {

using AbiVersionFn = int (*)();
using CreateProcessorFn = IImageProcessor* (*)();
using DestroyProcessorFn = void (*)(IImageProcessor*);

// 插件对象的销毁器：永远调用插件自己导出的 plugin_destroy_processor。
// 宿主侧 delete 会在宿主 CRT 堆上释放插件 CRT 分配的块，是必然的堆破坏。
struct ProcessorDeleter {
    DestroyProcessorFn destroy = nullptr;
    void operator()(IImageProcessor* processor) const noexcept;
};

using ProcessorPtr = std::unique_ptr<IImageProcessor, ProcessorDeleter>;

// RAII 动态库句柄：open 取得句柄，析构即 FreeLibrary / dlclose。
class DynamicLibrary {
public:
    DynamicLibrary() noexcept = default;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;
    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    ~DynamicLibrary();

    // 失败时 error 里带 GetLastError/FormatMessage（Win）或 dlerror()（POSIX）。
    bool open(const std::filesystem::path& path, std::string& error);
    void close() noexcept;
    void* symbol(const char* name) const noexcept;

private:
    void* handle_ = nullptr;
};

struct LoadedPlugin {
    // 【顺序即析构保证】成员析构按声明的逆序进行：instance 声明在 library 之后，
    // 因此任何路径下都是先销毁插件对象（plugin_destroy_processor），
    // 再让 DynamicLibrary 归还动态库（FreeLibrary / dlclose）。
    DynamicLibrary library;
    ProcessorPtr instance;
    std::string name;
    std::filesystem::path path;
};

// 负责：扫描目录 -> 校验 ABI -> 造对象 -> 持有 -> 逆序卸载。
class PluginManager {
public:
    explicit PluginManager(std::filesystem::path plugin_directory);
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    // 先 unload_all() 再重新扫描；单个插件失败只记诊断，不影响其它插件。
    void load_all();

    // 逆序显式释放：先 plugin_destroy_processor，再 FreeLibrary/dlclose。不分配内存。
    void unload_all() noexcept;

    // 逐插件调用 process()；某个插件抛异常只记诊断，绝不中断其它插件或击穿宿主。
    void process_all(Image& image) noexcept;

    [[nodiscard]] std::size_t loaded_count() const noexcept { return plugins_.size(); }
    [[nodiscard]] std::vector<std::string> loaded_names() const;
    [[nodiscard]] const std::vector<std::string>& diagnostics() const noexcept { return diagnostics_; }

private:
    void load_one(const std::filesystem::path& path);

    // 分片拼一行日志，全过程不抛异常（供 noexcept 路径调用）。
    void log(std::string_view first, std::string_view second = {}, std::string_view third = {}) noexcept;

    std::filesystem::path directory_;
    std::vector<LoadedPlugin> plugins_;
    std::vector<std::string> diagnostics_;
};

}  // namespace screenshot::plugin
