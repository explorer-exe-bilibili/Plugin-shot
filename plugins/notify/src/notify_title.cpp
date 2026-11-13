// 通知标题模板的展开：把 {path} / {width} / {height} 换成实际值。
//
// 只用朴素的单趟扫描做替换：一次从左到右扫描模板，命中占位符就整段追加替换值并跳过
// 占位符长度，**不回头重新扫描**刚刚插入的内容。这样即使图片路径里恰好含有
// "{path}"，也不会像"替换后再次 find"的写法那样原地打转（用户可控输入导致的
// 死循环是这类字符串替换最典型的坑）。扫描过程中不需要正则、不需要模板引擎。

#include "notify_title.h"

#include <cstddef>
#include <string_view>
#include <utility>

namespace notify {
namespace {

/** @brief 一个占位符与它对应的替换值；生命周期只在一次 expandTitle 调用内。 */
struct Placeholder {
    std::string_view token;   /**< 占位符原文，如 "{path}"。 */
    const std::string* value; /**< 替换值；空串表示"这里没有信息"。 */
};

}  // namespace

std::string expandTitle(const std::string& titleTemplate, const std::string& path, int width,
                        int height) {
    if (titleTemplate.empty()) {
        return std::string();  // 空模板 = 用默认多语言标题，交给调用方回落
    }
    // 宽高未知（失败通知 / 老宿主没回报）时替换成空串，而不是 "0"：用户写
    // "{width}x{height}" 时不会显示成 "0x0"。
    const std::string widthText = (width > 0) ? std::to_string(width) : std::string();
    const std::string heightText = (height > 0) ? std::to_string(height) : std::string();
    const Placeholder placeholders[] = {
        {"{path}", &path},
        {"{width}", &widthText},
        {"{height}", &heightText},
    };

    std::string result;
    result.reserve(titleTemplate.size() + path.size());
    std::size_t index = 0;
    while (index < titleTemplate.size()) {
        bool replaced = false;
        if (titleTemplate[index] == '{') {
            for (const Placeholder& placeholder : placeholders) {
                if (titleTemplate.compare(index, placeholder.token.size(), placeholder.token) == 0) {
                    result.append(*placeholder.value);
                    index += placeholder.token.size();
                    replaced = true;  // 替换值不再参与匹配
                    break;
                }
            }
        }
        if (!replaced) {
            result.push_back(titleTemplate[index]);
            ++index;
        }
    }
    return result;
}

}  // namespace notify
