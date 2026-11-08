#include"base.h"

#ifdef _WIN32
#include <windows.h>
#include <ShlObj_core.h>
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
        //用Linux API获取当前程序的路径
        char buffer[1024];
        ssize_t len = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
        if (len != -1) {
            buffer[len] = '\0';
            std::string path(buffer);
            path = path.substr(0, path.find_last_of("\\/"));
            appPath = path;
        }
#endif
        if(appPath.back()=='\\'||appPath.back()=='/') appPath.pop_back();
    }
    void getDataPath(){
#ifdef _WIN32
        //获取APPDATA路径
        char buffer[MAX_PATH];
        SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, buffer);
        std::string path(buffer);
        dataPath=path+"/screen-shot/";
#else
        const char* home = std::getenv("HOME");
        dataPath=std::string(home)+"/.screen-shot/";
#endif
        if(dataPath.back()=='\\'||dataPath.back()=='/') dataPath.pop_back();
    }
}