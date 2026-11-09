#include"base.h"

#include <cstdint>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <ShlObj_core.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <climits>
#else
#include <unistd.h>
#endif

namespace core{
    std::string appPath;
    std::string dataPath;
    void getAppPath(){
#ifdef _WIN32
        //用Windows API获取当前程序的路径
        char buffer[MAX_PATH];
        GetModuleFileNameA(NULL, buffer, MAX_PATH);
        std::string path(buffer);
        path = path.substr(0, path.find_last_of("\\/"));
        appPath = path;
#else
        // POSIX 没有统一的"当前可执行文件" API：macOS 走 dyld，Linux 读 /proc/self/exe。
        // 两者都可能失败（无 /proc 的容器、被删除的 exe），失败时 appPath 保持空串，
        // 下面的 .back() 必须先判空 —— 否则空串 back() 是未定义行为（实测 macOS 直接崩）。
        std::string path;
#if defined(__APPLE__)
        // _NSGetExecutablePath 不保证返回绝对路径（可能含 "." / ".." / 符号链接），
        // 用 realpath 归一化；归一化失败就退回 dyld 给的原串，至少不是空。
        char rawPath[PATH_MAX] = {};
        std::uint32_t rawSize = sizeof(rawPath);
        if (_NSGetExecutablePath(rawPath, &rawSize) == 0) {
            char resolvedPath[PATH_MAX] = {};
            if (realpath(rawPath, resolvedPath) != nullptr) {
                path = resolvedPath;
            } else {
                path = rawPath;
            }
        }
#else
        char buffer[1024];
        const ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1) {
            buffer[len] = '\0';  // readlink 不补 NUL 结尾
            path = buffer;
        }
#endif
        if (!path.empty()) {
            appPath = path.substr(0, path.find_last_of("\\/"));
        }
#endif
        if(!appPath.empty() && (appPath.back()=='\\'||appPath.back()=='/')) appPath.pop_back();
    }
    void getDataPath(){
#ifdef _WIN32
        //获取APPDATA路径
        char buffer[MAX_PATH];
        SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffer);
        std::string path(buffer);
        dataPath=path+"/screen-shot/";
#else
        // POSIX 用 $HOME；HOME 未设置（极端环境）时退回当前目录，避免空串构造路径。
        const char* home = std::getenv("HOME");
        dataPath=std::string(home != nullptr ? home : ".")+"/.screen-shot/";
#endif
        if(!dataPath.empty() && (dataPath.back()=='\\'||dataPath.back()=='/')) dataPath.pop_back();
    }
}