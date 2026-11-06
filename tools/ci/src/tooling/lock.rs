use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use anyhow::{Context, Result, ensure};
use serde::Deserialize;

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub(super) struct ToolchainLock {
    pub schema_version: u32,
    pub tools: BTreeMap<String, ToolDefinition>,
    pub dependencies: BTreeMap<String, DependencyDefinition>,
    pub compilers: BTreeMap<String, CompilerDefinition>,
}

impl ToolchainLock {
    pub(super) fn read(path: &Path) -> Result<Self> {
        let metadata = fs::symlink_metadata(path)
            .with_context(|| format!("could not inspect toolchain lock {}", path.display()))?;
        ensure!(
            metadata.is_file() && !metadata.file_type().is_symlink(),
            "toolchain lock must be a regular file"
        );
        let contents = fs::read_to_string(path)
            .with_context(|| format!("could not read toolchain lock {}", path.display()))?;
        let lock: Self = serde_json::from_str(&contents)
            .with_context(|| format!("toolchain lock {} is not valid JSON", path.display()))?;
        ensure!(
            lock.schema_version == 1,
            "unsupported toolchain lock schema"
        );
        Ok(lock)
    }
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub(super) struct ToolDefinition {
    pub version: String,
    pub platforms: BTreeMap<String, ToolAsset>,
    #[serde(default)]
    pub kind: Option<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub(super) struct ToolAsset {
    pub url: String,
    pub sha256: String,
    pub archive: String,
    pub executable: String,
    pub probe_arguments: Vec<String>,
    pub version_regex: String,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub(super) struct DependencyDefinition {
    pub version: String,
    pub url: String,
    pub sha256: String,
    pub archive: String,
    pub extract_root: String,
    pub expected_paths: Vec<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub(super) struct CompilerDefinition {
    pub toolset_version: String,
    pub vcvars_version: String,
    pub cl_file_version_regex: String,
    pub bootstrapper: Option<CompilerBootstrapper>,
    pub components: Vec<String>,
}

#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub(super) struct CompilerBootstrapper {
    pub url: String,
    pub sha256: String,
    pub channel_uri: String,
    pub install_channel_uri: String,
}
