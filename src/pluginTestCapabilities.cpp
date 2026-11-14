// 插件自检的能力检查：按实例的真实类型分别调用插件能力。
//
// 这里的每一段都在"真的调用插件"——这是自检和"--help 列插件"最大的区别：
//   image_processor：构造带行尾填充的合成图，断言成功、字段不变、填充不被踩。
//   image_saver    ：真落盘一张小图，检查文件存在 / 大小 / 魔数，然后删掉。
//   screen_capture ：describe + capture 真抓一张（无显示环境返回 1 → SKIP）。
//   lib_provider   ：只读库名/版本（版本不可解析只 WARN，不判失败）。
//   ui_provider / generic：只报告类型（GUI 类插件会在宿主加载时自己 init，自检不再开界面）。
//
// 线程模型：全部检查都在宿主 main 线程执行；能力回调按契约本来就是同步调用，
// 这里不另起线程。任何插件抛出的 C++ 异常都是契约违规，就地兜底成 FAIL，
// 绝不让异常继续穿过后续检查（宿主整体还有一层 catch 作为最后防线）。
//
// TODO: 只兜底 C++ 异常；Windows 上插件越界访问/栈溢出会直接崩掉自检进程（拿不到
//       JSON）。要隔离访问违例得用 SEH（__try/__except）或独立子进程 + 崩溃转储，
//       而 SEH 与 C++ 对象析构混用有坑，本期不做——第三方插件崩溃请用调试器现场。

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "PluginInstances/ImageProcessor.h"
#include "PluginInstances/ImageSaver.h"
#include "PluginInstances/LibProvider.h"
#include "PluginInstances/ScreenCapture.h"
#include "PluginInstances/UIProvider.h"
#include "pluginTest.h"
#include "pluginTestChecks.h"
#include "shot_semver.h"

namespace Host {

namespace {

namespace fs = std::filesystem;

using Plugin::PluginType;

constexpr std::uint8_t kFillByte = 0xCD;       /**< 合成图的填充字节：任何未声明写的位置都该保持它。 */
constexpr std::int32_t kImageWidth = 64;       /**< 处理检查的合成图宽。 */
constexpr std::int32_t kImageHeight = 48;      /**< 处理检查的合成图高。 */
constexpr std::int32_t kRowPadding = 16;       /**< 故意留的行尾填充字节数。 */
constexpr std::int32_t kSaverWidth = 16;       /**< 保存检查的合成图宽。 */
constexpr std::int32_t kSaverHeight = 12;      /**< 保存检查的合成图高。 */
constexpr std::int32_t kMaxCaptureSide = 16384;/**< describe 尺寸合理性上限，防坏插件把内存撑爆。 */

/**
 * @brief 构造一张 BGRA8 合成图（带行尾填充）。
 * @param[out] buffer 像素缓冲；函数内部分配并填 0xCD。
 * @param[out] image  填好的 ShotImage，data 指向 buffer。
 * @param[in]  width  宽（像素）。
 * @param[in]  height 高（像素）。
 * @param[in]  padding 每行末尾的填充字节数。
 */
void buildSyntheticImage(std::vector<std::uint8_t>& buffer, ShotImage& image, std::int32_t width,
                         std::int32_t height, std::int32_t padding) {
    const std::int32_t stride = width * 4 + padding;
    buffer.assign(static_cast<std::size_t>(height) * static_cast<std::size_t>(stride), kFillByte);
    image = ShotImage{};
    image.struct_size = sizeof(ShotImage);
    image.pixel_format = SHOT_PIXEL_FORMAT_BGRA8;
    image.data = buffer.data();
    image.width = width;
    image.height = height;
    image.stride = stride;
    image.channels = 4;
}

/**
 * @brief 检查 ShotImage 的布局字段是否与插件调用前一致。
 * @param[in] image  插件调用后的图像。
 * @param[in] buffer 原缓冲（用来比对 data 指针）。
 * @param[in] width  期望宽。
 * @param[in] height 期望高。
 * @param[in] stride 期望行距。
 * @param[in] channels 期望通道数。
 * @return true = 全部不变；false = 有字段被插件改了。
 */
bool imageLayoutUnchanged(const ShotImage& image, const std::vector<std::uint8_t>& buffer,
                          std::int32_t width, std::int32_t height, std::int32_t stride,
                          std::int32_t channels) {
    return image.data == buffer.data() && image.width == width && image.height == height &&
           image.stride == stride && image.channels == channels &&
           image.pixel_format == SHOT_PIXEL_FORMAT_BGRA8;
}

/** @brief image_processor：process 成功性、结构体字段不被篡改、行尾填充不被踩、1x1 不崩。 */
void checkImageProcessor(const std::shared_ptr<Plugin::ImageProcessorInstance>& instance,
                         std::vector<PluginTestCheck>& checks) {
    std::vector<std::uint8_t> buffer;
    ShotImage image{};
    buildSyntheticImage(buffer, image, kImageWidth, kImageHeight, kRowPadding);
    const std::int32_t stride = image.stride;

    bool processed = false;
    bool threw = false;
    try {
        processed = instance->process(image);
    } catch (...) {
        threw = true;
    }
    if (threw) {
        addPluginTestCheck(checks, "capability.image_processor.process", PluginTestStatus::Fail,
                           "process 抛出 C++ 异常（契约要求异常不得跨 DLL 边界）");
        return;  // 后面的布局/填充检查没有意义，且插件状态未知。
    }
    addPluginTestCheck(checks, "capability.image_processor.process",
                       processed ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       processed ? "64x48 BGRA8（stride=width*4+16）处理返回 true"
                                 : "process 返回 false（插件返回了非 0）");

    const bool layoutOk = imageLayoutUnchanged(image, buffer, kImageWidth, kImageHeight, stride, 4);
    addPluginTestCheck(checks, "capability.image_processor.layout",
                       layoutOk ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       layoutOk ? "data/width/height/stride/channels/pixel_format 均未被修改"
                                : "插件修改了 ShotImage 的布局字段（契约只允许改像素）");

    // 行尾填充是这类插件最容易踩的越界点：只验证 width*channels 字节以内的写入。
    bool paddingIntact = true;
    std::string firstBroken;
    for (std::int32_t row = 0; row < kImageHeight && paddingIntact; ++row) {
        for (std::int32_t column = kImageWidth * 4; column < stride; ++column) {
            if (buffer[static_cast<std::size_t>(row) * static_cast<std::size_t>(stride) +
                       static_cast<std::size_t>(column)] != kFillByte) {
                paddingIntact = false;
                firstBroken = "第 " + std::to_string(row) + " 行第 " + std::to_string(column) +
                              " 字节";
                break;
            }
        }
    }
    addPluginTestCheck(checks, "capability.image_processor.stride_padding",
                       paddingIntact ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       paddingIntact ? "行尾填充字节（每行 16 字节）未被写入"
                                     : "插件写到了 stride 的行尾填充区（首个越界：" + firstBroken +
                                           "）");

    // 1x1 极小图：只要求不崩、返回成功；不少算法有按行/按块的假设，这里能提前暴露。
    std::vector<std::uint8_t> tinyBuffer;
    ShotImage tinyImage{};
    buildSyntheticImage(tinyBuffer, tinyImage, 1, 1, 0);
    bool tinyOk = false;
    bool tinyThrew = false;
    try {
        tinyOk = instance->process(tinyImage);
    } catch (...) {
        tinyThrew = true;
    }
    const bool tinyLayoutOk = !tinyThrew && imageLayoutUnchanged(tinyImage, tinyBuffer, 1, 1, 4, 4);
    addPluginTestCheck(checks, "capability.image_processor.tiny",
                       (tinyOk && tinyLayoutOk) ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       tinyThrew ? "1x1 图像让 process 抛出 C++ 异常"
                                 : (tinyOk ? "1x1 图像处理返回 true"
                                           : "1x1 图像处理返回 false（可能按行/按块假设了最小尺寸）"));
}

/** @brief 文件头魔数识别结果。 */
enum class SavedFormat {
    Unknown, /**< 不认识（契约外格式或写坏了）。 */
    Png,     /**< PNG（89 50 4E 47）。 */
    Jpeg,    /**< JPEG（FF D8）。 */
    Bmp      /**< BMP（"BM"）。 */
};

/**
 * @brief 用文件头 8 字节识别保存格式。
 * @param[in] path 文件路径。
 * @return 识别结果；文件打不开时返回 Unknown。
 */
SavedFormat detectSavedFormat(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return SavedFormat::Unknown;
    }
    std::uint8_t header[8] = {};
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    const std::streamsize count = stream.gcount();
    if (count >= 4 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E &&
        header[3] == 0x47) {
        return SavedFormat::Png;
    }
    if (count >= 2 && header[0] == 0xFF && header[1] == 0xD8) {
        return SavedFormat::Jpeg;
    }
    if (count >= 2 && header[0] == 'B' && header[1] == 'M') {
        return SavedFormat::Bmp;
    }
    return SavedFormat::Unknown;
}

/** @brief 格式名（写进检查明细）。 */
const char* savedFormatName(SavedFormat format) {
    switch (format) {
        case SavedFormat::Png: return "PNG";
        case SavedFormat::Jpeg: return "JPEG";
        case SavedFormat::Bmp: return "BMP";
        case SavedFormat::Unknown: return "未知";
    }
    return "未知";
}

/** @brief image_saver：真落盘一张小图，检查存在/大小/魔数后清理。 */
void checkImageSaver(const std::shared_ptr<Plugin::ImageSaverInstance>& instance,
                     const fs::path& workDir, std::vector<PluginTestCheck>& checks) {
    std::error_code ec;
    fs::create_directories(workDir, ec);
    if (ec) {
        addPluginTestCheck(checks, "capability.image_saver.directory", PluginTestStatus::Fail,
                           "无法创建保存检查的临时目录 " + workDir.string() + "：" + ec.message());
        return;
    }
    std::vector<std::uint8_t> buffer;
    ShotImage image{};
    buildSyntheticImage(buffer, image, kSaverWidth, kSaverHeight, 0);

    char pathBuffer[1024] = {};
    std::int32_t saveResult = -1;
    bool threw = false;
    try {
        saveResult = instance->save(image, workDir.string().c_str(), "plugin_test_shot", pathBuffer,
                                    static_cast<std::uint32_t>(sizeof(pathBuffer)));
    } catch (...) {
        threw = true;
    }
    if (threw) {
        addPluginTestCheck(checks, "capability.image_saver.save", PluginTestStatus::Fail,
                           "save 抛出 C++ 异常（契约要求异常不得跨 DLL 边界）");
        return;
    }
    addPluginTestCheck(checks, "capability.image_saver.save",
                       saveResult == 0 ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       saveResult == 0 ? "16x12 BGRA8 保存返回 0"
                                       : "save 返回 " + std::to_string(saveResult) +
                                             "（0 才是成功）");
    if (saveResult != 0) {
        return;
    }
    const fs::path savedPath(pathBuffer);
    const bool exists = !savedPath.empty() && fs::exists(savedPath, ec) && !ec &&
                        fs::is_regular_file(savedPath, ec) && !ec;
    addPluginTestCheck(checks, "capability.image_saver.file",
                       exists ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       exists ? "落盘文件存在：" + savedPath.string()
                              : "save 返回成功但 path 指向的文件不存在：" + savedPath.string());
    if (!exists) {
        return;
    }
    const std::uintmax_t fileSize = fs::file_size(savedPath, ec);
    addPluginTestCheck(checks, "capability.image_saver.size",
                       (!ec && fileSize > 0) ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       ec ? "无法读取文件大小：" + ec.message()
                          : "文件大小 " + std::to_string(fileSize) + " 字节");
    const SavedFormat format = detectSavedFormat(savedPath);
    addPluginTestCheck(checks, "capability.image_saver.magic",
                       format != SavedFormat::Unknown ? PluginTestStatus::Pass
                                                      : PluginTestStatus::Fail,
                       format != SavedFormat::Unknown
                           ? std::string("文件头魔数是 ") + savedFormatName(format)
                           : "文件头既不是 PNG/JPEG 也不是 BMP（保存插件必须产出这三种之一）");
    // 清理临时产物：自检不应该在隔离目录之外留文件；目录删不掉时忽略（数据目录会整体删）。
    std::error_code cleanupEc;
    fs::remove(savedPath, cleanupEc);
    fs::remove(workDir, cleanupEc);
}

/** @brief screen_capture：describe 合理性 + 真抓一张（无显示环境返回 1 记 SKIP）。 */
void checkScreenCapture(const std::shared_ptr<Plugin::ScreenCaptureInstance>& instance,
                        std::vector<PluginTestCheck>& checks) {
    ShotCaptureInfo info{};
    std::int32_t describeResult = 2;
    bool threw = false;
    try {
        describeResult = instance->describe(info);
    } catch (...) {
        threw = true;
    }
    if (threw) {
        addPluginTestCheck(checks, "capability.screen_capture.describe", PluginTestStatus::Fail,
                           "describe 抛出 C++ 异常（契约要求异常不得跨 DLL 边界）");
        return;
    }
    if (describeResult == 1) {
        addPluginTestCheck(checks, "capability.screen_capture.describe", PluginTestStatus::Skip,
                           "describe 返回 1：当前没有可采集的屏幕（无显示器/远程会话），按 SKIP 处理");
        return;
    }
    addPluginTestCheck(checks, "capability.screen_capture.describe",
                       describeResult == 0 ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       describeResult == 0 ? "describe 返回 0"
                                           : "describe 返回 " + std::to_string(describeResult) +
                                                 "（0 才是成功）");
    if (describeResult != 0) {
        return;
    }
    const bool sizeOk = info.width > 0 && info.height > 0 && info.width <= kMaxCaptureSide &&
                        info.height <= kMaxCaptureSide;
    const bool formatOk = info.pixel_format == SHOT_PIXEL_FORMAT_BGRA8;
    addPluginTestCheck(checks, "capability.screen_capture.geometry",
                       (sizeOk && formatOk) ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       std::string("describe: ") + std::to_string(info.width) + "x" +
                           std::to_string(info.height) + "，pixel_format=" +
                           std::to_string(info.pixel_format) +
                           (sizeOk ? "" : "（尺寸非法或超过 " + std::to_string(kMaxCaptureSide) +
                                              " 上限）") +
                           (formatOk ? "" : "（不是 BGRA8）"));
    if (!sizeOk || !formatOk) {
        return;
    }

    const std::int32_t stride = info.width * 4;
    const std::size_t byteCount = static_cast<std::size_t>(info.height) *
                                  static_cast<std::size_t>(stride);
    std::vector<std::uint8_t> buffer;
    try {
        buffer.assign(byteCount, 0);
    } catch (const std::exception& exception) {
        addPluginTestCheck(checks, "capability.screen_capture.allocate", PluginTestStatus::Fail,
                           std::string("分配采集缓冲失败（describe 尺寸可能异常）：") +
                               exception.what());
        return;
    }
    ShotImage image{};
    image.struct_size = sizeof(ShotImage);
    image.pixel_format = SHOT_PIXEL_FORMAT_BGRA8;
    image.data = buffer.data();
    image.width = info.width;
    image.height = info.height;
    image.stride = stride;
    image.channels = 4;

    std::int32_t captureResult = 2;
    threw = false;
    try {
        captureResult = instance->capture(image);
    } catch (...) {
        threw = true;
    }
    if (threw) {
        addPluginTestCheck(checks, "capability.screen_capture.capture", PluginTestStatus::Fail,
                           "capture 抛出 C++ 异常（契约要求异常不得跨 DLL 边界）");
        return;
    }
    addPluginTestCheck(checks, "capability.screen_capture.capture",
                       captureResult == 0 ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       captureResult == 0 ? "capture 返回 0"
                                          : "capture 返回 " + std::to_string(captureResult) +
                                                "（0 才是成功）");
    if (captureResult != 0) {
        return;
    }
    const bool layoutOk = imageLayoutUnchanged(image, buffer, info.width, info.height, stride, 4);
    addPluginTestCheck(checks, "capability.screen_capture.layout",
                       layoutOk ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       layoutOk ? "capture 未修改 ShotImage 布局字段（契约只允许写像素）"
                                : "capture 修改了 ShotImage 的布局字段");
    bool anyPixel = false;
    for (std::size_t index = 0; index < buffer.size(); ++index) {
        if (buffer[index] != 0) {
            anyPixel = true;
            break;
        }
    }
    // 全 0 可能是真的黑屏（锁屏/纯色桌面），不能据此判失败；但值得提示用户。
    addPluginTestCheck(checks, "capability.screen_capture.pixels",
                       anyPixel ? PluginTestStatus::Pass : PluginTestStatus::Warn,
                       anyPixel ? "采集缓冲里有非零像素"
                                : "采集缓冲全为 0（可能是全黑画面/锁屏，不判失败）");
}

/** @brief lib_provider：库名与版本非空；版本不可解析只 WARN。 */
void checkLibProvider(const std::shared_ptr<Plugin::LibProvider>& instance,
                      std::vector<PluginTestCheck>& checks) {
    const ShotLibProviderV1* ops = instance->getLibOps();
    if (ops == nullptr || ops->get_library_name == nullptr) {
        addPluginTestCheck(checks, "capability.lib_provider.name", PluginTestStatus::Fail,
                           "实例被包装成 lib_provider，但函数表缺少 get_library_name");
        return;
    }
    const char* libraryName = nullptr;
    bool threw = false;
    try {
        libraryName = ops->get_library_name(instance->nativeHandle());
    } catch (...) {
        threw = true;
    }
    if (threw) {
        addPluginTestCheck(checks, "capability.lib_provider.name", PluginTestStatus::Fail,
                           "get_library_name 抛出 C++ 异常（契约要求异常不得跨 DLL 边界）");
        return;
    }
    const bool nameOk = libraryName != nullptr && libraryName[0] != '\0';
    addPluginTestCheck(checks, "capability.lib_provider.name",
                       nameOk ? PluginTestStatus::Pass : PluginTestStatus::Fail,
                       nameOk ? std::string("库名：") + libraryName
                              : "get_library_name 返回空（依赖提供者必须报告库名）");

    const std::string version = instance->getLibraryVersion();
    if (version.empty()) {
        addPluginTestCheck(checks, "capability.lib_provider.version", PluginTestStatus::Fail,
                           "get_library_version 返回空（依赖版本校验需要它）");
        return;
    }
    ShotVersion parsed;
    const bool parseable = parsed.parse(version);
    addPluginTestCheck(checks, "capability.lib_provider.version",
                       parseable ? PluginTestStatus::Pass : PluginTestStatus::Warn,
                       parseable ? "库版本：" + version
                                 : "库版本 \"" + version +
                                       "\" 不是宿主能解析的数字版本（依赖校验可能失败）");
}

}  // namespace

void runCapabilityChecks(const std::shared_ptr<Plugin::PluginBase>& instance,
                         const std::string& workDir, std::vector<PluginTestCheck>& checks) {
    if (instance == nullptr) {
        addPluginTestCheck(checks, "capability", PluginTestStatus::Fail, "插件实例为空");
        return;
    }
    switch (instance->getType()) {
        case PluginType::ImageProcessor: {
            const auto typed = std::dynamic_pointer_cast<Plugin::ImageProcessorInstance>(instance);
            if (typed == nullptr) {
                addPluginTestCheck(checks, "capability.image_processor",
                                   PluginTestStatus::Fail,
                                   "类型标记是 image_processor，但实例无法转换成对应包装类");
                return;
            }
            checkImageProcessor(typed, checks);
            return;
        }
        case PluginType::ImageSaver: {
            const auto typed = std::dynamic_pointer_cast<Plugin::ImageSaverInstance>(instance);
            if (typed == nullptr) {
                addPluginTestCheck(checks, "capability.image_saver", PluginTestStatus::Fail,
                                   "类型标记是 image_saver，但实例无法转换成对应包装类");
                return;
            }
            checkImageSaver(typed, fs::path(workDir) / "saver", checks);
            return;
        }
        case PluginType::ScreenCapture: {
            const auto typed = std::dynamic_pointer_cast<Plugin::ScreenCaptureInstance>(instance);
            if (typed == nullptr) {
                addPluginTestCheck(checks, "capability.screen_capture", PluginTestStatus::Fail,
                                   "类型标记是 screen_capture，但实例无法转换成对应包装类");
                return;
            }
            checkScreenCapture(typed, checks);
            return;
        }
        case PluginType::Lib: {
            const auto typed = std::dynamic_pointer_cast<Plugin::LibProvider>(instance);
            if (typed == nullptr) {
                addPluginTestCheck(checks, "capability.lib_provider", PluginTestStatus::Fail,
                                   "类型标记是 lib_provider，但实例无法转换成对应包装类");
                return;
            }
            checkLibProvider(typed, checks);
            return;
        }
        case PluginType::UIProvider:
            // GUI 类插件在宿主加载（createPlugin -> UIProvider::init）时已经初始化过，
            // 自检不该再打开/关闭一次界面（会抢焦点、动系统托盘）。只报告类型。
            addPluginTestCheck(checks, "capability.ui_provider", PluginTestStatus::Skip,
                               "GUI 类插件只做加载检查，不重复 init/deinit 界面");
            return;
        case PluginType::Generic:
            addPluginTestCheck(checks, "capability.generic", PluginTestStatus::Skip,
                               "通用插件（纯配置/事件订阅）没有宿主可直接调用的能力表，只验证加载、"
                               "配置项与列表贡献");
            return;
        case PluginType::None:
            addPluginTestCheck(checks, "capability", PluginTestStatus::Warn,
                               "实例类型为 None（未分类），跳过能力检查");
            return;
    }
}

}  // namespace Host
