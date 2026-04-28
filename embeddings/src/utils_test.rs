use super::utils::*;
use approx::assert_abs_diff_eq;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_get_max_input_length_valid() {
        let config = r#"{"max_position_embeddings": 512}"#;
        assert_eq!(get_max_input_length(config).unwrap(), 512);

        let config_large = r#"{"max_position_embeddings": 8192}"#;
        assert_eq!(get_max_input_length(config_large).unwrap(), 8192);
    }

    #[test]
    fn test_get_max_input_length_invalid() {
        let invalid_config = r#"{"some_other_field": 512}"#;
        assert!(get_max_input_length(invalid_config).is_err());

        let malformed_json = r#"{"max_position_embeddings": "not_a_number"}"#;
        assert!(get_max_input_length(malformed_json).is_err());

        let empty_config = r#"{}"#;
        assert!(get_max_input_length(empty_config).is_err());

        let invalid_json = r#"{"max_position_embeddings": 512"#; // Missing closing brace
        assert!(get_max_input_length(invalid_json).is_err());
    }

    #[test]
    fn test_get_hidden_size_valid() {
        let config = r#"{"hidden_size": 768}"#;
        assert_eq!(get_hidden_size(config).unwrap(), 768);

        let config_small = r#"{"hidden_size": 384}"#;
        assert_eq!(get_hidden_size(config_small).unwrap(), 384);

        let config_large = r#"{"hidden_size": 1536}"#;
        assert_eq!(get_hidden_size(config_large).unwrap(), 1536);
    }

    #[test]
    fn test_get_hidden_size_invalid() {
        let invalid_config = r#"{"some_other_field": 768}"#;
        assert!(get_hidden_size(invalid_config).is_err());

        let malformed_json = r#"{"hidden_size": "not_a_number"}"#;
        assert!(get_hidden_size(malformed_json).is_err());

        let empty_config = r#"{}"#;
        assert!(get_hidden_size(empty_config).is_err());

        let invalid_json = r#"{"hidden_size": 768"#; // Missing closing brace
        assert!(get_hidden_size(invalid_json).is_err());
    }

    #[test]
    fn test_normalize_standard_vector() {
        let mut v = vec![3.0, 4.0];
        normalize(&mut v);
        assert_abs_diff_eq!(v[0], 0.6, epsilon = 1e-6);
        assert_abs_diff_eq!(v[1], 0.8, epsilon = 1e-6);

        // Verify it's normalized (length = 1)
        let length: f32 = v.iter().map(|x| x * x).sum::<f32>().sqrt();
        assert_abs_diff_eq!(length, 1.0, epsilon = 1e-6);
    }

    #[test]
    fn test_normalize_zero_vector() {
        let mut zero_vector = vec![0.0, 0.0, 0.0];
        normalize(&mut zero_vector);
        assert_eq!(zero_vector, vec![0.0, 0.0, 0.0]);
    }

    #[test]
    fn test_normalize_single_element() {
        let mut single = vec![5.0];
        normalize(&mut single);
        assert_abs_diff_eq!(single[0], 1.0, epsilon = 1e-6);
    }

    #[test]
    fn test_normalize_negative_values() {
        let mut v = vec![-3.0, 4.0];
        normalize(&mut v);
        let length: f32 = v.iter().map(|x| x * x).sum::<f32>().sqrt();
        assert_abs_diff_eq!(length, 1.0, epsilon = 1e-6);
        assert_abs_diff_eq!(v[0], -0.6, epsilon = 1e-6);
        assert_abs_diff_eq!(v[1], 0.8, epsilon = 1e-6);
    }

    #[test]
    fn test_normalize_empty_vector() {
        let mut empty: Vec<f32> = vec![];
        normalize(&mut empty);
        assert_eq!(empty, Vec::<f32>::new());
    }

    #[test]
    fn test_pre_truncate_text_noop_for_short() {
        let text = "short text";
        assert_eq!(pre_truncate_text(text, 512), text);
    }

    #[test]
    fn test_pre_truncate_text_cuts_long() {
        let long = "x".repeat(100_000);
        let result = pre_truncate_text(&long, 128);
        assert_eq!(result.len(), 128 * 8);
    }

    #[test]
    fn test_pre_truncate_text_respects_char_boundary() {
        // 2-byte chars (Cyrillic)
        let cyrillic = "Б".repeat(5000);
        let result = pre_truncate_text(&cyrillic, 256);
        assert!(result.len() <= 256 * 8);
        // Must be valid UTF-8 (wouldn't compile otherwise, but ensure no panic)
        assert!(result.len().is_multiple_of(2)); // Б is 2 bytes
    }
}
