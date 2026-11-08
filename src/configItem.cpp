#include "configItem.h"

#include <spdlog/spdlog.h>

#include "base.h"


namespace core{
ConfigItem::ConfigItem(const std::string& value) : value(value) {}
ConfigItem::ConfigItem() : value("") {}

const std::string& ConfigItem::getOriginalValue() const {
    return value;
}

std::string ConfigItem::toString() const {
    return value;
}
std::string ConfigItem::toPath() const {
#ifdef _WIN32
    if(value[0]=='\\' || value[1]==':'){
        return value;
    }
#else
    if(value[0]=='/'){
        return value;
    }
#endif
    else{
        return core::appPath + "/" + value;
    }
}
int ConfigItem::toInt() const {
    try {
        return std::stoi(value);
    } catch (const std::invalid_argument&) {
        spdlog::error("Invalid argument for toInt: {}", value);
        return 0;
    } catch (const std::out_of_range&) {
        spdlog::error("Out of range for toInt: {}", value);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("Exception in toInt: {}", e.what());
        return 0;
    }
}
double ConfigItem::toDouble() const {
    try {
        return std::stod(value);
    } catch (const std::invalid_argument&) {
        spdlog::error("Invalid argument for toDouble: {}", value);
        return 0.0;
    } catch (const std::out_of_range&) {
        spdlog::error("Out of range for toDouble: {}", value);
        return 0.0;
    } catch (const std::exception& e) {
        spdlog::error("Exception in toDouble: {}", e.what());
        return 0.0;
    }
}
bool ConfigItem::toBool() const {
    return value == "true" || value == "1";
}

void ConfigItem::setValue(const std::string& newValue) {
    value = newValue;
}

ConfigItem::operator std::string() const {
    return value;
}
ConfigItem::operator int() const {
    return toInt();
}
ConfigItem::operator double() const {
    return toDouble();
}
ConfigItem::operator bool() const {
    return toBool();
}

}