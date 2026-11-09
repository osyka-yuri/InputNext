//! SPDX 2.2 runtime inventory generation and validation.

use std::collections::BTreeSet;

use anyhow::{Context, Result, ensure};
use serde_json::{Value, json};
use sha1::{Digest, Sha1};
use time::OffsetDateTime;
use time::format_description::well_known::Rfc3339;

use super::identity::ReleaseIdentity;
use super::package::{DependencyPin, SourcePins, ValidatedPackage, hex_encode};

const GAMEINPUT_LICENSE_FILE: &str = "licenses/third-party/Microsoft-GameInput.txt";

pub(super) fn generate(
    package: &ValidatedPackage,
    architecture: &str,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
) -> Result<Vec<u8>> {
    let value = document(package, architecture, identity, pins)?;
    validate_value(&value, package, architecture, identity, pins)?;
    let mut bytes =
        serde_json::to_vec_pretty(&value).context("could not serialize SPDX document")?;
    bytes.push(b'\n');
    Ok(bytes)
}

pub(super) fn validate(
    bytes: &[u8],
    package: &ValidatedPackage,
    architecture: &str,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
) -> Result<Value> {
    let value = super::strict_json::parse_slice(bytes).context("SPDX document is invalid JSON")?;
    validate_value(&value, package, architecture, identity, pins)?;
    Ok(value)
}

fn document(
    package: &ValidatedPackage,
    architecture: &str,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
) -> Result<Value> {
    let name = runtime_name(identity.version(), architecture);
    let created = OffsetDateTime::now_utc()
        .format(&Rfc3339)
        .context("could not format SPDX creation timestamp")?;
    let gameinput_license = package
        .selected_contents
        .get(GAMEINPUT_LICENSE_FILE)
        .context("runtime package does not contain the GameInput license")?;
    let gameinput_license =
        std::str::from_utf8(gameinput_license).context("GameInput license is not valid UTF-8")?;

    let files = file_entries(package);
    let packages = package_entries(package, identity, pins, &name);
    let relationships = package_relationships(package);
    let namespace = format!(
        "https://spdx.org/spdxdocs/inputnext/{}/{architecture}/{}/{}",
        identity.version(),
        identity.source_commit(),
        package.sha256
    );
    Ok(json!({
        "spdxVersion": "SPDX-2.2",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": name,
        "documentNamespace": namespace,
        "creationInfo": {
            "creators": ["Tool: inputnext-ci"],
            "created": created
        },
        "packages": packages,
        "files": files,
        "relationships": relationships,
        "hasExtractedLicensingInfos": [{
            "licenseId": "LicenseRef-Microsoft-GameInput",
            "name": "Microsoft GameInput Software License Terms",
            "extractedText": gameinput_license,
            "seeAlsos": [pins.game_input.download_location]
        }]
    }))
}

fn file_entries(package: &ValidatedPackage) -> Vec<Value> {
    package
        .files
        .iter()
        .enumerate()
        .map(|(index, (path, digest))| {
            json!({
                "fileName": format!("./{path}"),
                "SPDXID": format!("SPDXRef-File-{}", index + 1),
                "checksums": [
                    {"algorithm": "SHA1", "checksumValue": digest.sha1},
                    {"algorithm": "SHA256", "checksumValue": digest.sha256}
                ],
                "fileTypes": [file_type(path)],
                "licenseConcluded": "NOASSERTION",
                "licenseInfoInFiles": ["NOASSERTION"],
                "copyrightText": "NOASSERTION"
            })
        })
        .collect()
}

fn package_entries(
    package: &ValidatedPackage,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
    name: &str,
) -> Vec<Value> {
    let verification_code = package_verification_code(package);
    vec![
        json!({
            "name": name,
            "SPDXID": "SPDXRef-Package-InputNext",
            "versionInfo": identity.version(),
            "downloadLocation": "NOASSERTION",
            "filesAnalyzed": true,
            "packageVerificationCode": {"packageVerificationCodeValue": verification_code},
            "packageFileName": package.path.file_name().and_then(|value| value.to_str()),
            "checksums": [
                {"algorithm": "SHA1", "checksumValue": package.sha1},
                {"algorithm": "SHA256", "checksumValue": package.sha256}
            ],
            "licenseConcluded": "NOASSERTION",
            "licenseDeclared": "MIT",
            "copyrightText": "NOASSERTION"
        }),
        dependency_package(
            &pins.game_input,
            "SPDXRef-Package-Microsoft-GameInput",
            "LicenseRef-Microsoft-GameInput",
        ),
        dependency_package(&pins.tomlplusplus, "SPDXRef-Package-tomlplusplus", "MIT"),
    ]
}

fn package_relationships(package: &ValidatedPackage) -> Vec<Value> {
    let mut relationships = vec![relationship(
        "SPDXRef-DOCUMENT",
        "DESCRIBES",
        "SPDXRef-Package-InputNext",
    )];
    relationships.extend(package.files.keys().enumerate().map(|(index, _)| {
        relationship(
            "SPDXRef-Package-InputNext",
            "CONTAINS",
            &format!("SPDXRef-File-{}", index + 1),
        )
    }));
    relationships.push(relationship(
        "SPDXRef-Package-InputNext",
        "DEPENDS_ON",
        "SPDXRef-Package-Microsoft-GameInput",
    ));
    relationships.push(relationship(
        "SPDXRef-Package-InputNext",
        "DEPENDS_ON",
        "SPDXRef-Package-tomlplusplus",
    ));
    relationships
}

fn dependency_package(pin: &DependencyPin, spdx_id: &str, license: &str) -> Value {
    json!({
        "name": pin.name,
        "SPDXID": spdx_id,
        "versionInfo": pin.version,
        "downloadLocation": pin.download_location,
        "filesAnalyzed": false,
        "checksums": [{"algorithm": "SHA256", "checksumValue": pin.source_sha256}],
        "licenseConcluded": "NOASSERTION",
        "licenseDeclared": license,
        "copyrightText": "NOASSERTION",
        "sourceInfo": format!("Pinned source archive SHA-256: {}", pin.source_sha256),
        "externalRefs": [{
            "referenceCategory": "PACKAGE_MANAGER",
            "referenceType": "purl",
            "referenceLocator": pin.purl
        }]
    })
}

fn relationship(source: &str, kind: &str, target: &str) -> Value {
    json!({
        "spdxElementId": source,
        "relationshipType": kind,
        "relatedSpdxElement": target
    })
}

fn package_verification_code(package: &ValidatedPackage) -> String {
    let mut checksums = package
        .files
        .values()
        .map(|digest| digest.sha1.as_str())
        .collect::<Vec<_>>();
    checksums.sort_unstable();
    let mut digest = Sha1::new();
    for checksum in checksums {
        digest.update(checksum.as_bytes());
    }
    hex_encode(&digest.finalize())
}

fn file_type(path: &str) -> &'static str {
    let extension = std::path::Path::new(path)
        .extension()
        .and_then(|value| value.to_str());
    if extension.is_some_and(|value| {
        ["dll", "exe", "pdb"]
            .iter()
            .any(|expected| value.eq_ignore_ascii_case(expected))
    }) {
        "BINARY"
    } else if extension.is_some_and(|value| {
        ["c", "cpp", "h"]
            .iter()
            .any(|expected| value.eq_ignore_ascii_case(expected))
    }) || path == "examples/CMakeLists.txt"
    {
        "SOURCE"
    } else {
        "TEXT"
    }
}

fn runtime_name(version: &str, architecture: &str) -> String {
    format!("InputNext-{version}-windows-{architecture}")
}

fn validate_value(
    document: &Value,
    package: &ValidatedPackage,
    architecture: &str,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
) -> Result<()> {
    let expected_name = runtime_name(identity.version(), architecture);
    validate_document_identity(document, &expected_name)?;
    validate_file_inventory(document, package)?;
    validate_package_inventory(document, package, identity, pins, &expected_name)?;
    validate_extracted_license(document, package, pins)?;
    validate_relationships(document, package)
}

fn validate_document_identity(document: &Value, expected_name: &str) -> Result<()> {
    ensure!(
        document["spdxVersion"].as_str() == Some("SPDX-2.2")
            && document["dataLicense"].as_str() == Some("CC0-1.0")
            && document["SPDXID"].as_str() == Some("SPDXRef-DOCUMENT")
            && document["name"].as_str() == Some(expected_name)
            && document["documentNamespace"]
                .as_str()
                .is_some_and(|value| !value.is_empty()),
        "SPDX document identity fields are missing or incorrect"
    );
    let created = document["creationInfo"]["created"]
        .as_str()
        .context("SPDX creation date is missing")?;
    OffsetDateTime::parse(created, &Rfc3339).context("SPDX creation date is not RFC3339")?;
    let creators = document["creationInfo"]["creators"]
        .as_array()
        .context("SPDX creators are missing")?;
    ensure!(
        creators
            .iter()
            .any(|value| value.as_str().is_some_and(|s| s.starts_with("Tool: "))),
        "SPDX must identify its creator tool"
    );
    Ok(())
}

fn validate_file_inventory(document: &Value, package: &ValidatedPackage) -> Result<()> {
    let files = document["files"]
        .as_array()
        .context("SPDX file inventory is missing")?;
    ensure!(
        files.len() == package.files.len(),
        "SPDX file inventory count differs from ZIP"
    );
    let mut seen_files = BTreeSet::new();
    let mut seen_ids = BTreeSet::new();
    for file in files {
        let file_name = file["fileName"]
            .as_str()
            .context("SPDX file name is missing")?;
        let relative = file_name
            .strip_prefix("./")
            .context("SPDX file path is not package-relative")?;
        ensure!(
            !relative.contains('\\')
                && !relative
                    .split('/')
                    .any(|part| part.is_empty() || part == "." || part == ".."),
            "SPDX file path is unsafe: {file_name}"
        );
        let expected = package.files.get(relative).with_context(|| {
            format!("SPDX contains a file absent from the runtime ZIP: {relative}")
        })?;
        ensure!(
            seen_files.insert(relative.to_owned()),
            "SPDX contains a duplicate file path"
        );
        let spdx_id = file["SPDXID"]
            .as_str()
            .context("SPDX file identifier is missing")?;
        ensure!(
            seen_ids.insert(spdx_id.to_owned()),
            "SPDX file identifier is duplicated"
        );
        ensure!(
            file["checksums"].as_array().is_some_and(|values| {
                values.len() == 2
                    && checksum_value(values, "SHA1") == Some(expected.sha1.as_str())
                    && checksum_value(values, "SHA256") == Some(expected.sha256.as_str())
            }),
            "SPDX checksums differ from runtime ZIP file {relative}"
        );
        ensure!(
            file["fileTypes"].as_array().is_some_and(|values| {
                values.len() == 1 && values[0].as_str() == Some(file_type(relative))
            }) && file["licenseConcluded"].as_str() == Some("NOASSERTION")
                && file["licenseInfoInFiles"].as_array().is_some_and(|values| {
                    values.len() == 1 && values[0].as_str() == Some("NOASSERTION")
                })
                && file["copyrightText"].as_str() == Some("NOASSERTION"),
            "SPDX file metadata is incomplete for {relative}"
        );
    }
    ensure!(
        seen_files.len() == package.files.len(),
        "SPDX does not describe every runtime ZIP file"
    );
    Ok(())
}

fn validate_package_inventory(
    document: &Value,
    package: &ValidatedPackage,
    identity: &ReleaseIdentity,
    pins: &SourcePins,
    expected_name: &str,
) -> Result<()> {
    let packages = document["packages"]
        .as_array()
        .context("SPDX package inventory is missing")?;
    ensure!(
        packages.len() == 3,
        "runtime SPDX must list the product and two pinned dependencies"
    );
    let root = packages
        .iter()
        .find(|value| value["SPDXID"].as_str() == Some("SPDXRef-Package-InputNext"))
        .context("SPDX product package is missing")?;
    ensure!(
        root["name"].as_str() == Some(expected_name)
            && root["versionInfo"].as_str() == Some(identity.version())
            && root["downloadLocation"].as_str() == Some("NOASSERTION")
            && root["filesAnalyzed"].as_bool() == Some(true)
            && root["packageFileName"].as_str()
                == package.path.file_name().and_then(|value| value.to_str())
            && root["licenseDeclared"].as_str() == Some("MIT")
            && root["licenseConcluded"].as_str() == Some("NOASSERTION")
            && root["copyrightText"].as_str() == Some("NOASSERTION"),
        "SPDX product package metadata is incomplete"
    );
    let root_checksums = root["checksums"]
        .as_array()
        .context("SPDX product package checksums are missing")?;
    ensure!(
        root_checksums.len() == 2
            && checksum_value(root_checksums, "SHA1") == Some(package.sha1.as_str())
            && checksum_value(root_checksums, "SHA256") == Some(package.sha256.as_str()),
        "SPDX product checksums differ from the runtime ZIP"
    );
    let verification = root["packageVerificationCode"]["packageVerificationCodeValue"]
        .as_str()
        .context("SPDX package verification code is missing")?;
    ensure!(
        verification == package_verification_code(package),
        "SPDX package verification code differs from runtime files"
    );
    validate_dependency(
        packages,
        &pins.game_input,
        "SPDXRef-Package-Microsoft-GameInput",
        "LicenseRef-Microsoft-GameInput",
    )?;
    validate_dependency(
        packages,
        &pins.tomlplusplus,
        "SPDXRef-Package-tomlplusplus",
        "MIT",
    )?;
    Ok(())
}

fn validate_extracted_license(
    document: &Value,
    package: &ValidatedPackage,
    pins: &SourcePins,
) -> Result<()> {
    let gameinput_license = package
        .selected_contents
        .get(GAMEINPUT_LICENSE_FILE)
        .and_then(|bytes| std::str::from_utf8(bytes).ok())
        .context("runtime package does not contain valid GameInput license text")?;
    let extracted = document["hasExtractedLicensingInfos"]
        .as_array()
        .context("SPDX extracted license information is missing")?;
    ensure!(
        extracted.len() == 1
            && extracted[0]["licenseId"].as_str() == Some("LicenseRef-Microsoft-GameInput")
            && extracted[0]["extractedText"].as_str() == Some(gameinput_license)
            && extracted[0]["seeAlsos"].as_array().is_some_and(|values| {
                values.len() == 1
                    && values[0].as_str() == Some(pins.game_input.download_location.as_str())
            }),
        "SPDX GameInput extracted license differs from the package"
    );
    Ok(())
}

fn checksum_value<'a>(checksums: &'a [Value], algorithm: &str) -> Option<&'a str> {
    let mut matches = checksums
        .iter()
        .filter(|value| value["algorithm"].as_str() == Some(algorithm));
    let first = matches.next()?.get("checksumValue")?.as_str()?;
    matches.next().is_none().then_some(first)
}

fn validate_dependency(
    packages: &[Value],
    pin: &DependencyPin,
    spdx_id: &str,
    license: &str,
) -> Result<()> {
    let dependency = packages
        .iter()
        .find(|value| value["SPDXID"].as_str() == Some(spdx_id))
        .with_context(|| format!("SPDX dependency package is missing: {}", pin.name))?;
    ensure!(
        dependency["name"].as_str() == Some(pin.name.as_str())
            && dependency["versionInfo"].as_str() == Some(pin.version.as_str())
            && dependency["downloadLocation"].as_str() == Some(pin.download_location.as_str())
            && dependency["filesAnalyzed"].as_bool() == Some(false)
            && dependency["licenseDeclared"].as_str() == Some(license)
            && dependency["licenseConcluded"].as_str() == Some("NOASSERTION")
            && dependency["copyrightText"].as_str() == Some("NOASSERTION")
            && dependency["sourceInfo"].as_str()
                == Some(format!("Pinned source archive SHA-256: {}", pin.source_sha256).as_str()),
        "SPDX dependency metadata differs from the current CMake pin for {}",
        pin.name
    );
    let checksums = dependency["checksums"]
        .as_array()
        .context("SPDX dependency source checksum is missing")?;
    ensure!(
        checksums.len() == 1
            && checksum_value(checksums, "SHA256") == Some(pin.source_sha256.as_str()),
        "SPDX dependency source checksum differs from the current CMake pin for {}",
        pin.name
    );
    let references = dependency["externalRefs"]
        .as_array()
        .context("SPDX dependency purl is missing")?;
    ensure!(
        references.len() == 1
            && references[0]["referenceCategory"].as_str() == Some("PACKAGE_MANAGER")
            && references[0]["referenceType"].as_str() == Some("purl")
            && references[0]["referenceLocator"].as_str() == Some(pin.purl.as_str()),
        "SPDX dependency purl differs from the current CMake pin for {}",
        pin.name
    );
    Ok(())
}

fn validate_relationships(document: &Value, package: &ValidatedPackage) -> Result<()> {
    let relationships = document["relationships"]
        .as_array()
        .context("SPDX relationships are missing")?;
    ensure!(
        relationships.len() == package.files.len() + 3,
        "SPDX relationship inventory is incomplete"
    );
    let actual = relationships
        .iter()
        .map(|relationship| {
            Ok((
                relationship["spdxElementId"]
                    .as_str()
                    .context("SPDX relationship source is missing")?
                    .to_owned(),
                relationship["relationshipType"]
                    .as_str()
                    .context("SPDX relationship type is missing")?
                    .to_owned(),
                relationship["relatedSpdxElement"]
                    .as_str()
                    .context("SPDX relationship target is missing")?
                    .to_owned(),
            ))
        })
        .collect::<Result<BTreeSet<_>>>()?;
    let mut expected = BTreeSet::new();
    expected.insert((
        "SPDXRef-DOCUMENT".to_owned(),
        "DESCRIBES".to_owned(),
        "SPDXRef-Package-InputNext".to_owned(),
    ));
    expected.extend(package.files.keys().enumerate().map(|(index, _)| {
        (
            "SPDXRef-Package-InputNext".to_owned(),
            "CONTAINS".to_owned(),
            format!("SPDXRef-File-{}", index + 1),
        )
    }));
    expected.insert((
        "SPDXRef-Package-InputNext".to_owned(),
        "DEPENDS_ON".to_owned(),
        "SPDXRef-Package-Microsoft-GameInput".to_owned(),
    ));
    expected.insert((
        "SPDXRef-Package-InputNext".to_owned(),
        "DEPENDS_ON".to_owned(),
        "SPDXRef-Package-tomlplusplus".to_owned(),
    ));
    ensure!(
        actual == expected,
        "SPDX relationships do not exactly describe contents and dependencies"
    );
    Ok(())
}
