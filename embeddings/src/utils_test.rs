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
    fn test_chunk_input_tokens_basic() {
        let tokens = vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
        let max_seq_len = 4;
        let stride = 2;

        let chunks = chunk_input_tokens(&tokens, max_seq_len, stride);
        assert_eq!(
            chunks,
            vec![
                vec![1, 2, 3, 4],
                vec![3, 4, 5, 6],
                vec![5, 6, 7, 8],
                vec![7, 8, 9, 10]
            ]
        );
    }

    #[test]
    fn test_chunk_input_tokens_short_input() {
        let short_tokens = vec![1, 2, 3];
        let max_seq_len = 4;
        let stride = 2;

        let short_chunks = chunk_input_tokens(&short_tokens, max_seq_len, stride);
        assert_eq!(short_chunks, vec![vec![1, 2, 3]]);
    }

    #[test]
    fn test_chunk_input_tokens_exact_length() {
        let tokens = vec![1, 2, 3, 4];
        let max_seq_len = 4;
        let stride = 2;

        let chunks = chunk_input_tokens(&tokens, max_seq_len, stride);
        assert_eq!(chunks, vec![vec![1, 2, 3, 4]]);
    }

    #[test]
    fn test_chunk_input_tokens_no_overlap() {
        let tokens = vec![1, 2, 3, 4, 5, 6, 7, 8];
        let max_seq_len = 4;
        let stride = 4; // No overlap

        let chunks = chunk_input_tokens(&tokens, max_seq_len, stride);
        assert_eq!(chunks, vec![vec![1, 2, 3, 4], vec![5, 6, 7, 8]]);
    }

    #[test]
    fn test_chunk_input_tokens_large_stride() {
        let tokens = vec![1, 2, 3, 4, 5, 6];
        let max_seq_len = 3;
        let stride = 5; // Stride larger than max_seq_len

        let chunks = chunk_input_tokens(&tokens, max_seq_len, stride);
        assert_eq!(
            chunks,
            vec![
                vec![1, 2, 3],
                vec![6] // Only one element left
            ]
        );
    }

    #[test]
    fn test_chunk_input_tokens_empty_input() {
        let empty_tokens: Vec<u32> = vec![];
        let max_seq_len = 4;
        let stride = 2;

        let chunks = chunk_input_tokens(&empty_tokens, max_seq_len, stride);
        assert_eq!(chunks, vec![Vec::<u32>::new()]);
    }

    #[test]
    fn test_get_mean_vector_basic() {
        let results = vec![
            vec![1.0, 2.0, 3.0],
            vec![4.0, 5.0, 6.0],
            vec![7.0, 8.0, 9.0],
        ];

        let mean_vector = get_mean_vector(&results);
        assert_eq!(mean_vector.len(), 3);

        // Calculate expected values with first chunk weighted 1.2, others 1.0
        let weight_sum = 1.2 + 1.0 + 1.0;
        let expected = [
            (1.2 * 1.0 + 1.0 * 4.0 + 1.0 * 7.0) / weight_sum,
            (1.2 * 2.0 + 1.0 * 5.0 + 1.0 * 8.0) / weight_sum,
            (1.2 * 3.0 + 1.0 * 6.0 + 1.0 * 9.0) / weight_sum,
        ];

        for (i, &val) in mean_vector.iter().enumerate() {
            assert_abs_diff_eq!(val, expected[i], epsilon = 1e-6);
        }
    }

    #[test]
    fn test_get_mean_vector_empty_input() {
        let empty_results: Vec<Vec<f32>> = vec![];
        assert_eq!(get_mean_vector(&empty_results), Vec::<f32>::new());
    }

    #[test]
    fn test_get_mean_vector_single_vector() {
        let single_result = vec![vec![1.0, 2.0, 3.0]];
        let single_mean = get_mean_vector(&single_result);
        assert_eq!(single_mean, vec![1.0, 2.0, 3.0]);
    }

    #[test]
    fn test_get_mean_vector_two_vectors() {
        let two_results = vec![vec![2.0, 4.0], vec![4.0, 8.0]];
        let mean = get_mean_vector(&two_results);

        // First vector weight: 1.2, second: 1.0
        let weight_sum = 1.2 + 1.0;
        let expected = [
            (1.2 * 2.0 + 1.0 * 4.0) / weight_sum,
            (1.2 * 4.0 + 1.0 * 8.0) / weight_sum,
        ];

        for (i, &val) in mean.iter().enumerate() {
            assert_abs_diff_eq!(val, expected[i], epsilon = 1e-6);
        }
    }

    #[test]
    fn test_get_mean_vector_zero_vectors() {
        let zero_results = vec![vec![0.0, 0.0], vec![0.0, 0.0], vec![0.0, 0.0]];
        let mean = get_mean_vector(&zero_results);
        assert_eq!(mean, vec![0.0, 0.0]);
    }

    #[test]
    fn test_get_mean_vector_negative_values() {
        let results = vec![vec![-1.0, 2.0], vec![3.0, -4.0]];
        let mean = get_mean_vector(&results);

        let weight_sum = 1.2 + 1.0;
        let expected = [
            (-1.2 + 1.0 * 3.0) / weight_sum,
            (1.2 * 2.0 + 1.0 * -4.0) / weight_sum,
        ];

        for (i, &val) in mean.iter().enumerate() {
            assert_abs_diff_eq!(val, expected[i], epsilon = 1e-6);
        }
    }

    #[test]
    fn test_get_mean_vector_large_dataset() {
        let results: Vec<Vec<f32>> = (0..100).map(|i| vec![i as f32, (i * 2) as f32]).collect();

        let mean = get_mean_vector(&results);
        assert_eq!(mean.len(), 2);

        // Verify the mean is reasonable
        assert!(mean[0] > 0.0 && mean[0] < 100.0);
        assert!(mean[1] > 0.0 && mean[1] < 200.0);
    }
}
