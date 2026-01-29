use super::local::{build_model_info, ModelInfo};
use super::TextModel;
use crate::utils::{
    chunk_input_tokens, get_hidden_size, get_max_input_length, get_mean_vector, normalize,
};
use crate::LibError;
use candle_core::{DType, Device, Module, Result as CandleResult, Tensor};
use candle_nn::{Activation, VarBuilder};
// Reused from candle: qwen3::Config, with_tracing::{linear_no_bias, Linear, RmsNorm}, repeat_kv,
// candle_nn::rotary_emb::rope, candle_nn::ops::softmax_last_dim. Candle's Qwen3RotaryEmbedding,
// Qwen3MLP, Qwen3Attention and Model are pub(crate); Model also has no public clear_kv_cache (only
// ModelForCausalLM does), so we keep our own stateless impl for embedding.
use candle_transformers::models::qwen3::Config as QwenConfig;
use candle_transformers::models::with_tracing::{linear_no_bias, Linear, RmsNorm};
use candle_transformers::utils::repeat_kv;
use serde::Deserialize;
use serde_json::Value;
use std::error::Error;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use tokenizers::Tokenizer;

pub fn is_qwen_model(model_id: &str, cache_path: &Path) -> Result<bool, Box<dyn Error>> {
    let info = build_model_info(cache_path.to_path_buf(), model_id, "main", false)?;
    let config =
        std::fs::read_to_string(&info.config_path).map_err(|_| LibError::ModelConfigReadFailed)?;
    Ok(is_qwen_config(&config))
}

fn is_qwen_config(config: &str) -> bool {
    serde_json::from_str::<Value>(config)
        .ok()
        .and_then(|v| {
            v.get("model_type")
                .and_then(Value::as_str)
                .map(str::to_string)
        })
        .is_some_and(|s| matches!(s.as_str(), "qwen2" | "qwen3"))
}

pub struct QwenModel {
    model: QwenEmbedModel,
    tokenizer: Tokenizer,
    max_input_len: usize,
    hidden_size: usize,
}

impl QwenModel {
    pub fn new(model_id: &str, cache_path: PathBuf, use_gpu: bool) -> Result<Self, Box<dyn Error>> {
        let device = if use_gpu {
            Device::new_cuda(0).map_err(|_| LibError::DeviceCudaInitFailed)?
        } else {
            Device::Cpu
        };
        let model_info = build_model_info(cache_path, model_id, "main", false)?;
        let config = std::fs::read_to_string(&model_info.config_path)
            .map_err(|_| LibError::ModelConfigReadFailed)?;
        if !is_qwen_config(&config) {
            return Err(Box::new(LibError::ModelConfigParseFailed));
        }
        let max_input_len =
            get_max_input_length(&config).map_err(|_| LibError::ModelMaxInputLenGetFailed)?;
        let hidden_size =
            get_hidden_size(&config).map_err(|_| LibError::ModelHiddenSizeGetFailed)?;
        let mut tokenizer = load_tokenizer(&model_info.tokenizer_path)?;
        let tokenizer = tokenizer
            .with_padding(None)
            .with_truncation(None)
            .map_err(|_| LibError::ModelTokenizerConfigurationFailed)?;
        let dtype = match (dtype_from_config(&config), &device) {
            (DType::BF16, Device::Cpu) => DType::F16,
            (d, _) => d,
        };
        let model = QwenEmbedModel::new(&config, &model_info, dtype, &device)?;

        Ok(Self {
            model,
            tokenizer: tokenizer.clone().into(),
            max_input_len,
            hidden_size,
        })
    }
}

impl TextModel for QwenModel {
    fn predict(&self, texts: &[&str]) -> Result<Vec<Vec<f32>>, Box<dyn Error>> {
        let device = self.model.device();
        let mut all_results: Vec<Vec<f32>> = Vec::new();
        for text in texts.iter() {
            let tokens = self
                .tokenizer
                .encode(*text, true)
                .map_err(|_| LibError::ModelTokenizerEncodeFailed)?
                .get_ids()
                .to_vec();
            let chunks = chunk_input_tokens(&tokens, self.max_input_len, self.max_input_len / 10);
            let mut results: Vec<Vec<f32>> = Vec::new();
            for chunk in chunks.iter() {
                let token_ids = Tensor::new(&chunk[..], device)?.unsqueeze(0)?;
                let embeddings = self.model.forward(&token_ids)?;
                let (_n_sentences, n_tokens, _hidden_size) = embeddings.dims3()?;
                let embeddings = (embeddings.sum(1)? / (n_tokens as f64))?.to_dtype(DType::F32)?;

                if let Ok(e_j) = embeddings.get(0) {
                    let mut emb: Vec<f32> = e_j.to_vec1()?;
                    normalize(&mut emb);
                    results.push(emb);
                }
            }

            if results.is_empty() {
                return Err(Box::new(LibError::ModelLoadFailed));
            }

            let mean_vector = get_mean_vector(&results);
            all_results.push(mean_vector);
        }

        if all_results.is_empty() || all_results.len() != texts.len() {
            return Err(Box::new(LibError::ModelLoadFailed));
        }

        Ok(all_results)
    }

    fn get_hidden_size(&self) -> usize {
        self.hidden_size
    }

    fn get_max_input_len(&self) -> usize {
        self.max_input_len
    }
}

fn dtype_from_config(config: &str) -> DType {
    serde_json::from_str::<Value>(config)
        .ok()
        .and_then(|v| {
            v.get("torch_dtype")
                .and_then(Value::as_str)
                .map(|s| match s {
                    "bfloat16" => DType::BF16,
                    "float16" => DType::F16,
                    "float32" => DType::F32,
                    _ => DType::F16,
                })
        })
        .unwrap_or(DType::F16)
}

fn load_tokenizer(path: &Path) -> Result<Tokenizer, Box<dyn Error>> {
    if let Ok(tok) = Tokenizer::from_file(path) {
        return Ok(tok);
    }
    let contents = std::fs::read_to_string(path).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
    let mut value: Value =
        serde_json::from_str(&contents).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
    if let Some(model) = value.get_mut("model").and_then(Value::as_object_mut) {
        model.remove("ignore_merges");
        if let Some(merges) = model.get_mut("merges").and_then(Value::as_array_mut) {
            for item in merges.iter_mut() {
                if let Value::Array(parts) = item {
                    if parts.len() == 2 {
                        let a = parts[0].as_str().unwrap_or_default();
                        let b = parts[1].as_str().unwrap_or_default();
                        *item = Value::String(format!("{a} {b}"));
                    }
                }
            }
        }
    }
    let bytes = serde_json::to_vec(&value).map_err(|_| LibError::ModelTokenizerLoadFailed)?;
    Tokenizer::from_bytes(&bytes).map_err(|_| LibError::ModelTokenizerLoadFailed.into())
}

#[derive(Debug, Clone, Deserialize)]
struct QwenConfigRaw {
    vocab_size: usize,
    hidden_size: usize,
    intermediate_size: usize,
    num_hidden_layers: usize,
    num_attention_heads: usize,
    num_key_value_heads: usize,
    #[serde(default)]
    head_dim: Option<usize>,
    max_position_embeddings: usize,
    #[serde(default)]
    sliding_window: Option<usize>,
    rope_theta: f64,
    rms_norm_eps: f64,
    #[serde(default)]
    hidden_act: Activation,
}

impl QwenConfigRaw {
    /// Build candle's Qwen3 Config (same shape as upstream); we fill extra fields with defaults.
    fn normalize(self, head_dim_override: Option<usize>) -> QwenConfig {
        let sliding_window = self.sliding_window.unwrap_or(self.max_position_embeddings);
        let head_dim = head_dim_override
            .or(self.head_dim)
            .unwrap_or(self.hidden_size / self.num_attention_heads);
        QwenConfig {
            vocab_size: self.vocab_size,
            hidden_size: self.hidden_size,
            intermediate_size: self.intermediate_size,
            num_hidden_layers: self.num_hidden_layers,
            num_attention_heads: self.num_attention_heads,
            head_dim,
            attention_bias: false,
            num_key_value_heads: self.num_key_value_heads,
            max_position_embeddings: self.max_position_embeddings,
            sliding_window: Some(sliding_window),
            max_window_layers: 0,
            tie_word_embeddings: false,
            rope_theta: self.rope_theta,
            rms_norm_eps: self.rms_norm_eps,
            use_sliding_window: false,
            hidden_act: self.hidden_act,
        }
    }
}

#[derive(Debug, Clone)]
struct RotaryEmbedding {
    sin: Tensor,
    cos: Tensor,
}

impl RotaryEmbedding {
    fn new(dtype: DType, cfg: &QwenConfig, dev: &Device) -> CandleResult<Self> {
        let dim = cfg.head_dim;
        let max_seq_len = cfg.max_position_embeddings;
        let inv_freq: Vec<_> = (0..dim)
            .step_by(2)
            .map(|i| 1f32 / cfg.rope_theta.powf(i as f64 / dim as f64) as f32)
            .collect();
        let inv_freq_len = inv_freq.len();
        let inv_freq = Tensor::from_vec(inv_freq, (1, inv_freq_len), dev)?.to_dtype(dtype)?;
        let t = Tensor::arange(0u32, max_seq_len as u32, dev)?
            .to_dtype(dtype)?
            .reshape((max_seq_len, 1))?;
        let freqs = t.matmul(&inv_freq)?;
        Ok(Self {
            sin: freqs.sin()?,
            cos: freqs.cos()?,
        })
    }

    fn apply_rotary_emb_qkv(
        &self,
        q: &Tensor,
        k: &Tensor,
        seqlen_offset: usize,
    ) -> CandleResult<(Tensor, Tensor)> {
        let (_b_sz, _h, seq_len, _n_embd) = q.dims4()?;
        let cos = self.cos.narrow(0, seqlen_offset, seq_len)?;
        let sin = self.sin.narrow(0, seqlen_offset, seq_len)?;
        let q_embed = candle_nn::rotary_emb::rope(&q.contiguous()?, &cos, &sin)?;
        let k_embed = candle_nn::rotary_emb::rope(&k.contiguous()?, &cos, &sin)?;
        Ok((q_embed, k_embed))
    }
}

#[derive(Debug, Clone)]
#[allow(clippy::upper_case_acronyms)]
struct MLP {
    gate_proj: Linear,
    up_proj: Linear,
    down_proj: Linear,
    act_fn: Activation,
}

impl MLP {
    fn new(cfg: &QwenConfig, vb: VarBuilder) -> CandleResult<Self> {
        let hidden_sz = cfg.hidden_size;
        let intermediate_sz = cfg.intermediate_size;
        let gate_proj = linear_no_bias(hidden_sz, intermediate_sz, vb.pp("gate_proj"))?;
        let up_proj = linear_no_bias(hidden_sz, intermediate_sz, vb.pp("up_proj"))?;
        let down_proj = linear_no_bias(intermediate_sz, hidden_sz, vb.pp("down_proj"))?;
        Ok(Self {
            gate_proj,
            up_proj,
            down_proj,
            act_fn: cfg.hidden_act,
        })
    }
}

impl Module for MLP {
    fn forward(&self, xs: &Tensor) -> CandleResult<Tensor> {
        let lhs = xs.apply(&self.gate_proj)?.apply(&self.act_fn)?;
        let rhs = xs.apply(&self.up_proj)?;
        (lhs * rhs)?.apply(&self.down_proj)
    }
}

#[derive(Debug, Clone)]
struct Attention {
    q_proj: Linear,
    k_proj: Linear,
    v_proj: Linear,
    o_proj: Linear,
    num_heads: usize,
    num_kv_heads: usize,
    num_kv_groups: usize,
    head_dim: usize,
    rotary_emb: Arc<RotaryEmbedding>,
}

impl Attention {
    fn new(
        rotary_emb: Arc<RotaryEmbedding>,
        cfg: &QwenConfig,
        vb: VarBuilder,
    ) -> CandleResult<Self> {
        let hidden_sz = cfg.hidden_size;
        let num_heads = cfg.num_attention_heads;
        let num_kv_heads = cfg.num_key_value_heads;
        let num_kv_groups = num_heads / num_kv_heads;
        let head_dim = cfg.head_dim;
        Ok(Self {
            q_proj: linear_no_bias(hidden_sz, num_heads * head_dim, vb.pp("q_proj"))?,
            k_proj: linear_no_bias(hidden_sz, num_kv_heads * head_dim, vb.pp("k_proj"))?,
            v_proj: linear_no_bias(hidden_sz, num_kv_heads * head_dim, vb.pp("v_proj"))?,
            o_proj: linear_no_bias(num_heads * head_dim, hidden_sz, vb.pp("o_proj"))?,
            num_heads,
            num_kv_heads,
            num_kv_groups,
            head_dim,
            rotary_emb,
        })
    }

    fn forward(&self, xs: &Tensor, attention_mask: Option<&Tensor>) -> CandleResult<Tensor> {
        let (b_sz, q_len, _) = xs.dims3()?;
        let query_states = self
            .q_proj
            .forward(xs)?
            .reshape((b_sz, q_len, self.num_heads, self.head_dim))?
            .transpose(1, 2)?;
        let key_states = self
            .k_proj
            .forward(xs)?
            .reshape((b_sz, q_len, self.num_kv_heads, self.head_dim))?
            .transpose(1, 2)?;
        let value_states = self
            .v_proj
            .forward(xs)?
            .reshape((b_sz, q_len, self.num_kv_heads, self.head_dim))?
            .transpose(1, 2)?;

        let (query_states, key_states) =
            self.rotary_emb
                .apply_rotary_emb_qkv(&query_states, &key_states, 0)?;

        let key_states = repeat_kv(key_states, self.num_kv_groups)?.contiguous()?;
        let value_states = repeat_kv(value_states, self.num_kv_groups)?.contiguous()?;

        let attn_output = {
            let scale = 1f64 / f64::sqrt(self.head_dim as f64);
            let attn_weights = (query_states.matmul(&key_states.transpose(2, 3)?)? * scale)?;

            let attn_weights = match attention_mask {
                None => attn_weights,
                Some(mask) => attn_weights.broadcast_add(mask)?,
            };
            let attn_weights = candle_nn::ops::softmax_last_dim(&attn_weights)?;
            attn_weights.matmul(&value_states)?
        };
        attn_output
            .transpose(1, 2)?
            .reshape((b_sz, q_len, self.num_heads * self.head_dim))?
            .apply(&self.o_proj)
    }
}

#[derive(Debug, Clone)]
struct DecoderLayer {
    self_attn: Attention,
    mlp: MLP,
    input_layernorm: RmsNorm,
    post_attention_layernorm: RmsNorm,
}

impl DecoderLayer {
    fn new(
        rotary_emb: Arc<RotaryEmbedding>,
        cfg: &QwenConfig,
        vb: VarBuilder,
    ) -> CandleResult<Self> {
        Ok(Self {
            self_attn: Attention::new(rotary_emb, cfg, vb.pp("self_attn"))?,
            mlp: MLP::new(cfg, vb.pp("mlp"))?,
            input_layernorm: RmsNorm::new(
                cfg.hidden_size,
                cfg.rms_norm_eps,
                vb.pp("input_layernorm"),
            )?,
            post_attention_layernorm: RmsNorm::new(
                cfg.hidden_size,
                cfg.rms_norm_eps,
                vb.pp("post_attention_layernorm"),
            )?,
        })
    }

    fn forward(&self, xs: &Tensor, attention_mask: Option<&Tensor>) -> CandleResult<Tensor> {
        let h = xs.apply(&self.input_layernorm)?;
        let xs = (xs + self.self_attn.forward(&h, attention_mask)?)?;
        let h = xs.apply(&self.post_attention_layernorm)?;
        xs + self.mlp.forward(&h)?
    }
}

#[derive(Debug, Clone)]
struct QwenEmbedModel {
    embed_tokens: candle_nn::Embedding,
    layers: Vec<DecoderLayer>,
    norm: RmsNorm,
    sliding_window: usize,
    device: Device,
    dtype: DType,
}

impl QwenEmbedModel {
    fn new(
        config: &str,
        model_info: &ModelInfo,
        dtype: DType,
        device: &Device,
    ) -> Result<Self, Box<dyn Error>> {
        let raw_cfg: QwenConfigRaw =
            serde_json::from_str(config).map_err(|_| LibError::ModelConfigParseFailed)?;
        let header = (!model_info.use_pth)
            .then(|| read_safetensors_header(&model_info.weights_path))
            .flatten();
        let head_dim_override = header.as_ref().and_then(|v| {
            tensor_dim0(
                v,
                &[
                    "layers.0.self_attn.q_proj.weight",
                    "model.layers.0.self_attn.q_proj.weight",
                ],
            )
            .and_then(|out| {
                (out > 0 && out % raw_cfg.num_attention_heads == 0)
                    .then(|| out / raw_cfg.num_attention_heads)
            })
        });
        let cfg = raw_cfg.normalize(head_dim_override);

        let vb = if model_info.use_pth {
            VarBuilder::from_pth(&model_info.weights_path, dtype, device)
                .map_err(|_| LibError::ModelWeightsLoadFailed)?
        } else {
            unsafe {
                VarBuilder::from_mmaped_safetensors(&[&model_info.weights_path], dtype, device)
                    .map_err(|_| LibError::ModelWeightsLoadFailed)?
            }
        };
        let vb_m = if header.as_ref().is_none_or(has_model_prefix) {
            vb.pp("model")
        } else {
            vb
        };
        let embed_tokens =
            candle_nn::embedding(cfg.vocab_size, cfg.hidden_size, vb_m.pp("embed_tokens"))?;
        let rotary_emb = Arc::new(RotaryEmbedding::new(vb_m.dtype(), &cfg, vb_m.device())?);
        let vb_l = vb_m.pp("layers");
        let layers = (0..cfg.num_hidden_layers)
            .map(|i| DecoderLayer::new(rotary_emb.clone(), &cfg, vb_l.pp(i)))
            .collect::<CandleResult<Vec<_>>>()?;
        let norm = RmsNorm::new(cfg.hidden_size, cfg.rms_norm_eps, vb_m.pp("norm"))?;
        Ok(Self {
            embed_tokens,
            layers,
            norm,
            sliding_window: cfg.sliding_window.unwrap_or(cfg.max_position_embeddings),
            device: device.clone(),
            dtype,
        })
    }

    fn prepare_decoder_attention_mask(
        &self,
        b_size: usize,
        tgt_len: usize,
    ) -> CandleResult<Tensor> {
        let mask: Vec<_> = (0..tgt_len)
            .flat_map(|i| {
                (0..tgt_len).map(move |j| {
                    if i < j || j + self.sliding_window < i {
                        f32::NEG_INFINITY
                    } else {
                        0.
                    }
                })
            })
            .collect();
        let mask = Tensor::from_slice(&mask, (tgt_len, tgt_len), &self.device)?;
        mask.expand((b_size, 1, tgt_len, tgt_len))?
            .to_dtype(self.dtype)
    }

    fn forward(&self, input_ids: &Tensor) -> CandleResult<Tensor> {
        let (b_size, seq_len) = input_ids.dims2()?;
        let attention_mask = if seq_len <= 1 {
            None
        } else {
            let mask = self.prepare_decoder_attention_mask(b_size, seq_len)?;
            Some(mask)
        };
        let mut xs = self.embed_tokens.forward(input_ids)?;
        for layer in self.layers.iter() {
            xs = layer.forward(&xs, attention_mask.as_ref())?
        }
        xs.apply(&self.norm)
    }

    fn device(&self) -> &Device {
        &self.device
    }
}

fn read_safetensors_header(path: &Path) -> Option<Value> {
    let mut file = std::fs::File::open(path).ok()?;
    let mut len_buf = [0u8; 8];
    file.read_exact(&mut len_buf).ok()?;
    let header_len = u64::from_le_bytes(len_buf) as usize;
    file.seek(SeekFrom::Start(8)).ok()?;
    let mut header = vec![0u8; header_len];
    file.read_exact(&mut header).ok()?;
    serde_json::from_slice(&header).ok()
}

fn has_model_prefix(value: &Value) -> bool {
    value.get("model.embed_tokens.weight").is_some()
        || value
            .as_object()
            .is_some_and(|o| o.keys().any(|k| k.starts_with("model.")))
}

fn tensor_dim0(value: &Value, keys: &[&str]) -> Option<usize> {
    keys.iter().find_map(|key| {
        value
            .get(*key)
            .and_then(|v| v.get("shape"))
            .and_then(|s| s.get(0))
            .and_then(Value::as_u64)
            .map(|d| d as usize)
    })
}

#[cfg(test)]
mod tests {
    use super::is_qwen_config;

    #[test]
    fn test_is_qwen_config() {
        let qwen = r#"{"model_type":"qwen3"}"#;
        assert!(is_qwen_config(qwen));

        let non_qwen = r#"{"model_type":"bert"}"#;
        assert!(!is_qwen_config(non_qwen));
    }
}
