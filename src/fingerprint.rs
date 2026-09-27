use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};
use std::io::Read;
use std::path::Path;

pub fn compute_params_hash(
    max_lvl: Option<i32>,
    enable_texture_compress: bool,
    enable_meshopt: bool,
    enable_draco: bool,
    enable_unlit: bool,
) -> String {
    let mut hasher = DefaultHasher::new();
    max_lvl.hash(&mut hasher);
    enable_texture_compress.hash(&mut hasher);
    enable_meshopt.hash(&mut hasher);
    enable_draco.hash(&mut hasher);
    enable_unlit.hash(&mut hasher);
    format!("{:x}", hasher.finish())
}

pub fn compute_block_fingerprint(input_path: &Path) -> std::io::Result<String> {
    let metadata = std::fs::metadata(input_path)?;
    let size = metadata.len();
    let modified = metadata
        .modified()
        .map(|t| {
            t.duration_since(std::time::UNIX_EPOCH)
                .unwrap_or_default()
                .as_secs()
        })
        .unwrap_or(0);

    let mut hasher = DefaultHasher::new();
    size.hash(&mut hasher);
    modified.hash(&mut hasher);

    let mut file = std::fs::File::open(input_path)?;
    let mut buffer = vec![0u8; 8192.min(size as usize)];
    let n = file.read(&mut buffer)?;
    buffer[..n].hash(&mut hasher);

    Ok(format!("{:x}", hasher.finish()))
}

pub fn validate_block_output(output_dir: &Path) -> bool {
    if !output_dir.exists() || !output_dir.is_dir() {
        return false;
    }

    let tileset_path = output_dir.join("tileset.json");
    if !tileset_path.is_file() {
        return false;
    }

    if let Ok(content) = std::fs::read_to_string(&tileset_path) {
        if content.trim().is_empty() {
            return false;
        }
        if let Ok(json) = serde_json::from_str::<serde_json::Value>(&content) {
            if json.get("root").is_some() && json.get("geometricError").is_some() {
                return true;
            }
        }
    }

    false
}
