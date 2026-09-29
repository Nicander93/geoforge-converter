#pragma once

#include "coordinate_system.h"
#include "GeoidHeight.h"
#include <ogr_spatialref.h>
#include <memory>
#include <mutex>
#include <vector>
#include <glm/glm.hpp>

namespace coords {

// 杞崲妯″紡
enum class TransformMode {
    None,               // 鏃犲湴鐞嗚浆鎹紙OSGB鈫扜LTF鍦烘櫙锛?
    WithGeoReference    // 甯﹀湴鐞嗗弬鑰冭浆鎹紙3D Tiles鍦烘櫙锛?
};

// Geoid閰嶇疆
// 鐢ㄤ簬閰嶇疆楂樼▼鍩哄噯鏍℃鍙傛暟
struct GeoidConfig {
    bool enabled = false;                                // 鏄惁鍚敤Geoid鏍℃
    GeoidHeight::GeoidModel model = GeoidHeight::GeoidModel::EGM96;  // Geoid妯″瀷
    std::string data_path;                               // Geoid鏁版嵁鏂囦欢璺緞

    // 鍒涘缓绂佺敤Geoid鏍℃鐨勯厤缃?
    static GeoidConfig Disabled() { return {false}; }

    // 鍒涘缓EGM96妯″瀷閰嶇疆
    static GeoidConfig EGM96(const std::string& path = "") {
        return {true, GeoidHeight::GeoidModel::EGM96, path};
    }

    // 鍒涘缓EGM2008妯″瀷閰嶇疆
    static GeoidConfig EGM2008(const std::string& path = "") {
        return {true, GeoidHeight::GeoidModel::EGM2008, path};
    }
};

// 鍧愭爣杞崲鍣?
// 璐熻矗灏嗘簮鍧愭爣绯昏浆鎹㈠埌鐩爣鍧愭爣绯?ENU灞€閮ㄥ潗鏍?
// 瀹炰緥绫诲瀷锛屾瘡涓疄渚嬬淮鎶ょ嫭绔嬬殑杞崲鐘舵€侊紝绾跨▼瀹夊叏
class CoordinateTransformer {
public:
    // 妯″紡1锛氭棤鍦扮悊杞崲锛堢函鏍煎紡杞崲锛屽OSGB鈫扜LTF锛?
    // 浠呮敮鎸佽酱鏂瑰悜杞崲
    explicit CoordinateTransformer(const CoordinateSystem& cs);

    // 妯″紡2锛氬甫鍦扮悊鍙傝€冭浆鎹紙3D Tiles鍦烘櫙锛?
    // 鏀寔瀹屾暣鐨勫潗鏍囪浆鎹㈤摼
    CoordinateTransformer(const CoordinateSystem& cs, const GeoReference& geo_ref);

    // 妯″紡3锛氬甫鍦扮悊鍙傝€冨拰Geoid閰嶇疆
    // 鐢ㄤ簬闇€瑕侀珮绋嬪熀鍑嗘牎姝ｇ殑鍦烘櫙
    CoordinateTransformer(const CoordinateSystem& cs, const GeoReference& geo_ref,
                          const GeoidConfig& geoid_config);

    ~CoordinateTransformer();

    // 绂佹鎷疯礉锛屽厑璁哥Щ鍔?
    CoordinateTransformer(const CoordinateTransformer&) = delete;
    CoordinateTransformer& operator=(const CoordinateTransformer&) = delete;
    CoordinateTransformer(CoordinateTransformer&&) noexcept;
    CoordinateTransformer& operator=(CoordinateTransformer&&) noexcept;

    // ----- 妯″紡鏌ヨ -----

    // 鑾峰彇杞崲妯″紡
    TransformMode GetMode() const { return mode_; }

    // Whether geographic reference is available.
    bool HasGeoReference() const { return mode_ == TransformMode::WithGeoReference; }
    bool HasProjectedTransform() const {
        return source_cs_.NeedsOGRTransform() && ogr_transform_ != nullptr;
    }
    glm::dvec3 GeoOrigin() const { return {geo_origin_lon_, geo_origin_lat_, geo_origin_height_}; }

    // ----- 鍧愭爣杞崲锛堜粎HasGeoReference鏃舵湁鏁堬級-----

    // 杞崲鍒癢GS84鍦扮悊鍧愭爣(缁忓害, 绾害, 楂樺害)
    glm::dvec3 ToWGS84(const glm::dvec3& point) const;

    // 杞崲鍒癊CEF鍦板績鍦板浐鍧愭爣
    glm::dvec3 ToECEF(const glm::dvec3& point) const;

    // 杞崲鍒癊NU灞€閮ㄥ潗鏍?涓? 鍖? 澶?
    // 杩欐槸3D Tiles浣跨敤鐨勫潗鏍囩郴缁?
    glm::dvec3 ToLocalENU(const glm::dvec3& point) const;

    // 鎵归噺杞崲
    void TransformToWGS84(std::vector<glm::dvec3>& points) const;
    void TransformToLocalENU(std::vector<glm::dvec3>& points) const;

    // ----- 杞存柟鍚戣浆鎹紙鎵€鏈夋ā寮忛兘鍙敤锛?----

    // 杞崲杞存柟鍚?
    // 渚嬪锛歓-Up 鈫?Y-Up (OSGB 鈫?glTF)
    glm::dvec3 ConvertUpAxis(const glm::dvec3& point,
                             UpAxis target_axis = UpAxis::Y_UP) const;

    // ----- 鐭╅樀璁块棶 -----

    // 鑾峰彇ENU鍒癊CEF鐨勮浆鎹㈢煩闃?
    const glm::dmat4& GetEnuToEcefMatrix() const { return enu_to_ecef_; }

    // 鑾峰彇ECEF鍒癊NU鐨勮浆鎹㈢煩闃?
    const glm::dmat4& GetEcefToEnuMatrix() const { return ecef_to_enu_; }

    // ----- 鍘熺偣淇℃伅 -----

    // 鑾峰彇鍦扮悊鍘熺偣缁忓害(搴?
    double GeoOriginLon() const { return geo_origin_lon_; }

    // 鑾峰彇鍦扮悊鍘熺偣绾害(搴?
    double GeoOriginLat() const { return geo_origin_lat_; }

    // 鑾峰彇鍦扮悊鍘熺偣楂樺害(绫? 妞悆楂?
    double GeoOriginHeight() const { return geo_origin_height_; }

    // ----- Geoid閰嶇疆 -----

    // 鍚敤/绂佺敤Geoid鏍℃
    void EnableGeoidCorrection(bool enabled) { geoid_config_.enabled = enabled; }

    // 鏄惁鍚敤Geoid鏍℃
    bool IsGeoidCorrectionEnabled() const { return geoid_config_.enabled; }

    // 鑾峰彇Geoid閰嶇疆
    const GeoidConfig& GetGeoidConfig() const { return geoid_config_; }

    // ----- 闈欐€佸伐鍏锋柟娉?-----

    // 璁＄畻ENU鍒癊CEF鐨勮浆鎹㈢煩闃?
    // 鍩轰簬WGS84妞悆鍙傛暟
    static glm::dmat4 CalcEnuToEcefMatrix(double lon_deg, double lat_deg, double height);

    // 灏嗗湴鐞嗗潗鏍?缁忓害, 绾害, 楂樺害)杞崲涓篍CEF鍧愭爣
    // 鍩轰簬WGS84妞悆鍙傛暟
    static glm::dvec3 CartographicToEcef(double lon_deg, double lat_deg, double height);

    // 鑾峰彇杞存柟鍚戣浆鎹㈢煩闃?
    static glm::dmat4 GetAxisTransformMatrix(UpAxis from, UpAxis to);

private:
    // 浣跨敤鍦扮悊鍙傝€冨垵濮嬪寲
    void InitializeWithGeoRef(const GeoReference& geo_ref);

    // 鍒涘缓OGR鍧愭爣杞崲鍣?
    void CreateOGRTransform();

    // 搴旂敤Geoid楂樺害鏍℃
    // 灏嗘楂樿浆鎹负妞悆楂?
    double ApplyGeoidCorrection(double lat, double lon, double height) const;

    // 鍒ゆ柇鏄惁搴旇搴旂敤Geoid鏍℃
    bool ShouldApplyGeoidCorrection() const;

    CoordinateSystem source_cs_;            // 婧愬潗鏍囩郴
    TransformMode mode_ = TransformMode::None;  // 杞崲妯″紡

    // 鍦扮悊鍘熺偣(WGS84)
    double geo_origin_lon_ = 0.0;           // 缁忓害(搴?
    double geo_origin_lat_ = 0.0;           // 绾害(搴?
    double geo_origin_height_ = 0.0;        // 楂樺害(绫? 妞悆楂?

    // 杞崲鐭╅樀
    glm::dmat4 enu_to_ecef_{1.0};           // ENU 鈫?ECEF
    glm::dmat4 ecef_to_enu_{1.0};           // ECEF 鈫?ENU
    glm::dmat4 axis_transform_{1.0};        // 杞存柟鍚戣浆鎹?

    // OGR鍧愭爣杞崲鍣?鐢ㄤ簬EPSG/WKT绫诲瀷)
    struct OGRCTDeleter {
        void operator()(OGRCoordinateTransformation* pCT) const {
            if (pCT != nullptr) {
                OGRCoordinateTransformation::DestroyCT(pCT);
            }
        }
    };
    std::unique_ptr<OGRCoordinateTransformation, OGRCTDeleter> ogr_transform_;
    // OGR CT is not thread-safe; guard all Transform() calls from rayon workers.
    mutable std::mutex ogr_mutex_;

    // Geoid閰嶇疆
    GeoidConfig geoid_config_;
};

} // namespace coords
