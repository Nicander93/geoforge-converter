#include "coordinate_transformer.h"
#include <cstdio>
#include <cmath>

namespace coords {

// WGS84妞悆鍙傛暟
// 闀垮崐杞村拰鎵佺巼鐢ㄤ簬ECEF鍧愭爣璁＄畻
static constexpr double WGS84_A = 6378137.0;                    // 闀垮崐杞?绫?
static constexpr double WGS84_F = 1.0 / 298.257223563;          // 鎵佺巼
static constexpr double WGS84_E2 = WGS84_F * (2.0 - WGS84_F);   // 绗竴鍋忓績鐜囩殑骞虫柟

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs)
    : source_cs_(cs)
    , mode_(TransformMode::None) {
    // 鏃犲湴鐞嗗弬鑰冩ā寮忥紝浠呮敮鎸佽酱鏂瑰悜杞崲
    // 閫傜敤浜嶰SGB鈫扜LTF绛夌函鏍煎紡杞崲鍦烘櫙
}

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs,
                                             const GeoReference& geo_ref)
    : source_cs_(cs)
    , mode_(TransformMode::WithGeoReference)
    , geoid_config_(GeoidConfig::Disabled()) {
    // 甯﹀湴鐞嗗弬鑰冩ā寮忥紝涓嶆敮鎸丟eoid鏍℃
    InitializeWithGeoRef(geo_ref);
}

CoordinateTransformer::CoordinateTransformer(const CoordinateSystem& cs,
                                             const GeoReference& geo_ref,
                                             const GeoidConfig& geoid_config)
    : source_cs_(cs)
    , mode_(TransformMode::WithGeoReference)
    , geoid_config_(geoid_config) {
    // 甯﹀湴鐞嗗弬鑰冨拰Geoid閰嶇疆妯″紡
    // 閫傜敤浜庨渶瑕侀珮绋嬪熀鍑嗘牎姝ｇ殑鍦烘櫙
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
    // 鏍规嵁鍧愭爣绯荤被鍨嬪垵濮嬪寲
    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU绫诲瀷锛氫娇鐢ㄥ唴缃湴鐞嗗弬鑰?
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            geo_origin_lon_ = enu_params->origin_lon;
            geo_origin_lat_ = enu_params->origin_lat;
            geo_origin_height_ = enu_params->origin_height;
        }
    } else if (source_cs_.NeedsOGRTransform()) {
        // EPSG/WKT绫诲瀷锛氬垱寤篛GR杞崲鍣ㄧ敤浜庡悗缁潗鏍囪浆鎹?
        CreateOGRTransform();

        // A zero longitude/latitude anchor is valid. Presence is a separate
        // part of the contract so origin coordinates do not change behavior.
        if (geo_ref.has_position) {
            geo_origin_lon_ = geo_ref.lon;
            geo_origin_lat_ = geo_ref.lat;
            geo_origin_height_ = geo_ref.height;

            // 濡傛灉Geoid閰嶇疆鍚敤浣嗛珮搴︽湭鏍℃锛屽簲鐢ㄦ牎姝?
            if (geoid_config_.enabled && GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) {
                geo_origin_height_ = ApplyGeoidCorrection(geo_origin_lat_, geo_origin_lon_, geo_origin_height_);
            }
        } else {
            // 鑷繁璁＄畻鍘熺偣
            auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
            glm::dvec3 origin{origin_x, origin_y, origin_z};

            if (ogr_transform_) {
                {
                    std::lock_guard<std::mutex> lock(ogr_mutex_);
                    ogr_transform_->Transform(1, &origin.x, &origin.y, &origin.z);
                }
            }

            geo_origin_lon_ = origin.x;
            geo_origin_lat_ = origin.y;
            geo_origin_height_ = origin.z;

            geo_origin_height_ = ApplyGeoidCorrection(geo_origin_lat_, geo_origin_lon_, geo_origin_height_);
        }

        fprintf(stderr, "[CoordinateTransformer] OGR transform result: lon=%.10f lat=%.10f h=%.3f\n",
                geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
    } else {
        // LocalCartesian绫诲瀷锛氫娇鐢ㄧ敤鎴锋彁渚涚殑鍦扮悊鍙傝€?
        geo_origin_lon_ = geo_ref.lon;
        geo_origin_lat_ = geo_ref.lat;
        geo_origin_height_ = geo_ref.height;
    }

    // 璁＄畻ENU<->ECEF杞崲鐭╅樀
    enu_to_ecef_ = CalcEnuToEcefMatrix(geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
    ecef_to_enu_ = glm::inverse(enu_to_ecef_);

    // 璁＄畻杞存柟鍚戣浆鎹㈢煩闃?
    axis_transform_ = GetAxisTransformMatrix(source_cs_.GetUpAxis(), UpAxis::Y_UP);

    fprintf(stderr, "[CoordinateTransformer] Initialized: geo_origin=(%.10f, %.10f, %.3f)\n",
            geo_origin_lon_, geo_origin_lat_, geo_origin_height_);
}

void CoordinateTransformer::CreateOGRTransform() {
    // 鍒涘缓鐩爣鍧愭爣绯?WGS84)
    OGRSpatialReference outRs;
    outRs.importFromEPSG(4326);
    outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    // 鍒涘缓婧愬潗鏍囩郴
    OGRSpatialReference inRs;
    inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    if (source_cs_.Type() == CoordinateType::EPSG) {
        // 浠嶦PSG缂栫爜鍒涘缓
        auto code = source_cs_.GetEPSGCode();
        if (code) {
            inRs.importFromEPSG(*code);
        }
    } else if (source_cs_.Type() == CoordinateType::WKT) {
        // 浠嶹KT瀛楃涓插垱寤?
        auto wkt = source_cs_.GetWKTString();
        if (wkt) {
            inRs.importFromWkt(wkt->c_str());
        }
    }

    // 鍒涘缓鍧愭爣杞崲鍣?
    OGRCoordinateTransformation* poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
    if (poCT) {
        ogr_transform_.reset(poCT);
        fprintf(stderr, "[CoordinateTransformer] OGR transform created successfully\n");
    } else {
        fprintf(stderr, "[CoordinateTransformer] Failed to create OGR transform\n");
    }
}

bool CoordinateTransformer::ShouldApplyGeoidCorrection() const {
    // 1. Geoid閰嶇疆蹇呴』鍚敤
    if (!geoid_config_.enabled) return false;

    // 2. Geoid璁＄畻鍣ㄥ繀椤诲凡鍒濆鍖?
    if (!GeoidHeight::GetGlobalGeoidCalculator().IsInitialized()) return false;

    // 3. 鏍规嵁鍧愭爣绯荤被鍨嬪拰鍨傜洿鍩哄噯鍒ゆ柇
    switch (source_cs_.Type()) {
        case CoordinateType::EPSG:
        case CoordinateType::WKT: {
            // EPSG/WKT鍧愭爣绯伙細妫€鏌ュ瀭鐩村熀鍑?
            auto datum = source_cs_.GetVerticalDatum();
            // 姝ｉ珮鎴栨湭鐭ユ椂闇€瑕佹牎姝?
            return datum == VerticalDatum::Orthometric || datum == VerticalDatum::Unknown;
        }
        case CoordinateType::ENU:
            // ENU鍧愭爣绯伙細鍋囪涓篧GS84妞悆楂橈紝涓嶉渶瑕佹牎姝?
            return false;
        case CoordinateType::LocalCartesian:
            // 鏈湴绗涘崱灏旓細鐢ㄦ埛鎸囧畾鐨勯珮搴﹀亣璁句负妞悆楂?
            return false;
        default:
            return false;
    }
}

double CoordinateTransformer::ApplyGeoidCorrection(double lat, double lon, double height) const {
    if (!ShouldApplyGeoidCorrection()) return height;

    // 姝ｉ珮 鈫?妞悆楂?
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

    // 搴旂敤杞存柟鍚戣浆鎹?
    result = axis_transform_ * glm::dvec4(result, 1.0);

    // 鏍规嵁鍧愭爣绯荤被鍨嬪鐞?
    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU: 鍔犱笂鍋忕Щ閲忓悗杞崲鍒癊CEF锛屽啀杞崲鍒癢GS84
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        // ENU鍧愭爣宸茬粡鏄浉瀵逛簬鍘熺偣鐨勶紝鐩存帴閫氳繃鐭╅樀杞崲
        glm::dvec3 ecef = enu_to_ecef_ * glm::dvec4(result, 1.0);
        // ECEF 鈫?WGS84 (绠€鍖栧鐞嗭紝瀹為檯搴斾娇鐢ㄨ凯浠ｇ畻娉?
        // 杩欓噷杩斿洖鍦扮悊鍘熺偣浣滀负杩戜技
        return {geo_origin_lon_, geo_origin_lat_, geo_origin_height_ + result.z};
    } else if (source_cs_.NeedsOGRTransform() && ogr_transform_) {
        // EPSG/WKT: 浣跨敤OGR杞崲
        // 鍏堝噺鍘诲師鐐瑰亸绉?
        auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
        result.x += origin_x;
        result.y += origin_y;
        result.z += origin_z;

        {
            std::lock_guard<std::mutex> lock(ogr_mutex_);
            ogr_transform_->Transform(1, &result.x, &result.y, &result.z);
        }
    } else {
        // LocalCartesian: 浣跨敤鍦扮悊鍘熺偣
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

    // 搴旂敤杞存柟鍚戣浆鎹?
    result = axis_transform_ * glm::dvec4(result, 1.0);

    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU: 鍔犱笂鍋忕Щ閲忓悗閫氳繃鐭╅樀杞崲
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        return enu_to_ecef_ * glm::dvec4(result, 1.0);
    } else {
        // 鍏朵粬绫诲瀷: 鍏堣浆WGS84锛屽啀杞珽CEF
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

    // 鏍规嵁鍧愭爣绯荤被鍨嬪鐞?
    if (source_cs_.Type() == CoordinateType::ENU) {
        // ENU绫诲瀷锛歅oint鏄浉瀵逛簬SRSOrigin鐨凟NU鍧愭爣
        // 1. 鍔犱笂SRSOrigin鍋忕Щ寰楀埌缁濆ENU鍧愭爣
        auto enu_params = source_cs_.GetENUParams();
        if (enu_params) {
            result.x += enu_params->offset_x;
            result.y += enu_params->offset_y;
            result.z += enu_params->offset_z;
        }
        // 2. ENU 鈫?ECEF锛堜娇鐢ㄥ湴鐞嗗師鐐圭殑ENU鈫扙CEF鐭╅樀锛?
        glm::dvec3 ecef = enu_to_ecef_ * glm::dvec4(result, 1.0);
        // 3. ECEF 鈫?灞€閮‥NU锛堜娇鐢ㄥ湴鐞嗗師鐐圭殑ECEF鈫扙NU鐭╅樀锛?
        glm::dvec4 enu = ecef_to_enu_ * glm::dvec4(ecef, 1.0);
        return {enu.x, enu.y, enu.z};
    } else if (source_cs_.NeedsOGRTransform() && ogr_transform_) {
        // EPSG/WKT绫诲瀷锛歅oint鏄姇褰卞潗鏍?
        // 1. 鍔犱笂婧愬潗鏍囧師鐐瑰亸绉?
        auto [origin_x, origin_y, origin_z] = source_cs_.GetSourceOrigin();
        result.x += origin_x;
        result.y += origin_y;
        result.z += origin_z;

        // 2. 鎶曞奖鍧愭爣 鈫?WGS84鍦扮悊鍧愭爣
        {
            std::lock_guard<std::mutex> lock(ogr_mutex_);
            ogr_transform_->Transform(1, &result.x, &result.y, &result.z);
        }

        // 3. 搴旂敤Geoid楂樺害鏍℃
        result.z = ApplyGeoidCorrection(result.y, result.x, result.z);

        // 4. WGS84 鈫?ECEF
        glm::dvec3 ecef = CartographicToEcef(result.x, result.y, result.z);

        // 5. ECEF 鈫?灞€閮‥NU
        glm::dvec4 enu = ecef_to_enu_ * glm::dvec4(ecef, 1.0);
        return {enu.x, enu.y, enu.z};
    } else {
        // LocalCartesian绫诲瀷锛氭棤鍦扮悊鍙傝€冿紝鐩存帴杩斿洖
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

    // 瑙掑害杞姬搴?
    double lon = lon_deg * pi / 180.0;
    double phi = lat_deg * pi / 180.0;

    double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    double sinLon = std::sin(lon), cosLon = std::cos(lon);

    // 璁＄畻鍗厜鍦堟洸鐜囧崐寰凬
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi * sinPhi);

    // 璁＄畻ECEF鍧愭爣
    double x0 = (N + height) * cosPhi * cosLon;
    double y0 = (N + height) * cosPhi * sinLon;
    double z0 = (N * (1.0 - WGS84_E2) + height) * sinPhi;

    // ENU鍩哄悜閲忓湪ECEF涓殑琛ㄧず
    // 涓?E): -sin(lon), cos(lon), 0
    // 鍖?N): -sin(lat)*cos(lon), -sin(lat)*sin(lon), cos(lat)
    // 澶?U): cos(lat)*cos(lon), cos(lat)*sin(lon), sin(lat)
    glm::dvec3 east(-sinLon,           cosLon,            0.0);
    glm::dvec3 north(-sinPhi * cosLon, -sinPhi * sinLon,  cosPhi);
    glm::dvec3 up(   cosPhi * cosLon,   cosPhi * sinLon,  sinPhi);

    // 鏋勫缓ENU鈫扙CEF鍙樻崲鐭╅樀(鏃嬭浆+骞崇Щ)锛屽垪涓诲簭
    glm::dmat4 T(1.0);
    T[0] = glm::dvec4(east,  0.0);
    T[1] = glm::dvec4(north, 0.0);
    T[2] = glm::dvec4(up,    0.0);
    T[3] = glm::dvec4(x0, y0, z0, 1.0);

    return T;
}

glm::dvec3 CoordinateTransformer::CartographicToEcef(double lon_deg, double lat_deg, double height) {
    const double pi = std::acos(-1.0);

    // 瑙掑害杞姬搴?
    double lon = lon_deg * pi / 180.0;
    double phi = lat_deg * pi / 180.0;

    double sinPhi = std::sin(phi), cosPhi = std::cos(phi);
    double sinLon = std::sin(lon), cosLon = std::cos(lon);

    // 璁＄畻鍗厜鍦堟洸鐜囧崐寰凬
    double N = WGS84_A / std::sqrt(1.0 - WGS84_E2 * sinPhi * sinPhi);

    // 璁＄畻ECEF鍧愭爣
    double x = (N + height) * cosPhi * cosLon;
    double y = (N + height) * cosPhi * sinLon;
    double z = (N * (1.0 - WGS84_E2) + height) * sinPhi;

    return {x, y, z};
}

glm::dmat4 CoordinateTransformer::GetAxisTransformMatrix(UpAxis from, UpAxis to) {
    if (from == to) {
        return glm::dmat4(1.0);
    }

    // Z-Up 鈫?Y-Up: (x, y, z) 鈫?(x, -z, y)
    // Y-Up 鈫?Z-Up: (x, y, z) 鈫?(x, z, -y)
    if (from == UpAxis::Z_UP && to == UpAxis::Y_UP) {
        // Z-Up 鈫?Y-Up
        // 鏂癤 = 鍘焁
        // 鏂癥 = 鍘焃
        // 鏂癦 = -鍘焂
        return glm::dmat4(
            1,  0,  0, 0,
            0,  0,  1, 0,
            0, -1,  0, 0,
            0,  0,  0, 1
        );
    } else {
        // Y-Up 鈫?Z-Up
        // 鏂癤 = 鍘焁
        // 鏂癥 = -鍘焃
        // 鏂癦 = 鍘焂
        return glm::dmat4(
            1,  0,  0, 0,
            0,  0, -1, 0,
            0,  1,  0, 0,
            0,  0,  0, 1
        );
    }
}

} // namespace coords
