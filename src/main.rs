extern crate clap;
extern crate serde;
#[macro_use]
extern crate serde_json;
extern crate serde_xml_rs;
#[macro_use]
extern crate log;
extern crate byteorder;
extern crate chrono;
extern crate env_logger;
extern crate libc;

mod common;
mod fbx;
pub mod fun_c;
mod osgb;
mod shape;

use chrono::prelude::*;
use clap::{Arg, ArgAction, Command};
use log::LevelFilter;
use serde::Deserialize;
use std::io::Write;

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct ModelConfig {
    #[serde(default = "default_model_config_version")]
    version: u32,
    model: ModelImportConfig,
    #[serde(default)]
    georeference: ModelGeoreference,
    #[serde(default)]
    texture: serde_json::Value,
    #[serde(default, rename = "modelOutput")]
    model_output: serde_json::Value,
}

fn default_model_config_version() -> u32 {
    1
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct ModelImportConfig {
    format: String,
    unit: String,
    axes: String,
    #[serde(default, rename = "missingTexturePolicy")]
    missing_texture_policy: Option<String>,
    #[serde(default, rename = "textureRoots")]
    texture_roots: Vec<String>,
}

#[derive(Debug, Deserialize)]
#[serde(tag = "mode", rename_all = "camelCase")]
enum ModelGeoreference {
    Local,
    Anchor {
        #[serde(rename = "longitudeDeg")]
        longitude_deg: f64,
        #[serde(rename = "latitudeDeg")]
        latitude_deg: f64,
        #[serde(rename = "ellipsoidHeightM")]
        ellipsoid_height_m: f64,
    },
    Projected {
        #[serde(rename = "sourceCrs")]
        source_crs: String,
        #[serde(rename = "axisMapping")]
        axis_mapping: ProjectedAxisMapping,
        #[serde(rename = "originOffset", default)]
        origin_offset: [f64; 3],
    },
}

#[derive(Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
enum ProjectedAxisMapping {
    EastNorthHeight,
    NorthEastHeight,
}

impl Default for ModelGeoreference {
    fn default() -> Self {
        Self::Local
    }
}

fn read_model_config(path: &str, format: &str) -> Result<ModelConfig, String> {
    let text = std::fs::read_to_string(path)
        .map_err(|error| format!("cannot read model config {path}: {error}"))?;
    let config: ModelConfig = serde_json::from_str(&text)
        .map_err(|error| format!("invalid model config {path}: {error}"))?;
    if config.version != 1 {
        return Err(format!(
            "unsupported model config version {}",
            config.version
        ));
    }
    if !config.model.format.eq_ignore_ascii_case(format) {
        return Err(format!(
            "model config format {} does not match --format {format}",
            config.model.format
        ));
    }
    if config.model.unit == "fromMetadata" && format.eq_ignore_ascii_case("obj") {
        return Err("OBJ model config requires an explicit unit".into());
    }
    if config.model.axes == "fromMetadata" && format.eq_ignore_ascii_case("obj") {
        return Err("OBJ model config requires explicit axes".into());
    }
    if !matches!(
        config.model.unit.as_str(),
        "fromMetadata" | "meters" | "centimeters" | "millimeters" | "feet"
    ) {
        return Err(format!("unsupported model unit {}", config.model.unit));
    }
    if !matches!(
        config.model.axes.as_str(),
        "fromMetadata" | "yUpRightHanded" | "zUpRightHanded"
    ) {
        return Err(format!("unsupported model axes {}", config.model.axes));
    }
    validate_missing_texture_policy(config.model.missing_texture_policy.as_deref())?;
    if let ModelGeoreference::Projected { source_crs, .. } = &config.georeference {
        if source_crs.trim().is_empty() {
            return Err("projected model config requires sourceCrs".into());
        }
    }
    Ok(config)
}

fn validate_missing_texture_policy(policy: Option<&str>) -> Result<(), String> {
    match policy {
        None | Some("warn" | "error") => Ok(()),
        Some(policy) => Err(format!(
            "unsupported missingTexturePolicy {policy}; expected warn or error"
        )),
    }
}

fn model_normalization(
    format: &str,
    model_config: Option<&ModelConfig>,
) -> Result<(f64, bool), String> {
    let Some(config) = model_config else {
        return Ok((1.0, false));
    };
    if format.eq_ignore_ascii_case("fbx") {
        if config.model.unit != "fromMetadata" || config.model.axes != "fromMetadata" {
            return Err("FBX supports only fromMetadata unit and axes; ufbx normalizes FBX metadata to meters and Y-up".into());
        }
        return Ok((1.0, false));
    }
    let unit_to_meters = match config.model.unit.as_str() {
        "meters" => 1.0,
        "centimeters" => 0.01,
        "millimeters" => 0.001,
        "feet" => 0.3048,
        _ => return Err("OBJ model config requires an explicit supported unit".into()),
    };
    let axes_z_up = match config.model.axes.as_str() {
        "yUpRightHanded" => false,
        "zUpRightHanded" => true,
        _ => return Err("OBJ model config requires explicit supported axes".into()),
    };
    Ok((unit_to_meters, axes_z_up))
}

/// Setup OpenSceneGraph environment variables for plugin loading
fn setup_osg_environment() {
    use std::env;

    // Get the executable directory
    let exe_path = env::current_exe().ok();
    let exe_dir = exe_path.as_ref().and_then(|p| p.parent());

    // Try to set OSG_LIBRARY_PATH if not already set
    if env::var("OSG_LIBRARY_PATH").is_err() {
        if let Some(dir) = exe_dir {
            // Check for osgPlugins directory next to the executable
            let plugins_dir = dir.join("osgPlugins-3.6.5");
            if plugins_dir.exists() {
                unsafe { env::set_var("OSG_LIBRARY_PATH", &plugins_dir) };
                info!("OSG_LIBRARY_PATH set to: {:?}", plugins_dir);
            } else {
                // Fallback: try to find plugins in common locations
                let alt_plugins = dir.join("lib").join("osgPlugins-3.6.5");
                if alt_plugins.exists() {
                    unsafe { env::set_var("OSG_LIBRARY_PATH", &alt_plugins) };
                    info!("OSG_LIBRARY_PATH set to: {:?}", alt_plugins);
                }
            }
        }
    }

    // Setup GDAL_DATA and PROJ_DATA paths
    if env::var("GDAL_DATA").is_err() {
        if let Some(dir) = exe_dir {
            let gdal_data = dir.join("gdal");
            if gdal_data.exists() {
                unsafe { env::set_var("GDAL_DATA", &gdal_data) };
            }
        }
    }

    if env::var("PROJ_DATA").is_err() {
        if let Some(dir) = exe_dir {
            let proj_data = dir.join("proj");
            if proj_data.exists() {
                unsafe { env::set_var("PROJ_DATA", &proj_data) };
            }
        }
    }
}

fn main() {
    use std::env;

    // Setup OSG plugin path for runtime plugin loading
    setup_osg_environment();

    if let Err(_) = env::var("RUST_LOG") {
        unsafe { env::set_var("RUST_LOG", "info") };
    }
    unsafe { env::set_var("RUST_BACKTRACE", "1") };
    let mut builder = env_logger::Builder::from_default_env();
    builder
        .format(|buf, record| {
            let dt = Local::now();
            writeln!(
                buf,
                "{}: {} - {}",
                record.level(),
                dt.format("%Y-%m-%d %H:%M:%S").to_string(),
                record.args()
            )
        })
        .filter(None, LevelFilter::Info)
        .init();
    //env_logger::init();
    let matches = Command::new("Make 3dtile program")
        .version(env!("CARGO_PKG_VERSION"))
        .author("fanvanzh <fanvanzh@sina.com>")
        .about("GeoForge converter: fast OSGB/model to 3D Tiles conversion")
        .arg(
            Arg::new("input")
                .short('i')
                .long("input")
                .value_name("FILE")
                .help("Set the input file (required for convert)")
                .required(false)
                .num_args(1),
        )
        .arg(
            Arg::new("output")
                .short('o')
                .long("output")
                .value_name("FILE")
                .help("Set the out file (required for convert)")
                .required(false)
                .num_args(1),
        )
        .arg(
            Arg::new("format")
                .short('f')
                .long("format")
                .value_name("osgb,shape,gltf,b3dm,fbx,obj")
                .help("Set input format (required for convert)")
                .required(false)
                .value_parser(["osgb", "shape", "gltf", "b3dm", "fbx", "obj"])
                .num_args(1),
        )
        .arg(
            Arg::new("config")
                .short('c')
                .long("config")
                .help(
                    "Set the tile config:
{
    \"x\": x,
    \"y\": y,
    \"offset\": 0,
    \"max_lvl\" : 20,
    \"pbr\" : false,
}",
                )
                .num_args(1),
        )
        .arg(
            Arg::new("model-config")
                .long("model-config")
                .value_name("FILE")
                .help("Versioned FBX/OBJ import and georeference configuration")
                .num_args(1),
        )
        .arg(
            Arg::new("capabilities-json")
                .long("capabilities-json")
                .help("Print machine-readable converter capabilities and exit")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("show-env-help")
                .long("show-env-help")
                .help("Display environment variable documentation and exit")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("height")
                .long("height")
                .help("Set the shapefile height field")
                .num_args(1),
        )
        .arg(
            Arg::new("verbose")
                .short('v')
                .long("verbose")
                .help("Set output verbose ")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("enable-draco")
                .long("enable-draco")
                .help("Enable Draco mesh compression")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("enable-simplify")
                .long("enable-simplify")
                .help("Enable mesh simplification")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("enable-texture-compress")
                .long("enable-texture-compress")
                .help("Enable texture compression (KTX2)")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("enable-lod")
                .long("enable-lod")
                .help("Enable LOD (Level of Detail) with default configuration")
                .action(ArgAction::SetTrue),
        )
        .arg(
            Arg::new("enable-unlit")
                .long("enable-unlit")
                .help("Enable KHR_materials_unlit extension (useful for baked lighting)")
                .action(ArgAction::SetTrue),
        )
        .arg(
           Arg::new("lon")
            .long("lon")
            .help("Set the longitude")
            .num_args(1),
        )
        .arg(
           Arg::new("lat")
            .long("lat")
            .help("Set the latitude")
            .num_args(1),
        )
        .arg(
           Arg::new("alt")
            .long("alt")
            .help("Set the altitude")
            .num_args(1),
        )
        .arg(
           Arg::new("geoid")
            .long("geoid")
            .help("Set the geoid model for height conversion (none, egm84, egm96, egm2008). Converts orthometric height (e.g., China 1985) to ellipsoidal height (WGS84)")
            .value_parser(["none", "egm84", "egm96", "egm2008"])
            .num_args(1),
        )
        .arg(
           Arg::new("geoid-path")
            .long("geoid-path")
            .help("Set the path to geoid data files (egm96-5.pgm, etc.). Default: GEOGRAPHICLIB_GEOID_PATH env or /usr/local/share/GeographicLib/geoids")
            .num_args(1),
        )
        .get_matches();

    if matches.get_flag("capabilities-json") {
        println!(
            "{}",
            serde_json::json!({
                "version": 1,
                "converterVersion": env!("CARGO_PKG_VERSION"),
                "converterName": "geoforge-converter",
                "formats": ["osgb", "fbx", "obj"],
                "modelConfigVersion": 1,
                "georeferenceModes": ["local", "anchor", "projected"],
                "projectedGeoreference": true,
                "features": {
                    "osgb": {
                        "supported": true,
                        "parallel": true,
                        "threadControl": "GEOFORGE_CONVERT_THREADS",
                        "defaultThreads": "available_parallelism/2, clamped 1-8"
                    },
                    "compression": {
                        "draco": true,
                        "ktx2": true,
                        "meshopt": true
                    },
                    "extensions": {
                        "KHR_draco_mesh_compression": true,
                        "KHR_texture_basisu": true,
                        "KHR_materials_unlit": true
                    }
                }
            })
        );
        return;
    }

    if matches.get_flag("show-env-help") {
        println!("GeoForge Converter Environment Variables:");
        println!();
        println!("GEOFORGE_CONVERT_THREADS");
        println!("  Controls OSGB parallel conversion worker threads.");
        println!("  Value: positive integer");
        println!("  Default: available_parallelism/2, clamped to 1-8");
        println!("  Example: GEOFORGE_CONVERT_THREADS=4");
        println!();
        println!("RUST_LOG");
        println!("  Controls logging verbosity.");
        println!("  Values: error, warn, info, debug, trace");
        println!("  Default: info");
        println!();
        println!("OSG_LIBRARY_PATH");
        println!("  OpenSceneGraph plugin search path (auto-detected).");
        println!();
        println!("GDAL_DATA, PROJ_DATA");
        println!("  GDAL/PROJ data paths for coordinate transformations (auto-detected).");
        println!();
        return;
    }

    let input = match matches.get_one::<String>("input") {
        Some(s) => s.as_str(),
        None => {
            error!("--input is required for convert");
            std::process::exit(2);
        }
    };
    let output = match matches.get_one::<String>("output") {
        Some(s) => s.as_str(),
        None => {
            error!("--output is required for convert");
            std::process::exit(2);
        }
    };
    let format = match matches.get_one::<String>("format") {
        Some(s) => s.as_str(),
        None => {
            error!("--format is required for convert");
            std::process::exit(2);
        }
    };
    let tile_config = matches
        .get_one::<String>("config")
        .map(|s| s.as_str())
        .unwrap_or("");
    let model_config = matches
        .get_one::<String>("model-config")
        .map(|path| read_model_config(path, format))
        .transpose();
    let model_config = match model_config {
        Ok(config) => config,
        Err(error) => {
            error!("{error}");
            std::process::exit(2);
        }
    };
    let height_field = matches
        .get_one::<String>("height")
        .map(|s| s.as_str())
        .unwrap_or("");

    let lat_val = matches
        .get_one::<String>("lat")
        .and_then(|s| s.parse::<f64>().ok());
    let lon_val = matches
        .get_one::<String>("lon")
        .and_then(|s| s.parse::<f64>().ok());
    let alt_val = matches
        .get_one::<String>("alt")
        .and_then(|s| s.parse::<f64>().ok());
    let geoid_model = matches
        .get_one::<String>("geoid")
        .map(|s| s.as_str())
        .unwrap_or("none");
    let geoid_path = matches
        .get_one::<String>("geoid-path")
        .map(|s| s.as_str())
        .unwrap_or("");

    // Parse feature flags
    let enable_draco = matches.get_flag("enable-draco");
    let enable_simplify = matches.get_flag("enable-simplify");
    let enable_texture_compress = matches.get_flag("enable-texture-compress");
    let enable_lod = matches.get_flag("enable-lod");
    let enable_unlit = matches.get_flag("enable-unlit");

    if matches.get_flag("verbose") {
        info!("set program versose on");
    }
    if enable_draco {
        info!("Draco compression enabled");
    }
    if enable_simplify {
        info!("Mesh simplification enabled");
    }
    if enable_texture_compress {
        info!("Texture compression (KTX2) enabled");
    }
    if enable_lod {
        info!("LOD (Level of Detail) enabled with default configuration [1.0, 0.5, 0.25]");
    }

    // Initialize geoid calculator if geoid model is specified
    if geoid_model != "none" {
        info!(
            "Initializing geoid model: {} with path: {}",
            geoid_model,
            if geoid_path.is_empty() {
                "default"
            } else {
                geoid_path
            }
        );
        let geoid_path_c = std::ffi::CString::new(geoid_path).unwrap_or_default();
        let geoid_model_c = std::ffi::CString::new(geoid_model).unwrap_or_default();
        let success = unsafe { fun_c::init_geoid(geoid_model_c.as_ptr(), geoid_path_c.as_ptr()) };
        if !success {
            error!(
                "Failed to initialize geoid model: {}. Height conversion will be disabled.",
                geoid_model
            );
        }
    }

    let in_path = std::path::Path::new(input);
    if !in_path.exists() {
        error!("{} does not exists.", input);
        std::process::exit(2);
    }
    // Keep an already-absolute Windows path in its normal UTF-8 form.  The
    // Windows canonicalize implementation may add the `\\?\` extended-path
    // prefix; the bundled OSG UTF-8 loader handles ordinary absolute paths
    // reliably, while that prefix can be reinterpreted by older OSG plugins.
    // Relative paths still get canonicalized so the native layer receives a
    // stable absolute path.
    let input_path = if in_path.is_absolute() {
        in_path.to_path_buf()
    } else {
        in_path.canonicalize().unwrap_or(in_path.to_path_buf())
    };
    let input = input_path.to_string_lossy();

    match format {
        "osgb" => {
            if !convert_osgb(
                &input,
                output,
                tile_config,
                enable_simplify,
                enable_texture_compress,
                enable_draco,
                enable_unlit,
            ) {
                std::process::exit(1);
            }
        }
        "shape" => {
            convert_shapefile(
                &input,
                output,
                height_field,
                enable_lod,
                enable_simplify,
                enable_draco,
            );
        }
        "gltf" => {
            convert_gltf(&input, output);
        }
        "b3dm" => {
            convert_b3dm(&input, output);
        }
        "fbx" | "obj" => {
            if let Err(error) = convert_model_cmd(
                format,
                &input,
                output,
                tile_config,
                model_config.as_ref(),
                enable_texture_compress,
                enable_simplify,
                enable_draco,
                enable_unlit,
                enable_lod,
                lat_val,
                lon_val,
                alt_val,
            ) {
                error!("FBX conversion failed: {error}");
                std::process::exit(1);
            }
        }
        _ => {
            error!("not support now.");
            std::process::exit(2);
        }
    }

    // The native conversion functions predate a Result-based CLI boundary and
    // report most failures through logs. Make the process status authoritative
    // for the processor: a missing final artifact is always a failed command.
    if !conversion_output_exists(format, output) {
        error!("conversion did not produce the expected output: {}", output);
        std::process::exit(1);
    }
}

fn runtime_cstring(name: &str) -> Result<std::ffi::CString, String> {
    let exe = std::env::current_exe()
        .map_err(|error| format!("cannot locate converter executable: {error}"))?;
    let parent = exe
        .parent()
        .ok_or_else(|| "converter executable has no parent directory".to_string())?;
    let path = parent.join(name);
    let text = path
        .to_str()
        .ok_or_else(|| format!("runtime path is not valid UTF-8: {}", path.display()))?;
    std::ffi::CString::new(text)
        .map_err(|_| format!("runtime path contains NUL byte: {}", path.display()))
}

fn conversion_output_exists(format: &str, output: &str) -> bool {
    let path = std::path::Path::new(output);
    match format {
        "osgb" | "shape" | "fbx" | "obj" => path.join("tileset.json").is_file(),
        "gltf" | "b3dm" => path.is_file(),
        _ => false,
    }
}

fn convert_model_cmd(
    format: &str,
    input: &str,
    output: &str,
    config: &str,
    model_config: Option<&ModelConfig>,
    enable_texture_compress: bool,
    enable_simplify: bool,
    enable_draco: bool,
    enable_unlit: bool,
    enable_lod: bool,
    lat: Option<f64>,
    lon: Option<f64>,
    height: Option<f64>,
) -> Result<(), String> {
    use serde_json::Value;

    validate_model_input(format, input)?;
    let (model_unit_to_meters, model_axes_z_up) = model_normalization(format, model_config)?;
    let texture_roots = model_config
        .map(|config| config.model.texture_roots.as_slice())
        .unwrap_or(&[]);
    let missing_texture_is_error = model_config
        .and_then(|config| config.model.missing_texture_policy.as_deref())
        == Some("error");

    let mut max_lvl: Option<i32> = None;
    // Default to CLI args, or 0.0
    let mut longitude = lon.unwrap_or(0.0);
    let mut latitude = lat.unwrap_or(0.0);
    let mut height_f = height.unwrap_or(0.0);
    let mut has_georeference = lon.is_some() || lat.is_some() || height.is_some();

    if let Some(model_config) = model_config {
        match &model_config.georeference {
            ModelGeoreference::Local => {}
            ModelGeoreference::Anchor {
                longitude_deg,
                latitude_deg,
                ellipsoid_height_m,
            } => {
                longitude = *longitude_deg;
                latitude = *latitude_deg;
                height_f = *ellipsoid_height_m;
                has_georeference = true;
            }
            ModelGeoreference::Projected {
                source_crs,
                axis_mapping,
                origin_offset,
            } => {
                fbx::convert_fbx_projected(
                    input,
                    output,
                    max_lvl,
                    enable_texture_compress,
                    enable_simplify,
                    enable_draco,
                    enable_unlit,
                    source_crs,
                    matches!(axis_mapping, ProjectedAxisMapping::NorthEastHeight),
                    *origin_offset,
                    model_unit_to_meters,
                    model_axes_z_up,
                    texture_roots,
                    missing_texture_is_error,
                )
                .map_err(|error| error.to_string())?;
                info!(
                    "Projected {} conversion finished successfully.",
                    format.to_uppercase()
                );
                return Ok(());
            }
        }
    }

    if !config.is_empty() {
        if let Ok(val) = serde_json::from_str::<Value>(config) {
            if let Some(lvl) = val["max_lvl"].as_i64() {
                max_lvl = Some(lvl as i32);
            }
            // Only use config values if CLI args are not provided
            if lon.is_none() {
                if let Some(x) = val["x"].as_f64() {
                    longitude = x;
                    has_georeference = true;
                }
            }
            if lat.is_none() {
                if let Some(y) = val["y"].as_f64() {
                    latitude = y;
                    has_georeference = true;
                }
            }
            if height.is_none() {
                if let Some(h) = val["height"].as_f64() {
                    height_f = h;
                    has_georeference = true;
                } else if let Some(offset) = val["offset"].as_f64() {
                    height_f = offset;
                    has_georeference = true;
                }
            }
        } else {
            return Err("config is not valid JSON".into());
        }
    }

    info!(
        "Starting {} conversion: {} -> {}",
        format.to_uppercase(),
        input,
        output
    );
    info!(
        "Origin: lon={}, lat={}, height={}",
        longitude, latitude, height_f
    );
    if enable_lod {
        warn!("LOD is not supported for {format}; flag will be ignored");
    }

    fbx::convert_fbx(
        input,
        output,
        max_lvl,
        enable_texture_compress,
        enable_simplify,
        enable_draco,
        enable_unlit,
        longitude,
        latitude,
        height_f,
        has_georeference,
        model_unit_to_meters,
        model_axes_z_up,
        texture_roots,
        missing_texture_is_error,
    )
    .map_err(|error| error.to_string())?;
    info!("FBX conversion finished successfully.");
    Ok(())
}

fn validate_model_input(format: &str, input: &str) -> Result<(), String> {
    let expected_extension = match format {
        "fbx" => "fbx",
        "obj" => "obj",
        _ => return Err(format!("unsupported model format: {format}")),
    };
    let extension = std::path::Path::new(input)
        .extension()
        .and_then(|value| value.to_str())
        .unwrap_or_default();
    if extension.eq_ignore_ascii_case(expected_extension) {
        return Ok(());
    }
    Err(format!(
        "--format {format} requires a .{expected_extension} input, got: {input}"
    ))
}

#[cfg(test)]
mod model_config_tests {
    use super::{
        model_normalization, validate_missing_texture_policy, ModelConfig, ModelGeoreference,
        ProjectedAxisMapping,
    };

    #[test]
    fn accepts_supported_missing_texture_policies() {
        assert!(validate_missing_texture_policy(None).is_ok());
        assert!(validate_missing_texture_policy(Some("warn")).is_ok());
        assert!(validate_missing_texture_policy(Some("error")).is_ok());
        assert!(validate_missing_texture_policy(Some("ignore")).is_err());
    }

    #[test]
    fn accepts_processor_model_config_shape() {
        let config: ModelConfig = serde_json::from_str(
            r#"{
              "version": 1,
              "model": {
                "format": "obj",
                "unit": "meters",
                "axes": "zUpRightHanded",
                "missingTexturePolicy": "error",
                "textureRoots": ["D:/models/textures"]
              },
              "georeference": {
                "mode": "anchor",
                "longitudeDeg": 0,
                "latitudeDeg": 0,
                "ellipsoidHeightM": 0
              },
              "texture": { "mode": "keep" },
              "modelOutput": { "format": "3dtiles-1.0", "tiling": "single", "lod": false }
            }"#,
        )
        .expect("processor config parses");
        assert_eq!(config.version, 1);
        assert_eq!(config.model.texture_roots.len(), 1);
        assert!(matches!(
            config.georeference,
            ModelGeoreference::Anchor { .. }
        ));
    }

    #[test]
    fn accepts_projected_model_config_shape() {
        let config: ModelConfig = serde_json::from_str(
            r#"{
              "version": 1,
              "model": {
                "format": "obj",
                "unit": "meters",
                "axes": "zUpRightHanded"
              },
              "georeference": {
                "mode": "projected",
                "sourceCrs": "EPSG:4547",
                "axisMapping": "northEastHeight",
                "originOffset": [500000, 3500000, 12]
              }
            }"#,
        )
        .expect("processor projected config parses");

        assert!(matches!(
            config.georeference,
            ModelGeoreference::Projected {
                axis_mapping: ProjectedAxisMapping::NorthEastHeight,
                origin_offset: [500000.0, 3500000.0, 12.0],
                ..
            }
        ));
    }

    #[test]
    fn normalizes_explicit_obj_units_and_axes() {
        let config: ModelConfig = serde_json::from_str(
            r#"{
              "version": 1,
              "model": {
                "format": "obj",
                "unit": "centimeters",
                "axes": "zUpRightHanded"
              },
              "georeference": { "mode": "local" }
            }"#,
        )
        .expect("OBJ config parses");

        assert_eq!(
            model_normalization("obj", Some(&config)).unwrap(),
            (0.01, true)
        );
    }

    #[test]
    fn rejects_explicit_fbx_units_and_axes() {
        let config: ModelConfig = serde_json::from_str(
            r#"{
              "version": 1,
              "model": {
                "format": "fbx",
                "unit": "meters",
                "axes": "zUpRightHanded"
              },
              "georeference": { "mode": "local" }
            }"#,
        )
        .expect("FBX config parses");

        assert!(model_normalization("fbx", Some(&config)).is_err());
    }
}

#[cfg(test)]
mod model_tests {
    use super::validate_model_input;

    #[test]
    fn accepts_model_format_matching_input_extension() {
        assert!(validate_model_input("fbx", "C:/模型/建筑.FBX").is_ok());
        assert!(validate_model_input("obj", "C:/模型/设备.obj").is_ok());
    }

    #[test]
    fn rejects_model_format_mismatch() {
        let error = validate_model_input("obj", "C:/模型/建筑.fbx").unwrap_err();
        assert!(error.contains("requires a .obj"));
    }
}

fn convert_b3dm(src: &str, dest: &str) {
    use std::fs::File;
    use std::io::prelude::*;
    use std::path::Path;

    use byteorder::{LittleEndian, ReadBytesExt};
    use std::io::Cursor;
    //use std::io::SeekFrom;

    if !dest.ends_with(".gltf") && !dest.ends_with(".glb") {
        error!("output format not support now: {}", dest);
        return;
    }
    if !src.ends_with(".b3dm") {
        error!("input format must be b3dm");
        return;
    }
    if Path::new(src).exists() && Path::new(src).is_file() {
        if let Ok(mut f) = File::open(src) {
            let mut buffer = Vec::new();
            if let Err(error) = f.read_to_end(&mut buffer) {
                error!("read b3dm failed: {error}");
                return;
            }
            if buffer.len() < 28 || &buffer[0..4] != b"b3dm" {
                error!("invalid b3dm header: {}", src);
                return;
            }
            let mut rdr = Cursor::new(buffer);
            let offset = {
                rdr.set_position(12);
                let fj_len = match rdr.read_u32::<LittleEndian>() {
                    Ok(value) => value as usize,
                    Err(error) => {
                        error!("invalid b3dm feature table length: {error}");
                        return;
                    }
                };
                let fb_len = match rdr.read_u32::<LittleEndian>() {
                    Ok(value) => value as usize,
                    Err(error) => {
                        error!("invalid b3dm feature table binary length: {error}");
                        return;
                    }
                };
                let bj_len = match rdr.read_u32::<LittleEndian>() {
                    Ok(value) => value as usize,
                    Err(error) => {
                        error!("invalid b3dm batch table JSON length: {error}");
                        return;
                    }
                };
                let bb_len = match rdr.read_u32::<LittleEndian>() {
                    Ok(value) => value as usize,
                    Err(error) => {
                        error!("invalid b3dm batch table binary length: {error}");
                        return;
                    }
                };
                28usize
                    .checked_add(fj_len)
                    .and_then(|value| value.checked_add(fb_len))
                    .and_then(|value| value.checked_add(bj_len))
                    .and_then(|value| value.checked_add(bb_len))
                    .unwrap_or(usize::MAX)
            };
            let buf = rdr.get_ref();
            if offset >= buf.len() {
                error!("b3dm tables exceed file: {}", src);
                return;
            }
            match File::create(dest).and_then(|mut file| file.write_all(&buf[offset..])) {
                Ok(()) => info!("wrote {}", dest),
                Err(error) => error!("write gltf failed: {error}"),
            }
        } else {
            error!("open b3dm failed: {}", src);
        }
    } else {
        error!("input b3dm does not exist: {}", src);
    }
}

// convert any thing to gltf
fn convert_gltf(src: &str, dest: &str) {
    use std::ffi::CString;
    if !dest.ends_with(".gltf") && !dest.ends_with(".glb") {
        error!("output format not support now: {}", dest);
        return;
    }
    if !src.ends_with(".osgb")
        && !src.ends_with(".osg")
        && !src.ends_with(".obj")
        && !src.ends_with(".fbx")
        && !src.ends_with(".3ds")
    {
        error!("input format not support now: {}", src);
        return;
    }
    unsafe {
        let c_str = match CString::new(dest) {
            Ok(value) => value,
            Err(error) => {
                error!("output path contains NUL: {error}");
                return;
            }
        };
        let ret = osgb::osgb2glb(src.as_ptr(), c_str.as_ptr() as *const u8);
        if !ret {
            error!("convert failed");
        } else {
            info!("task over");
        }
    }
}

#[allow(dead_code)]
#[allow(non_snake_case)]
#[derive(Debug, Deserialize, PartialEq)]
struct ModelMetadata {
    #[serde(rename = "@version")]
    pub version: String,
    pub SRS: String,
    pub SRSOrigin: String,
}

fn parse_origin_values(value: &str) -> Option<Vec<f64>> {
    let mut values = Vec::new();
    for part in value.split(',') {
        values.push(part.trim().parse::<f64>().ok()?);
    }
    Some(values)
}

fn convert_osgb(
    src: &str,
    dest: &str,
    config: &str,
    enable_simplify: bool,
    enable_texture_compress: bool,
    enable_draco: bool,
    enable_unlit: bool,
) -> bool {
    use serde_json::Value;
    use std::fs::File;
    use std::io::prelude::*;
    use std::time;

    info!(
        "GeoForge converter v{} starting OSGB conversion",
        env!("CARGO_PKG_VERSION")
    );
    info!("Input: {}, Output: {}", src, dest);

    let dir = std::path::Path::new(src);
    let dir_dest = std::path::Path::new(dest);

    let mut center_x = 0f64;
    let mut center_y = 0f64;
    let mut max_lvl = None;
    let mut trans_region = None;
    let mut enu_offset: Option<(f64, f64, f64)> = None;
    let mut origin_height: Option<f64> = None;

    // try parse metadata.xml
    let metadata_file = dir.join("metadata.xml");
    info!("Looking for metadata.xml at: {:?}", metadata_file);
    if metadata_file.exists() {
        info!("metadata.xml exists, reading...");
        // read and parse
        if let Ok(mut f) = File::open(&metadata_file) {
            let mut buffer = String::new();
            if let Ok(_) = f.read_to_string(&mut buffer) {
                info!("metadata.xml content: {}", buffer);
                //
                match serde_xml_rs::from_str::<ModelMetadata>(buffer.as_str()) {
                    Ok(metadata) => {
                        info!(
                            "Parsed metadata.xml: SRS={}, SRSOrigin={}",
                            metadata.SRS, metadata.SRSOrigin
                        );
                        let v: Vec<&str> = metadata.SRS.split(":").collect();
                        info!("SRS split result: {:?}", v);
                        if v.len() > 1 {
                            if v[0] == "ENU" {
                                let v1: Vec<&str> = v[1].split(",").collect();
                                if v1.len() > 1 {
                                    let v1_num = (*v1[0]).parse::<f64>();
                                    let v2_num = v1[1].parse::<f64>();
                                    if let (Ok(parsed_y), Ok(parsed_x)) = (v1_num, v2_num) {
                                        center_y = parsed_y;
                                        center_x = parsed_x;

                                        // Parse and apply SRSOrigin offset
                                        let origin_parts: Vec<&str> =
                                            metadata.SRSOrigin.split(",").collect();
                                        if origin_parts.len() >= 2 {
                                            if let (Ok(offset_x), Ok(offset_y)) = (
                                                origin_parts[0].parse::<f64>(),
                                                origin_parts[1].parse::<f64>(),
                                            ) {
                                                // Parse Z offset (height) if available
                                                let offset_z = if origin_parts.len() >= 3 {
                                                    match origin_parts[2].parse::<f64>() {
                                                        Ok(value) => value,
                                                        Err(_) => {
                                                            error!("OSGB_ENU_ORIGIN_INVALID: SRSOrigin z is not numeric");
                                                            return false;
                                                        }
                                                    }
                                                } else {
                                                    0.0
                                                };

                                                // Call enu_init to set up GeoTransform for geometry correction
                                                let gdal_c_str = match runtime_cstring("gdal") {
                                                    Ok(value) => value,
                                                    Err(error) => {
                                                        error!("ENU runtime setup failed: {error}");
                                                        return false;
                                                    }
                                                };
                                                let proj_c_str = match runtime_cstring("proj") {
                                                    Ok(value) => value,
                                                    Err(error) => {
                                                        error!("ENU runtime setup failed: {error}");
                                                        return false;
                                                    }
                                                };

                                                unsafe {
                                                    let mut origin_enu =
                                                        vec![offset_x, offset_y, offset_z];
                                                    let gdal_ptr = gdal_c_str.as_ptr();
                                                    let proj_ptr = proj_c_str.as_ptr();
                                                    if !osgb::enu_init(
                                                        center_x,
                                                        center_y,
                                                        origin_enu.as_mut_ptr(),
                                                        gdal_ptr,
                                                        proj_ptr,
                                                    ) {
                                                        error!("OSGB_ENU_INIT_FAILED: enu_init failed for SRSOrigin {offset_x},{offset_y},{offset_z}");
                                                        return false;
                                                    }
                                                }

                                                // ENU mode: OSGB vertices are in local coords relative to SRSOrigin.
                                                // Apply SRSOrigin offset via the root tileset transform matrix
                                                // (per-vertex Correction is skipped for ENU).
                                                enu_offset = Some((offset_x, offset_y, offset_z));
                                                // Use the geoid-corrected height from GeoTransform (if geoid is initialized)
                                                let geo_origin_height =
                                                    unsafe { osgb::get_geo_origin_height() };
                                                origin_height = Some(geo_origin_height);

                                                info!("ENU SRSOrigin offset detected: x={}, y={}, z={}", offset_x, offset_y, offset_z);
                                                info!("Using geographic origin for transform: lon={}, lat={}, h={}", center_x, center_y, geo_origin_height);
                                            } else {
                                                error!("OSGB_ENU_ORIGIN_INVALID: failed to parse SRSOrigin values");
                                                return false;
                                            }
                                        } else {
                                            error!("OSGB_ENU_ORIGIN_INVALID: SRSOrigin must contain x,y,z");
                                            return false;
                                        }
                                    } else {
                                        error!("OSGB_ENU_SRS_INVALID: ENU longitude/latitude is not numeric");
                                        return false;
                                    }
                                } else {
                                    error!("OSGB_ENU_SRS_INVALID: ENU SRS must contain latitude and longitude");
                                    return false;
                                }
                            } else if v[0] == "EPSG" {
                                // call gdal to convert
                                if let Ok(srs) = v[1].parse::<i32>() {
                                    let Some(mut pt) = parse_origin_values(&metadata.SRSOrigin)
                                    else {
                                        error!("SRSOrigin contains a non-numeric value");
                                        return false;
                                    };
                                    if pt.len() >= 3 {
                                        let gdal_c_str = match runtime_cstring("gdal") {
                                            Ok(value) => value,
                                            Err(error) => {
                                                error!("EPSG runtime setup failed: {error}");
                                                return false;
                                            }
                                        };
                                        let proj_c_str = match runtime_cstring("proj") {
                                            Ok(value) => value,
                                            Err(error) => {
                                                error!("EPSG runtime setup failed: {error}");
                                                return false;
                                            }
                                        };
                                        unsafe {
                                            let gdal_ptr = gdal_c_str.as_ptr();
                                            let proj_ptr = proj_c_str.as_ptr();
                                            if osgb::epsg_convert(
                                                srs,
                                                pt.as_mut_ptr(),
                                                gdal_ptr,
                                                proj_ptr,
                                            ) {
                                                center_x = pt[0];
                                                center_y = pt[1];
                                                // Use the geoid-corrected height from GeoTransform (if geoid is initialized)
                                                // This handles the conversion from orthometric height (China 1985) to ellipsoidal height (WGS84)
                                                let geo_origin_height =
                                                    osgb::get_geo_origin_height();
                                                origin_height = Some(geo_origin_height);
                                                info!("epsg: x->{}, y->{}, h={} (geoid-corrected from original h={})", pt[0], pt[1], geo_origin_height, pt[2]);
                                            } else {
                                                error!("OSGB_EPSG_TRANSFORM_FAILED: EPSG:{srs} origin conversion failed");
                                                return false;
                                            }
                                        }
                                    } else {
                                        error!("OSGB_EPSG_ORIGIN_INVALID: SRSOrigin must contain x,y,z");
                                        return false;
                                    }
                                } else {
                                    error!("OSGB_EPSG_SRS_INVALID: EPSG code is not numeric");
                                    return false;
                                }
                            //
                            } else {
                                error!("OSGB_SRS_INVALID: expected EPSG or ENU SRS");
                                return false;
                            }
                        } else {
                            // error!("SRS content error");
                            // treat as wkt
                            let Some(mut pt) = parse_origin_values(&metadata.SRSOrigin) else {
                                error!("SRSOrigin contains a non-numeric value");
                                return false;
                            };
                            if pt.len() >= 3 {
                                let gdal_c_str = match runtime_cstring("gdal_data") {
                                    Ok(value) => value,
                                    Err(error) => {
                                        error!("WKT runtime setup failed: {error}");
                                        return false;
                                    }
                                };
                                unsafe {
                                    let wkt: String = metadata.SRS;
                                    // println!("{:?}", wkt);
                                    let ptr = gdal_c_str.as_ptr();
                                    let wkt_cstr = match std::ffi::CString::new(wkt) {
                                        Ok(value) => value,
                                        Err(_) => {
                                            error!("WKT contains a NUL byte");
                                            return false;
                                        }
                                    };
                                    let wkt_ptr = wkt_cstr.as_ptr();
                                    if osgb::wkt_convert(wkt_ptr, pt.as_mut_ptr(), ptr) {
                                        center_x = pt[0];
                                        center_y = pt[1];
                                        info!("wkt: x->{}, y->{}", pt[0], pt[1]);
                                    } else {
                                        error!("OSGB_WKT_TRANSFORM_FAILED: WKT origin conversion failed");
                                        return false;
                                    }
                                }
                            } else {
                                error!("OSGB_WKT_ORIGIN_INVALID: SRSOrigin must contain x,y,z");
                                return false;
                            }
                        }
                    }
                    Err(e) => {
                        error!("OSGB_METADATA_INVALID: parse metadata.xml error: {}", e);
                        return false;
                    }
                }
            } else {
                error!(
                    "OSGB_METADATA_READ_FAILED: read {} failed",
                    metadata_file.display()
                );
                return false;
            }
        } else {
            error!(
                "OSGB_METADATA_READ_FAILED: open {} failed",
                metadata_file.display()
            );
            return false;
        }
    } else {
        error!(
            "OSGB_METADATA_MISSING: {} is missing",
            metadata_file.display()
        );
        return false;
    }
    if let Ok(v) = serde_json::from_str::<Value>(config) {
        if let Some(x) = v["x"].as_f64() {
            center_x = x;
        }
        if let Some(y) = v["y"].as_f64() {
            center_y = y;
        }
        if let Some(h) = v["offset"].as_f64() {
            trans_region = Some(h);
        }
        if let Some(lvl) = v["max_lvl"].as_i64() {
            max_lvl = Some(lvl as i32);
        }
    } else if config.len() > 0 {
        error!("OSGB_CONFIG_INVALID: config is not valid JSON: {}", config);
        unsafe {
            fun_c::cleanup_global_resources();
        }
        return false;
    }
    let tick = time::SystemTime::now();
    if let Err(e) = osgb::osgb_batch_convert(
        &dir,
        &dir_dest,
        max_lvl,
        center_x,
        center_y,
        trans_region,
        enu_offset,
        origin_height,
        enable_texture_compress,
        enable_simplify,
        enable_draco,
        enable_unlit,
    ) {
        error!("{}", e);
        unsafe {
            fun_c::cleanup_global_resources();
        }
        return false;
    }
    let elap_sec = tick.elapsed().unwrap_or_default();
    let tick_num = elap_sec.as_secs() as f64 + elap_sec.subsec_nanos() as f64 * 1e-9;
    info!("OSGB conversion completed successfully in {:.2}s", tick_num);
    unsafe {
        fun_c::cleanup_global_resources();
    }
    true
}

fn convert_shapefile(
    src: &str,
    dest: &str,
    height: &str,
    enable_lod: bool,
    enable_simplify: bool,
    enable_draco: bool,
) {
    if height.is_empty() {
        error!("you must set the height field by --height xxx");
        return;
    }
    let tick = std::time::SystemTime::now();

    let ret =
        shape::shape_batch_convert(src, dest, height, enable_lod, enable_simplify, enable_draco);
    if !ret {
        error!("convert shapefile failed");
    } else {
        let elap_sec = tick.elapsed().unwrap_or_default();
        let tick_num = elap_sec.as_secs() as f64 + elap_sec.subsec_nanos() as f64 * 1e-9;
        info!("task over, cost {:.2} s.", tick_num);
    }
}
