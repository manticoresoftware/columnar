use super::{create_model, Model, ModelOptions};

#[test]
fn test_create_model_allows_custom_openai_model_when_custom_api_url_is_set() {
    let model = create_model(ModelOptions {
        model_id: "openai/rubert-tiny-turbo".to_string(),
        cache_path: None,
        api_key: Some("test-key".to_string()),
        api_url: Some("http://localhost:8080/v1/embeddings".to_string()),
        api_timeout: None,
        use_gpu: None,
    });

    assert!(model.is_ok());

    match model.unwrap() {
        Model::OpenAI(model) => assert_eq!(model.model, "rubert-tiny-turbo"),
        _ => panic!("expected OpenAI model"),
    }
}

#[test]
fn test_create_model_with_custom_url_still_uses_prefixed_jina_as_remote_signal() {
    let model = create_model(ModelOptions {
        model_id: "jina/custom-model".to_string(),
        cache_path: None,
        api_key: Some("test-key".to_string()),
        api_url: Some("http://localhost:8080/v1/embeddings".to_string()),
        api_timeout: None,
        use_gpu: None,
    });

    assert!(model.is_ok());

    match model.unwrap() {
        Model::Jina(model) => assert_eq!(model.model, "custom-model"),
        _ => panic!("expected Jina model"),
    }
}

#[test]
fn test_create_model_supports_explicit_openai_colon_syntax() {
    let model = create_model(ModelOptions {
        model_id: "openai:openai/text-embedding-ada-002".to_string(),
        cache_path: None,
        api_key: Some("test-key".to_string()),
        api_url: Some("http://localhost:8080/v1/embeddings".to_string()),
        api_timeout: None,
        use_gpu: None,
    });

    assert!(model.is_ok());

    match model.unwrap() {
        Model::OpenAI(model) => assert_eq!(model.model, "openai/text-embedding-ada-002"),
        _ => panic!("expected OpenAI model"),
    }
}

#[test]
fn test_create_model_supports_explicit_openai_colon_syntax_with_simple_model() {
    let model = create_model(ModelOptions {
        model_id: "openai:text-embedding-ada-002".to_string(),
        cache_path: None,
        api_key: Some("test-key".to_string()),
        api_url: Some("http://localhost:8080/v1/embeddings".to_string()),
        api_timeout: None,
        use_gpu: None,
    });

    assert!(model.is_ok());

    match model.unwrap() {
        Model::OpenAI(model) => assert_eq!(model.model, "text-embedding-ada-002"),
        _ => panic!("expected OpenAI model"),
    }
}
