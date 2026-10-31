#pragma once

#include "base.h"
#include "plugin_api.h"

namespace Plugin{
class ImageProcessorInstance : public PluginBase{
    CPluginHandle* m_imageProcessor;
    void (*destoryFn_)(CPluginHandle*);
public:
    ImageProcessorInstance(std::shared_ptr<DllHandler> dllHandle, CPluginHandle* imageProcesser, void (*destoryFn)(CPluginHandle*))
        : PluginBase(dllHandle), m_imageProcessor(imageProcesser), destoryFn_(destoryFn) {}
    ~ImageProcessorInstance()override{
        if(m_imageProcessor && destoryFn_){
            destoryFn_(m_imageProcessor);
        }
    }

    bool process(Image& image){
        if(m_imageProcessor && m_imageProcessor->process){
            return m_imageProcessor->process(m_imageProcessor->internal_data, &image) != 0;
        }
        return false;
    }
    PluginType getType() const override {return PluginType::ImageProcessor;}
    std::string getName() const override {return m_imageProcessor && m_imageProcessor->name ? m_imageProcessor->name : "Unknown";}

    CPluginHandle* getRawHandle() const { return m_imageProcessor; }
};

}