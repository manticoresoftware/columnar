use super::TextModel;
use crate::LibError;
use reqwest::blocking::Client;

#[derive(Debug)]
pub struct OpenAIModel {
    pub client: Client,
    pub model: String,
    pub api_key: String,
    pub api_url: Option<String>,
}

pub fn validate_model(model: &str) -> Result<(), String> {
    match model {
        "text-embedding-ada-002" | "text-embedding-3-small" | "text-embedding-3-large" => Ok(()),
        _ => Err(format!("Invalid model: {}", model)),
    }
}

/// Validates API key by checking basic requirements (non-empty, no whitespace).
/// Real validation happens via actual API request in validate_api_key() method.
fn validate_api_key_basic(api_key: &str) -> Result<(), String> {
    if api_key.is_empty() {
        return Err("API key is required".to_string());
    }

    // Trim whitespace and check
    let trimmed = api_key.trim();
    if trimmed != api_key {
        return Err("API key must not have leading or trailing whitespace".to_string());
    }

    Ok(())
}

impl OpenAIModel {
    pub fn new(
        model_id: &str,
        api_key: &str,
        api_url: Option<&str>,
        api_timeout: Option<u64>,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        let model = model_id.trim_start_matches("openai/").to_string();
        validate_model(&model).map_err(|_| LibError::RemoteUnsupportedModel { status: None })?;
        // Only validate basic requirements (non-empty, no whitespace)
        // Real validation happens via actual API request in validate_api_key()
        validate_api_key_basic(api_key)
            .map_err(|_| LibError::RemoteInvalidAPIKey { status: None })?;
        let timeout_secs = api_timeout.unwrap_or(10); // Default 10 seconds
        Ok(Self {
            client: Client::builder()
                .timeout(std::time::Duration::from_secs(timeout_secs))
                .build()?,
            model,
            api_key: api_key.to_string(),
            api_url: api_url.map(|s| s.to_string()),
        })
    }
}

impl TextModel for OpenAIModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn std::error::Error>> {
        let url = self
            .api_url
            .as_deref()
            .unwrap_or("https://api.openai.com/v1/embeddings");

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

        let status = response.status();
        let status_code = status.as_u16();

        let response_text = response.text().map_err(|_| LibError::RemoteHttpError {
            status: status_code,
        })?;

        // Check HTTP status code first
        if !status.is_success() {
            // Try to parse JSON error response for more details
            if let Ok(response_body) = serde_json::from_str::<serde_json::Value>(&response_text) {
                if let Some(error) = response_body.get("error") {
                    let error_code = error
                        .get("code")
                        .and_then(|c| c.as_str())
                        .unwrap_or("unknown_error");

                    // Map OpenAI error codes to appropriate LibError types
                    let lib_error = match (status_code, error_code) {
                        (401 | 403, _) | (_, "invalid_api_key") => LibError::RemoteInvalidAPIKey {
                            status: Some(status_code),
                        },
                        (404, _) | (_, "model_not_found") => LibError::RemoteUnsupportedModel {
                            status: Some(status_code),
                        },
                        (429, _) | (_, "insufficient_quota" | "rate_limit_exceeded") => {
                            LibError::RemoteRequestSendFailed
                        }
                        _ => LibError::RemoteHttpError {
                            status: status_code,
                        },
                    };

                    return Err(Box::new(lib_error));
                }
            }
            // If we can't parse JSON or no error field, map common HTTP status codes
            let lib_error = match status_code {
                401 | 403 => LibError::RemoteInvalidAPIKey {
                    status: Some(status_code),
                },
                404 => LibError::RemoteUnsupportedModel {
                    status: Some(status_code),
                },
                429 => LibError::RemoteRequestSendFailed,
                _ => LibError::RemoteHttpError {
                    status: status_code,
                },
            };
            return Err(Box::new(lib_error));
        }

        // Try to parse JSON (only if status is success)
        let response_body: serde_json::Value =
            serde_json::from_str(&response_text).map_err(|_| LibError::RemoteHttpError {
                status: status_code,
            })?;

        let data_array = response_body["data"].as_array();
        if data_array.is_none() {
            return Err(Box::new(LibError::RemoteHttpError {
                status: status_code,
            }));
        }

        let empty_vec: Vec<serde_json::Value> = Vec::new();
        let embeddings: Vec<Vec<f32>> = data_array
            .unwrap()
            .iter()
            .map(|item| {
                item["embedding"]
                    .as_array()
                    .unwrap_or(&empty_vec)
                    .iter()
                    .map(|v| v.as_f64().unwrap_or(0.0) as f32)
                    .collect()
            })
            .collect();

        // Validate that we got embeddings
        if embeddings.is_empty() {
            return Err(Box::new(LibError::RemoteHttpError {
                status: status_code,
            }));
        }

        Ok(embeddings)
    }

    fn get_hidden_size(&self) -> usize {
        match self.model.as_str() {
            "text-embedding-ada-002" => 1536, // Fixed: was 768, should be 1536
            "text-embedding-3-small" => 1536,
            "text-embedding-3-large" => 3072,
            _ => panic!("Unknown model"),
        }
    }

    fn get_max_input_len(&self) -> usize {
        8192
    }

    fn validate_api_key(&self) -> Result<(), Box<dyn std::error::Error>> {
        // Make a minimal test request with a single character to validate the API key
        // This is cheaper than a full embedding request but still validates the key
        self.predict(&["test"])?;
        Ok(())
    }
}
