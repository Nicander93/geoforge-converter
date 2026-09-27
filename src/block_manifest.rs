use crate::block_job::BlockResult;
use std::fs::File;
use std::io::Write;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub struct BlockManifest {
    blocks: Vec<BlockResult>,
}

impl BlockManifest {
    pub fn new() -> Self {
        Self { blocks: Vec::new() }
    }

    pub fn add_block(&mut self, result: BlockResult) {
        self.blocks.push(result);
    }

    pub fn blocks(&self) -> &[BlockResult] {
        &self.blocks
    }

    pub fn sort_by_id(&mut self) {
        self.blocks.sort_by(|a, b| a.id.cmp(&b.id));
    }

    pub fn write_to_file(&self, path: &Path) -> std::io::Result<()> {
        let json = serde_json::json!({
            "version": 1,
            "blocks": self.blocks.iter().map(|b| {
                serde_json::json!({
                    "id": b.id,
                    "path": b.output_path.to_string_lossy(),
                    "boundingBox": b.bounding_box,
                    "geometricError": b.geometric_error,
                    "stats": {
                        "vertexCount": b.stats.vertex_count,
                        "triangleCount": b.stats.triangle_count,
                        "textureBytes": b.stats.texture_bytes,
                    }
                })
            }).collect::<Vec<_>>()
        });

        let mut file = File::create(path)?;
        file.write_all(serde_json::to_string_pretty(&json)?.as_bytes())?;
        Ok(())
    }

    pub fn load_from_file(path: &Path) -> std::io::Result<Self> {
        let content = std::fs::read_to_string(path)?;
        let json: serde_json::Value = serde_json::from_str(&content)?;

        let mut manifest = Self::new();
        if let Some(blocks) = json["blocks"].as_array() {
            for block in blocks {
                let result = BlockResult {
                    id: block["id"].as_str().unwrap_or("").to_string(),
                    output_path: PathBuf::from(
                        block["path"].as_str().unwrap_or(""),
                    ),
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
                        triangle_count: block["stats"]["triangleCount"].as_u64().unwrap_or(0),
                        texture_bytes: block["stats"]["textureBytes"].as_u64().unwrap_or(0),
                    },
                };
                manifest.add_block(result);
            }
        }

        Ok(manifest)
    }
}
