#pragma once

#include <string>
#include <memory>

#include "dllHander.h"

namespace Plugin{
enum PluginType{
    Window,
    Tray,
    ImageProcessor
};
class PluginBase{
    std::shared_ptr<DllHandler> m_dllHandle;
public:
    PluginBase(std::shared_ptr<DllHandler> dllHandle):m_dllHandle(dllHandle){}
    virtual ~PluginBase() = default;
    virtual std::string getName() const = 0;
    virtual PluginType getType() const = 0;
};
}