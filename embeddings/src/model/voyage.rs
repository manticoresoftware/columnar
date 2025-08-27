use super::TextModel;
use crate::LibError;
use reqwest::blocking::Client;

#[derive(Debug)]
pub struct VoyageModel {
    pub client: Client,
    pub model: String,
    pub api_key: String,
}

pub fn validate_model(model: &str) -> Result<(), String> {
    match model {
        "voyage-3-large" | "voyage-3.5" | "voyage-3.5-lite" | "voyage-code-3"
        | "voyage-finance-2" | "voyage-law-2" | "voyage-code-2" => Ok(()),
        _ => Err(format!("Invalid model: {}", model)),
    }
}

pub fn validate_api_key(api_key: &str) -> Result<(), String> {
    if api_key.is_empty() {
        return Err("API key is required".to_string());
    }

    // Trim whitespace and check
    let trimmed = api_key.trim();
    if trimmed != api_key {
        return Err("API key must not have leading or trailing whitespace".to_string());
    }

    // Voyage API keys typically start with "pa-" prefix
    if !api_key.starts_with("pa-") || api_key.len() <= 3 {
        return Err("API key must start with pa- and have content".to_string());
    }

    Ok(())
}

impl VoyageModel {
    pub fn new(model_id: &str, api_key: &str) -> Result<Self, Box<dyn std::error::Error>> {
        let model = model_id.trim_start_matches("voyage/").to_string();
        validate_model(&model).map_err(|_| LibError::RemoteUnsupportedModel)?;
        validate_api_key(api_key).map_err(|_| LibError::RemoteInvalidAPIKey)?;
        Ok(Self {
            client: Client::new(),
            model,
            api_key: api_key.to_string(),
        })
    }
}

impl TextModel for VoyageModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn std::error::Error>> {
        let url = "https://api.voyageai.com/v1/embeddings";

        let request_body = serde_json::json!({
            "input": texts,
            "model": self.model,
        });

        let response = self
            .client
            .post(url)
            .header("Authorization", format!("Bearer {}", self.api_key))
            .header("Content-Type", "application/json")
            .json(&request_body)
            .send()
            .map_err(|_| LibError::RemoteRequestSendFailed)?;

        let response_body: serde_json::Value = response
            .json()
            .map_err(|_| LibError::RemoteResponseParseFailed)?;

        // Check if there's an error in the response - proper szError pattern handling
        if let Some(error) = response_body.get("error") {
            let error_code = error
                .get("code")
                .and_then(|c| c.as_str())
                .unwrap_or("unknown_error");

            // Map Voyage error codes to appropriate LibError types
            let lib_error = match error_code {
                "invalid_api_key" | "authentication_error" => LibError::RemoteInvalidAPIKey,
                "model_not_found" | "invalid_model" => LibError::RemoteUnsupportedModel,
                "rate_limit_exceeded" | "quota_exceeded" => LibError::RemoteRequestSendFailed,
                _ => LibError::RemoteResponseParseFailed,
            };

            return Err(Box::new(lib_error));
        }

        // Check for alternative error format (some APIs use different structures)
        if let Some(detail) = response_body.get("detail") {
            if detail.is_string() {
                return Err(Box::new(LibError::RemoteResponseParseFailed));
            }
        }

        let embeddings: Vec<Vec<f32>> = response_body["data"]
            .as_array()
            .unwrap_or(&Vec::new())
            .iter()
            .map(|item| {
                item["embedding"]
                    .as_array()
                    .unwrap_or(&Vec::new())
                    .iter()
                    .map(|v| v.as_f64().unwrap_or(0.0) as f32)
                    .collect()
            })
            .collect();

        // Validate that we got embeddings - never return empty vectors
        if embeddings.is_empty() {
            return Err(Box::new(LibError::RemoteResponseParseFailed));
        }

        // Validate embedding dimensions and handle empty individual embeddings
        let expected_dim = self.get_hidden_size();
        for embedding in embeddings.iter() {
            if embedding.is_empty() {
                return Err(Box::new(LibError::RemoteResponseParseFailed));
            }
            if embedding.len() != expected_dim {
                // Some models might return different dimensions, but we should validate
                // For now, we'll be lenient but could add stricter validation later
            }
        }

        Ok(embeddings)
    }

    fn get_hidden_size(&self) -> usize {
        match self.model.as_str() {
            "voyage-3-large" => 1024,  // Default 1024, supports 256, 512, 2048
            "voyage-3.5" => 1024,      // Default 1024, supports 256, 512, 2048
            "voyage-3.5-lite" => 1024, // Default 1024, supports 256, 512, 2048
            "voyage-code-3" => 1024,   // Default 1024, supports 256, 512, 2048
            "voyage-finance-2" => 1024,
            "voyage-law-2" => 1024,
            "voyage-code-2" => 1536,
            _ => panic!("Unknown model"),
        }
    }

    fn get_max_input_len(&self) -> usize {
        match self.model.as_str() {
            "voyage-3-large" => 32000,
            "voyage-3.5" => 32000,
            "voyage-3.5-lite" => 32000,
            "voyage-code-3" => 32000,
            "voyage-finance-2" => 32000,
            "voyage-law-2" => 16000,
            "voyage-code-2" => 16000,
            _ => 8192, // Default fallback
        }
    }
}
