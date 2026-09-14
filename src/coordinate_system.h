#pragma once

#include <string>
#include <variant>
#include <optional>
#include <tuple>
#include <glm/glm.hpp>

namespace coords {

// 坐标系类型枚举
enum class CoordinateType {
    Unknown,            // 未知/未初始化
    LocalCartesian,     // 本地笛卡尔坐标系(FBX, OSGB)
    ENU,                // ENU局部坐标系(倾斜摄影)
    EPSG,               // EPSG编码定义的坐标系
    WKT                 // WKT字符串定义的坐标系
};

// 坐标轴方向
enum class UpAxis {
    Y_UP,               // Y轴向上(FBX, glTF)
    Z_UP                // Z轴向上(OSGB)
};

// 坐标系手性
enum class Handedness {
    RightHanded,        // 右手坐标系
    LeftHanded          // 左手坐标系
};

// 高程基准类型
enum class VerticalDatum {
    Ellipsoidal,        // 椭球高（无需校正）
    Orthometric,        // 正高（需要Geoid校正）
    Unknown             // 未知（根据坐标系推断）
};

// 地理参考点
// 用于将本地坐标系原点映射到地球表面
struct GeoReference {
    double lon;                                     // 经度(度)
    double lat;                                     // 纬度(度)
    double height;                                  // 高度(米)
    VerticalDatum datum = VerticalDatum::Ellipsoidal;  // 高程基准

    static GeoReference FromDegrees(double lon, double lat, double height,
                                    VerticalDatum datum = VerticalDatum::Ellipsoidal) {
        return {lon, lat, height, datum};
    }
};

// 本地笛卡尔坐标系参数
// 适用于FBX、单个OSGB文件等无地理参考的数据
struct LocalCartesianParams {
    UpAxis up_axis = UpAxis::Y_UP;                  // 上轴方向
    Handedness handedness = Handedness::RightHanded; // 坐标系手性

    static LocalCartesianParams YUp() {
        return {UpAxis::Y_UP, Handedness::RightHanded};
    }
    static LocalCartesianParams ZUp() {
        return {UpAxis::Z_UP, Handedness::RightHanded};
    }
};

// ENU坐标系参数
// 适用于倾斜摄影metadata.xml中定义的ENU坐标系
// ENU是在地球表面某点建立的局部切平面坐标系
struct ENUParams {
    double origin_lon;      // ENU原点经度(度)
    double origin_lat;      // ENU原点纬度(度)
    double origin_height;   // ENU原点高度(米)
    double offset_x;        // SRSOrigin X偏移(米) - 东向
    double offset_y;        // SRSOrigin Y偏移(米) - 北向
    double offset_z;        // SRSOrigin Z偏移(米) - 天向

    // 获取ENU原点的地理参考
    GeoReference GetGeoReference() const {
        return {origin_lon, origin_lat, origin_height, VerticalDatum::Ellipsoidal};
    }
};

// EPSG坐标系参数
// EPSG编码可表示地理坐标系(如EPSG:4326)或投影坐标系(如EPSG:4545)
struct EPSGParams {
    int code;                                           // EPSG编码
    double origin_x;                                    // 原点X(投影坐标或经度)
    double origin_y;                                    // 原点Y(投影坐标或纬度)
    double origin_z;                                    // 原点Z(高度)
    VerticalDatum vertical_datum = VerticalDatum::Unknown;  // 高程基准
};

// WKT坐标系参数
// WKT字符串可定义任意坐标系
struct WKTParams {
    std::string wkt;                                    // WKT字符串
    double origin_x;                                    // 原点X
    double origin_y;                                    // 原点Y
    double origin_z;                                    // 原点Z
    VerticalDatum vertical_datum = VerticalDatum::Unknown;  // 高程基准
};

// 坐标系类
// 值类型，可安全复制和传递
class CoordinateSystem {
public:
    CoordinateSystem() = default;

    // ----- 工厂方法 -----

    // 创建本地笛卡尔坐标系(FBX, 单个OSGB)
    // 无地理参考概念，只有轴方向和手性
    static CoordinateSystem LocalCartesian(UpAxis up_axis = UpAxis::Y_UP,
                                           Handedness handedness = Handedness::RightHanded);
    static CoordinateSystem LocalCartesian(const LocalCartesianParams& params);

    // 创建ENU局部坐标系(倾斜摄影)
    // 自带地理参考信息
    static CoordinateSystem ENU(double lon, double lat, double height,
                                double offset_x, double offset_y, double offset_z);

    // 创建EPSG坐标系
    // 需要通过OGR转换得到地理参考
    static CoordinateSystem EPSG(int code,
                                 double origin_x, double origin_y, double origin_z,
                                 VerticalDatum vertical_datum = VerticalDatum::Unknown);

    // 创建WKT坐标系
    // 需要通过OGR转换得到地理参考
    static CoordinateSystem WKT(const std::string& wkt,
                                double origin_x, double origin_y, double origin_z,
                                VerticalDatum vertical_datum = VerticalDatum::Unknown);

    // ----- 查询方法 -----

    // 获取坐标系类型
    CoordinateType Type() const { return type_; }

    // 检查坐标系是否有效
    bool IsValid() const { return type_ != CoordinateType::Unknown; }

    // 是否需要OGR进行坐标转换
    // EPSG和WKT类型需要通过OGR转换到WGS84
    bool NeedsOGRTransform() const;

    // 是否自带地理参考
    // ENU类型自带地理参考，其他类型需要外部提供
    bool HasBuiltinGeoReference() const;

    // 获取内置地理参考(仅ENU类型有效)
    std::optional<GeoReference> GetBuiltinGeoReference() const;

    // 获取源坐标系原点
    // 返回投影坐标或偏移量
    std::tuple<double, double, double> GetSourceOrigin() const;

    // 获取各类型参数
    std::optional<ENUParams> GetENUParams() const;
    std::optional<LocalCartesianParams> GetLocalCartesianParams() const;
    std::optional<int> GetEPSGCode() const;
    std::optional<std::string> GetWKTString() const;

    // 获取高程基准
    VerticalDatum GetVerticalDatum() const;

    // 设置高程基准
    void SetVerticalDatum(VerticalDatum datum);

    // 获取轴方向
    UpAxis GetUpAxis() const;

    // 获取坐标系手性
    Handedness GetHandedness() const;

    // 转换为字符串(用于调试)
    std::string ToString() const;

private:
    CoordinateType type_ = CoordinateType::Unknown;
    std::variant<std::monostate, LocalCartesianParams, ENUParams, EPSGParams, WKTParams> params_;
};

} // namespace coords
