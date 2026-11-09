//! JSON parsing that rejects duplicate object keys before typed validation.

use std::collections::BTreeMap;

use serde::de::{MapAccess, SeqAccess, Visitor};
use serde::{Deserialize, Deserializer};
use serde_json::{Number, Value};

#[derive(Debug)]
enum StrictValue {
    Null,
    Bool(bool),
    Number(Number),
    String(String),
    Array(Vec<Self>),
    Object(BTreeMap<String, Self>),
}

impl<'de> Deserialize<'de> for StrictValue {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct StrictValueVisitor;

        impl<'de> Visitor<'de> for StrictValueVisitor {
            type Value = StrictValue;

            fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                formatter.write_str("a JSON value with unique object keys")
            }

            fn visit_unit<E>(self) -> Result<Self::Value, E> {
                Ok(StrictValue::Null)
            }

            fn visit_none<E>(self) -> Result<Self::Value, E> {
                Ok(StrictValue::Null)
            }

            fn visit_bool<E>(self, value: bool) -> Result<Self::Value, E> {
                Ok(StrictValue::Bool(value))
            }

            fn visit_i64<E>(self, value: i64) -> Result<Self::Value, E> {
                Ok(StrictValue::Number(Number::from(value)))
            }

            fn visit_u64<E>(self, value: u64) -> Result<Self::Value, E> {
                Ok(StrictValue::Number(Number::from(value)))
            }

            fn visit_f64<E>(self, value: f64) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                Number::from_f64(value)
                    .map(StrictValue::Number)
                    .ok_or_else(|| E::custom("non-finite JSON number"))
            }

            fn visit_str<E>(self, value: &str) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                Ok(StrictValue::String(value.to_owned()))
            }

            fn visit_string<E>(self, value: String) -> Result<Self::Value, E> {
                Ok(StrictValue::String(value))
            }

            fn visit_seq<A>(self, mut sequence: A) -> Result<Self::Value, A::Error>
            where
                A: SeqAccess<'de>,
            {
                let mut values = Vec::new();
                while let Some(value) = sequence.next_element()? {
                    values.push(value);
                }
                Ok(StrictValue::Array(values))
            }

            fn visit_map<A>(self, mut map: A) -> Result<Self::Value, A::Error>
            where
                A: MapAccess<'de>,
            {
                let mut values = BTreeMap::new();
                while let Some((key, value)) = map.next_entry::<String, StrictValue>()? {
                    if values.insert(key.clone(), value).is_some() {
                        return Err(serde::de::Error::custom(format!(
                            "duplicate JSON object key {key:?}"
                        )));
                    }
                }
                Ok(StrictValue::Object(values))
            }
        }

        deserializer.deserialize_any(StrictValueVisitor)
    }
}

impl From<StrictValue> for Value {
    fn from(value: StrictValue) -> Self {
        match value {
            StrictValue::Null => Self::Null,
            StrictValue::Bool(value) => Self::Bool(value),
            StrictValue::Number(value) => Self::Number(value),
            StrictValue::String(value) => Self::String(value),
            StrictValue::Array(values) => Self::Array(values.into_iter().map(Self::from).collect()),
            StrictValue::Object(values) => Self::Object(
                values
                    .into_iter()
                    .map(|(key, value)| (key, Self::from(value)))
                    .collect(),
            ),
        }
    }
}

pub(super) fn parse_slice(bytes: &[u8]) -> serde_json::Result<Value> {
    serde_json::from_slice::<StrictValue>(bytes).map(Value::from)
}

#[cfg(test)]
mod tests {
    use super::parse_slice;

    #[test]
    fn rejects_duplicate_keys_at_any_depth() -> anyhow::Result<()> {
        let Err(error) = parse_slice(br#"{"outer":{"same":1,"same":2}}"#) else {
            anyhow::bail!("nested duplicate keys were accepted");
        };
        assert!(error.to_string().contains("duplicate JSON object key"));
        Ok(())
    }

    #[test]
    fn accepts_unique_json_values() -> anyhow::Result<()> {
        let parsed = parse_slice(br#"{"items":[true,null,3],"name":"release"}"#)?;
        assert_eq!(parsed["name"], "release");
        assert_eq!(parsed["items"][2], 3);
        Ok(())
    }
}
