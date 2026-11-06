use super::{CodeqlArgs, parse_sarif_file, parse_severity};
use crate::process::SystemRunner;
use anyhow::{Context, Result};
use serde_json::{Value, json};
use std::fs;

#[test]
fn security_severity_accepts_numeric_strings_and_numbers() -> Result<()> {
    assert_eq!(parse_severity(&json!("7.0"))?, 7.0);
    assert_eq!(parse_severity(&json!(8.0))?, 8.0);
    Ok(())
}

#[test]
fn security_severity_rejects_out_of_range_values() {
    assert!(parse_severity(&json!("10.1")).is_err());
    assert!(parse_severity(&json!(-1)).is_err());
}

#[test]
fn malformed_invocation_status_is_rejected() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let data = json!({
        "version": "2.1.0",
        "runs": [{
            "tool": {"driver": {"name": "CodeQL", "rules": []}},
            "results": [],
            "invocations": [{}]
        }]
    });
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

fn empty_report() -> Value {
    json!({
        "version": "2.1.0",
        "runs": [{
            "tool": {"driver": {"name": "CodeQL", "rules": []}},
            "results": []
        }]
    })
}

#[test]
fn omitted_and_empty_invocations_are_valid() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = empty_report();
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path)?.findings.is_empty());
    data["runs"][0]["invocations"] = json!([]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path)?.findings.is_empty());
    Ok(())
}

#[test]
fn supplied_invocations_reject_failure_or_malformed_status() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    for invocations in [
        json!(null),
        json!({"executionSuccessful": true}),
        json!([null]),
        json!([{"executionSuccessful": false}]),
        json!([{"executionSuccessful": "true"}]),
        json!([{"executionSuccessful": true}, {"executionSuccessful": false}]),
    ] {
        let mut data = empty_report();
        data["runs"][0]["invocations"] = invocations;
        fs::write(&path, serde_json::to_vec(&data)?)?;
        assert!(parse_sarif_file(&path).is_err());
    }
    let mut data = empty_report();
    data["runs"][0]["invocations"] = json!([{"executionSuccessful": true}]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_ok());
    Ok(())
}

#[test]
fn security_score_controls_the_gate_independently_of_result_level() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let report = directory.path().join("evaluation.json");
    for (score, level, blocked, unclassified) in [
        (None, "error", false, 1),
        (Some(6.9), "error", false, 0),
        (Some(7.0), "warning", true, 0),
    ] {
        let mut data = empty_report();
        let mut rule = json!({"id": "finding"});
        if let Some(score) = score {
            rule["properties"] = json!({"security-severity": score});
        }
        data["runs"][0]["tool"]["driver"]["rules"] = json!([rule]);
        data["runs"][0]["results"] = json!([
            {"ruleId": "finding", "level": level, "message": {"text": "finding"}}
        ]);
        fs::write(&path, serde_json::to_vec(&data)?)?;
        let outcome = CodeqlArgs {
            results: path.clone(),
            report: Some(report.clone()),
        }
        .run(&mut SystemRunner);
        assert_eq!(outcome.is_err(), blocked);
        let evaluation: Value = serde_json::from_slice(&fs::read(&report)?)?;
        assert_eq!(evaluation["unclassified_count"], unclassified);
        assert_eq!(evaluation["blocker_count"], u64::from(blocked));
    }
    Ok(())
}

#[test]
fn unclassified_and_blocker_findings_are_counted() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let data = json!({
        "version": "2.1.0",
        "runs": [{
            "tool": {"driver": {"name": "CodeQL", "rules": [
                {"id": "high", "properties": {"security-severity": "7.5"}},
                {"id": "quality"},
                {"id": "low", "properties": {"security-severity": 4.2}}
            ]}},
            "results": [
                {"ruleId": "high", "message": {"text": "critical"}, "level": "error"},
                {"ruleId": "quality", "message": {"text": "unknown severity"}},
                {"ruleId": "low", "message": {"text": "quality issue"}}
            ],
            "invocations": [{"executionSuccessful": true}]
        }]
    });
    fs::write(&path, serde_json::to_vec(&data)?)?;
    let found = parse_sarif_file(&path)?;
    assert_eq!(found.blocker_count, 1);
    assert_eq!(found.unclassified_count, 1);
    assert_eq!(found.findings.len(), 3);
    assert_eq!(found.findings[0].rule_id, "high");
    assert_eq!(found.findings[0].security_severity, Some(7.5));
    Ok(())
}

fn extension_report(result: &Value) -> Value {
    json!({
        "version": "2.1.0",
        "runs": [{
            "tool": {
                "driver": {
                    "name": "CodeQL",
                    "rules": [
                        {"id": "finding", "properties": {"security-severity": 3.0}}
                    ]
                },
                "extensions": [{
                    "name": "codeql/cpp-queries",
                    "rules": [
                        {"id": "finding", "properties": {"security-severity": "7.5"}},
                        {"id": "other", "properties": {"security-severity": "8.0"}}
                    ]
                }]
            },
            "results": [result]
        }]
    })
}

#[test]
fn extension_rule_reference_uses_its_component_and_legacy_results_use_driver() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = extension_report(&json!({
        "ruleId": "finding",
        "rule": {"id": "finding", "index": 0, "toolComponent": {"index": 0}},
        "message": {"text": "extension blocker"}
    }));
    data["runs"][0]["results"] = json!([
        data["runs"][0]["results"][0].clone(),
        {
            "ruleId": "finding",
            "message": {"text": "legacy driver finding"}
        }
    ]);
    fs::write(&path, serde_json::to_vec(&data)?)?;

    let found = parse_sarif_file(&path)?;
    assert_eq!(found.blocker_count, 1);
    assert_eq!(found.findings.len(), 2);
    assert_eq!(found.findings[0].security_severity, Some(7.5));
    assert_eq!(found.findings[1].security_severity, Some(3.0));
    Ok(())
}

#[test]
fn malformed_rule_references_fail_closed_instead_of_falling_back_to_driver() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let valid_result = json!({
        "ruleId": "finding",
        "rule": {"id": "finding", "index": 0, "toolComponent": {"index": 0}},
        "message": {"text": "finding"}
    });

    let mut cases = Vec::new();
    let mut out_of_range_component = valid_result.clone();
    out_of_range_component["rule"]["toolComponent"]["index"] = json!(1);
    cases.push(out_of_range_component);

    let mut malformed_component_index = valid_result.clone();
    malformed_component_index["rule"]["toolComponent"]["index"] = json!("0");
    cases.push(malformed_component_index);

    let mut out_of_range_rule = valid_result.clone();
    out_of_range_rule["rule"]["index"] = json!(4);
    cases.push(out_of_range_rule);

    let mut negative_rule_index = valid_result.clone();
    negative_rule_index["rule"]["index"] = json!(-1);
    cases.push(negative_rule_index);

    let mut disagreeing_indices = valid_result.clone();
    disagreeing_indices["ruleIndex"] = json!(1);
    cases.push(disagreeing_indices);

    let mut disagreeing_ids = valid_result.clone();
    disagreeing_ids["rule"]["id"] = json!("other");
    cases.push(disagreeing_ids);

    let mut wrong_descriptor_at_index = valid_result.clone();
    wrong_descriptor_at_index["rule"]["index"] = json!(1);
    cases.push(wrong_descriptor_at_index);

    let mut mismatched_driver_name = valid_result.clone();
    mismatched_driver_name["rule"]["toolComponent"] = json!({"name": "codeql/cpp-queries"});
    cases.push(mismatched_driver_name);

    let mut guid_only_component = valid_result.clone();
    guid_only_component["rule"]["toolComponent"] = json!({"guid": "unsupported-guid"});
    cases.push(guid_only_component);

    let mut guid_qualified_component = valid_result.clone();
    guid_qualified_component["rule"]["toolComponent"] =
        json!({"index": 0, "guid": "unsupported-guid"});
    cases.push(guid_qualified_component);

    let mut guid_rule_reference = valid_result;
    guid_rule_reference["rule"]["guid"] = json!("unsupported-guid");
    cases.push(guid_rule_reference);

    for result in cases {
        let data = extension_report(&result);
        fs::write(&path, serde_json::to_vec(&data)?)?;
        assert!(parse_sarif_file(&path).is_err());
    }
    Ok(())
}

#[test]
fn extension_component_does_not_fall_back_to_a_matching_driver_rule() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let result = json!({
        "ruleId": "driver-only",
        "rule": {"id": "driver-only", "index": 0, "toolComponent": {"index": 0}},
        "message": {"text": "finding"}
    });
    let mut data = extension_report(&result);
    data["runs"][0]["tool"]["driver"]["rules"] = json!([
        {"id": "finding", "properties": {"security-severity": 3.0}},
        {"id": "driver-only", "properties": {"security-severity": 9.0}}
    ]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

#[test]
fn driver_rules_may_be_omitted_when_an_extension_resolves_the_finding() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = extension_report(&json!({
        "ruleId": "finding",
        "rule": {"id": "finding", "index": 0, "toolComponent": {"index": 0}},
        "message": {"text": "extension finding"}
    }));
    data["runs"][0]["tool"]["driver"] = json!({"name": "CodeQL"});
    fs::write(&path, serde_json::to_vec(&data)?)?;
    let found = parse_sarif_file(&path)?;
    assert_eq!(found.findings.len(), 1);
    assert_eq!(found.findings[0].security_severity, Some(7.5));

    let mut empty_components = empty_report();
    empty_components["runs"][0]["tool"]["driver"] = json!({"name": "CodeQL"});
    empty_components["runs"][0]["tool"]["extensions"] = json!([{"name": "empty-pack"}]);
    fs::write(&path, serde_json::to_vec(&empty_components)?)?;
    assert!(parse_sarif_file(&path)?.findings.is_empty());
    Ok(())
}

#[test]
fn duplicate_rule_ids_are_resolved_by_index_and_id_only_lookup_is_ambiguous() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = extension_report(&json!({
        "ruleId": "finding",
        "rule": {"id": "finding", "index": 0, "toolComponent": {"index": 0}},
        "message": {"text": "first duplicate"}
    }));
    data["runs"][0]["tool"]["extensions"][0]["rules"] = json!([
        {"id": "finding", "properties": {"security-severity": 3.0}},
        {"id": "finding", "properties": {"security-severity": 7.5}}
    ]);
    data["runs"][0]["results"] = json!([
        {
            "ruleId": "finding",
            "rule": {"id": "finding", "index": 0, "toolComponent": {"index": 0}},
            "message": {"text": "first duplicate"}
        },
        {
            "ruleId": "finding",
            "ruleIndex": 1,
            "rule": {"id": "finding", "index": 1, "toolComponent": {"index": 0}},
            "message": {"text": "second duplicate"}
        }
    ]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    let found = parse_sarif_file(&path)?;
    assert_eq!(found.findings.len(), 2);
    assert_eq!(found.findings[0].security_severity, Some(3.0));
    assert_eq!(found.findings[1].security_severity, Some(7.5));

    data["runs"][0]["results"] = json!([{
        "ruleId": "finding",
        "rule": {"toolComponent": {"index": 0}},
        "message": {"text": "ambiguous duplicate"}
    }]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

#[test]
fn component_and_rule_guid_references_resolve_case_insensitively_and_check_indexes() -> Result<()> {
    const COMPONENT_GUID: &str = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const RULE_GUID: &str = "abcdef12-3456-4aab-8def-0123456789ab";

    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let data = json!({
        "version": "2.1.0",
        "runs": [{
            "tool": {
                "driver": {"name": "CodeQL", "rules": []},
                "extensions": [{
                    "name": "codeql/cpp-queries",
                    "guid": COMPONENT_GUID,
                    "rules": [{
                        "id": "finding",
                        "guid": RULE_GUID,
                        "properties": {"security-severity": 7.5}
                    }]
                }]
            },
            "results": [{
                "ruleId": "finding",
                "rule": {
                    "id": "finding",
                    "index": 0,
                    "guid": "ABCDEF12-3456-4AAB-8DEF-0123456789AB",
                    "toolComponent": {
                        "index": 0,
                        "guid": "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE"
                    }
                },
                "message": {"text": "GUID-resolved finding"}
            }, {
                "ruleId": "finding",
                "rule": {
                    "id": "finding",
                    "guid": "ABCDEF12-3456-4AAB-8DEF-0123456789AB",
                    "toolComponent": {
                        "guid": "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE"
                    }
                },
                "message": {"text": "GUID-only finding"}
            }]
        }]
    });
    fs::write(&path, serde_json::to_vec(&data)?)?;
    let found = parse_sarif_file(&path)?;
    assert_eq!(found.findings.len(), 2);
    assert_eq!(found.findings[0].security_severity, Some(7.5));
    assert_eq!(found.findings[1].security_severity, Some(7.5));

    for (reference, field) in [
        (
            json!({"index": 0, "guid": "bbbbbbbb-1111-4111-8111-111111111111"}),
            "toolComponent",
        ),
        (json!({"index": 0, "guid": "bad-guid"}), "toolComponent"),
        (json!("bad-guid"), "guid"),
    ] {
        let mut malformed = data.clone();
        malformed["runs"][0]["results"][0]["rule"][field] = reference;
        fs::write(&path, serde_json::to_vec(&malformed)?)?;
        assert!(parse_sarif_file(&path).is_err());
    }

    let mut wrong_rule_guid = data;
    wrong_rule_guid["runs"][0]["results"][0]["rule"]["guid"] =
        json!("abcdef12-3456-4aab-8def-1123456789ab");
    fs::write(&path, serde_json::to_vec(&wrong_rule_guid)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

#[test]
fn duplicate_guids_are_not_silently_resolved() -> Result<()> {
    const COMPONENT_GUID: &str = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    const RULE_GUID: &str = "abcdef12-3456-4aab-8def-0123456789ab";

    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = json!({
        "version": "2.1.0",
        "runs": [{
            "tool": {
                "driver": {"name": "CodeQL", "rules": []},
                "extensions": [{
                    "name": "first-pack",
                    "guid": COMPONENT_GUID,
                    "rules": [
                        {"id": "finding", "guid": RULE_GUID, "properties": {"security-severity": 7.5}},
                        {"id": "other", "guid": RULE_GUID, "properties": {"security-severity": 3.0}}
                    ]
                }]
            },
            "results": [{
                "ruleId": "finding",
                "rule": {
                    "id": "finding",
                    "guid": RULE_GUID,
                    "toolComponent": {"index": 0}
                },
                "message": {"text": "ambiguous rule GUID"}
            }]
        }]
    });
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());

    data["runs"][0]["tool"]["extensions"]
        .as_array_mut()
        .context("test SARIF extensions should be an array")?
        .push(json!({
            "name": "second-pack",
            "guid": COMPONENT_GUID,
            "rules": [{"id": "finding", "properties": {"security-severity": 7.5}}]
        }));
    data["runs"][0]["tool"]["extensions"][0]["rules"] = json!([
        {"id": "finding", "guid": RULE_GUID, "properties": {"security-severity": 7.5}}
    ]);
    data["runs"][0]["results"][0]["rule"]["toolComponent"] = json!({"guid": COMPONENT_GUID});
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

#[test]
fn driver_name_only_component_reference_selects_and_validates_the_driver() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = empty_report();
    data["runs"][0]["tool"]["driver"]["rules"] = json!([
        {"id": "finding", "properties": {"security-severity": 7.5}}
    ]);
    data["runs"][0]["results"] = json!([{
        "ruleId": "finding",
        "rule": {"id": "finding", "toolComponent": {"name": "CodeQL"}},
        "message": {"text": "driver finding"}
    }]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert_eq!(parse_sarif_file(&path)?.blocker_count, 1);

    data["runs"][0]["results"][0]["rule"]["toolComponent"]["name"] = json!("another-driver");
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

#[test]
fn indexed_composed_rule_ids_allow_one_suffix_and_reject_other_shapes() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = empty_report();
    data["runs"][0]["tool"]["driver"]["rules"] = json!([
        {"id": "CA5350", "properties": {"security-severity": 7.5}}
    ]);
    data["runs"][0]["results"] = json!([{
        "ruleId": "CA5350/sha-1",
        "rule": {"id": "CA5350/sha-1", "index": 0},
        "message": {"text": "composed rule ID"}
    }]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert_eq!(parse_sarif_file(&path)?.blocker_count, 1);

    for invalid_id in ["OTHER/sha-1", "CA5350/sha-1/extra"] {
        data["runs"][0]["results"][0]["ruleId"] = json!(invalid_id);
        data["runs"][0]["results"][0]["rule"]["id"] = json!(invalid_id);
        fs::write(&path, serde_json::to_vec(&data)?)?;
        assert!(parse_sarif_file(&path).is_err());
    }
    Ok(())
}

#[test]
fn composed_id_only_lookup_requires_a_unique_candidate() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = empty_report();
    data["runs"][0]["tool"]["driver"]["rules"] = json!([
        {"id": "CA5350", "properties": {"security-severity": 7.5}}
    ]);
    data["runs"][0]["results"] = json!([{
        "ruleId": "CA5350/sha-1",
        "message": {"text": "unique base-ID lookup"}
    }]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert_eq!(parse_sarif_file(&path)?.blocker_count, 1);

    data["runs"][0]["tool"]["driver"]["rules"] = json!([
        {"id": "CA5350", "properties": {"security-severity": 7.5}},
        {"id": "CA5350/sha-1", "properties": {"security-severity": 3.0}}
    ]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

#[test]
fn result_and_nested_rule_ids_must_match_but_nested_id_can_replace_rule_id() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = empty_report();
    data["runs"][0]["tool"]["driver"]["rules"] = json!([
        {"id": "finding", "properties": {"security-severity": 7.5}}
    ]);
    data["runs"][0]["results"] = json!([{
        "rule": {"id": "finding", "index": 0},
        "message": {"text": "nested ID only"}
    }]);
    fs::write(&path, serde_json::to_vec(&data)?)?;
    let found = parse_sarif_file(&path)?;
    assert_eq!(found.findings[0].rule_id, "finding");
    assert_eq!(found.blocker_count, 1);

    data["runs"][0]["results"][0]["ruleId"] = json!("other");
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());

    data["runs"][0]["results"][0]
        .as_object_mut()
        .context("test SARIF result should be an object")?
        .remove("ruleId");
    data["runs"][0]["results"][0]["rule"]
        .as_object_mut()
        .context("test SARIF rule reference should be an object")?
        .remove("id");
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}

#[test]
fn invalid_extension_rule_severity_is_rejected() -> Result<()> {
    let directory = tempfile::tempdir()?;
    let path = directory.path().join("results.sarif");
    let mut data = extension_report(&json!({
        "ruleId": "finding",
        "rule": {"id": "finding", "index": 0, "toolComponent": {"index": 0}},
        "message": {"text": "finding"}
    }));
    data["runs"][0]["tool"]["extensions"][0]["rules"][0]["properties"]["security-severity"] =
        json!("invalid");
    fs::write(&path, serde_json::to_vec(&data)?)?;
    assert!(parse_sarif_file(&path).is_err());
    Ok(())
}
