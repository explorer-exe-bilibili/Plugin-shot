#include "config.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "version.h"

namespace core {

std::string Config::ConfigFilePath;

Config::Config(bool isDefault) : isDefault(isDefault) {}

Config& Config::getInstance(bool isDefault) {
    if (isDefault) {
        static Config defaultInstance(true);
        return defaultInstance;
    }
    static Config instance(false);
    return instance;
}

void Config::init(const std::string& configFilePath) {
    ConfigFilePath = configFilePath;
    readConfigFile();
    setDefaultConfig();
}

void Config::readConfigFile() {
    std::lock_guard<std::mutex> lock(rwMutex);
    std::ifstream configFile(ConfigFilePath);
    if (!configFile.is_open()) {
        // 首次运行没有配置文件是正常状态：安静记录一条 info 就返回，
        // 第一次 saveConfigFile() 会把它建出来。文件存在却打不开才是真错误。
        if (std::filesystem::exists(ConfigFilePath)) {
            spdlog::error("Failed to open config file: {}", ConfigFilePath);
        } else {
            spdlog::info("Config file not found, starting with defaults: {}", ConfigFilePath);
        }
        return;
    }

    nlohmann::json configJson;
    try {
        configJson = nlohmann::json::parse(configFile);
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse config file: {} Error info: {}", ConfigFilePath, e.what());
    } catch (...) {
        spdlog::error("Failed to parse config file: {} Error info: Unknown error", ConfigFilePath);
    }

    const std::string configVersion = configJson.value("version", "0");
    if (configVersion != VERSION_STRING) {
        spdlog::info("Config file version mismatch. Expected: {}, Found: {}; updating config file",
                     VERSION_STRING, configVersion);
        configJson["version"] = VERSION_STRING;
    }

    try {
        const nlohmann::json configItemsJson =
            configJson.value("configItems", nlohmann::json::object());
        for (const auto& configItemJson : configItemsJson) {
            configItems[configItemJson.value("name", "")] = ConfigItem(configItemJson.value("value", ""));
        }
    } catch (...) {
        spdlog::error("Failed to read config items from config file: {}", ConfigFilePath);
    }
}

ConfigItem& Config::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(rwMutex);
    if (configItems.find(key) == configItems.end()) {
        if (isDefault) {
            spdlog::error("Default config item not found: {}", key);
            return configItems[""];
        }
        spdlog::warn("Config item not found: {}", key);
        return Config::getInstance(true).get(key);
    }
    return configItems[key];
}

bool Config::contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(rwMutex);
    return configItems.find(key) != configItems.end();
}

void Config::set(const std::string& key, const ConfigItem& value) {
    std::lock_guard<std::mutex> lock(rwMutex);
    configItems[key] = value;
}

bool Config::setifno(const std::string& key, const ConfigItem& value) {
    if (!isDefault) {
        Config::getInstance(true).set(key, value);
    }
    if (configItems.find(key) == configItems.end()) {
        set(key, value);
        return true;
    }
    return false;
}

bool Config::saveConfigFile() {
    std::lock_guard<std::mutex> lock(rwMutex);
    nlohmann::json configJson;
    configJson["version"] = VERSION_STRING;
    configJson["fullVersion"] = VERSION_FULL_STRING;
    configJson["appName"] = "screen-shot";
    nlohmann::json configItemsJson;
    for (const auto& [key, value] : configItems) {
        nlohmann::json configItemJson;
        configItemJson["name"] = key;
        configItemJson["value"] = value.getOriginalValue();
        configItemsJson.push_back(configItemJson);
    }
    configJson["configItems"] = configItemsJson;

    spdlog::info("Saving config file: {}", ConfigFilePath);
    std::ofstream configFile(ConfigFilePath);
    if (!configFile.is_open()) {
        spdlog::error("Failed to open config file for writing: {}", ConfigFilePath);
        return false;
    }
    try {
        configFile << configJson.dump(4);
    } catch (const std::exception& e) {
        spdlog::error("Failed to write config file: {} Error info: {}", ConfigFilePath, e.what());
        return false;
    } catch (...) {
        spdlog::error("Failed to write config file: {} Error info: Unknown error", ConfigFilePath);
        return false;
    }
    if (!configFile) {
        // ofstream 默认不抛异常：磁盘满/权限问题只会置 failbit，必须自己查。
        spdlog::error("Failed to flush config file: {}", ConfigFilePath);
        return false;
    }
    return true;
}

}  // namespace core
