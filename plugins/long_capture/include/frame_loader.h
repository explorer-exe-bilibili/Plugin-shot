#pragma once

// ============================================================================
//  include/frame_loader.h —— 从 capture.done 的落盘文件解码一帧（平台无关核心）
//
//  为什么不用 stbi_load(path)：stb 的按文件名接口在 Windows 上吃 char*（ANSI），
//  中文用户目录会打不开；这里先用 std::ifstream + std::filesystem::path（UTF-8
//  构造）把整个文件读进内存，再交给 stbi_load_from_memory（与 clipboard 插件
//  同一套写法，见 plugins/clipboard/src/clipboard_writer.cpp）。
//
//  像素统一成 RGBA（4 字节/像素）：PNG/JPEG 都能解码，行间无 padding，
//  后续画布行列拷贝就是一次 memcpy。
// ============================================================================

#include <string>

#include "capture_frame.h"

namespace long_capture {

/**
 * @brief 截屏帧解码器：把 image_saver 落盘的 PNG/JPEG 读成 RGBA 帧。
 *
 * 无状态、任意线程可调；解码失败的异常/错误码都收敛成 false + 中文原因。
 */
class FrameLoader {
public:
    /**
     * @brief 读取并解码一个图像文件。
     *
     * @param[in]  utf8Path 宿主 capture.done 给出的路径（UTF-8）；允许相对路径（相对宿主当前目录）。
     * @param[out] out      成功时填好 width/height/rgba；失败时保持调用前的内容（调用方负责 release）。
     * @param[out] error    失败时的中文原因；成功时清空。
     * @return true = 解码成功（RGBA8）。
     * @note 文件大小上限 64 MiB：长截图单帧远小于它，超过多半是路径指向了别的东西。
     */
    static bool load(const std::string& utf8Path, CaptureFrame& out, std::string& error);

    /** @brief 读取整个文件到字节缓冲（UTF-8 路径），供解码与排障复用。 */
    static bool readFile(const std::string& utf8Path, std::vector<std::uint8_t>& bytes,
                         std::string& error);
};

}  // namespace long_capture
