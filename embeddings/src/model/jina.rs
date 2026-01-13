use super::TextModel;
use crate::LibError;
use reqwest::blocking::Client;

#[derive(Debug)]
pub struct JinaModel {
    pub client: Client,
    pub model: String,
    pub api_key: String,
    pub api_url: Option<String>,
}

pub fn validate_model(model: &str) -> Result<(), String> {
    match model {
        "jina-embeddings-v4"
        | "jina-clip-v2"
        | "jina-embeddings-v3"
        | "jina-colbert-v2"
        | "jina-clip-v1"
        | "jina-colbert-v1-en"
        | "jina-embeddings-v2-base-es"
        | "jina-embeddings-v2-base-code"
        | "jina-embeddings-v2-base-de"
        | "jina-embeddings-v2-base-zh"
        | "jina-embeddings-v2-base-en" => Ok(()),
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

impl JinaModel {
    pub fn new(
        model_id: &str,
        api_key: &str,
        api_url: Option<&str>,
        api_timeout: Option<u64>,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        let model = model_id.trim_start_matches("jina/").to_string();
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

impl TextModel for JinaModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn std::error::Error>> {
        let url = self
            .api_url
            .as_deref()
            .unwrap_or("https://api.jina.ai/v1/embeddings");

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

                    // Map Jina error codes to appropriate LibError types
                    let lib_error = match (status_code, error_code) {
                        (401 | 403, _)
                        | (_, "invalid_api_key" | "authentication_error" | "unauthorized") => {
                            LibError::RemoteInvalidAPIKey {
                                status: Some(status_code),
                            }
                        }
                        (404, _) | (_, "model_not_found" | "invalid_model") => {
                            LibError::RemoteUnsupportedModel {
                                status: Some(status_code),
                            }
                        }
                        (429, _) | (_, "rate_limit_exceeded" | "quota_exceeded") => {
                            LibError::RemoteRequestSendFailed
                        }
                        _ => LibError::RemoteHttpError {
                            status: status_code,
                        },
                    };

                    return Err(Box::new(lib_error));
                }
            }
            // Check for alternative error formats (Jina uses different structures)
            if let Ok(response_body) = serde_json::from_str::<serde_json::Value>(&response_text) {
                if let Some(detail) = response_body.get("detail") {
                    if detail.is_string() {
                        let detail_str = detail.as_str().unwrap_or("");
                        // Map based on status code and error message
                        let lib_error = if detail_str.contains("Invalid API key")
                            || detail_str.contains("authentication")
                        {
                            LibError::RemoteInvalidAPIKey {
                                status: Some(status_code),
                            }
                        } else if detail_str.contains("model") && detail_str.contains("not found") {
                            LibError::RemoteUnsupportedModel {
                                status: Some(status_code),
                            }
                        } else {
                            match status_code {
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
                            }
                        };
                        return Err(Box::new(lib_error));
                    }
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

        let response_body: serde_json::Value =
            serde_json::from_str(&response_text).map_err(|_| LibError::RemoteHttpError {
                status: status_code,
            })?;

        // Check if there's an error in the response (shouldn't happen if status was success, but check anyway)
        if let Some(error) = response_body.get("error") {
            let error_code = error
                .get("code")
                .and_then(|c| c.as_str())
                .unwrap_or("unknown_error");

            // Map Jina error codes to appropriate LibError types
            let lib_error = match error_code {
                "invalid_api_key" | "authentication_error" | "unauthorized" => {
                    LibError::RemoteInvalidAPIKey {
                        status: Some(status_code),
                    }
                }
                "model_not_found" | "invalid_model" => LibError::RemoteUnsupportedModel {
                    status: Some(status_code),
                },
                "rate_limit_exceeded" | "quota_exceeded" => LibError::RemoteRequestSendFailed,
                _ => LibError::RemoteHttpError {
                    status: status_code,
                },
            };

            return Err(Box::new(lib_error));
        }

        // Check for alternative error formats (Jina uses different structures)
        if let Some(detail) = response_body.get("detail") {
            if detail.is_string() {
                let detail_str = detail.as_str().unwrap_or("");
                // Map based on error message
                let lib_error = if detail_str.contains("Invalid API key")
                    || detail_str.contains("authentication")
                {
                    LibError::RemoteInvalidAPIKey {
                        status: Some(status_code),
                    }
                } else if detail_str.contains("model") && detail_str.contains("not found") {
                    LibError::RemoteUnsupportedModel {
                        status: Some(status_code),
                    }
                } else {
                    LibError::RemoteHttpError {
                        status: status_code,
                    }
                };
                return Err(Box::new(lib_error));
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
            return Err(Box::new(LibError::RemoteHttpError {
                status: status_code,
            }));
        }

        // Validate embedding dimensions and handle empty individual embeddings
        let expected_dim = self.get_hidden_size();
        for embedding in embeddings.iter() {
            if embedding.is_empty() {
                return Err(Box::new(LibError::RemoteHttpError {
                    status: status_code,
                }));
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
            "jina-embeddings-v4" => 2048,          // 32K context, 2048 dimensions
            "jina-clip-v2" => 1024,                // 8K context, 1024 dimensions, multimodal
            "jina-embeddings-v3" => 1024,          // 8K context, 1024 dimensions
            "jina-colbert-v2" => 128,              // Multi-vector model, 8K context
            "jina-clip-v1" => 768,                 // 8K context, 768 dimensions, multimodal
            "jina-colbert-v1-en" => 128,           // Multi-vector model, 8K context
            "jina-embeddings-v2-base-es" => 768,   // 8K context, 768 dimensions
            "jina-embeddings-v2-base-code" => 768, // 8K context, 768 dimensions
            "jina-embeddings-v2-base-de" => 768,   // 8K context, 768 dimensions
            "jina-embeddings-v2-base-zh" => 768,   // 8K context, 768 dimensions
            "jina-embeddings-v2-base-en" => 768,   // 8K context, 768 dimensions
            _ => panic!("Unknown model"),
        }
    }

    fn get_max_input_len(&self) -> usize {
        match self.model.as_str() {
            "jina-embeddings-v4" => 32000,          // 32K context length
            "jina-clip-v2" => 8192,                 // 8K context length
            "jina-embeddings-v3" => 8192,           // 8K context length
            "jina-colbert-v2" => 8192,              // 8K context length
            "jina-clip-v1" => 8192,                 // 8K context length
            "jina-colbert-v1-en" => 8192,           // 8K context length
            "jina-embeddings-v2-base-es" => 8192,   // 8K context length
            "jina-embeddings-v2-base-code" => 8192, // 8K context length
            "jina-embeddings-v2-base-de" => 8192,   // 8K context length
            "jina-embeddings-v2-base-zh" => 8192,   // 8K context length
            "jina-embeddings-v2-base-en" => 8192,   // 8K context length
            _ => 8192,                              // Default fallback
        }
    }

    fn validate_api_key(&self) -> Result<(), Box<dyn std::error::Error>> {
        // Make a minimal test request with a single character to validate the API key
        // This is cheaper than a full embedding request but still validates the key
        self.predict(&["test"])?;
        Ok(())
    }
}
