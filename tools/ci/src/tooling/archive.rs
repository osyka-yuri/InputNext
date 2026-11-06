use std::collections::HashMap;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Component, Path, PathBuf};

use anyhow::{Context, Result, ensure};
use flate2::read::GzDecoder;
use lzma_rust2::XzReader;
use tar::{Archive as TarArchive, EntryType};
use zip::ZipArchive;

pub(super) const MAX_ARCHIVE_BYTES: u64 = 3 * 1024 * 1024 * 1024;
const MAX_ENTRY_BYTES: u64 = 2 * 1024 * 1024 * 1024;
const MAX_EXPANDED_BYTES: u64 = 8 * 1024 * 1024 * 1024;
const MAX_ENTRIES: usize = 100_000;
const MAX_MEMBER_NAME_BYTES: usize = 2048;
const XZ_MEMORY_LIMIT_KIB: u32 = 1_048_576;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum ArchiveFormat {
    Zip,
    Tar,
    TarGz,
    TarXz,
}

impl ArchiveFormat {
    pub(super) fn parse(value: &str) -> Result<Self> {
        match value {
            "zip" => Ok(Self::Zip),
            "tar" => Ok(Self::Tar),
            "tar.gz" => Ok(Self::TarGz),
            "tar.xz" => Ok(Self::TarXz),
            _ => anyhow::bail!("unsupported locked archive format '{value}'"),
        }
    }
}

#[derive(Debug, Default)]
struct Audit {
    names: HashMap<String, MemberRecord>,
    count: usize,
    expanded_bytes: u64,
}

#[derive(Debug)]
struct MemberRecord {
    name: String,
    kind: MemberKind,
    explicit: bool,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum MemberKind {
    File,
    Directory,
}

impl Audit {
    fn add(&mut self, raw_name: &str, kind: MemberKind, size: u64) -> Result<String> {
        self.count += 1;
        ensure!(self.count <= MAX_ENTRIES, "archive has too many members");
        ensure!(
            size <= MAX_ENTRY_BYTES,
            "archive member exceeds the size limit"
        );
        self.expanded_bytes = self
            .expanded_bytes
            .checked_add(size)
            .context("archive member size overflow")?;
        ensure!(
            self.expanded_bytes <= MAX_EXPANDED_BYTES,
            "archive exceeds the expanded size limit"
        );

        let normalized = canonical_member_path(raw_name, kind)?;
        let parts = normalized.split('/').collect::<Vec<_>>();
        let mut parent = String::new();
        for (index, part) in parts.iter().enumerate() {
            if !parent.is_empty() {
                parent.push('/');
            }
            parent.push_str(part);
            let key = parent.to_lowercase();
            let expected_kind = if index + 1 == parts.len() {
                kind
            } else {
                MemberKind::Directory
            };
            let explicit = index + 1 == parts.len();
            if let Some(record) = self.names.get_mut(&key) {
                ensure!(
                    record.name == parent,
                    "archive contains case-colliding paths '{}' and '{parent}'",
                    record.name
                );
                ensure!(
                    record.kind == expected_kind,
                    "archive member '{parent}' conflicts with another member type"
                );
                if explicit {
                    ensure!(
                        !record.explicit,
                        "archive contains a duplicate member '{parent}'"
                    );
                    record.explicit = true;
                }
            } else {
                self.names.insert(
                    key,
                    MemberRecord {
                        name: parent.clone(),
                        kind: expected_kind,
                        explicit,
                    },
                );
            }
        }
        Ok(normalized)
    }
}

pub(super) fn inspect(path: &Path, format: ArchiveFormat) -> Result<()> {
    let mut audit = Audit::default();
    walk_archive(path, format, None, &mut audit)
}

pub(super) fn extract(path: &Path, format: ArchiveFormat, destination: &Path) -> Result<()> {
    ensure!(
        destination.is_absolute(),
        "archive destination must be absolute"
    );
    fs::create_dir_all(destination).context("could not create archive destination")?;
    let destination =
        dunce::canonicalize(destination).context("could not resolve archive destination")?;
    let mut audit = Audit::default();
    walk_archive(path, format, Some(&destination), &mut audit)
}

fn walk_archive(
    path: &Path,
    format: ArchiveFormat,
    destination: Option<&Path>,
    audit: &mut Audit,
) -> Result<()> {
    let archive_metadata = fs::symlink_metadata(path).context("could not inspect archive file")?;
    ensure!(archive_metadata.is_file(), "archive must be a regular file");
    ensure!(
        archive_metadata.len() <= MAX_ARCHIVE_BYTES,
        "archive exceeds the compressed size limit"
    );

    match format {
        ArchiveFormat::Zip => walk_zip(path, destination, audit),
        ArchiveFormat::Tar => walk_tar(File::open(path)?, destination, audit),
        ArchiveFormat::TarGz => walk_tar(GzDecoder::new(File::open(path)?), destination, audit),
        ArchiveFormat::TarXz => walk_tar(
            XzReader::new_mem_limit(File::open(path)?, false, XZ_MEMORY_LIMIT_KIB),
            destination,
            audit,
        ),
    }
}

fn walk_zip(path: &Path, destination: Option<&Path>, audit: &mut Audit) -> Result<()> {
    let file = File::open(path).context("could not open ZIP archive")?;
    let mut archive = ZipArchive::new(file).context("could not parse ZIP archive")?;
    ensure!(archive.len() <= MAX_ENTRIES, "archive has too many members");

    for index in 0..archive.len() {
        let mut member = archive
            .by_index(index)
            .with_context(|| format!("could not read ZIP member {index}"))?;
        ensure!(
            !member.encrypted(),
            "encrypted ZIP members are not supported"
        );
        ensure!(!member.is_symlink(), "ZIP symbolic links are not supported");

        let mode = member.unix_mode();
        if let Some(mode) = mode {
            let file_type = mode & 0o170_000;
            ensure!(
                file_type == 0 || file_type == 0o100_000 || file_type == 0o040_000,
                "ZIP member has a special file type"
            );
            ensure!(
                (file_type != 0o040_000 || member.is_dir())
                    && (file_type != 0o100_000 || !member.is_dir()),
                "ZIP member path and file type disagree"
            );
        }

        let kind = if member.is_dir() {
            MemberKind::Directory
        } else {
            MemberKind::File
        };
        let name = member.name()?.into_owned();
        let size = member.size();
        ensure!(
            kind != MemberKind::Directory || size == 0,
            "ZIP directory member contains data"
        );
        let member_path =
            audit.add(&name, kind, if kind == MemberKind::File { size } else { 0 })?;
        if let Some(root) = destination {
            let output_path = checked_output_path(root, &member_path)?;
            match kind {
                MemberKind::Directory => fs::create_dir_all(output_path)?,
                MemberKind::File => {
                    create_parent(&output_path)?;
                    let mut output = OpenOptions::new()
                        .write(true)
                        .create_new(true)
                        .open(&output_path)
                        .with_context(|| {
                            format!("could not create extracted member '{member_path}'")
                        })?;
                    copy_member(&mut member, &mut output, size)?;
                    #[cfg(unix)]
                    set_zip_permissions(&output_path, mode)?;
                    #[cfg(not(unix))]
                    set_zip_permissions(&output_path, mode);
                }
            }
        } else if kind == MemberKind::File {
            copy_member(&mut member, &mut io::sink(), size)?;
        }
    }
    Ok(())
}

fn walk_tar<R: Read>(reader: R, destination: Option<&Path>, audit: &mut Audit) -> Result<()> {
    let mut archive = TarArchive::new(reader);
    let entries = archive.entries().context("could not parse tar archive")?;
    for (index, entry) in entries.enumerate() {
        let mut member = entry.with_context(|| format!("could not read tar member {index}"))?;
        let entry_type = member.header().entry_type();
        let kind = match entry_type {
            EntryType::Regular => MemberKind::File,
            EntryType::Directory => MemberKind::Directory,
            _ => anyhow::bail!("tar member has an unsupported link or special file type"),
        };
        let member_name = member
            .path()
            .context("could not decode tar member path")?
            .to_str()
            .context("tar member path is not valid UTF-8")?
            .to_owned();
        let size = if kind == MemberKind::File {
            member
                .header()
                .size()
                .context("could not read tar member size")?
        } else {
            ensure!(
                member
                    .header()
                    .size()
                    .context("could not read tar directory size")?
                    == 0,
                "tar directory member contains data"
            );
            0
        };
        let member_path = audit.add(&member_name, kind, size)?;

        if let Some(root) = destination {
            let output_path = checked_output_path(root, &member_path)?;
            match kind {
                MemberKind::Directory => fs::create_dir_all(output_path)?,
                MemberKind::File => {
                    create_parent(&output_path)?;
                    let mut output = OpenOptions::new()
                        .write(true)
                        .create_new(true)
                        .open(&output_path)
                        .with_context(|| {
                            format!("could not create extracted member '{member_path}'")
                        })?;
                    copy_member(&mut member, &mut output, size)?;
                    #[cfg(unix)]
                    set_tar_permissions(&output_path, member.header().mode().ok())?;
                    #[cfg(not(unix))]
                    set_tar_permissions(&output_path, member.header().mode().ok());
                }
            }
        } else if kind == MemberKind::File {
            copy_member(&mut member, &mut io::sink(), size)?;
        }
    }
    Ok(())
}

fn canonical_member_path(raw_name: &str, kind: MemberKind) -> Result<String> {
    ensure!(
        !raw_name.is_empty(),
        "archive contains an empty member path"
    );
    ensure!(
        raw_name.len() <= MAX_MEMBER_NAME_BYTES,
        "archive member path is too long"
    );
    ensure!(
        !raw_name.contains(['\\', '\0', ':']),
        "archive member path contains a forbidden character"
    );
    ensure!(
        !raw_name.starts_with('/') && !raw_name.starts_with("//"),
        "archive member path is absolute"
    );

    let path = Path::new(raw_name);
    ensure!(
        path.components()
            .all(|component| matches!(component, Component::Normal(_))),
        "archive member path is not canonical"
    );

    let trimmed = if kind == MemberKind::Directory {
        raw_name.strip_suffix('/').unwrap_or(raw_name)
    } else {
        raw_name
    };
    ensure!(!trimmed.is_empty(), "archive contains an empty member path");
    let components = trimmed.split('/').collect::<Vec<_>>();
    ensure!(
        components.iter().all(|component| {
            !component.is_empty()
                && *component != "."
                && *component != ".."
                && !component.ends_with(['.', ' '])
                && !component
                    .chars()
                    .any(|character| matches!(character, '<' | '>' | '"' | '|' | '?' | '*'))
                && !is_windows_device_name(component)
        }),
        "archive member path has a noncanonical component"
    );
    Ok(trimmed.to_owned())
}

fn is_windows_device_name(component: &str) -> bool {
    let stem = component
        .split('.')
        .next()
        .unwrap_or_default()
        .to_ascii_uppercase();
    matches!(stem.as_str(), "CON" | "PRN" | "AUX" | "NUL")
        || ["COM", "LPT"].iter().any(|prefix| {
            stem.strip_prefix(prefix).is_some_and(|suffix| {
                matches!(suffix, "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9")
            })
        })
}

fn checked_output_path(root: &Path, member: &str) -> Result<PathBuf> {
    let output = root.join(member);
    ensure!(
        output.starts_with(root),
        "archive member escaped extraction root"
    );
    Ok(output)
}

fn create_parent(path: &Path) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).context("could not create extracted member parent")?;
    }
    Ok(())
}

fn copy_member<R: Read, W: Write>(input: &mut R, output: &mut W, declared_size: u64) -> Result<()> {
    let copied = io::copy(&mut input.take(MAX_ENTRY_BYTES + 1), output)
        .context("could not extract archive member")?;
    ensure!(
        copied <= MAX_ENTRY_BYTES,
        "archive member exceeds the size limit"
    );
    ensure!(
        copied == declared_size,
        "archive member size does not match its header"
    );
    output
        .flush()
        .context("could not flush extracted archive member")?;
    Ok(())
}

#[cfg(unix)]
fn set_zip_permissions(path: &Path, mode: Option<u32>) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;

    let permissions = mode.unwrap_or(0o644);
    ensure!(
        permissions & 0o7000 == 0,
        "archive member has special permission bits"
    );
    fs::set_permissions(path, fs::Permissions::from_mode(permissions & 0o777))
        .context("could not set extracted ZIP member permissions")
}

#[cfg(not(unix))]
fn set_zip_permissions(_path: &Path, _mode: Option<u32>) {}

#[cfg(unix)]
fn set_tar_permissions(path: &Path, mode: Option<u32>) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;

    let permissions = mode.unwrap_or(0o644);
    ensure!(
        permissions & 0o7000 == 0,
        "archive member has special permission bits"
    );
    fs::set_permissions(path, fs::Permissions::from_mode(permissions & 0o777))
        .context("could not set extracted tar member permissions")
}

#[cfg(not(unix))]
fn set_tar_permissions(_path: &Path, _mode: Option<u32>) {}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::io::{Cursor, Write};

    use anyhow::Result;
    use tempfile::tempdir;
    use zip::ZipWriter;
    use zip::write::SimpleFileOptions;

    use super::{ArchiveFormat, Audit, MemberKind, extract, inspect};

    #[test]
    fn member_audit_rejects_traversal_collisions_and_duplicates() {
        let mut audit = Audit::default();
        assert!(audit.add("../escape", MemberKind::File, 1).is_err());
        assert!(audit.add("A/file.txt", MemberKind::File, 1).is_ok());
        assert!(audit.add("a/other.txt", MemberKind::File, 1).is_err());

        let mut duplicates = Audit::default();
        assert!(duplicates.add("entry.txt", MemberKind::File, 1).is_ok());
        assert!(duplicates.add("entry.txt", MemberKind::File, 1).is_err());
        assert!(
            duplicates
                .add("too-large.bin", MemberKind::File, u64::MAX)
                .is_err()
        );
    }

    #[test]
    fn zip_extraction_writes_only_canonical_regular_members() -> Result<()> {
        let directory = tempdir()?;
        let archive_path = directory.path().join("tools.zip");
        let archive = fs::File::create(&archive_path)?;
        let mut writer = ZipWriter::new(archive);
        writer.start_file("bin/tool.exe", SimpleFileOptions::default())?;
        writer.write_all(b"pinned executable")?;
        writer.finish()?;

        let destination = directory.path().join("installed");
        extract(&archive_path, ArchiveFormat::Zip, &destination)?;
        assert_eq!(
            fs::read(destination.join("bin/tool.exe"))?,
            b"pinned executable"
        );
        Ok(())
    }

    #[test]
    fn zip_inspection_rejects_unsafe_paths_and_case_collisions() -> Result<()> {
        for (file_name, members) in [
            ("traversal.zip", vec!["../escape.txt"]),
            ("collision.zip", vec!["Bin/tool.exe", "bin/other.exe"]),
        ] {
            let directory = tempdir()?;
            let archive_path = directory.path().join(file_name);
            let mut writer = ZipWriter::new(fs::File::create(&archive_path)?);
            for name in members {
                writer.start_file(name, SimpleFileOptions::default())?;
                writer.write_all(b"data")?;
            }
            writer.finish()?;
            assert!(inspect(&archive_path, ArchiveFormat::Zip).is_err());
        }
        Ok(())
    }

    #[test]
    fn canonical_path_rules_reject_windows_devices_and_noncanonical_components() {
        let mut audit = Audit::default();
        for path in ["CON.txt", "name.", "folder//file", "C:/rooted"] {
            assert!(
                audit.add(path, MemberKind::File, 0).is_err(),
                "accepted {path}"
            );
        }
    }

    #[test]
    fn streaming_audit_does_not_need_to_retain_member_contents() {
        let mut input = Cursor::new(b"small content".to_vec());
        let mut output = Vec::new();
        assert!(super::copy_member(&mut input, &mut output, 13).is_ok());
        assert_eq!(output, b"small content");
    }
}
