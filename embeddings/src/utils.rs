use serde_json::Value;
use anyhow::Result;

/// Get maximum input length for sequence for the current model
pub fn get_max_input_length(contents: &str) -> Result<usize> {
	let config: Value = serde_json::from_str(&contents)?;
	let max_length = config["max_position_embeddings"]
		.as_u64()
		.ok_or_else(|| std::io::Error::new(std::io::ErrorKind::Other, "Max position embeddings not found"))?;
	Ok(max_length as usize)
}

pub fn get_hidden_size(contents: &str) -> Result<usize> {
	let config: Value = serde_json::from_str(&contents)?;
	let max_length = config["hidden_size"]
		.as_u64()
		.ok_or_else(|| std::io::Error::new(std::io::ErrorKind::Other, "Hidden size not found"))?;
	Ok(max_length as usize)
}

#[inline]
pub fn normalize(v: &mut Vec<f32>) {
	let length: f32 = v.iter().map(|x| x * x).sum::<f32>().sqrt();
	if length > 0.0 {
		v.iter_mut().for_each(|x| *x /= length);
	}
}

pub fn chunk_input_tokens(tokens: &[u32], max_seq_len: usize, stride: usize) -> Vec<Vec<u32>> {
	if tokens.len() <= max_seq_len {
		return vec![tokens.to_vec()];
	}

	let mut chunks = Vec::new();
	let mut start = 0;
	let len = tokens.len();

	while start < len {
		let end = std::cmp::min(start + max_seq_len, len);
		chunks.push(tokens[start..end].to_vec());

		if end == len {
			break;
		}

		start += stride;
	}

	chunks
}

pub fn get_mean_vector(results: &[Vec<f32>]) -> Vec<f32> {
	if results.is_empty() {
		return Vec::new();
	}

	let num_cols = results[0].len();
	let mut mean_vector = vec![0.0; num_cols];

	let mut weight_sum = 0.0;

	for (i, row) in results.iter().enumerate() {
		let weight = if i == 0 { 1.2 } else { 1.0 }; // Adjust the weight for the first chunk here
		weight_sum += weight;

		for (j, val) in row.iter().enumerate() {
			mean_vector[j] += weight * val;
		}
	}

	for val in &mut mean_vector {
		*val /= weight_sum;
	}

	mean_vector
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
	fn test_get_hidden_size() {
		let config = r#"{"hidden_size": 768}"#;
		assert_eq!(get_hidden_size(config).unwrap(), 768);

		let invalid_config = r#"{"some_other_field": 768}"#;
		assert!(get_hidden_size(invalid_config).is_err());
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
	fn test_chunk_input_tokens() {
		let tokens = vec![1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
		let max_seq_len = 4;
		let stride = 2;

		let chunks = chunk_input_tokens(&tokens, max_seq_len, stride);
		assert_eq!(chunks, vec![
			vec![1, 2, 3, 4],
			vec![3, 4, 5, 6],
			vec![5, 6, 7, 8],
			vec![7, 8, 9, 10]
		]);

		// Test when input is shorter than max_seq_len
		let short_tokens = vec![1, 2, 3];
		let short_chunks = chunk_input_tokens(&short_tokens, max_seq_len, stride);
		assert_eq!(short_chunks, vec![vec![1, 2, 3]]);
	}

	#[test]
	fn test_get_mean_vector() {
		let results = vec![
			vec![1.0, 2.0, 3.0],
			vec![4.0, 5.0, 6.0],
			vec![7.0, 8.0, 9.0]
		];

		let mean_vector = get_mean_vector(&results);
		assert_eq!(mean_vector.len(), 3);

		// Calculate expected values
		let weight_sum = 1.2 + 1.0 + 1.0;
		let expected = [
			(1.2 * 1.0 + 1.0 * 4.0 + 1.0 * 7.0) / weight_sum,
			(1.2 * 2.0 + 1.0 * 5.0 + 1.0 * 8.0) / weight_sum,
			(1.2 * 3.0 + 1.0 * 6.0 + 1.0 * 9.0) / weight_sum
		];

		for (i, &val) in mean_vector.iter().enumerate() {
			assert!((val - expected[i]).abs() < 1e-6, "Mismatch at index {}: expected {}, got {}", i, expected[i], val);
		}

		// Test with empty input
		let empty_results: Vec<Vec<f32>> = vec![];
		assert_eq!(get_mean_vector(&empty_results), Vec::<f32>::new());

		// Test with single vector
		let single_result = vec![vec![1.0, 2.0, 3.0]];
		let single_mean = get_mean_vector(&single_result);
		assert_eq!(single_mean, vec![1.0, 2.0, 3.0]);

		// Test with multiple vectors
		let multiple_results = vec![
			vec![1.0, 2.0, 3.0],
			vec![4.0, 5.0, 6.0],
			vec![7.0, 8.0, 9.0],
			vec![10.0, 11.0, 12.0]
		];
		let multiple_mean = get_mean_vector(&multiple_results);

		// Calculate expected values for multiple vectors
		let weight_sum = 1.2 + 1.0 + 1.0 + 1.0;
		let expected = [
			(1.2 * 1.0 + 1.0 * 4.0 + 1.0 * 7.0 + 1.0 * 10.0) / weight_sum,
			(1.2 * 2.0 + 1.0 * 5.0 + 1.0 * 8.0 + 1.0 * 11.0) / weight_sum,
			(1.2 * 3.0 + 1.0 * 6.0 + 1.0 * 9.0 + 1.0 * 12.0) / weight_sum
		];

		for (i, &val) in multiple_mean.iter().enumerate() {
			assert!((val - expected[i]).abs() < 1e-6, "Mismatch at index {}: expected {}, got {}", i, expected[i], val);
		}
	}
}
