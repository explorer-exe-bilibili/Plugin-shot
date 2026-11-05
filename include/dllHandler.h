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

class DllHandler {
    std::string m_path;
    LibHandle m_handle = nullptr;
public:
    DllHandler(const std::string& path) : m_path(path) {
#ifdef _WIN32
        m_handle = LoadLibraryA(path.c_str());
#else
        m_handle = dlopen(path.c_str(), RTLD_LAZY);
#endif
        if (!m_handle) {
            throw std::runtime_error("Failed to load DLL: " + path);
        }
    }

    ~DllHandler() {
        if (m_handle) {
#ifdef _WIN32
            FreeLibrary(m_handle);
#else
            dlclose(m_handle);
#endif
        }
    }
    DllHandler(const DllHandler&) = delete;
    DllHandler& operator=(const DllHandler&) = delete;

    // 取符号：找不到返回 nullptr，不抛。
    // 契约只有一个必需符号（shot_module_entry），"找不到"本身就是一种可处理的
    // 结果（这个 DLL 不是插件），所以这里不需要抛异常的版本。
    void* tryGetFunction(const std::string& funcName) noexcept {
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(m_handle, funcName.c_str()));
#else
        return dlsym(m_handle, funcName.c_str());
#endif
    }
};
