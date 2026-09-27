use std::path::PathBuf;
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone)]
pub struct BlockJob {
    pub id: String,
    pub input_path: PathBuf,
    pub output_dir: PathBuf,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(rename_all = "lowercase")]
pub enum BlockStatus {
    Pending,
    Running,
    Succeeded,
    Failed,
}

#[derive(Debug, Clone)]
pub struct BlockResult {
    pub id: String,
    pub output_path: PathBuf,
    pub bounding_box: [f64; 6],
    pub geometric_error: f64,
    pub stats: BlockStats,
}

#[derive(Debug, Clone, Default)]
pub struct BlockStats {
    pub vertex_count: u64,
    pub triangle_count: u64,
    pub texture_bytes: u64,
}

impl BlockJob {
    pub fn new(id: String, input_path: PathBuf, output_dir: PathBuf) -> Self {
        Self {
            id,
            input_path,
            output_dir,
        }
    }

    pub fn staging_dir(&self) -> PathBuf {
        self.output_dir.with_file_name(format!("{}_staging", self.id))
    }

    pub fn done_dir(&self) -> PathBuf {
        self.output_dir.clone()
    }
}

impl BlockResult {
    pub fn new(
        id: String,
        output_path: PathBuf,
        bounding_box: [f64; 6],
        geometric_error: f64,
    ) -> Self {
        Self {
            id,
            output_path,
            bounding_box,
            geometric_error,
            stats: BlockStats::default(),
        }
    }

    pub fn with_stats(mut self, stats: BlockStats) -> Self {
        self.stats = stats;
        self
    }
}
