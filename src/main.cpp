#include <iostream>
#include <vector>
#include "pluginManager.h"
#include "PluginInstances/ImageProcessor.h"

int main() {
    std::cout<< "Starting plugin manager..." << std::endl;
    Plugin::PluginManager manager;

    std::string pluginDir = "./plugins"; // 插件目录
    if (!manager.loadPlugins(pluginDir)) {
        std::cerr << "Failed to load plugins from directory: " << pluginDir << std::endl;
        return 1;
    }
    std::cout << "Plugins loaded successfully." << std::endl;
    auto imageProcessors = manager.getPluginsByType<Plugin::ImageProcessorInstance>(Plugin::PluginType::ImageProcessor);
    std::cout << "Loaded " << imageProcessors.size() << " image processor plugins." << std::endl;
    int w=2,h=2;
    std::vector<uint8_t> imageData(w * h * 4, 255); // 创建一个简单的白色图像
    for(int i=0;i<w*h*4;i+=4){
        imageData[i]=255; // R
        imageData[i+1]=0; // G
        imageData[i+2]=0; // B
        imageData[i+3]=255; // A
    }
    Image image{imageData.data(), w, h, w * 4, 4};

    std::cout<<"Before processing: "
        <<(int)image.data[0]<<","
        <<(int)image.data[1]<<","
        <<(int)image.data[2]<<","
        <<(int)image.data[3]<<std::endl;

    for(auto& processor : imageProcessors){
        processor->process(image);
    }

    std::cout<<"After processing: "
        <<(int)image.data[0]<<","
        <<(int)image.data[1]<<","
        <<(int)image.data[2]<<","
        <<(int)image.data[3]<<std::endl;
}