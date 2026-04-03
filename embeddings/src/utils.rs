use anyhow::Result;
use serde_json::Value;

/// Upper bound on bytes per token for BPE tokenizers.
/// Most tokenizers average 3–5 bytes/token; 8 covers worst-case (CJK, emoji).
const BYTES_PER_TOKEN_UPPER_BOUND: usize = 8;

/// Pre-truncate text to avoid running BPE on excessively long input.
/// Cuts at a valid UTF-8 char boundary with a safe byte margin.
/// `truncate_tokens` remains the final guarantee on token count.
#[inline]
pub fn pre_truncate_text(text: &str, max_seq_len: usize) -> &str {
    let byte_limit = max_seq_len.saturating_mul(BYTES_PER_TOKEN_UPPER_BOUND);
    if text.len() <= byte_limit {
        text
    } else {
        &text[..text.floor_char_boundary(byte_limit)]
    }
}

/// Get maximum input length for sequence for the current model
/// Supports multiple config field names for different model architectures
pub fn get_max_input_length(contents: &str) -> Result<usize> {
    let config: Value = serde_json::from_str(contents)?;

    // Try standard field first (BERT, Llama, etc.)
    if let Some(max_len) = config
        .get("max_position_embeddings")
        .and_then(Value::as_u64)
    {
        return Ok(max_len as usize);
    }

    // Try n_positions (some models use this)
    if let Some(n_pos) = config.get("n_positions").and_then(Value::as_u64) {
        return Ok(n_pos as usize);
    }

    // T5 models use relative_attention_max_distance or default to 512
    if config.get("model_type").and_then(Value::as_str) == Some("t5") {
        // T5 uses relative position embeddings, default max is 512
        return Ok(512);
    }

    Err(std::io::Error::other("Max position embeddings not found").into())
}

/// Get hidden size for the current model
/// Supports multiple config field names for different model architectures
pub fn get_hidden_size(contents: &str) -> Result<usize> {
    let config: Value = serde_json::from_str(contents)?;

    // Try standard field first (BERT, Llama, etc.)
    if let Some(hidden) = config.get("hidden_size").and_then(Value::as_u64) {
        return Ok(hidden as usize);
    }

    // T5 models use d_model instead of hidden_size
    if let Some(d_model) = config.get("d_model").and_then(Value::as_u64) {
        return Ok(d_model as usize);
    }

    Err(std::io::Error::other("Hidden size not found").into())
}

#[inline]
pub fn normalize(v: &mut [f32]) {
    let length: f32 = v.iter().map(|x| x * x).sum::<f32>().sqrt();
    if length > 0.0 {
        v.iter_mut().for_each(|x| *x /= length);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_get_max_input_length() {
        let config = r#"{"max_position_embeddings": 512}"#;
        assert_eq!(get_max_input_length(config).unwrap(), 512);

        let invalid_config = r#"{"some_other_field": 512}"#;
        assert!(get_max_input_length(invalid_config).is_err());
    }

    #[test]
    fn test_get_max_input_length_t5() {
        // T5 model with model_type - should return default 512
        let t5_config = r#"{"model_type": "t5", "d_model": 768}"#;
        assert_eq!(get_max_input_length(t5_config).unwrap(), 512);

        // T5 model with relative_attention_max_distance
        let t5_config_with_relative =
            r#"{"model_type": "t5", "d_model": 768, "relative_attention_max_distance": 128}"#;
        assert_eq!(get_max_input_length(t5_config_with_relative).unwrap(), 512);

        // n_positions fallback
        let n_positions_config = r#"{"n_positions": 1024}"#;
        assert_eq!(get_max_input_length(n_positions_config).unwrap(), 1024);
    }

    #[test]
    fn test_get_hidden_size() {
        let config = r#"{"hidden_size": 768}"#;
        assert_eq!(get_hidden_size(config).unwrap(), 768);

        let invalid_config = r#"{"some_other_field": 768}"#;
        assert!(get_hidden_size(invalid_config).is_err());
    }

    #[test]
    fn test_get_hidden_size_t5() {
        // T5 model uses d_model instead of hidden_size
        let t5_config = r#"{"model_type": "t5", "d_model": 1536}"#;
        assert_eq!(get_hidden_size(t5_config).unwrap(), 1536);

        // FRIDA-like config
        let frida_config = r#"{"architectures": ["T5EncoderModel"], "model_type": "t5", "d_model": 1536, "d_ff": 4096}"#;
        assert_eq!(get_hidden_size(frida_config).unwrap(), 1536);

        // Standard hidden_size should still work
        let bert_config = r#"{"model_type": "bert", "hidden_size": 768}"#;
        assert_eq!(get_hidden_size(bert_config).unwrap(), 768);
    }

    #[test]
    fn test_normalize() {
        let mut v = vec![3.0, 4.0];
        normalize(&mut v);
        assert!((v[0] - 0.6).abs() < 1e-6);
        assert!((v[1] - 0.8).abs() < 1e-6);

        let mut zero_vector = vec![0.0, 0.0];
        normalize(&mut zero_vector);
        assert_eq!(zero_vector, vec![0.0, 0.0]);
    }

    #[test]
    fn test_pre_truncate_text_short_noop() {
        assert_eq!(pre_truncate_text("hello", 512), "hello");
    }

    #[test]
    fn test_pre_truncate_text_long_ascii() {
        let long = "a".repeat(10_000);
        let result = pre_truncate_text(&long, 512);
        assert_eq!(result.len(), 512 * BYTES_PER_TOKEN_UPPER_BOUND);
    }

    #[test]
    fn test_pre_truncate_text_multibyte() {
        // 4-byte emoji repeated — must cut at char boundary
        let emojis = "🦀".repeat(2000);
        let result = pre_truncate_text(&emojis, 512);
        assert!(result.len() <= 512 * BYTES_PER_TOKEN_UPPER_BOUND);
        assert!(result.is_char_boundary(result.len()));
    }

    #[test]
    fn test_pre_truncate_text_empty() {
        assert_eq!(pre_truncate_text("", 512), "");
    }
}
