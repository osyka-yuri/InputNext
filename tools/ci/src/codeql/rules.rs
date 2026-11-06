//! Rule metadata lookup for SARIF driver and extension components.

use std::collections::BTreeMap;

use anyhow::{Context, Result, ensure};
use serde_json::{Map, Value};

use super::{parse_severity, required_nonempty_string, required_object};

#[derive(Debug)]
pub(super) struct RuleComponents {
    components: Vec<RuleComponent>,
    by_guid: BTreeMap<String, usize>,
}

#[derive(Debug)]
struct RuleComponent {
    name: String,
    guid: Option<String>,
    rules: Vec<RuleDescriptor>,
    by_id: BTreeMap<String, Option<usize>>,
    by_guid: BTreeMap<String, usize>,
}

#[derive(Debug)]
struct RuleDescriptor {
    id: String,
    guid: Option<String>,
    security_severity: Option<f64>,
}

#[derive(Debug)]
pub(super) struct ResolvedRule {
    pub id: String,
    pub security_severity: Option<f64>,
}

impl RuleComponents {
    pub(super) fn parse(tool: &Map<String, Value>) -> Result<Self> {
        let driver = parse_component(required_object(tool, "driver")?)?;
        let mut components = vec![driver];
        for extension in optional_array(tool, "extensions")? {
            let extension = extension
                .as_object()
                .context("SARIF tool extension must be an object")?;
            components.push(parse_component(extension)?);
        }
        let mut by_guid = BTreeMap::new();
        for (index, component) in components.iter().enumerate() {
            if let Some(guid) = &component.guid {
                ensure!(
                    by_guid.insert(guid.clone(), index).is_none(),
                    "SARIF tool components contain duplicate GUID '{guid}'"
                );
            }
        }
        Ok(Self {
            components,
            by_guid,
        })
    }

    pub(super) fn resolve(&self, result: &Map<String, Value>) -> Result<ResolvedRule> {
        let empty_reference = Map::new();
        let reference = result
            .get("rule")
            .map(|value| {
                value
                    .as_object()
                    .context("SARIF result rule must be an object")
            })
            .transpose()?
            .unwrap_or(&empty_reference);
        let top_id = optional_nonempty_string(result, "ruleId")?;
        let nested_id = optional_nonempty_string(reference, "id")?;
        if let (Some(top), Some(nested)) = (top_id, nested_id) {
            ensure!(top == nested, "SARIF result ruleId does not match rule.id");
        }
        let id = top_id
            .or(nested_id)
            .context("SARIF result has no ruleId or rule.id")?;
        let top_index = optional_index(result, "ruleIndex")?;
        let nested_index = optional_index(reference, "index")?;
        if let (Some(top), Some(nested)) = (top_index, nested_index) {
            ensure!(
                top == nested,
                "SARIF result ruleIndex does not match rule.index"
            );
        }
        let component = self.resolve_component(reference.get("toolComponent"))?;
        let guid = optional_guid(reference)?;
        let descriptor = component.resolve(id, nested_index.or(top_index), guid.as_deref())?;
        Ok(ResolvedRule {
            id: id.to_owned(),
            security_severity: descriptor.security_severity,
        })
    }

    fn resolve_component(&self, reference: Option<&Value>) -> Result<&RuleComponent> {
        let Some(reference) = reference else {
            return Ok(&self.components[0]);
        };
        let reference = reference
            .as_object()
            .context("SARIF result toolComponent must be an object")?;
        let guid = optional_guid(reference)?;
        let index = match optional_index(reference, "index")? {
            Some(index) => index
                .checked_add(1)
                .context("SARIF tool component index overflow")?,
            None => match &guid {
                Some(guid) => *self
                    .by_guid
                    .get(guid)
                    .context("SARIF tool component GUID is unknown")?,
                None => 0,
            },
        };
        let component = self
            .components
            .get(index)
            .context("SARIF tool component index is out of range")?;
        if let Some(guid) = guid {
            ensure!(
                component.guid.as_deref() == Some(&guid),
                "SARIF tool component index and GUID disagree"
            );
        }
        if let Some(name) = optional_nonempty_string(reference, "name")? {
            ensure!(
                component.name == name,
                "SARIF tool component name does not match the referenced component"
            );
        }
        Ok(component)
    }
}

impl RuleComponent {
    fn resolve(
        &self,
        id: &str,
        index: Option<usize>,
        guid: Option<&str>,
    ) -> Result<&RuleDescriptor> {
        let index = match index {
            Some(index) => index,
            None => match guid {
                Some(guid) => *self
                    .by_guid
                    .get(guid)
                    .context("SARIF rule GUID is unknown")?,
                None => self.resolve_id(id)?,
            },
        };
        let descriptor = self
            .rules
            .get(index)
            .context("SARIF rule index is out of range")?;
        if let Some(guid) = guid {
            ensure!(
                descriptor.guid.as_deref() == Some(guid),
                "SARIF rule index and GUID disagree"
            );
        }
        ensure!(
            id == descriptor.id
                || id
                    .rsplit_once('/')
                    .is_some_and(|(base, _)| base == descriptor.id),
            "SARIF result rule ID '{id}' does not match descriptor '{}'",
            descriptor.id
        );
        Ok(descriptor)
    }

    fn resolve_id(&self, id: &str) -> Result<usize> {
        let exact = self.by_id.get(id);
        let parent = id
            .rsplit_once('/')
            .and_then(|(base, _)| self.by_id.get(base));
        ensure!(
            exact.is_none() || parent.is_none(),
            "SARIF rule ID '{id}' matches multiple descriptors"
        );
        exact
            .or(parent)
            .with_context(|| {
                format!(
                    "SARIF result refers to unknown rule '{id}' in '{}'",
                    self.name
                )
            })?
            .with_context(|| format!("SARIF rule ID '{id}' is ambiguous without an index or GUID"))
    }
}

fn parse_component(component: &Map<String, Value>) -> Result<RuleComponent> {
    let name = required_nonempty_string(component, "name")?.to_owned();
    let guid = optional_guid(component)?;
    let mut rules = Vec::new();
    let mut by_id = BTreeMap::new();
    let mut by_guid = BTreeMap::new();
    for (index, rule) in optional_array(component, "rules")?.iter().enumerate() {
        let rule = rule.as_object().context("SARIF rule must be an object")?;
        let id = required_nonempty_string(rule, "id")?.to_owned();
        let guid = optional_guid(rule)?;
        let security_severity = match rule.get("properties") {
            Some(properties) => properties
                .as_object()
                .context("SARIF rule properties must be an object")?
                .get("security-severity")
                .map(parse_severity)
                .transpose()?,
            None => None,
        };
        by_id
            .entry(id.clone())
            .and_modify(|value| *value = None)
            .or_insert(Some(index));
        if let Some(guid) = &guid {
            ensure!(
                by_guid.insert(guid.clone(), index).is_none(),
                "SARIF rules contain duplicate GUID '{guid}'"
            );
        }
        rules.push(RuleDescriptor {
            id,
            guid,
            security_severity,
        });
    }
    Ok(RuleComponent {
        name,
        guid,
        rules,
        by_id,
        by_guid,
    })
}

fn optional_array<'a>(object: &'a Map<String, Value>, name: &str) -> Result<&'a [Value]> {
    object.get(name).map_or(Ok(&[]), |value| {
        value
            .as_array()
            .map(Vec::as_slice)
            .with_context(|| format!("SARIF '{name}' must be an array"))
    })
}

fn optional_index(object: &Map<String, Value>, name: &str) -> Result<Option<usize>> {
    object
        .get(name)
        .map(|value| {
            let index = value
                .as_u64()
                .with_context(|| format!("SARIF '{name}' must be a nonnegative integer"))?;
            usize::try_from(index).context("SARIF index exceeds the platform limit")
        })
        .transpose()
}

fn optional_guid(object: &Map<String, Value>) -> Result<Option<String>> {
    optional_nonempty_string(object, "guid")?
        .map(|guid| {
            ensure!(
                guid.len() == 36
                    && guid.bytes().enumerate().all(|(index, byte)| {
                        if [8, 13, 18, 23].contains(&index) {
                            byte == b'-'
                        } else {
                            byte.is_ascii_hexdigit()
                        }
                    }),
                "SARIF GUID must use the 36-character hexadecimal form"
            );
            Ok(guid.to_ascii_lowercase())
        })
        .transpose()
}

fn optional_nonempty_string<'a>(
    object: &'a Map<String, Value>,
    name: &str,
) -> Result<Option<&'a str>> {
    object
        .get(name)
        .map(|value| {
            let value = value
                .as_str()
                .with_context(|| format!("SARIF '{name}' must be a string"))?;
            ensure!(!value.trim().is_empty(), "SARIF '{name}' must not be empty");
            Ok(value)
        })
        .transpose()
}
