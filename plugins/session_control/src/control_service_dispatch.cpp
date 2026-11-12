// D-Bus 接口描述与方法的翻译表：org.pluginshot.Control → 事件总线主题。
//
//   Capture()        → capture.request（空负载 = 整屏）
//   CaptureRegion()  → capture.request {"mode":"region"}（请开始框选；流水线忽略，
//                      region_selector 收到后弹覆盖层，用户确认后再发带 region 的请求）
//   OpenSettings()   → settings.open
//   CaptureLong()    → long_capture.start（滚动长图能力的预留入口；没有订阅者时无效果）
//
// 每个方法都回复 (b ok, s message)：publish 返回 false 说明宿主没有事件总线或队列满，
// 原因原样回给 gdbus 调用方，方便用户在终端里看到失败而不是"什么都没发生"。
//
// 线程：本文件的回调全部由 GDBus 在 ControlService 的服务线程上派发（该线程 push 了
// 私有 GMainContext）；EventBus::publish 任意线程可调，所以这里不需要再切线程。

#include "control_service_impl.h"

#include <cstring>
#include <string>

#include "shot_plugin_helpers.h"

namespace session_control {
namespace detail {
namespace {

/**
 * @brief D-Bus 方法与事件主题的固定映射。
 *
 * 表是静态常量：接口名、方法名与主题一起改在这里，方法回调只做查表，避免
 * "方法名写一份、日志再写一份"的漂移。
 */
struct MethodBinding {
    const char* name;     /**< D-Bus 方法名。 */
    const char* topic;    /**< 发布到事件总线的主题。 */
    const char* payload;  /**< 事件负载（空串 = 无负载）。 */
    const char* success;  /**< 发布成功时回给调用方的消息。 */
};

const MethodBinding kMethodBindings[] = {
    {"Capture", "capture.request", "", "已发布 capture.request（整屏截图）"},
    {"CaptureRegion", "capture.request", R"({"mode":"region"})",
     "已发布 capture.request（区域截图，等待框选）"},
    {"OpenSettings", "settings.open", "", "已发布 settings.open"},
    // TODO: 长截图能力还没有任何消费者订阅 long_capture.start；等长截图插件订阅它之后
    //       这个入口才有实际效果，在那之前它只发布事件并回报成功。
    {"CaptureLong", "long_capture.start", "", "已发布 long_capture.start（无订阅者时无效果）"},
};

const char kIntrospectionXml[] = R"xml(<node>
  <interface name="org.pluginshot.Control">
    <method name="Capture">
      <arg type="b" name="ok" direction="out"/>
      <arg type="s" name="message" direction="out"/>
    </method>
    <method name="CaptureRegion">
      <arg type="b" name="ok" direction="out"/>
      <arg type="s" name="message" direction="out"/>
    </method>
    <method name="OpenSettings">
      <arg type="b" name="ok" direction="out"/>
      <arg type="s" name="message" direction="out"/>
    </method>
    <method name="CaptureLong">
      <arg type="b" name="ok" direction="out"/>
      <arg type="s" name="message" direction="out"/>
    </method>
  </interface>
</node>)xml";

/**
 * @brief 按方法名查固定映射。
 * @param[in] name D-Bus 方法名，可为 nullptr。
 * @return 匹配项；找不到返回 nullptr。
 */
const MethodBinding* findBinding(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (const MethodBinding& binding : kMethodBindings) {
        if (std::strcmp(binding.name, name) == 0) {
            return &binding;
        }
    }
    return nullptr;
}

/**
 * @brief 统一回复 (b ok, s message)。
 * @param[in] invocation 回复句柄，不能为空。
 * @param[in] ok         事件是否已入队。
 * @param[in] message    给调用方看的说明；允许为 nullptr（按空串回复）。
 */
void reply(GDBusMethodInvocation* invocation, bool ok, const char* message) {
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(bs)", static_cast<gboolean>(ok), (message != nullptr) ? message : ""));
}

/**
 * @brief GDBus 的方法调用入口：转给 ControlServiceImpl。
 *
 * 签名由 GDBus 固定，只能是自由函数（STYLE §6.7 的合法例外）；user_data 是
 * g_dbus_connection_register_object 注册时传入的 ControlServiceImpl*。
 */
void onMethodCall(GDBusConnection* /*connection*/, const gchar* /*sender*/,
                  const gchar* /*objectPath*/, const gchar* /*interfaceName*/,
                  const gchar* methodName, GVariant* parameters,
                  GDBusMethodInvocation* invocation, gpointer userData) {
    auto* impl = static_cast<ControlServiceImpl*>(userData);
    if (impl == nullptr || invocation == nullptr) {
        return;
    }
    impl->dispatchMethod(methodName, parameters, invocation);
}

}  // namespace

const char* introspectionXml() { return kIntrospectionXml; }

const GDBusInterfaceVTable& methodVTable() {
    // 静态只读：GDBus 只保存指针，注销对象之前必须一直有效（进程生命周期）。
    // 不用聚合初始化是为了不依赖 GDBusInterfaceVTable 里的 padding 字段个数。
    static const GDBusInterfaceVTable vtable = [] {
        GDBusInterfaceVTable table{};
        table.method_call = &onMethodCall;
        return table;
    }();
    return vtable;
}

void ControlServiceImpl::dispatchMethod(const char* methodName, GVariant* parameters,
                                        GDBusMethodInvocation* invocation) {
    if (invocation == nullptr) {
        return;
    }
    const shot::Logger log(host);
    if (methodName == nullptr) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD, "方法名为空");
        return;
    }
    if (parameters != nullptr && g_variant_n_children(parameters) != 0) {
        reply(invocation, false, "参数错误：本接口的方法都不接受参数");
        log.warn(std::string("[session_control] D-Bus 方法 ") + methodName + " 带了多余参数，已拒绝");
        return;
    }
    const MethodBinding* binding = findBinding(methodName);
    if (binding == nullptr) {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
                                              G_DBUS_ERROR_UNKNOWN_METHOD, "未知方法：%s",
                                              methodName);
        return;
    }
    const bool published = shot::EventBus(host).publish(binding->topic, binding->payload);
    if (published) {
        reply(invocation, true, binding->success);
        // 成功只记 DEBUG：桌面快捷键可能被连按，INFO 会把日志刷满。
        log.debug(std::string("[session_control] D-Bus ") + methodName + " → " + binding->topic);
    } else {
        reply(invocation, false, "事件发布失败：宿主事件总线不可用或事件队列已满");
        log.warn(std::string("[session_control] D-Bus ") + methodName + " 发布 " +
                 binding->topic + " 失败（事件总线不可用或队列满）");
    }
}

}  // namespace detail
}  // namespace session_control
