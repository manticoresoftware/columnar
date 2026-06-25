//! Document chunking + embedding strategies.
//!
//! One document maps to one or many vectors depending on the strategy:
//! - `truncate` / `mean` collapse to **one** vector per document.
//! - `fixed` / `recursive` / `sentence` keep **N** vectors per document (one per
//!   chunk) — the daemon groups them via the row-offsets sidecar.
//!
//! Local models chunk token-accurately via their loaded tokenizer; remote API
//! models (no local tokenizer) fall back to a conservative char/byte heuristic.
//! `ChunkSettings` arrives from the daemon's DDL surface and selects everything.

use crate::utils::normalize;
use tokenizers::Tokenizer;

/// Strategy, mirrored as a `u32` across the FFI in [`ChunkSettings`].
pub const STRATEGY_TRUNCATE: u32 = 0; // 1 vector/doc: first `max_tokens` tokens
pub const STRATEGY_MEAN: u32 = 1; // 1 vector/doc: chunk → embed → average
pub const STRATEGY_FIXED: u32 = 2; // N vectors/doc: fixed token windows
pub const STRATEGY_RECURSIVE: u32 = 3; // N vectors/doc: recursive token-aware split
pub const STRATEGY_SENTENCE: u32 = 4; // N vectors/doc: UAX-29 sentence segmentation

/// Special tokens (`[CLS]`/`[SEP]`) that `predict()` re-adds when it re-tokenizes
/// a chunk string. We size chunks to leave room so a chunk is never truncated.
const SPECIAL_TOKENS_MARGIN: usize = 2;

/// Conservative bytes-per-token for the remote heuristic. Deliberately low so a
/// heuristic chunk lands *under* the provider's token cap rather than over it.
const REMOTE_BYTES_PER_TOKEN: usize = 3;

/// Chunking parameters. `#[repr(C)]` — passed straight across the FFI by the
/// daemon, which owns the DDL surface and validates against the model.
#[repr(C)]
pub struct ChunkSettings {
    /// One of the `STRATEGY_*` constants.
    pub strategy: u32,
    /// Target chunk size in tokens. `0` ⇒ use the model's max. Always clamped to
    /// the model's real input limit.
    pub max_tokens: u32,
    /// Token overlap between consecutive chunks. `0` ⇒ none.
    pub overlap_tokens: u32,
    /// Hard cap on chunks per document. `0` ⇒ unlimited. Overflow merges the
    /// tail into the last chunk (matches OpenSearch's `max_chunk_limit`).
    pub max_chunks: u32,
}

impl ChunkSettings {
    /// True when the document must be split (every strategy except `truncate`).
    pub fn needs_chunking(&self) -> bool {
        self.strategy != STRATEGY_TRUNCATE
    }

    /// True when the strategy collapses to one vector per document
    /// (`truncate`/`mean`); false for the multi-vector strategies.
    pub fn is_single_vector(&self) -> bool {
        self.strategy == STRATEGY_TRUNCATE || self.strategy == STRATEGY_MEAN
    }
}

/// Resolve the chunk size in *content* tokens, clamped to what the model can
/// actually take (minus the special-token budget). `0` ⇒ model max.
pub fn effective_max(settings: &ChunkSettings, model_max: usize) -> usize {
    let ceiling = model_max.saturating_sub(SPECIAL_TOKENS_MARGIN).max(1);
    let target = settings.max_tokens as usize;
    if target == 0 {
        ceiling
    } else {
        target.min(ceiling)
    }
}

/// Average a document's chunk embeddings into one L2-normalized vector. Chunk
/// vectors are already normalized by `predict`; the mean of unit vectors is
/// re-normalized so the result is a unit vector too.
pub fn mean_pool(chunks: &[Vec<f32>]) -> Vec<f32> {
    let dim = chunks.first().map(Vec::len).unwrap_or(0);
    let mut acc = vec![0.0f32; dim];
    for v in chunks {
        for (a, x) in acc.iter_mut().zip(v.iter()) {
            *a += *x;
        }
    }
    let n = chunks.len().max(1) as f32;
    for a in acc.iter_mut() {
        *a /= n;
    }
    normalize(&mut acc);
    acc
}

/// Arrow-style per-row offsets (length `counts.len() + 1`) from per-document
/// vector counts: document `i` owns vectors `[offsets[i] .. offsets[i + 1]]`.
pub fn row_offsets_from_counts(counts: &[usize]) -> Vec<usize> {
    let mut offsets = Vec::with_capacity(counts.len() + 1);
    let mut acc = 0usize;
    offsets.push(0);
    for &c in counts {
        acc += c;
        offsets.push(acc);
    }
    offsets
}

/// Split one document into chunk byte spans `(start, end)` according to the
/// strategy's split method. `tokenizer` is `Some` for local models (token-exact)
/// and `None` for remote API models (char heuristic). `STRATEGY_TRUNCATE` never
/// reaches here (it does not chunk); `STRATEGY_MEAN` splits recursively.
pub fn chunk_text(
    text: &str,
    max_tokens: usize,
    overlap: usize,
    strategy: u32,
    tokenizer: Option<&Tokenizer>,
) -> Vec<(usize, usize)> {
    match strategy {
        STRATEGY_SENTENCE => chunk_sentence(text, max_tokens, overlap, tokenizer),
        STRATEGY_FIXED => window(text, max_tokens, overlap, false, tokenizer),
        // recursive (3), mean's internal split (1), and any other → recursive.
        _ => window(text, max_tokens, overlap, true, tokenizer),
    }
}

/// Token-window split, picking the token-accurate or heuristic backend.
fn window(
    text: &str,
    max_tokens: usize,
    overlap: usize,
    snap: bool,
    tokenizer: Option<&Tokenizer>,
) -> Vec<(usize, usize)> {
    match tokenizer {
        Some(t) => chunk_local(text, t, max_tokens, overlap, snap),
        None => chunk_chars(text, max_tokens, overlap),
    }
}

/// Token-accurate chunking for local models. One tokenization pass; windows by
/// token count; `recursive` snaps each cut back to a natural boundary; overlap
/// is realigned to the (possibly snapped) cut so no tokens are dropped.
///
/// NOTE: assumes `tokenizer.encode(..).get_offsets()` returns byte offsets into
/// the input (true for the WordPiece/BPE paths Manticore loads).
pub fn chunk_local(
    text: &str,
    tokenizer: &Tokenizer,
    max_tokens: usize,
    overlap: usize,
    recursive: bool,
) -> Vec<(usize, usize)> {
    if text.is_empty() {
        return vec![(0, 0)];
    }
    // `false` = no special tokens, so offsets map cleanly onto the input text.
    let encoding = match tokenizer.encode(text, false) {
        Ok(e) => e,
        // Tokenizer failure (rare): fall back to the whole doc as one chunk.
        // predict() truncates it if needed — degrades to truncate, but is safe.
        Err(_) => return vec![(0, text.len())],
    };
    let offsets = encoding.get_offsets();
    let n = offsets.len();
    if max_tokens == 0 || n <= max_tokens {
        return vec![(0, text.len())];
    }

    let overlap = overlap.min(max_tokens / 2);
    let mut chunks = Vec::new();
    let mut start = 0usize; // start token index
    loop {
        let win_end = (start + max_tokens).min(n); // exclusive token index
        let byte_start = offsets[start].0;
        let mut byte_end = offsets[win_end - 1].1;

        if recursive && win_end < n {
            if let Some(cut) = snap_back(text, byte_start, byte_end) {
                byte_end = cut;
            }
        }
        chunks.push((byte_start, byte_end));

        // First token that begins at/after the (possibly snapped) cut.
        let cut_tok = offsets.partition_point(|o| o.0 < byte_end);
        if cut_tok >= n {
            break;
        }
        start = cut_tok.saturating_sub(overlap).max(start + 1); // step back overlap, always progress
    }
    chunks
}

/// Char/byte heuristic for remote API models (no local tokenizer). Windows by an
/// estimated byte budget, snapping to whitespace and never cutting mid-UTF-8.
pub fn chunk_chars(text: &str, max_tokens: usize, overlap_tokens: usize) -> Vec<(usize, usize)> {
    if text.is_empty() {
        return vec![(0, 0)];
    }
    let window = (max_tokens * REMOTE_BYTES_PER_TOKEN).max(1);
    if text.len() <= window {
        return vec![(0, text.len())];
    }
    let overlap = (overlap_tokens * REMOTE_BYTES_PER_TOKEN).min(window / 2);

    let mut chunks = Vec::new();
    let mut start = 0usize;
    while start < text.len() {
        let mut end = text.floor_char_boundary((start + window).min(text.len()));
        if end <= start {
            // A single char wider than the window — take at least one char.
            end = text.ceil_char_boundary(start + 1).min(text.len());
        }
        if end < text.len() {
            if let Some(cut) = snap_back(text, start, end) {
                end = cut;
            }
        }
        chunks.push((start, end));
        if end >= text.len() {
            break;
        }
        let mut next = end.saturating_sub(overlap);
        if next <= start {
            next = end;
        }
        start = text.ceil_char_boundary(next);
    }
    chunks
}

/// Count tokens in `text`: exact via the model tokenizer when available, else
/// the conservative byte estimate the remote heuristic uses.
fn count_tokens(text: &str, tokenizer: Option<&Tokenizer>) -> usize {
    match tokenizer {
        Some(t) => t
            .encode(text, false)
            .map(|e| e.get_ids().len())
            .unwrap_or(text.len() / REMOTE_BYTES_PER_TOKEN + 1),
        None => text.len() / REMOTE_BYTES_PER_TOKEN + 1,
    }
}

/// UAX-29 sentence boundaries — the same standard Elasticsearch's ICU uses —
/// as contiguous byte spans tiling the whole document.
fn sentence_spans(text: &str) -> Vec<(usize, usize)> {
    use unicode_segmentation::UnicodeSegmentation;
    text.split_sentence_bound_indices()
        .map(|(offset, s)| (offset, offset + s.len()))
        .collect()
}

/// Sentence chunking, mirroring Elasticsearch's algorithm: detect sentences
/// (UAX-29), then greedily group whole sentences up to `max_tokens`. A single
/// sentence larger than the budget is split with the token-window splitter (ES
/// splits such a sentence across chunks). `overlap` re-seeds the next chunk with
/// trailing whole sentences summing ≤ `overlap` tokens.
pub fn chunk_sentence(
    text: &str,
    max_tokens: usize,
    overlap: usize,
    tokenizer: Option<&Tokenizer>,
) -> Vec<(usize, usize)> {
    if text.is_empty() {
        return vec![(0, 0)];
    }
    let units: Vec<(usize, usize, usize)> = sentence_spans(text)
        .into_iter()
        .map(|(a, b)| (a, b, count_tokens(&text[a..b], tokenizer)))
        .collect();
    // 0/1 sentence (e.g. terminator-free text): nothing to group on — fall back
    // to the token-window splitter so an over-long blob still gets divided.
    if units.len() <= 1 {
        return window(text, max_tokens, overlap, true, tokenizer);
    }

    let mut chunks = Vec::new();
    let mut i = 0usize;
    while i < units.len() {
        // Greedily pack whole sentences up to the budget (always take ≥ 1).
        let mut j = i;
        let mut sum = 0usize;
        while j < units.len() {
            let t = units[j].2;
            if j > i && sum + t > max_tokens {
                break;
            }
            sum += t;
            j += 1;
        }

        let start = units[i].0;
        let end = units[j - 1].1;
        if j == i + 1 && units[i].2 > max_tokens {
            // One sentence bigger than the budget — split it like ES does.
            for (a, b) in window(&text[start..end], max_tokens, overlap, true, tokenizer) {
                chunks.push((start + a, start + b));
            }
        } else {
            chunks.push((start, end));
        }

        if j >= units.len() {
            break;
        }

        // Overlap: re-seed with trailing whole sentences summing ≤ overlap,
        // never back to i (guarantee forward progress).
        let mut next = j;
        if overlap > 0 {
            let mut acc = 0usize;
            let mut k = j;
            while k > i + 1 {
                let t = units[k - 1].2;
                if acc + t > overlap {
                    break;
                }
                acc += t;
                k -= 1;
            }
            next = k.max(i + 1);
        }
        i = next;
    }
    chunks
}

/// Enforce `max_chunks`: if exceeded, merge the overflow tail into the last kept
/// chunk so no document text is dropped. `0` ⇒ unlimited.
pub fn cap_chunks(mut chunks: Vec<(usize, usize)>, max_chunks: usize) -> Vec<(usize, usize)> {
    if max_chunks == 0 || chunks.len() <= max_chunks {
        return chunks;
    }
    let last_end = chunks.last().map(|c| c.1).unwrap_or(0);
    chunks.truncate(max_chunks);
    if let Some(last) = chunks.last_mut() {
        last.1 = last_end; // extend to cover everything merged in
    }
    chunks
}

/// Pull a chunk's end back to the nearest paragraph/line/sentence/space boundary
/// within `[start, end)`, but never past the chunk midpoint (don't over-shrink).
/// Returns the new end byte offset, or `None` if no good boundary was found.
fn snap_back(text: &str, start: usize, end: usize) -> Option<usize> {
    let floor = start + (end - start) / 2;
    for sep in ["\n\n", "\n", ". ", " "] {
        if let Some(pos) = text[start..end].rfind(sep) {
            let cut = start + pos + sep.len();
            if cut > floor && cut < end {
                return Some(cut);
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings(strategy: u32, max: u32, overlap: u32, max_chunks: u32) -> ChunkSettings {
        ChunkSettings {
            strategy,
            max_tokens: max,
            overlap_tokens: overlap,
            max_chunks,
        }
    }

    #[test]
    fn strategy_classification() {
        assert!(!settings(STRATEGY_TRUNCATE, 0, 0, 0).needs_chunking());
        assert!(settings(STRATEGY_MEAN, 0, 0, 0).needs_chunking());
        assert!(settings(STRATEGY_SENTENCE, 0, 0, 0).needs_chunking());
        assert!(settings(STRATEGY_TRUNCATE, 0, 0, 0).is_single_vector());
        assert!(settings(STRATEGY_MEAN, 0, 0, 0).is_single_vector());
        assert!(!settings(STRATEGY_FIXED, 0, 0, 0).is_single_vector());
        assert!(!settings(STRATEGY_SENTENCE, 0, 0, 0).is_single_vector());
    }

    #[test]
    fn effective_max_defaults_clamps_and_floors() {
        assert_eq!(effective_max(&settings(STRATEGY_MEAN, 0, 0, 0), 512), 510);
        assert_eq!(
            effective_max(&settings(STRATEGY_MEAN, 100_000, 0, 0), 512),
            510
        );
        assert_eq!(
            effective_max(&settings(STRATEGY_MEAN, 256, 0, 0), 8192),
            256
        );
        assert!(effective_max(&settings(STRATEGY_MEAN, 0, 0, 0), 0) >= 1);
    }

    #[test]
    fn mean_pool_averages_then_normalizes() {
        let m = mean_pool(&[vec![1.0, 0.0], vec![0.0, 1.0]]);
        let e = std::f32::consts::FRAC_1_SQRT_2;
        assert!((m[0] - e).abs() < 1e-6 && (m[1] - e).abs() < 1e-6);
    }

    #[test]
    fn mean_pool_single_chunk_is_normalized() {
        let m = mean_pool(&[vec![3.0, 4.0]]);
        assert!((m[0] - 0.6).abs() < 1e-6 && (m[1] - 0.8).abs() < 1e-6);
    }

    #[test]
    fn row_offsets_prefix_sum() {
        assert_eq!(row_offsets_from_counts(&[3, 1, 2]), vec![0, 3, 4, 6]);
        assert_eq!(row_offsets_from_counts(&[1, 1, 1]), vec![0, 1, 2, 3]);
        assert_eq!(row_offsets_from_counts(&[]), vec![0]);
    }

    #[test]
    fn short_text_is_one_chunk() {
        assert_eq!(chunk_chars("hello world", 100, 0), vec![(0, 11)]);
    }

    #[test]
    fn chunk_chars_tiles_document_with_no_overlap() {
        let text = "word ".repeat(2000);
        let chunks = chunk_chars(&text, 100, 0);
        assert!(chunks.len() > 1);
        assert_eq!(chunks.first().unwrap().0, 0);
        assert_eq!(chunks.last().unwrap().1, text.len());
        for w in chunks.windows(2) {
            assert_eq!(w[0].1, w[1].0, "no gap/overlap when overlap=0");
        }
        for (a, b) in &chunks {
            assert!(b > a && text.is_char_boundary(*a) && text.is_char_boundary(*b));
        }
    }

    #[test]
    fn chunk_chars_overlap_steps_back() {
        let text = "word ".repeat(2000);
        let chunks = chunk_chars(&text, 100, 20);
        assert!(chunks.len() > 1);
        for w in chunks.windows(2) {
            assert!(w[1].0 < w[0].1, "overlap: next starts before prev end");
        }
        assert_eq!(chunks.last().unwrap().1, text.len());
    }

    #[test]
    fn chunk_chars_multibyte_never_splits_a_char() {
        let text = "🦀 данные ".repeat(500);
        let chunks = chunk_chars(&text, 50, 5);
        for (a, b) in &chunks {
            assert!(text.is_char_boundary(*a) && text.is_char_boundary(*b));
        }
        assert_eq!(chunks.last().unwrap().1, text.len());
    }

    #[test]
    fn chunk_sentence_splits_at_uax29_boundary() {
        let text = "Aaa bbb ccc. Ddd eee fff.";
        let chunks = chunk_sentence(text, 5, 0, None);
        assert_eq!(chunks.len(), 2);
        assert_eq!(chunks[0].0, 0);
        assert_eq!(chunks[1].1, text.len());
        assert_eq!(chunks[0].1, chunks[1].0);
        assert!((12..=13).contains(&chunks[0].1));
    }

    #[test]
    fn chunk_sentence_packs_and_splits_oversized() {
        let text = "S one. S two. S three. S four. S five. S six.";
        assert_eq!(chunk_sentence(text, 100, 0, None).len(), 1);
        assert!(chunk_sentence(text, 3, 0, None).len() > 1);
        // terminator-free over-long "sentence" still splits
        let blob = "word ".repeat(1000);
        assert!(chunk_sentence(&blob, 100, 0, None).len() > 1);
    }

    #[test]
    fn chunk_text_routes_every_multivector_strategy() {
        let text = "word ".repeat(400);
        for s in [
            STRATEGY_FIXED,
            STRATEGY_RECURSIVE,
            STRATEGY_SENTENCE,
            STRATEGY_MEAN,
        ] {
            let chunks = chunk_text(&text, 50, 0, s, None);
            assert!(chunks.len() > 1, "strategy {s} should split");
            assert_eq!(chunks.first().unwrap().0, 0);
            assert_eq!(chunks.last().unwrap().1, text.len());
        }
    }

    #[test]
    fn cap_chunks_merges_tail_and_noops() {
        assert_eq!(
            cap_chunks(vec![(0, 10), (10, 20), (20, 30), (30, 45)], 2),
            vec![(0, 10), (10, 45)]
        );
        let two = vec![(0, 10), (10, 20)];
        assert_eq!(cap_chunks(two.clone(), 5), two);
        assert_eq!(cap_chunks(two.clone(), 0), two);
    }
}
