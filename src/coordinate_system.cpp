#include "coordinate_system.h"
#include <sstream>

namespace coords {

CoordinateSystem CoordinateSystem::LocalCartesian(UpAxis up_axis, Handedness handedness) {
    return LocalCartesian(LocalCartesianParams{up_axis, handedness});
}

CoordinateSystem CoordinateSystem::LocalCartesian(const LocalCartesianParams& params) {
    CoordinateSystem cs;
    cs.type_ = CoordinateType::LocalCartesian;
    cs.params_ = params;
    return cs;
}

CoordinateSystem CoordinateSystem::ENU(double lon, double lat, double height,
                                       double offset_x, double offset_y, double offset_z) {
    CoordinateSystem cs;
    cs.type_ = CoordinateType::ENU;
    cs.params_ = ENUParams{lon, lat, height, offset_x, offset_y, offset_z};
    return cs;
}

CoordinateSystem CoordinateSystem::EPSG(int code,
                                        double origin_x, double origin_y, double origin_z,
                                        VerticalDatum vertical_datum) {
    CoordinateSystem cs;
    cs.type_ = CoordinateType::EPSG;
    cs.params_ = EPSGParams{code, origin_x, origin_y, origin_z, vertical_datum};
    return cs;
}

CoordinateSystem CoordinateSystem::WKT(const std::string& wkt,
                                       double origin_x, double origin_y, double origin_z,
                                       VerticalDatum vertical_datum) {
    CoordinateSystem cs;
    cs.type_ = CoordinateType::WKT;
    cs.params_ = WKTParams{wkt, origin_x, origin_y, origin_z, vertical_datum};
    return cs;
}

bool CoordinateSystem::NeedsOGRTransform() const {
    // EPSG和WKT类型需要通过OGR进行坐标转换
    return type_ == CoordinateType::EPSG || type_ == CoordinateType::WKT;
}

bool CoordinateSystem::HasBuiltinGeoReference() const {
    // ENU类型自带地理参考信息
    return type_ == CoordinateType::ENU;
}

std::optional<GeoReference> CoordinateSystem::GetBuiltinGeoReference() const {
    if (type_ == CoordinateType::ENU) {
        const auto& p = std::get<ENUParams>(params_);
        return p.GetGeoReference();
    }
    return std::nullopt;
}

std::tuple<double, double, double> CoordinateSystem::GetSourceOrigin() const {
    switch (type_) {
        case CoordinateType::ENU: {
            // ENU返回SRSOrigin偏移量
            const auto& p = std::get<ENUParams>(params_);
            return {p.offset_x, p.offset_y, p.offset_z};
        }
        case CoordinateType::EPSG: {
            // EPSG返回投影坐标原点
            const auto& p = std::get<EPSGParams>(params_);
            return {p.origin_x, p.origin_y, p.origin_z};
        }
        case CoordinateType::WKT: {
            // WKT返回投影坐标原点
            const auto& p = std::get<WKTParams>(params_);
            return {p.origin_x, p.origin_y, p.origin_z};
        }
        case CoordinateType::LocalCartesian:
        case CoordinateType::Unknown:
        default:
            // 本地笛卡尔和未知类型无原点概念
            return {0.0, 0.0, 0.0};
    }
}

std::optional<ENUParams> CoordinateSystem::GetENUParams() const {
    if (type_ == CoordinateType::ENU) {
        return std::get<ENUParams>(params_);
    }
    return std::nullopt;
}

std::optional<LocalCartesianParams> CoordinateSystem::GetLocalCartesianParams() const {
    if (type_ == CoordinateType::LocalCartesian) {
        return std::get<LocalCartesianParams>(params_);
    }
    return std::nullopt;
}

std::optional<int> CoordinateSystem::GetEPSGCode() const {
    if (type_ == CoordinateType::EPSG) {
        return std::get<EPSGParams>(params_).code;
    }
    return std::nullopt;
}

std::optional<std::string> CoordinateSystem::GetWKTString() const {
    if (type_ == CoordinateType::WKT) {
        return std::get<WKTParams>(params_).wkt;
    }
    return std::nullopt;
}

VerticalDatum CoordinateSystem::GetVerticalDatum() const {
    switch (type_) {
        case CoordinateType::EPSG:
            return std::get<EPSGParams>(params_).vertical_datum;
        case CoordinateType::WKT:
            return std::get<WKTParams>(params_).vertical_datum;
        case CoordinateType::ENU:
            // ENU默认使用椭球高
            return VerticalDatum::Ellipsoidal;
        case CoordinateType::LocalCartesian:
            // 本地笛卡尔的高度由用户指定，假设为椭球高
            return VerticalDatum::Ellipsoidal;
        default:
            return VerticalDatum::Unknown;
    }
}

void CoordinateSystem::SetVerticalDatum(VerticalDatum datum) {
    switch (type_) {
        case CoordinateType::EPSG:
            std::get<EPSGParams>(params_).vertical_datum = datum;
            break;
        case CoordinateType::WKT:
            std::get<WKTParams>(params_).vertical_datum = datum;
            break;
        default:
            // 其他类型不支持设置高程基准
            break;
    }
}

UpAxis CoordinateSystem::GetUpAxis() const {
    if (type_ == CoordinateType::LocalCartesian) {
        return std::get<LocalCartesianParams>(params_).up_axis;
    }
    // 默认Y轴向上(与glTF/3D Tiles一致)
    return UpAxis::Y_UP;
}

Handedness CoordinateSystem::GetHandedness() const {
    if (type_ == CoordinateType::LocalCartesian) {
        return std::get<LocalCartesianParams>(params_).handedness;
    }
    // 默认右手坐标系
    return Handedness::RightHanded;
}

std::string CoordinateSystem::ToString() const {
    std::ostringstream oss;
    switch (type_) {
        case CoordinateType::Unknown:
            oss << "CoordinateSystem(Unknown)";
            break;
        case CoordinateType::LocalCartesian: {
            const auto& p = std::get<LocalCartesianParams>(params_);
            oss << "CoordinateSystem(LocalCartesian, up_axis="
                << (p.up_axis == UpAxis::Y_UP ? "Y_UP" : "Z_UP")
                << ", handedness="
                << (p.handedness == Handedness::RightHanded ? "Right" : "Left")
                << ")";
            break;
        }
        case CoordinateType::ENU: {
            const auto& p = std::get<ENUParams>(params_);
            oss << "CoordinateSystem(ENU, origin=("
                << p.origin_lon << ", " << p.origin_lat << ", " << p.origin_height
                << "), offset=("
                << p.offset_x << ", " << p.offset_y << ", " << p.offset_z
                << "))";
            break;
        }
        case CoordinateType::EPSG: {
            const auto& p = std::get<EPSGParams>(params_);
            oss << "CoordinateSystem(EPSG:" << p.code
                << ", origin=(" << p.origin_x << ", " << p.origin_y << ", " << p.origin_z
                << "), datum="
                << (p.vertical_datum == VerticalDatum::Ellipsoidal ? "Ellipsoidal" :
                    p.vertical_datum == VerticalDatum::Orthometric ? "Orthometric" : "Unknown")
                << ")";
            break;
        }
        case CoordinateType::WKT: {
            const auto& p = std::get<WKTParams>(params_);
            oss << "CoordinateSystem(WKT, origin=("
                << p.origin_x << ", " << p.origin_y << ", " << p.origin_z
                << "), datum="
                << (p.vertical_datum == VerticalDatum::Ellipsoidal ? "Ellipsoidal" :
                    p.vertical_datum == VerticalDatum::Orthometric ? "Orthometric" : "Unknown")
                << ")";
            break;
        }
    }
    return oss.str();
}

} // namespace coords
