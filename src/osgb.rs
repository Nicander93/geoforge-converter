extern crate libc;
extern crate rayon;
extern crate serde;
extern crate serde_json;

use std::fs;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use rayon::prelude::*;

use std::error::Error;
use std::path::Path;

use crate::block_job::{BlockJob, BlockResult};
use crate::block_manifest::{BlockEntry, BlockManifest};
use crate::common::str_to_vec_c;
use crate::fingerprint;

extern "C" {

    fn osgb23dtile_path(
        in_path: *const u8,
        out_path: *const u8,
        box_ptr: *mut f64,
        len: *mut i32,
        x: f64,
        y: f64,
        max_lvl: i32,
        enable_texture_compress: bool,
        enable_meshopt: bool,
        enable_draco: bool,
        enable_unlit: bool,
    ) -> *mut libc::c_void;

    pub fn osgb2glb(name_in: *const u8, name_out: *const u8) -> bool;

    fn transform_c(radian_x: f64, radian_y: f64, height_min: f64, ptr: *mut f64);

    fn transform_c_with_enu_offset(
        center_x: f64,
        center_y: f64,
        height_min: f64,
        enu_offset_x: f64,
        enu_offset_y: f64,
        enu_offset_z: f64,
        ptr: *mut f64,
    );

    pub fn epsg_convert(
        insrs: i32,
        val: *mut f64,
        gdal: *const libc::c_char,
        proj: *const libc::c_char,
    ) -> bool;

    pub fn enu_init(
        lon: f64,
        lat: f64,
        origin_enu: *mut f64,
        gdal: *const libc::c_char,
        proj: *const libc::c_char,
    ) -> bool;

    pub fn wkt_convert(gdal: *const libc::c_char, val: *mut f64, gdal: *const libc::c_char)
        -> bool;

    fn degree2rad(val: f64) -> f64;

    #[allow(dead_code)]
    fn meter_to_lati(m: f64) -> f64;

    #[allow(dead_code)]
    fn meter_to_longti(m: f64, lati: f64) -> f64;

    pub fn get_geo_origin_height() -> f64;

}

enum WorkerMessage {
    Success(BlockResult),
    Error { block_id: String, error: String },
}

struct OsgbWorkerContext {
    job: BlockJob,
    sender: ::std::sync::mpsc::SyncSender<WorkerMessage>,
    cancel_flag: Arc<AtomicBool>,
}

fn convert_threads() -> usize {
    if let Ok(value) = std::env::var("GEOFORGE_CONVERT_THREADS") {
        if let Ok(parsed) = value.parse::<usize>() {
            if parsed > 0 {
                return parsed;
            }
        }
    }
    std::thread::available_parallelism()
        .map(|value| (value.get() / 2).clamp(1, 8))
        .unwrap_or(1)
}

pub fn osgb_batch_convert(
    dir: &Path,
    dir_dest: &Path,
    max_lvl: Option<i32>,
    center_x: f64,
    center_y: f64,
    region_offset: Option<f64>,
    enu_offset: Option<(f64, f64, f64)>,
    origin_height: Option<f64>,
    enable_texture_compress: bool,
    enable_meshopt: bool,
    enable_draco_compress: bool,
    enable_unlit: bool,
    enable_resume: bool,
) -> Result<(), Box<dyn Error>> {
    use std::fs::File;
    use std::io::prelude::*;
    use std::sync::mpsc::sync_channel;

    let path = dir.join("Data");
    if !path.exists() || !path.is_dir() {
        return Err(From::from(format!("dir {} not exist", path.display())));
    }

    fs::create_dir_all(dir_dest)?;
    let data_dir = dir_dest.join("Data");
    fs::create_dir_all(&data_dir)?;

    let mut jobs = vec![];
    for entry in fs::read_dir(&path)? {
        let entry = entry?;
        let path_tile = entry.path();
        if path_tile.is_dir() {
            let stem = path_tile
                .file_stem()
                .map(|value| value.to_string_lossy().into_owned())
                .ok_or_else(|| format!("tile directory has no name: {}", path_tile.display()))?;
            let osgb = path_tile.join(&stem).with_extension("osgb");
            if osgb.exists() && !osgb.is_dir() {
                let out_dir = data_dir.join(&stem);
                jobs.push(BlockJob::new(
                    stem,
                    osgb.to_path_buf(),
                    out_dir,
                ));
            } else {
                error!("dir error: {}", osgb.display());
            }
        }
    }

    if jobs.is_empty() {
        return Err("no valid OSGB tiles found".into());
    }

    jobs.sort_by(|a, b| a.id.cmp(&b.id));

    let converter_version = env!("CARGO_PKG_VERSION").to_string();
    let params_hash = fingerprint::compute_params_hash(
        max_lvl,
        enable_texture_compress,
        enable_meshopt,
        enable_draco_compress,
        enable_unlit,
    );

    let manifest_path = dir_dest.join("block_manifest.json");
    let mut manifest = if enable_resume && manifest_path.exists() {
        match BlockManifest::load_from_file(&manifest_path) {
            Ok(m) => {
                if m.converter_version() != converter_version {
                    log::warn!(
                        "Manifest converter version mismatch: {} vs {}. Starting fresh.",
                        m.converter_version(),
                        converter_version
                    );
                    BlockManifest::new(converter_version.clone(), params_hash.clone())
                } else if m.params_hash() != params_hash {
                    log::warn!(
                        "Manifest params hash mismatch: {} vs {}. Starting fresh.",
                        m.params_hash(),
                        params_hash
                    );
                    BlockManifest::new(converter_version.clone(), params_hash.clone())
                } else {
                    log::info!(
                        "Resuming from existing manifest: {} blocks",
                        m.pending_or_failed_blocks().len()
                    );
                    m
                }
            }
            Err(e) => {
                log::warn!("Failed to load manifest, starting fresh: {}", e);
                BlockManifest::new(converter_version.clone(), params_hash.clone())
            }
        }
    } else {
        BlockManifest::new(converter_version, params_hash)
    };

    let mut jobs_to_process = Vec::new();
    let mut reused_count = 0;

    for job in jobs {
        let fingerprint = match fingerprint::compute_block_fingerprint(&job.input_path) {
            Ok(fp) => fp,
            Err(e) => {
                log::warn!(
                    "Failed to compute fingerprint for {}: {}. Will process.",
                    job.id,
                    e
                );
                String::new()
            }
        };

        match manifest.get_entry(&job.id) {
            Some(entry) if entry.status == crate::block_job::BlockStatus::Succeeded => {
                if entry.fingerprint == fingerprint
                    && fingerprint::validate_block_output(&job.output_dir)
                {
                    log::info!("Reusing succeeded block: {}", job.id);
                    reused_count += 1;
                    continue;
                }
                log::info!(
                    "Block {} changed or invalid output, will reprocess",
                    job.id
                );
            }
            Some(entry) if entry.status == crate::block_job::BlockStatus::Running => {
                if entry.fingerprint == fingerprint
                    && fingerprint::validate_block_output(&job.output_dir)
                {
                    log::info!("Reclaiming completed block after crash: {}", job.id);
                    if let Some(result) = entry.result.clone() {
                        manifest.mark_succeeded(&job.id, result);
                        reused_count += 1;
                        continue;
                    }
                }
                log::info!("Block {} was running, will retry", job.id);
            }
            _ => {}
        }

        manifest.add_pending_block(job.id.clone(), fingerprint);
        jobs_to_process.push(job);
    }

    if reused_count > 0 {
        log::info!("Reused {} succeeded blocks", reused_count);
    }

    if jobs_to_process.is_empty() {
        log::info!("All blocks already completed, building root tileset");
        build_root_tileset(
            manifest,
            dir_dest,
            center_x,
            center_y,
            region_offset,
            enu_offset,
            origin_height,
        )?;
        return Ok(());
    }

    let thread_count = convert_threads();
    let queue_capacity = (thread_count * 2).max(4);
    
    log::info!(
        "OSGB conversion config: blocks={}, reused={}, to_process={}, threads={}, queue_capacity={}, max_lvl={}, texture_compress={}, meshopt={}, draco={}, unlit={}, resume={}",
        jobs_to_process.len() + reused_count,
        reused_count,
        jobs_to_process.len(),
        thread_count,
        queue_capacity,
        max_lvl.unwrap_or(100),
        enable_texture_compress,
        enable_meshopt,
        enable_draco_compress,
        enable_unlit,
        enable_resume
    );

    let (sender, receiver) = sync_channel(queue_capacity);
    let cancel_flag = Arc::new(AtomicBool::new(false));

    let rad_x = unsafe { degree2rad(center_x) };
    let rad_y = unsafe { degree2rad(center_y) };
    let max_lvl: i32 = max_lvl.unwrap_or(100);

    let total_jobs = jobs_to_process.len();
    let coordinator_cancel = cancel_flag.clone();
    let manifest_path_clone = manifest_path.clone();
    let coordinator_handle = std::thread::spawn(move || {
        coordinate_results(receiver, total_jobs, coordinator_cancel, manifest, manifest_path_clone)
    });

    let pool = rayon::ThreadPoolBuilder::new()
        .num_threads(thread_count)
        .build()?;

    let worker_result: Result<(), String> = pool.install(|| {
        jobs_to_process.into_par_iter()
            .map(|job| {
                if cancel_flag.load(Ordering::Relaxed) {
                    return Ok(());
                }

                let ctx = OsgbWorkerContext {
                    job: job.clone(),
                    sender: sender.clone(),
                    cancel_flag: cancel_flag.clone(),
                };

                process_block(
                    ctx,
                    rad_x,
                    rad_y,
                    max_lvl,
                    enable_texture_compress,
                    enable_meshopt,
                    enable_draco_compress,
                    enable_unlit,
                )
            })
            .collect::<Result<Vec<_>, _>>()
            .map(|_| ())
    });

    drop(sender);

    let manifest = coordinator_handle
        .join()
        .map_err(|_| "coordinator thread panicked")??;

    worker_result?;

    if manifest.succeeded_blocks().is_empty() {
        return Err("no blocks were successfully converted".into());
    }

    build_root_tileset(
        manifest,
        dir_dest,
        center_x,
        center_y,
        region_offset,
        enu_offset,
        origin_height,
    )?;

    Ok(())
}

fn process_block(
    ctx: OsgbWorkerContext,
    rad_x: f64,
    rad_y: f64,
    max_lvl: i32,
    enable_texture_compress: bool,
    enable_meshopt: bool,
    enable_draco: bool,
    enable_unlit: bool,
) -> Result<(), String> {
    use std::fs::File;
    use std::io::Write;

    if ctx.cancel_flag.load(Ordering::Relaxed) {
        return Ok(());
    }

    let staging_dir = ctx.job.staging_dir();
    if staging_dir.exists() {
        fs::remove_dir_all(&staging_dir)
            .map_err(|e| format!("failed to clean staging for {}: {}", ctx.job.id, e))?;
    }
    fs::create_dir_all(&staging_dir)
        .map_err(|e| format!("failed to create staging for {}: {}", ctx.job.id, e))?;

    let mut root_box = vec![0f64; 6];
    let mut json_buf = vec![];
    let mut json_len = 0i32;

    unsafe {
        let in_ptr = str_to_vec_c(ctx.job.input_path.to_string_lossy().as_ref());
        let out_ptr = str_to_vec_c(staging_dir.to_string_lossy().as_ref());
        let result_ptr = osgb23dtile_path(
            in_ptr.as_ptr(),
            out_ptr.as_ptr(),
            root_box.as_mut_ptr(),
            (&mut json_len) as *mut i32,
            rad_x,
            rad_y,
            max_lvl,
            enable_texture_compress,
            enable_meshopt,
            enable_draco,
            enable_unlit,
        );

        if result_ptr.is_null() {
            let error = format!("converter returned null for block {}", ctx.job.id);
            let _ = ctx.sender.send(WorkerMessage::Error {
                block_id: ctx.job.id.clone(),
                error,
            });
            return Ok(());
        }

        if json_len < 0 {
            libc::free(result_ptr);
            let error = format!("converter returned negative JSON length for block {}", ctx.job.id);
            let _ = ctx.sender.send(WorkerMessage::Error {
                block_id: ctx.job.id.clone(),
                error,
            });
            return Ok(());
        }

        if json_len as usize > 64 * 1024 * 1024 {
            libc::free(result_ptr);
            let error = format!("converter returned excessive JSON length for block {}: {} bytes", ctx.job.id, json_len);
            let _ = ctx.sender.send(WorkerMessage::Error {
                block_id: ctx.job.id.clone(),
                error,
            });
            return Ok(());
        }

        json_buf.resize(json_len as usize, 0);
        libc::memcpy(
            json_buf.as_mut_ptr() as *mut libc::c_void,
            result_ptr,
            json_len as usize,
        );
        libc::free(result_ptr);
    }

    let json_str = String::from_utf8(json_buf).map_err(|e| {
        format!("block {} JSON is not valid UTF-8: {}", ctx.job.id, e)
    })?;

    if json_str.is_empty() {
        let error = format!("converter returned empty JSON for block {}", ctx.job.id);
        let _ = ctx.sender.send(WorkerMessage::Error {
            block_id: ctx.job.id.clone(),
            error,
        });
        return Ok(());
    }

    let json_val: serde_json::Value = serde_json::from_str(&json_str)
        .map_err(|e| format!("invalid JSON for block {}: {}", ctx.job.id, e))?;

    let geometric_error = json_val["geometricError"].as_f64().unwrap_or(1000.0);

    let tileset_json = json!({
        "asset": {
            "version": "1.0",
            "gltfUpAxis": "Z"
        },
        "geometricError": geometric_error,
        "root": json_val
    });

    let tileset_path = staging_dir.join("tileset.json");
    let mut f = File::create(&tileset_path)
        .map_err(|e| format!("failed to create tileset for {}: {}", ctx.job.id, e))?;
    f.write_all(
        serde_json::to_string_pretty(&tileset_json)
            .map_err(|e| format!("failed to serialize tileset for {}: {}", ctx.job.id, e))?
            .as_bytes(),
    )
    .map_err(|e| format!("failed to write tileset for {}: {}", ctx.job.id, e))?;
    f.sync_all()
        .map_err(|e| format!("failed to sync tileset for {}: {}", ctx.job.id, e))?;
    drop(f);

    let done_dir = ctx.job.done_dir();
    if done_dir.exists() {
        fs::remove_dir_all(&done_dir)
            .map_err(|e| format!("failed to remove old output for {}: {}", ctx.job.id, e))?;
    }

    fs::rename(&staging_dir, &done_dir)
        .map_err(|e| format!("failed to commit block {}: {}", ctx.job.id, e))?;

    let result = BlockResult::new(
        ctx.job.id.clone(),
        done_dir,
        [
            root_box[0], root_box[1], root_box[2],
            root_box[3], root_box[4], root_box[5],
        ],
        geometric_error,
    );

    if ctx.sender.send(WorkerMessage::Success(result)).is_err() {
        log::warn!("coordinator dropped, block {} completed but not recorded", ctx.job.id);
    }

    Ok(())
}

fn coordinate_results(
    receiver: std::sync::mpsc::Receiver<WorkerMessage>,
    total_jobs: usize,
    cancel_flag: Arc<AtomicBool>,
    mut manifest: BlockManifest,
    manifest_path: PathBuf,
) -> Result<BlockManifest, String> {
    let mut completed = 0;
    let mut failed = Vec::new();

    while let Ok(msg) = receiver.recv() {
        match msg {
            WorkerMessage::Success(result) => {
                let block_id = result.id.clone();
                log::info!("Block {} completed successfully", block_id);
                manifest.mark_succeeded(&block_id, result);
                completed += 1;

                if let Err(e) = manifest.write_to_file(&manifest_path) {
                    log::warn!("Failed to write manifest after block completion: {}", e);
                }
            }
            WorkerMessage::Error { block_id, error } => {
                log::error!("Block {} failed: {}", block_id, error);
                manifest.mark_failed(&block_id, error.clone());
                failed.push((block_id, error));

                if let Err(e) = manifest.write_to_file(&manifest_path) {
                    log::warn!("Failed to write manifest after block failure: {}", e);
                }
            }
        }

        if completed + failed.len() >= total_jobs {
            break;
        }
    }

    if !failed.is_empty() {
        cancel_flag.store(true, Ordering::Relaxed);
        if let Err(e) = manifest.write_to_file(&manifest_path) {
            log::warn!("Failed to write final manifest: {}", e);
        }
        return Err(format!(
            "conversion failed: {}/{} blocks failed. First error: {}",
            failed.len(),
            total_jobs,
            failed[0].1
        ));
    }

    if let Err(e) = manifest.write_to_file(&manifest_path) {
        log::warn!("Failed to write final manifest: {}", e);
    }

    Ok(manifest)
}

fn build_root_tileset(
    manifest: BlockManifest,
    dir_dest: &Path,
    center_x: f64,
    center_y: f64,
    region_offset: Option<f64>,
    enu_offset: Option<(f64, f64, f64)>,
    origin_height: Option<f64>,
) -> Result<(), Box<dyn Error>> {
    use std::fs::File;
    use std::io::Write;

    let mut root_box = vec![-1.0E+38f64, -1.0E+38, -1.0E+38, 1.0E+38, 1.0E+38, 1.0E+38];
    let mut root_geometric_error = 0.0;

    let succeeded_blocks = manifest.succeeded_blocks();
    if succeeded_blocks.is_empty() {
        return Err("no succeeded blocks to build root tileset".into());
    }

    for block in succeeded_blocks.iter() {
        for i in 0..3 {
            if block.bounding_box[i] > root_box[i] {
                root_box[i] = block.bounding_box[i];
            }
        }
        for i in 3..6 {
            if block.bounding_box[i] < root_box[i] {
                root_box[i] = block.bounding_box[i];
            }
        }
        if block.geometric_error > root_geometric_error {
            root_geometric_error = block.geometric_error;
        }
    }

    let tras_height = if let Some(h) = origin_height {
        h
    } else if let Some((_, _, enu_z)) = enu_offset {
        enu_z
    } else if let Some(v) = region_offset {
        v - root_box[5]
    } else {
        0f64
    };

    let mut trans_vec = vec![0f64; 16];
    unsafe {
        if let Some((enu_x, enu_y, enu_z)) = enu_offset {
            transform_c_with_enu_offset(
                center_x,
                center_y,
                tras_height,
                enu_x,
                enu_y,
                enu_z,
                trans_vec.as_mut_ptr(),
            );
        } else {
            transform_c(center_x, center_y, tras_height, trans_vec.as_mut_ptr());
        }
    }

    let mut root_json = json!({
        "asset": {
            "version": "1.0",
            "gltfUpAxis": "Z"
        },
        "geometricError": root_geometric_error * 2.0,
        "root": {
            "transform": trans_vec,
            "boundingVolume": {
                "box": box_to_tileset_box(&root_box)
            },
            "geometricError": root_geometric_error * 2.0,
            "refine": "REPLACE",
            "children": []
        }
    });

    let mut sorted_blocks = succeeded_blocks.clone();
    sorted_blocks.sort_by(|a, b| a.id.cmp(&b.id));

    for block in sorted_blocks {
        let relative_path = block.output_path
            .strip_prefix(dir_dest)
            .map_err(|e| format!("block path outside output root: {}", e))?;
        let relative_uri = relative_path
            .to_string_lossy()
            .replace('\\', "/")
            .trim_start_matches('/')
            .to_string();

        if relative_uri.is_empty() {
            return Err(format!("empty relative URI for block {}", block.id).into());
        }

        let tile_uri = format!("./{}/tileset.json", relative_uri);
        
        let tileset_path = block.output_path.join("tileset.json");
        let tileset_content = fs::read_to_string(&tileset_path)
            .map_err(|e| format!("failed to read tileset for block {}: {}", block.id, e))?;
        let tileset_json: serde_json::Value = serde_json::from_str(&tileset_content)
            .map_err(|e| format!("invalid tileset JSON for block {}: {}", block.id, e))?;

        let tile_box = tileset_json["root"]["boundingVolume"]["box"]
            .as_array()
            .filter(|arr| arr.len() == 12)
            .ok_or_else(|| format!("block {} tileset has no valid bounding box", block.id))?;

        let tile_object = json!({
            "boundingVolume": {
                "box": tile_box
            },
            "geometricError": block.geometric_error,
            "content": {
                "uri": tile_uri
            }
        });

        root_json["root"]["children"]
            .as_array_mut()
            .ok_or("root children is not an array")?
            .push(tile_object);
    }

    let manifest_path = dir_dest.join("block_manifest.json");
    manifest.write_to_file(&manifest_path)?;

    let root_path = dir_dest.join("tileset.json");
    let mut f = File::create(root_path)?;
    f.write_all(serde_json::to_string_pretty(&root_json)?.as_bytes())?;
    f.sync_all()?;

    Ok(())
}


#[allow(dead_code)]
fn get_geometric_error(center_y: f64, lvl: i32) -> f64 {
    use std::f64;
    let x = center_y * f64::consts::PI / 180.0;
    let round = x.cos() * 2.0 * f64::consts::PI * 6378137.0;
    let pow = 2i32.pow(lvl as u32 - 2);
    4.0 * round / (256 * pow) as f64
}

fn box_to_tileset_box(box_v: &Vec<f64>) -> Vec<f64> {
    let mut box_new = vec![];
    box_new.push((box_v[0] + box_v[3]) / 2.0);
    box_new.push((box_v[1] + box_v[4]) / 2.0);
    box_new.push((box_v[2] + box_v[5]) / 2.0);

    box_new.push((box_v[3] - box_v[0]).abs() / 2.0);
    box_new.push(0.0);
    box_new.push(0.0);

    box_new.push(0.0);
    box_new.push((box_v[4] - box_v[1]).abs() / 2.0);
    box_new.push(0.0);

    box_new.push(0.0);
    box_new.push(0.0);
    box_new.push((box_v[5] - box_v[2]).abs() / 2.0);

    box_new
}
