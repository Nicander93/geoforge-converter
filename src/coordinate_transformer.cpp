#include "coordinate_transformer.h"
#include <cstdio>
#include <cmath>

namespace coords {

// WGS84椭球参数
// 长半轴和扁率用于ECEF坐标计算
static constexpr double WGS84_A = 6378137.0;                    // 长半轴(米)
static constexpr double WGS84_F = 1.0 / 298.257223563;          // 扁率
static constexpr double WGS84_E2 = WGS84_F * (2.0 - WGS84_F);   // 第一偏心率的平方

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs)
    : source_cs_(cs)
    , mode_(TransformMode::None) {
    // 无地理参考模式，仅支持轴方向转换
    // 适用于OSGB→GLTF等纯格式转换场景
}

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs,
                                             const GeoReference& geo_ref)
    : source_cs_(cs)
    , mode_(TransformMode::WithGeoReference)
    , geoid_config_(GeoidConfig::Disabled()) {
    // 带地理参考模式，不支持Geoid校正
    InitializeWithGeoRef(geo_ref);
}

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs,
                                             const GeoReference& geo_ref,
                                             const GeoidConfig& geoid_config)
    : source_cs_(cs)
    , mode_(TransformMode::WithGeoReference)
    , geoid_config_(geoid_config) {
    // 带地理参考和Geoid配置模式
    // 适用于需要高程基准校正的场景
    InitializeWithGeoRef(geo_ref);
}

CoordinateTransformer::~CoordinateTransformer() = default;

CoordinateTransformer::CoordinateTransformer(CoordinateTransformer&& other) noexcept
    : source_cs_(std::move(other.source_cs_))
    , mode_(other.mode_)
    , geo_origin_lon_(other.geo_origin_lon_)
    , geo_origin_lat_(other.geo_origin_lat_)
    , geo_origin_height_(other.geo_origin_height_)
    , enu_to_ecef_(other.enu_to_ecef_)
    , ecef_to_enu_(other.ecef_to_enu_)
    , axis_transform_(other.axis_transform_)
    , ogr_transform_(std::move(other.ogr_transform_))
    , geoid_config_(other.geoid_config_) {
}

CoordinateTransformer& CoordinateTransformer::operator=(CoordinateTransformer&& other) noexcept {
    if (this != &other) {
        source_cs_ = std::move(other.source_cs_);
        mode_ = other.mode_;
        geo_origin_lon_ = other.geo_origin_lon_;
        geo_origin_lat_ = other.geo_origin_lat_;
        geo_origin_height_ = other.geo_origin_height_;
        enu_to_ecef_ = other.enu_to_ecef_;
        ecef_to_enu_ = other.ecef_to_enu_;
        axis_transform_ = other.axis_transform_;
        ogr_transform_ = std::move(other.ogr_transform_);
        geoid_config_ = other.geoid_config_;
    }
    return *this;
}

void CoordinateTransformer::InitializeWithGeoRef(const GeoReference& geo_ref) {
    // 根据坐标系类型初始化
    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU类型：使用内置地理参考
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            geo_origin_lon_ = enu_params->origin_lon;
            geo_origin_lat_ = enu_params->origin_lat;
            geo_origin_height_ = enu_params->origin_height;
        }
    } else if (source_cs_.NeedsOGRTransform()) {
        // EPSG/WKT类型：创建OGR转换器用于后续坐标转换
        CreateOGRTransform();

        // 使用传入的地理参考（已经过Geoid校正）
        // 如果传入的geo_ref有效（非零），使用它；否则自己计算
        if (geo_ref.lon != 0.0 || geo_ref.lat != 0.0) {
            geo_origin_lon_ = geo_ref.lon;
            geo_origin_lat_ = geo_ref.lat;
            geo_origin_height_ = geo_ref.height;

            // 如果Geoid配置启用但高度未校正，应用校正
            if (geoid_config_.enabled && GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) {
                geo_origin_height_ = ApplyGeoidCorrection(geo_origin_lat_, geo_origin_lon_, geo_origin_height_);
            }
        } else {
            // 自己计算原点
            auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
            glm::dvec3 origin{origin_x, origin_y, origin_z};

            if (ogr_transform_) {
                ogr_transform_->Transform(1, &origin.x, &origin.y, &origin.z);
            }

            geo_origin_lon_ = origin.x;
            geo_origin_lat_ = origin.y;
            geo_origin_height_ = origin.z;

            geo_origin_height_ = ApplyGeoidCorrection(geo_origin_lat_, geo_origin_lon_, geo_origin_height_);
        }

        fprintf(stderr, "[CoordinateTransformer] OGR transform result: lon=%.10f lat=%.10f h=%.3f\n",
                geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
    } else {
        // LocalCartesian类型：使用用户提供的地理参考
        geo_origin_lon_ = geo_ref.lon;
        geo_origin_lat_ = geo_ref.lat;
        geo_origin_height_ = geo_ref.height;
    }

    // 计算ENU<->ECEF转换矩阵
    enu_to_ecef_ = CalcEnuToEcefMatrix(geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
    ecef_to_enu_ = glm::inverse(enu_to_ecef_);

    // 计算轴方向转换矩阵
    axis_transform_ = GetAxisTransformMatrix(source_cs_.GetUpAxis(), UpAxis::Y_UP);

    fprintf(stderr, "[CoordinateTransformer] Initialized: geo_origin=(%.10f, %.10f, %.3f)\n",
            geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
}

void CoordinateTransformer::CreateOGRTransform() {
    // 创建目标坐标系(WGS84)
    OGRSpatialReference outRs;
    outRs.importFromEPSG(4326);
    outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // 创建源坐标系
    OGRSpatialReference inRs;
    inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    if (source_cs_.Type() == CoordinateType::EPSG) {
        // 从EPSG编码创建
        auto code = source_cs_.GetEPSGCode();
        if (code) {
            inRs.importFromEPSG(*code);
        }
    } else if (source_cs_.Type() == CoordinateType::WKT) {
        // 从WKT字符串创建
        auto wkt = source_cs_.GetWKTString();
        if (wkt) {
            inRs.importFromWkt(wkt->c_str());
        }
    }

    // 创建坐标转换器
    OGRCoordinateTransformation* poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
    if (poCT) {
        ogr_transform_.reset(poCT);
        fprintf(stderr, "[CoordinateTransformer] OGR transform created successfully\n");
    } else {
        fprintf(stderr, "[CoordinateTransformer] Failed to create OGR transform\n");
    }
}

bool CoordinateTransformer::ShouldApplyGeoidCorrection() const {
    // 1. Geoid配置必须启用
    if (!geoid_config_.enabled) return false;

    // 2. Geoid计算器必须已初始化
    if (!GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) return false;

    // 3. 根据坐标系类型和垂直基准判断
    switch (source_cs_.Type()) {
        case CoordinateType::EPSG:
        case CoordinateType::WKT: {
            // EPSG/WKT坐标系：检查垂直基准
            auto datum = source_cs_.GetVerticalDatum();
            // 正高或未知时需要校正
            return datum == VerticalDatum::Orthometric || datum == VerticalDatum::Unknown;
        }
        case CoordinateType::ENU:
            // ENU坐标系：假设为WGS84椭球高，不需要校正
            return false;
        case CoordinateType::LocalCartesian:
            // 本地笛卡尔：用户指定的高度假设为椭球高
            return false;
        default:
            return false;
    }
}

double CoordinateTransformer::ApplyGeoidCorrection(double lat, double lon, double height) const {
    if (!ShouldApplyGeoidCorrection()) return height;

    // 正高 → 椭球高
    double corrected = GeoidHeight::GetGlobalGeoidCalculator()
        .ConvertOrthometricToEllipsoidal(lat, lon, height);

    fprintf(stderr, "[CoordinateTransformer] Geoid correction: orthometric=%.3f -> ellipsoidal=%.3f\n",
            height, corrected);

    return corrected;
}

glm::dvec3 CoordinateTransformer::ToWGS84(const glm::dvec3& point) const {
    if (!HasGeoReference()) {
        fprintf(stderr, "[CoordinateTransformer] Warning: ToWGS84 called without geo reference\n");
        return point;
    }

    glm::dvec3 result = point;

    // 应用轴方向转换
    result = axis_transform_ * glm::dvec4(result, 1.0);

    // 根据坐标系类型处理
    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU: 加上偏移量后转换到ECEF，再转换到WGS84
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        // ENU坐标已经是相对于原点的，直接通过矩阵转换
        glm::dvec3 ecef = enu_to_ecef_ * glm::dvec4(result, 1.0);
        // ECEF → WGS84 (简化处理，实际应使用迭代算法)
        // 这里返回地理原点作为近似
        return {geo_origin_lon_, geo_origin_lat_, geo_origin_height_ + result.z};
    } else if (source_cs_.NeedsOGRTransform() && ogr_transform_) {
        // EPSG/WKT: 使用OGR转换
        // 先减去原点偏移
        auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
        result.x += origin_x;
        result.y += origin_y;
        result.z += origin_z;

        ogr_transform_->Transform(1, &result.x, &result.y, &result.z);
    } else {
        // LocalCartesian: 使用地理原点
        result = {geo_origin_lon_, geo_origin_lat_, geo_origin_height_ + result.z};
    }

    return result;
}

glm::dvec3 CoordinateTransformer::ToECEF(const glm::dvec3& point) const {
    if (!HasGeoReference()) {
        fprintf(stderr, "[CoordinateTransformer] Warning: ToECEF called without geo reference\n");
        return point;
    }

    glm::dvec3 result = point;

    // 应用轴方向转换
    result = axis_transform_ * glm::dvec4(result, 1.0);

    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU: 加上偏移量后通过矩阵转换
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        return enu_to_ecef_ * glm::dvec4(result, 1.0);
    } else {
        // 其他类型: 先转WGS84，再转ECEF
        glm::dvec3 wgs84 = ToWGS84(point);
        return CartographicToEcef(wgs84.x, wgs84.y, wgs84.z);
    }
}

glm::dvec3 CoordinateTransformer::ToLocalENU(const glm::dvec3& point) const {
    if (!HasGeoReference()) {
        fprintf(stderr, "[CoordinateTransformer] Warning: ToLocalENU called without geo reference\n");
        return point;
    }

    glm::dvec3 result = point;

    // 根据坐标系类型处理
    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU类型：Point是相对于SRSOrigin的ENU坐标
        // 1. 加上SRSOrigin偏移得到绝对ENU坐标
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        // 2. ENU → ECEF（使用地理原点的ENU→ECEF矩阵）
        glm::dvec3 ecef = enu_to_ecef_ * glm::dvec4(result, 1.0);
        // 3. ECEF → 局部ENU（使用地理原点的ECEF→ENU矩阵）
        glm::dvec4 enu = ecef_to_enu_ * glm::dvec4(ecef, 1.0);
        return {enu.x, enu.y, enu.z};
    } else if (source_cs_.NeedsOGRTransform() && ogr_transform_) {
        // EPSG/WKT类型：Point是投影坐标
        // 1. 加上源坐标原点偏移
        auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
        result.x += origin_x;
        result.y += origin_y;
        result.z += origin_z;

        // 2. 投影坐标 → WGS84地理坐标
        ogr_transform_->Transform(1, &result.x, &result.y, &result.z);

        // 3. 应用Geoid高度校正
        result.z = ApplyGeoidCorrection(result.y, result.x, result.z);

        // 4. WGS84 → ECEF
        glm::dvec3 ecef = CartographicToEcef(result.x, result.y, result.z);

        // 5. ECEF → 局部ENU
        glm::dvec4 enu = ecef_to_enu_ * glm::dvec4(ecef, 1.0);
        return {enu.x, enu.y, enu.z};
    } else {
        // LocalCartesian类型：无地理参考，直接返回
        return result;
    }
}

void CoordinateTransformer::TransformToWGS84(std::vector<glm::dvec3>& points) const {
    for (auto& point : points) {
        point = ToWGS84(point);
    }
}

void CoordinateTransformer::TransformToLocalENU(std::vector<glm::dvec3>& points) const {
    for (auto& point : points) {
        point = ToLocalENU(point);
    }
}

glm::dvec3 CoordinateTransformer::ConvertUpAxis(const glm::dvec3& point,
                                                 UpAxis target_axis) const {
    glm::dmat4 transform = GetAxisTransformMatrix(source_cs_.GetUpAxis(), target_axis);
    glm::dvec4 result = transform * glm::dvec4(point, 1.0);
    return {result.x, result.y, result.z};
}

glm::dmat4 CoordinateTransformer::CalcEnuToEcefMatrix(double lon_deg, double lat_deg, double height) {
    const double pi = std::acos(-1.0);

    // 角度转弧度
    double lon = lon_deg * pi / 180.0;
    double phi = lat_deg * pi / 180.0;

    double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    double sinLon = std::sin(lon), cosLon = std::cos(lon);

    // 计算卯酉圈曲率半径N
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi * sinPhi);

    // 计算ECEF坐标
    double x0 = (N + height) * cosPhi * cosLon;
    double y0 = (N + height) * cosPhi * sinLon;
    double z0 = (N * (1.0 - WGS84_E2) + height) * sinPhi;

    // ENU基向量在ECEF中的表示
    // 东(E): -sin(lon), cos(lon), 0
    // 北(N): -sin(lat)*cos(lon), -sin(lat)*sin(lon), cos(lat)
    // 天(U): cos(lat)*cos(lon), cos(lat)*sin(lon), sin(lat)
    glm::dvec3 east(-sinLon,           cosLon,            0.0);
    glm::dvec3 north(-sinPhi * cosLon, -sinPhi * sinLon,  cosPhi);
    glm::dvec3 up(   cosPhi * cosLon,   cosPhi * sinLon,  sinPhi);

    // 构建ENU→ECEF变换矩阵(旋转+平移)，列主序
    glm::dmat4 T(1.0);
    T[0] = glm::dvec4(east,  0.0);
    T[1] = glm::dvec4(north, 0.0);
    T[2] = glm::dvec4(up,    0.0);
    T[3] = glm::dvec4(x0, y0, z0, 1.0);

    return T;
}

glm::dvec3 CoordinateTransformer::CartographicToEcef(double lon_deg, double lat_deg, double height) {
    const double pi = std::acos(-1.0);

    // 角度转弧度
    double lon = lon_deg * pi / 180.0;
    double phi = lat_deg * pi / 180.0;

    double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    double sinLon = std::sin(lon), cosLon = std::cos(lon);

    // 计算卯酉圈曲率半径N
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi * sinPhi);

    // 计算ECEF坐标
    double x = (N + height) * cosPhi * cosLon;
    double y = (N + height) * cosPhi * sinLon;
    double z = (N * (1.0 - WGS84_E2) + height) * sinPhi;

    return {x, y, z};
}

glm::dmat4 CoordinateTransformer::GetAxisTransformMatrix(UpAxis from, UpAxis to) {
    if (from == to) {
        return glm::dmat4(1.0);
    }

    // Z-Up → Y-Up: (x, y, z) → (x, -z, y)
    // Y-Up → Z-Up: (x, y, z) → (x, z, -y)
    if (from == UpAxis::Z_UP && to == UpAxis::Y_UP) {
        // Z-Up → Y-Up
        // 新X = 原X
        // 新Y = 原Z
        // 新Z = -原Y
        return glm::dmat4(
            1,  0,  0, 0,
            0,  0,  1, 0,
            0, -1,  0, 0,
            0,  0,  0, 1
        );
    } else {
        // Y-Up → Z-Up
        // 新X = 原X
        // 新Y = -原Z
        // 新Z = 原Y
        return glm::dmat4(
            1,  0,  0, 0,
            0,  0, -1, 0,
            0,  1,  0, 0,
            0,  0,  0, 1
        );
    }
}

} // namespace coords
