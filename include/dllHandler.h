#pragma once

#include <string>
#include <stdexcept>

#ifdef _WIN32
    #include <Windows.h>
    using LibHandle = HMODULE;
#else
    #include <dlfcn.h>
    using LibHandle = void*;
#endif

/**
 * @brief 动态库的 RAII 句柄：LoadLibraryA / dlopen 封装。
 *
 * 构造即加载，析构即卸载；不可拷贝。
 */
class DllHandler {
    std::string m_path;
    LibHandle m_handle = nullptr;
public:
    /**
     * @brief 加载一个动态库。
     * @param[in] path 库文件路径。
     * @note 加载失败时抛 std::runtime_error（消息里带路径），没有失败返回的分支。
     */
    DllHandler(const std::string& path) : m_path(path) {
#ifdef _WIN32
        m_handle = LoadLibraryA(path.c_str());
#else
        // TODO: POSIX 分支从未在 macOS/Linux 上验证；跑通后再删这行。
        m_handle = dlopen(path.c_str(), RTLD_LAZY);
#endif
        if (!m_handle) {
            throw std::runtime_error("Failed to load DLL: " + path);
        }
    }

    /** @brief 卸载动态库；句柄为空时是空操作。 */
    ~DllHandler() {
        if (m_handle) {
#ifdef _WIN32
            FreeLibrary(m_handle);
#else
            dlclose(m_handle);
#endif
        }
    }
    /** @brief 禁止拷贝：句柄由唯一所有者持有。 */
    DllHandler(const DllHandler&) = delete;
    /** @brief 禁止赋值：句柄由唯一所有者持有。 */
    DllHandler& operator=(const DllHandler&) = delete;

    /**
     * @brief 取导出符号。
     *
     * 契约只有一个必需符号（shot_module_entry），"找不到"本身就是一种可处理的
     * 结果（这个 DLL 不是插件），所以不提供抛异常的版本。
     * @param[in] funcName 符号名，区分大小写。
     * @return 符号地址；找不到返回 nullptr，不抛异常。
     */
    void* tryGetFunction(const std::string& funcName) noexcept {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(m_handle, funcName.c_str()));
#else
        return dlsym(m_handle, funcName.c_str());
#endif
    }
};
