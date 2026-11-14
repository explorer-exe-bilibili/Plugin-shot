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
        // 隔离测试 / 便携模式：环境变量优先。自检模式（--test-plugin）必须在调用本函数
        // 之前把它指向临时目录，否则日志与配置会落到用户真实数据目录里；Windows 与
        // POSIX 都要认，Linux 的 CI/容器里同样靠它隔离。空串视为未设置，保持原行为。
        const char* overridePath = std::getenv("PLUGIN_SHOT_DATA_DIR");
        if(overridePath != nullptr && overridePath[0] != '\0'){
            dataPath = overridePath;
        } else {
#ifdef _WIN32
            //获取APPDATA路径
            char buffer[MAX_PATH];
            SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffer);
            std::string path(buffer);
            // 目录名与仓库名、包名保持一致（小写）；0.1.0 起对外公布的就是这个路径。
            dataPath=path+"/plugin-shot/";
#else
            // POSIX 用 $HOME；HOME 未设置（极端环境）时退回当前目录，避免空串构造路径。
            const char* home = std::getenv("HOME");
            dataPath=std::string(home != nullptr ? home : ".")+"/.plugin-shot/";
#endif
        }
        // 末尾分隔符一律去掉；用 while 是为了让环境变量里手写的 "dir//" 也归一化。
        while(!dataPath.empty() && (dataPath.back()=='\\'||dataPath.back()=='/')) dataPath.pop_back();
    }
}