#include <cmath>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <memory>

/* vcpkg path */
#include <ogr_spatialref.h>
#include <ogrsf_frmts.h>

#include "extern.h"
#include "coordinate_transformer.h"
#include <glm/gtc/type_ptr.hpp>

///////////////////////
static const double pi = std::acos(-1);

// 全局坐标转换器实例
// 使用 shared_ptr 避免静态析构顺序问题
static std::shared_ptr<coords::CoordinateTransformer> g_transformer = nullptr;

// 获取全局坐标转换器
coords::CoordinateTransformer* GetGlobalTransformer() {
    return g_transformer.get();
}

extern "C" bool
epsg_convert(int insrs, double* val, char* gdal_data, char *proj_lib) {
    CPLSetConfigOption("GDAL_DATA", gdal_data);
    CPLSetConfigOption("PROJ_LIB", proj_lib);

    fprintf(stderr, "[SRS] EPSG:%d -> EPSG:4326 (axis=traditional)\n", insrs);
    fprintf(stderr, "[Origin ENU] x=%.6f y=%.6f z=%.3f\n", val[0], val[1], val[2]);

    // 创建EPSG坐标系
    auto cs = coords::CoordinateSystem::EPSG(insrs, val[0], val[1], val[2]);

    // 创建地理参考点（需要从原点转换得到）
    // 先创建一个临时的转换器来获取地理原点
    OGRSpatialReference inRs, outRs;
    OGRErr inErr = inRs.importFromEPSG(insrs);
    if (inErr != OGRERR_NONE) {
        LOG_E("importFromEPSG(%d) failed, err_code=%d", insrs, inErr);
        return false;
    }
    inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRErr outErr = outRs.importFromEPSG(4326);
    if (outErr != OGRERR_NONE) {
        LOG_E("importFromEPSG(4326) failed, err_code=%d", outErr);
        return false;
    }
    outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRCoordinateTransformation *poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
    if (!poCT) {
        LOG_E("Failed to create coordinate transformation");
        return false;
    }

    // 转换原点坐标
    double lon = val[0], lat = val[1], height = val[2];
    if (!poCT->Transform(1, &lon, &lat, &height)) {
        LOG_E("Failed to transform origin coordinates");
        OGRCoordinateTransformation::DestroyCT(poCT);
        return false;
    }

    fprintf(stderr, "[Origin LLA] lon=%.10f lat=%.10f\n", lon, lat);

    val[0] = lon;
    val[1] = lat;
    val[2] = height;

    auto geo_ref = coords::GeoReference::FromDegrees(lon, lat, height);

    // 创建全局坐标转换器（带Geoid配置）
    coords::GeoidConfig geoid_config;
    if (is_geoid_initialized()) {
        geoid_config = coords::GeoidConfig::EGM96();
        geoid_config.enabled = true;
    }
    g_transformer = std::make_shared<coords::CoordinateTransformer>(cs, geo_ref, geoid_config);

    // 销毁临时转换器
    OGRCoordinateTransformation::DestroyCT(poCT);

    return true;
}

extern "C" bool
enu_init(double lon, double lat, double* origin_enu, char* gdal_data, char* proj_lib) {
    CPLSetConfigOption("GDAL_DATA", gdal_data);
    CPLSetConfigOption("PROJ_LIB", proj_lib);

    fprintf(stderr, "[SRS] ENU:%.7f,%.7f (origin offset: %.3f, %.3f, %.3f)\n",
            lat, lon, origin_enu[0], origin_enu[1], origin_enu[2]);
    fprintf(stderr, "[Origin ENU] x=%.6f y=%.6f z=%.3f\n", origin_enu[0], origin_enu[1], origin_enu[2]);

    // 创建ENU坐标系
    auto cs = coords::CoordinateSystem::ENU(lon, lat, 0.0,
                                            origin_enu[0], origin_enu[1], origin_enu[2]);

    // ENU坐标系自带地理参考
    g_transformer = std::make_shared<coords::CoordinateTransformer>(cs);

    fprintf(stderr, "[Origin LLA] lon=%.10f lat=%.10f\n", lon, lat);
    return true;
}

extern "C" bool
wkt_convert(char* wkt, double* val, char* path) {
    CPLSetConfigOption("GDAL_DATA", path);

    const char* wkt_orig = wkt;
    fprintf(stderr, "[SRS] WKT -> EPSG:4326 (axis=traditional)\n");
    fprintf(stderr, "[Origin ENU] x=%.6f y=%.6f z=%.3f\n", val[0], val[1], val[2]);

    // 创建WKT坐标系
    auto cs = coords::CoordinateSystem::WKT(std::string(wkt_orig), val[0], val[1], val[2]);

    // 创建临时OGR转换器来获取地理原点
    OGRSpatialReference inRs, outRs;
    inRs.importFromWkt(&wkt);
    outRs.importFromEPSG(4326);
    inRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    outRs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRCoordinateTransformation *poCT = OGRCreateCoordinateTransformation(&inRs, &outRs);
    if (!poCT) {
        LOG_E("Failed to create coordinate transformation from WKT");
        return false;
    }

    // 转换原点坐标
    double lon = val[0], lat = val[1], height = val[2];
    if (!poCT->Transform(1, &lon, &lat, &height)) {
        LOG_E("Failed to transform origin coordinates");
        OGRCoordinateTransformation::DestroyCT(poCT);
        return false;
    }

    fprintf(stderr, "[Origin LLA] lon=%.10f lat=%.10f\n", lon, lat);

    val[0] = lon;
    val[1] = lat;
    val[2] = height;

    double final_height = height;
    if (is_geoid_initialized()) {
        double geoid_height = get_geoid_height(lat, lon);
        final_height = orthometric_to_ellipsoidal(lat, lon, height);
        fprintf(stderr, "[GeoTransform] Geoid correction applied: orthometric=%.3f + geoid=%.3f = ellipsoidal=%.3f\n",
                height, geoid_height, final_height);
    }

    // 创建地理参考
    auto geo_ref = coords::GeoReference::FromDegrees(lon, lat, final_height);

    // 创建全局坐标转换器
    g_transformer = std::make_shared<coords::CoordinateTransformer>(cs, geo_ref);

    // 销毁临时转换器
    OGRCoordinateTransformation::DestroyCT(poCT);

    return true;
}

extern "C"
{
	double degree2rad(double val) {
		return val * pi / 180.0;
	}
	double lati_to_meter(double diff) {
		return diff / 0.000000157891;
	}

	double longti_to_meter(double diff, double lati) {
		return diff / 0.000000156785 * std::cos(lati);
	}

	double meter_to_lati(double m) {
		return m * 0.000000157891;
	}

	double meter_to_longti(double m, double lati) {
		return m * 0.000000156785 / std::cos(lati);
	}
}

std::vector<double>
transfrom_xyz(double lon_deg, double lat_deg, double height_min){
    // 使用CoordinateTransformer的静态方法
    glm::dmat4 matrix = coords::CoordinateTransformer::CalcEnuToEcefMatrix(lon_deg, lat_deg, height_min);

    // 转换为vector格式（column-major）
    std::vector<double> result(16);
    const double* ptr = glm::value_ptr(matrix);
    std::memcpy(result.data(), ptr, 16 * sizeof(double));
    return result;
}

extern "C" void
transform_c(double center_x, double center_y, double height_min, double* ptr) {
    std::vector<double> v = transfrom_xyz(center_x, center_y, height_min);
    fprintf(stderr, "[transform_c] lon=%.10f lat=%.10f h=%.3f -> ECEF translation: x=%.10f y=%.10f z=%.10f\n",
            center_x, center_y, height_min, v[12], v[13], v[14]);
    std::memcpy(ptr, v.data(), v.size() * 8);
}

extern "C" void
transform_c_with_enu_offset(double center_x, double center_y, double height_min,
                           double enu_offset_x, double enu_offset_y, double enu_offset_z,
                           double* ptr) {
    std::vector<double> v = transfrom_xyz(center_x, center_y, height_min);
    fprintf(stderr, "[transform_c_with_enu_offset] Base ECEF at lon=%.10f lat=%.10f h=%.3f: x=%.10f y=%.10f z=%.10f\n",
            center_x, center_y, height_min, v[12], v[13], v[14]);

    // 应用ENU偏移到平移分量
    double lat_rad = center_y * pi / 180.0;
    double lon_rad = center_x * pi / 180.0;

    double sinLat = std::sin(lat_rad);
    double cosLat = std::cos(lat_rad);
    double sinLon = std::sin(lon_rad);
    double cosLon = std::cos(lon_rad);

    // ENU到ECEF转换
    double ecef_offset_x = -sinLon * enu_offset_x - sinLat * cosLon * enu_offset_y + cosLat * cosLon * enu_offset_z;
    double ecef_offset_y =  cosLon * enu_offset_x - sinLat * sinLon * enu_offset_y + cosLat * sinLon * enu_offset_z;
    double ecef_offset_z =  cosLat * enu_offset_y + sinLat * enu_offset_z;

    fprintf(stderr, "[transform_c_with_enu_offset] ENU offset (%.3f, %.3f, %.3f) -> ECEF offset (%.10f, %.10f, %.10f)\n",
            enu_offset_x, enu_offset_y, enu_offset_z, ecef_offset_x, ecef_offset_y, ecef_offset_z);

    v[12] += ecef_offset_x;
    v[13] += ecef_offset_y;
    v[14] += ecef_offset_z;

    fprintf(stderr, "[transform_c_with_enu_offset] Final ECEF translation: x=%.10f y=%.10f z=%.10f\n",
            v[12], v[13], v[14]);

    std::memcpy(ptr, v.data(), v.size() * 8);
}

bool
write_tileset_box(Transform* trans, Box& box, double geometricError,
                    const char* b3dm_file, const char* json_file) {
    std::vector<double> matrix;
    if (trans) {
        double lon_deg = trans->radian_x * 180.0 / pi;
        double lat_deg = trans->radian_y * 180.0 / pi;
        matrix = transfrom_xyz(lon_deg, lat_deg, trans->min_height);
    }
    std::string json_txt = "{\"asset\": {\
           \"version\": \"1.0\",\
           \"gltfUpAxis\": \"Z\"\
    },\
    \"geometricError\":";
    json_txt += std::to_string(geometricError);
    json_txt += ",\"root\": {";
    std::string trans_str = "\"transform\": [";
    if (trans) {
        for (int i = 0; i < 15 ; i++) {
            trans_str += std::to_string(matrix[i]);
            trans_str += ",";
        }
        trans_str += "1],";
        json_txt += trans_str;
    }
    json_txt += "\"boundingVolume\": {\
        \"box\": [";
    for (int i = 0; i < 11 ; i++) {
        json_txt += std::to_string(box.matrix[i]);
        json_txt += ",";
    }
    json_txt += std::to_string(box.matrix[11]);

    char last_buf[512];
    sprintf(last_buf,"]},\"geometricError\": %f,\
        \"refine\": \"REPLACE\",\
        \"content\": {\
            \"uri\": \"%s\"}}}", geometricError, b3dm_file);

    json_txt += last_buf;

    bool ret = write_file(json_file, json_txt.data(), (unsigned long)json_txt.size());
    if (!ret) {
        LOG_E("write file %s fail", json_file);
    }
    return ret;
}

bool write_tileset_region(
    Transform* trans,
    Region& region,
    double geometricError,
    const char* b3dm_file,
    const char* json_file)
{
    std::vector<double> matrix;
    if (trans) {
        double lon_deg = trans->radian_x * 180.0 / pi;
        double lat_deg = trans->radian_y * 180.0 / pi;
        matrix = transfrom_xyz(lon_deg, lat_deg, trans->min_height);
    }
    std::string json_txt = "{\"asset\": {\
           \"version\": \"1.0\",\
           \"gltfUpAxis\": \"Z\"\
    },\
    \"geometricError\":";
    json_txt += std::to_string(geometricError);
    json_txt += ",\"root\": {";
    std::string trans_str = "\"transform\": [";
    if (trans) {
        for (int i = 0; i < 15 ; i++) {
            trans_str += std::to_string(matrix[i]);
            trans_str += ",";
        }
        trans_str += "1],";
        json_txt += trans_str;
    }
    json_txt += "\"boundingVolume\": {\
    \"region\": [";
    double* pRegion = (double*)&region;
    for (int i = 0; i < 5 ; i++) {
        json_txt += std::to_string(pRegion[i]);
        json_txt += ",";
    }
    json_txt += std::to_string(pRegion[5]);

    char last_buf[512];
    sprintf(last_buf,"]},\"geometricError\": %f,\
        \"refine\": \"REPLACE\",\
        \"content\": {\
            \"uri\": \"%s\"}}}", geometricError, b3dm_file);

    json_txt += last_buf;

    bool ret = write_file(json_file, json_txt.data(), (unsigned long)json_txt.size());
    if (!ret) {
        LOG_E("write file %s fail", json_file);
    }
    return ret;
}

/***/
bool
write_tileset(double radian_x, double radian_y,
            double tile_w, double tile_h, double height_min, double height_max,
            double geometricError, const char* filename, const char* full_path) {
    double ellipsod_a = 40680631590769;
    double ellipsod_b = 40680631590769;
    double ellipsod_c = 40408299984661.4;

    const double pi = std::acos(-1);
    double lon_deg = radian_x * 180.0 / pi;
    double lat_deg = radian_y * 180.0 / pi;

    double xn = std::cos(radian_x) * std::cos(radian_y);
    double yn = std::sin(radian_x) * std::cos(radian_y);
    double zn = std::sin(radian_y);

    double x0 = ellipsod_a * xn;
    double y0 = ellipsod_b * yn;
    double z0 = ellipsod_c * zn;
    double gamma = std::sqrt(xn*x0 + yn*y0 + zn*z0);
    double px = x0 / gamma;
    double py = y0 / gamma;
    double pz = z0 / gamma;
    double dx = x0 * height_min;
    double dy = y0 * height_min;
    double dz = z0 * height_min;

    std::vector<double> east_mat = {-y0,x0,0};
    std::vector<double> north_mat = {
        (y0*east_mat[2] - east_mat[1]*z0),
        (z0*east_mat[0] - east_mat[2]*x0),
        (x0*east_mat[1] - east_mat[0]*y0)
    };
    double east_normal = std::sqrt(
        east_mat[0]*east_mat[0] +
        east_mat[1]*east_mat[1] +
        east_mat[2]*east_mat[2]
        );
    double north_normal = std::sqrt(
        north_mat[0]*north_mat[0] +
        north_mat[1]*north_mat[1] +
        north_mat[2]*north_mat[2]
        );

    std::vector<double> matrix = transfrom_xyz(lon_deg, lat_deg, height_min);

    double half_w = tile_w * 0.5;
    double half_h = tile_h * 0.5;
    double half_z = (height_max - height_min) * 0.5;
    double center_z = half_z;

    std::string json_txt = "{\"asset\": {\
        \"version\": \"0.0\",\
        \"gltfUpAxis\": \"Y\"\
    },\
    \"geometricError\":";
    json_txt += std::to_string(geometricError);
    json_txt += ",\"root\": {\
        \"transform\": [";
    for (int i = 0; i < 15 ; i++) {
        json_txt += std::to_string(matrix[i]);
        json_txt += ",";
    }
    json_txt += "1],\
    \"boundingVolume\": {\
    \"box\": [";
    double box_vals[12] = {
        0.0, 0.0, center_z,
        half_w, 0.0, 0.0,
        0.0, half_h, 0.0,
        0.0, 0.0, half_z
    };
    for (int i = 0; i < 11 ; i++) {
        json_txt += std::to_string(box_vals[i]);
        json_txt += ",";
    }
    json_txt += std::to_string(box_vals[11]);

    char last_buf[512];
    sprintf(last_buf,"]},\"geometricError\": %f,\
        \"refine\": \"REPLACE\",\
        \"content\": {\
        \"uri\": \"%s\"}}}", geometricError, filename);

    json_txt += last_buf;

    bool ret = write_file(full_path, json_txt.data(), (unsigned long)json_txt.size());
    if (!ret) {
        LOG_E("write file %s fail", filename);
    }
    return ret;
}

// 新增：获取全局坐标转换器的FFI接口
extern "C" void* get_coordinate_transformer() {
    return static_cast<void*>(g_transformer.get());
}

// 获取地理原点高度的FFI接口
extern "C" double get_geo_origin_height() {
    if (g_transformer && g_transformer->HasGeoReference()) {
        return g_transformer->GeoOriginHeight();
    }
    return 0.0;
}

// Geoid height conversion functions - C FFI implementations
#include "GeoidHeight.h"

extern "C" bool init_geoid(const char* model, const char* geoid_path) {
    std::string model_str = model ? model : "none";
    std::string path_str = geoid_path ? geoid_path : "";

    GeoidHeight::GeoidModel geoid_model = GeoidHeight::GeoidCalculator::StringToGeoidModel(model_str);
    return GeoidHeight::InitializeGlobalGeoidCalculator(geoid_model, path_str);
}

extern "C" double get_geoid_height(double lat, double lon) {
    auto& calculator = GeoidHeight::GetGlobalGeoidCalculator();
    auto height = calculator.GetGeoidHeight(lat, lon);
    return height.value_or(0.0);
}

extern "C" double orthometric_to_ellipsoidal(double lat, double lon, double orthometric_height) {
    auto& calculator = GeoidHeight::GetGlobalGeoidCalculator();
    return calculator.ConvertOrthometricToEllipsoidal(lat, lon, orthometric_height);
}

extern "C" double ellipsoidal_to_orthometric(double lat, double lon, double ellipsoidal_height) {
    auto& calculator = GeoidHeight::GetGlobalGeoidCalculator();
    return calculator.ConvertEllipsoidalToOrthometric(lat, lon, ellipsoidal_height);
}

extern "C" bool is_geoid_initialized() {
    return GeoidHeight::GetGlobalGeoidCalculator().IsInitialized();
}

extern "C" void cleanup_global_resources() {
    g_transformer.reset();
}
