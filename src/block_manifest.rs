use crate::block_job::{BlockResult, BlockStatus};
use std::collections::HashMap;
use std::fs::File;
use std::io::Write;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone)]
pub struct BlockEntry {
    pub id: String,
    pub status: BlockStatus,
    pub result: Option<BlockResult>,
    pub fingerprint: String,
    pub attempt_count: u32,
    pub error_message: Option<String>,
}

#[derive(Debug)]
pub struct BlockManifest {
    version: u32,
    converter_version: String,
    params_hash: String,
    entries: HashMap<String, BlockEntry>,
}

impl BlockManifest {
    pub fn new(converter_version: String, params_hash: String) -> Self {
        Self {
            version: 2,
            converter_version,
            params_hash,
            entries: HashMap::new(),
        }
    }

    pub fn add_pending_block(&mut self, id: String, fingerprint: String) {
        self.entries.insert(
            id.clone(),
            BlockEntry {
                id,
                status: BlockStatus::Pending,
                result: None,
                fingerprint,
                attempt_count: 0,
                error_message: None,
            },
        );
    }

    pub fn mark_running(&mut self, id: &str) {
        if let Some(entry) = self.entries.get_mut(id) {
            entry.status = BlockStatus::Running;
            entry.attempt_count += 1;
        }
    }

    pub fn mark_succeeded(&mut self, id: &str, result: BlockResult) {
        if let Some(entry) = self.entries.get_mut(id) {
            entry.status = BlockStatus::Succeeded;
            entry.result = Some(result);
            entry.error_message = None;
        }
    }

    pub fn mark_failed(&mut self, id: &str, error: String) {
        if let Some(entry) = self.entries.get_mut(id) {
            entry.status = BlockStatus::Failed;
            entry.error_message = Some(error);
        }
    }

    pub fn get_entry(&self, id: &str) -> Option<&BlockEntry> {
        self.entries.get(id)
    }

    pub fn succeeded_blocks(&self) -> Vec<&BlockResult> {
        self.entries
            .values()
            .filter_map(|e| {
                if e.status == BlockStatus::Succeeded {
                    e.result.as_ref()
                } else {
                    None
                }
            })
            .collect()
    }

    pub fn pending_or_failed_blocks(&self) -> Vec<&str> {
        self.entries
            .values()
            .filter_map(|e| match e.status {
                BlockStatus::Pending | BlockStatus::Failed | BlockStatus::Running => {
                    Some(e.id.as_str())
                }
                BlockStatus::Succeeded => None,
            })
            .collect()
    }

    pub fn sort_by_id(&mut self) -> Vec<String> {
        let mut ids: Vec<String> = self.entries.keys().cloned().collect();
        ids.sort();
        ids
    }

    pub fn converter_version(&self) -> &str {
        &self.converter_version
    }

    pub fn params_hash(&self) -> &str {
        &self.params_hash
    }

    pub fn write_to_file(&self, path: &Path) -> std::io::Result<()> {
        let entries_json: Vec<_> = self
            .entries
            .values()
            .map(|e| {
                let mut entry_obj = serde_json::json!({
                    "id": e.id,
                    "status": e.status,
                    "fingerprint": e.fingerprint,
                    "attemptCount": e.attempt_count,
                });

                if let Some(result) = &e.result {
                    entry_obj["path"] = serde_json::json!(result.output_path.to_string_lossy());
                    entry_obj["boundingBox"] = serde_json::json!(result.bounding_box);
                    entry_obj["geometricError"] = serde_json::json!(result.geometric_error);
                    entry_obj["stats"] = serde_json::json!({
                        "vertexCount": result.stats.vertex_count,
                        "triangleCount": result.stats.triangle_count,
                        "textureBytes": result.stats.texture_bytes,
                    });
                }

                if let Some(error) = &e.error_message {
                    entry_obj["error"] = serde_json::json!(error);
                }

                entry_obj
            })
            .collect();

        let json = serde_json::json!({
            "version": self.version,
            "converterVersion": self.converter_version,
            "paramsHash": self.params_hash,
            "blocks": entries_json,
        });

        let temp_path = path.with_extension("tmp");
        let mut file = File::create(&temp_path)?;
        file.write_all(serde_json::to_string_pretty(&json)?.as_bytes())?;
        file.sync_all()?;
        drop(file);

        std::fs::rename(&temp_path, path)?;
        Ok(())
    }

    pub fn load_from_file(path: &Path) -> std::io::Result<Self> {
        let content = std::fs::read_to_string(path)?;
        let json: serde_json::Value = serde_json::from_str(&content)?;

        let version = json["version"].as_u64().unwrap_or(1) as u32;
        let converter_version = json["converterVersion"]
            .as_str()
            .unwrap_or("")
            .to_string();
        let params_hash = json["paramsHash"].as_str().unwrap_or("").to_string();

        let mut entries = HashMap::new();

        if let Some(blocks) = json["blocks"].as_array() {
            for block in blocks {
                let id = block["id"].as_str().unwrap_or("").to_string();
                let status_str = block["status"].as_str().unwrap_or("succeeded");
                let status = match status_str {
                    "pending" => BlockStatus::Pending,
                    "running" => BlockStatus::Running,
                    "failed" => BlockStatus::Failed,
                    _ => BlockStatus::Succeeded,
                };

                let fingerprint = block["fingerprint"]
                    .as_str()
                    .unwrap_or("")
                    .to_string();
                let attempt_count = block["attemptCount"].as_u64().unwrap_or(0) as u32;
                let error_message = block["error"].as_str().map(|s| s.to_string());

                let result = if status == BlockStatus::Succeeded {
                    Some(BlockResult {
                        id: id.clone(),
                        output_path: PathBuf::from(block["path"].as_str().unwrap_or("")),
                        bounding_box: [
                            block["boundingBox"][0].as_f64().unwrap_or(0.0),
                            block["boundingBox"][1].as_f64().unwrap_or(0.0),
                            block["boundingBox"][2].as_f64().unwrap_or(0.0),
                            block["boundingBox"][3].as_f64().unwrap_or(0.0),
                            block["boundingBox"][4].as_f64().unwrap_or(0.0),
                            block["boundingBox"][5].as_f64().unwrap_or(0.0),
                        ],
                        geometric_error: block["geometricError"].as_f64().unwrap_or(0.0),
                        stats: crate::block_job::BlockStats {
                            vertex_count: block["stats"]["vertexCount"].as_u64().unwrap_or(0),
                            triangle_count: block["stats"]["triangleCount"]
                                .as_u64()
                                .unwrap_or(0),
                            texture_bytes: block["stats"]["textureBytes"]
                                .as_u64()
                                .unwrap_or(0),
                        },
                    })
                } else {
                    None
                };

                entries.insert(
                    id.clone(),
                    BlockEntry {
                        id,
                        status,
                        result,
                        fingerprint,
                        attempt_count,
                        error_message,
                    },
                );
            }
        }

        Ok(Self {
            version,
            converter_version,
            params_hash,
            entries,
        })
    }
}
