#pragma once

// ============================================================================
//  跨边界导出宏
//  · 插件侧（默认，不定义任何宏）：__declspec(dllexport) / visibility("default")
//    —— 第三方插件只 #include 契约头即可正确导出契约要求的 C 入口。
//  · 宿主侧（定义 SCREENSHOT_HOST_EXPORTS）：展开为空。
//    宿主从不参与链接期导入，它用 LoadLibraryW + GetProcAddress（POSIX 为
//    dlopen + dlsym）在运行期解析符号。若这里写成 __declspec(dllimport)，
//    宿主 TU 会得到导入声明，而插件 TU 同样会得到 dllimport —— 插件只声明不导入，
//    于是第三方插件必然 LNK2019/LNK2001 链接失败。
//
//  契约只有【一个】导出符号：shot_module_entry()（见 plugin_api.h）。
// ============================================================================

#ifdef _WIN32
    #ifdef SCREENSHOT_HOST_EXPORTS
        #define SCREENSHOT_API
    #else
        #define SCREENSHOT_API __declspec(dllexport)
    #endif
#else
    #ifdef SCREENSHOT_HOST_EXPORTS
        #define SCREENSHOT_API
    #else
        #define SCREENSHOT_API __attribute__((visibility("default")))
    #endif
#endif
