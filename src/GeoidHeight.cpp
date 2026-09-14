#include "GeoidHeight.h"
#include <GeographicLib/Geoid.hpp>
#include <filesystem>
#include <cstdlib>
#include <cstdio>

namespace GeoidHeight {

bool GeoidCalculator::Initialize(GeoidModel model, const std::string& geoidPath) {
    if (model == GeoidModel::NONE) {
        model_.store(GeoidModel::NONE);
        geoid_.reset();
        fprintf(stderr, "[GeoidHeight] Geoid model set to NONE, no height conversion will be applied\n");
        return true;
    }

    std::string geoidName;
    switch (model) {
        case GeoidModel::EGM84:
            geoidName = "egm84-15";
            break;
        case GeoidModel::EGM96:
            geoidName = "egm96-5";
            break;
        case GeoidModel::EGM2008:
            geoidName = "egm2008-5";
            break;
        default:
            fprintf(stderr, "[GeoidHeight] Unknown geoid model\n");
            return false;
    }

    try {
        std::string actualPath = geoidPath;
        if (actualPath.empty()) {
            actualPath = GetDefaultGeoidDataPath();
        }

        fprintf(stderr, "[GeoidHeight] Initializing geoid model: %s with path: %s\n", geoidName.c_str(), actualPath.c_str());

        auto g = std::make_shared<GeographicLib::Geoid>(geoidName, actualPath, true, true);

        geoid_ = g;
        model_.store(model);

        fprintf(stderr, "[GeoidHeight] Geoid model %s initialized successfully\n", geoidName.c_str());
        fprintf(stderr, "[GeoidHeight] Description: %s\n", g->Description().c_str());
        fprintf(stderr, "[GeoidHeight] DateTime: %s\n", g->DateTime().c_str());
        fprintf(stderr, "[GeoidHeight] Interpolation: %s\n", g->Interpolation().c_str());
        fprintf(stderr, "[GeoidHeight] MaxError: %f m\n", g->MaxError());
        fprintf(stderr, "[GeoidHeight] RMSError: %f m\n", g->RMSError());

        return true;
    } catch (const std::exception& e) {
        fprintf(stderr, "[GeoidHeight] Failed to initialize geoid model %s: %s\n", geoidName.c_str(), e.what());
        geoid_.reset();
        return false;
    }
}

std::optional<double> GeoidCalculator::GetGeoidHeight(double lat, double lon) const {
    auto local_geoid = geoid_;
    if (!local_geoid) {
        return std::nullopt;
    }

    try {
        return (*local_geoid)(lat, lon);
    } catch (const std::exception& e) {
        fprintf(stderr, "[GeoidHeight] Failed to get geoid height at (%f, %f): %s\n", lat, lon, e.what());
        return std::nullopt;
    }
}

double GeoidCalculator::ConvertOrthometricToEllipsoidal(double lat, double lon, double orthometricHeight) const {
    auto geoidHeight = GetGeoidHeight(lat, lon);
    if (geoidHeight) {
        return orthometricHeight + geoidHeight.value();
    }
    return orthometricHeight;
}

double GeoidCalculator::ConvertEllipsoidalToOrthometric(double lat, double lon, double ellipsoidalHeight) const {
    auto geoidHeight = GetGeoidHeight(lat, lon);
    if (geoidHeight) {
        return ellipsoidalHeight - geoidHeight.value();
    }
    return ellipsoidalHeight;
}

std::string GeoidCalculator::GeoidModelToString(GeoidModel model) {
    switch (model) {
        case GeoidModel::NONE: return "none";
        case GeoidModel::EGM84: return "egm84";
        case GeoidModel::EGM96: return "egm96";
        case GeoidModel::EGM2008: return "egm2008";
        default: return "unknown";
    }
}

GeoidModel GeoidCalculator::StringToGeoidModel(const std::string& str) {
    if (str == "egm84" || str == "EGM84") return GeoidModel::EGM84;
    if (str == "egm96" || str == "EGM96") return GeoidModel::EGM96;
    if (str == "egm2008" || str == "EGM2008") return GeoidModel::EGM2008;
    return GeoidModel::NONE;
}

std::string GeoidCalculator::GetDefaultGeoidDataPath() {
    const char* envPath = std::getenv("GEOGRAPHICLIB_GEOID_PATH");
    if (envPath && envPath[0] != '\0') {
        return std::string(envPath);
    }

    const char* dataPath = std::getenv("GEOGRAPHICLIB_DATA");
    if (dataPath && dataPath[0] != '\0') {
        return std::string(dataPath) + "/geoids";
    }

#ifdef _WIN32
    return "C:/ProgramData/GeographicLib/geoids";
#else
    return "/usr/local/share/GeographicLib/geoids";
#endif
}

GeoidCalculator& GetGlobalGeoidCalculator() {
    static GeoidCalculator instance;
    return instance;
}

bool InitializeGlobalGeoidCalculator(GeoidModel model, const std::string& geoidPath) {
    return GetGlobalGeoidCalculator().Initialize(model, geoidPath);
}

}
