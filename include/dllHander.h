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

        void* getFunction(const std::string& funcName) {
#ifdef _WIN32
            void* func = reinterpret_cast<void*>(GetProcAddress(m_handle, funcName.c_str()));
            if (!func) {
                throw std::runtime_error("Failed to get function: " + funcName);
            }
            return func;
#else
            void* func = dlsym(m_handle, funcName.c_str());
            if (!func) {
                throw std::runtime_error("Failed to get function: " + funcName);
            }
            return func;
#endif
        }
};
