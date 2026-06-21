//! Document chunking: split one input document into multiple embedding-sized
//! pieces, returning byte spans `(start, end)` into the original text.
//!
//! Local models chunk token-accurately via their loaded tokenizer; remote API
//! models (no local tokenizer) fall back to a conservative char/byte heuristic.
//! Chunk *settings* originate at the daemon's SQL/DDL surface and arrive here as
//! [`ChunkSettings`]; this module owns only the actual splitting.

use tokenizers::Tokenizer;

/// Chunking strategy, mirrored as a `u32` across the FFI in [`ChunkSettings`].
pub const STRATEGY_NONE: u32 = 0;
pub const STRATEGY_FIXED: u32 = 1;
pub const STRATEGY_RECURSIVE: u32 = 2;
pub const STRATEGY_SENTENCE: u32 = 3;

/// Special tokens (`[CLS]`/`[SEP]`) that `predict()` re-adds when it re-tokenizes
/// a chunk string. We size chunks to leave room for them so a chunk never gets
/// silently truncated at inference time.
const SPECIAL_TOKENS_MARGIN: usize = 2;

/// Conservative bytes-per-token for the remote heuristic. Deliberately low so a
/// heuristic chunk lands *under* the provider's token cap rather than over it.
const REMOTE_BYTES_PER_TOKEN: usize = 3;

/// Chunking parameters. `#[repr(C)]` — passed straight across the FFI by the
/// daemon, which owns the DDL surface and validates against the model.
#[repr(C)]
pub struct ChunkSettings {
    /// One of the `STRATEGY_*` constants. `STRATEGY_NONE` ⇒ no chunking.
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
    /// True when chunking should actually run for this request.
    pub fn enabled(&self) -> bool {
        self.strategy != STRATEGY_NONE
    }
}

/// Resolve the target chunk size in *content* tokens, clamped to what the model
/// can actually take (minus the special-token budget).
pub fn effective_max(settings: &ChunkSettings, model_max: usize) -> usize {
    let ceiling = model_max.saturating_sub(SPECIAL_TOKENS_MARGIN).max(1);
    let target = settings.max_tokens as usize;
    if target == 0 {
        ceiling
    } else {
        target.min(ceiling)
    }
}

/// Token-accurate chunking for local models. One tokenization pass; windows by
/// token count; `recursive` snaps each cut back to a natural boundary; overlap
/// is realigned to the (possibly snapped) cut so no tokens are dropped.
///
/// NOTE: assumes `tokenizer.encode(..).get_offsets()` returns byte offsets into
/// the input (true for the WordPiece/BPE paths Manticore loads). A normalizer
/// that rewrites text could make offsets approximate — acceptable for chunk
/// boundaries, but verify per model if exactness ever matters.
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

/// Strategy dispatcher used by every model backend. `tokenizer` is `Some` for
/// local models (token-accurate) and `None` for remote API models (char
/// heuristic). Remote models still honor `strategy` here.
pub fn chunk_text(
    text: &str,
    max_tokens: usize,
    overlap: usize,
    strategy: u32,
    tokenizer: Option<&Tokenizer>,
) -> Vec<(usize, usize)> {
    match strategy {
        STRATEGY_NONE => vec![(0, text.len())],
        STRATEGY_SENTENCE => chunk_sentence(text, max_tokens, overlap, tokenizer),
        STRATEGY_RECURSIVE => window(text, max_tokens, overlap, true, tokenizer),
        // STRATEGY_FIXED and any unknown value → plain token windows.
        _ => window(text, max_tokens, overlap, false, tokenizer),
    }
}

/// Token-window split, picking the token-accurate or heuristic backend.
/// (The remote heuristic always snaps to whitespace, so `snap` only affects
/// local models — fixed vs recursive is a no-op distinction on remote.)
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

/// UAX #29 sentence boundaries — the same standard Elasticsearch's ICU uses —
/// as contiguous byte spans tiling the whole document.
fn sentence_spans(text: &str) -> Vec<(usize, usize)> {
    use unicode_segmentation::UnicodeSegmentation;
    text.split_sentence_bound_indices()
        .map(|(offset, s)| (offset, offset + s.len()))
        .collect()
}

/// Sentence chunking, mirroring Elasticsearch's algorithm: detect sentences
/// (UAX #29), then greedily group whole sentences up to `max_tokens`. A single
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
    fn effective_max_defaults_to_model_minus_margin() {
        let s = settings(STRATEGY_RECURSIVE, 0, 0, 0);
        assert_eq!(effective_max(&s, 512), 510);
    }

    #[test]
    fn effective_max_clamps_target_to_model() {
        let s = settings(STRATEGY_RECURSIVE, 100_000, 0, 0);
        assert_eq!(effective_max(&s, 512), 510);
    }

    #[test]
    fn effective_max_honors_smaller_target() {
        let s = settings(STRATEGY_RECURSIVE, 256, 0, 0);
        assert_eq!(effective_max(&s, 8192), 256);
    }

    #[test]
    fn short_text_is_one_chunk() {
        assert_eq!(chunk_chars("hello world", 100, 0), vec![(0, 11)]);
    }

    #[test]
    fn chunk_chars_tiles_document_with_no_overlap() {
        let text = "word ".repeat(2000); // 10000 ASCII bytes
        let chunks = chunk_chars(&text, 100, 0);
        assert!(chunks.len() > 1);
        assert_eq!(chunks.first().unwrap().0, 0);
        assert_eq!(chunks.last().unwrap().1, text.len());
        for w in chunks.windows(2) {
            assert_eq!(w[0].1, w[1].0, "no gap/overlap when overlap=0");
        }
        for (a, b) in &chunks {
            assert!(b > a, "non-empty span");
            assert!(text.is_char_boundary(*a) && text.is_char_boundary(*b));
        }
    }

    #[test]
    fn chunk_chars_overlap_steps_back() {
        let text = "word ".repeat(2000);
        let chunks = chunk_chars(&text, 100, 20);
        assert!(chunks.len() > 1);
        for w in chunks.windows(2) {
            assert!(
                w[1].0 < w[0].1,
                "next chunk starts before prev end (overlap)"
            );
        }
        assert_eq!(chunks.last().unwrap().1, text.len(), "still covers the doc");
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
    fn cap_chunks_merges_tail_into_last() {
        let chunks = vec![(0, 10), (10, 20), (20, 30), (30, 45)];
        assert_eq!(cap_chunks(chunks, 2), vec![(0, 10), (10, 45)]);
    }

    #[test]
    fn cap_chunks_noop_under_limit() {
        let chunks = vec![(0, 10), (10, 20)];
        assert_eq!(cap_chunks(chunks.clone(), 5), chunks);
        assert_eq!(cap_chunks(chunks.clone(), 0), chunks);
    }

    #[test]
    fn chunk_sentence_groups_whole_sentences_and_tiles() {
        let text = "One two three. Four five six. Seven eight nine. Ten eleven twelve.";
        // small budget + byte estimate forces multiple chunks (no tokenizer here)
        let chunks = chunk_sentence(text, 8, 0, None);
        assert!(chunks.len() > 1);
        assert_eq!(chunks.first().unwrap().0, 0);
        assert_eq!(chunks.last().unwrap().1, text.len());
        for w in chunks.windows(2) {
            assert_eq!(
                w[0].1, w[1].0,
                "sentence chunks tile with no gap when overlap=0"
            );
        }
    }

    #[test]
    fn chunk_sentence_splits_an_oversized_sentence() {
        // one terminator-free "sentence" longer than the budget must still split
        let text = "word ".repeat(1000);
        let chunks = chunk_sentence(&text, 100, 0, None);
        assert!(chunks.len() > 1);
        assert_eq!(chunks.last().unwrap().1, text.len());
    }

    #[test]
    fn chunk_sentence_splits_at_uax29_boundary() {
        // UAX-29 breaks after ". " before an uppercase letter; grouping must land there.
        let text = "Aaa bbb ccc. Ddd eee fff.";
        let chunks = chunk_sentence(text, 5, 0, None); // ~5 est tokens/sentence → 1 each
        assert_eq!(chunks.len(), 2);
        assert_eq!(chunks[0].0, 0);
        assert_eq!(chunks[1].1, text.len());
        assert_eq!(chunks[0].1, chunks[1].0, "tile");
        assert!(
            (12..=13).contains(&chunks[0].1),
            "split falls on the sentence boundary after '. '"
        );
    }

    #[test]
    fn chunk_sentence_packs_several_per_chunk() {
        let text = "S one. S two. S three. S four. S five. S six.";
        let few = chunk_sentence(text, 100, 0, None); // big budget → one chunk
        let many = chunk_sentence(text, 3, 0, None); // tiny budget → ~one sentence each
        assert_eq!(few.len(), 1);
        assert!(many.len() > few.len());
        assert_eq!(many.last().unwrap().1, text.len());
    }

    #[test]
    fn chunk_sentence_overlap_repeats_a_sentence() {
        let text = "One two. Three four. Five six. Seven eight.";
        let chunks = chunk_sentence(text, 10, 5, None); // 2 sentences/chunk, 1 overlapped
        assert!(chunks.len() > 1);
        for w in chunks.windows(2) {
            assert!(w[1].0 < w[0].1, "overlap: next starts before prev end");
            assert!(w[1].0 > w[0].0, "forward progress");
        }
        assert_eq!(chunks.last().unwrap().1, text.len());
    }

    #[test]
    fn chunk_chars_overlap_has_no_gaps() {
        let text = "alpha ".repeat(500);
        let chunks = chunk_chars(&text, 40, 8);
        assert_eq!(chunks.first().unwrap().0, 0);
        assert_eq!(chunks.last().unwrap().1, text.len());
        for w in chunks.windows(2) {
            assert!(w[1].0 <= w[0].1, "no gap between chunks");
            assert!(w[1].0 > w[0].0, "forward progress");
        }
    }

    #[test]
    fn chunk_text_none_is_whole_doc() {
        let text = "anything at all here";
        assert_eq!(
            chunk_text(text, 4, 0, STRATEGY_NONE, None),
            vec![(0, text.len())]
        );
    }

    #[test]
    fn chunk_text_every_strategy_covers_the_doc() {
        let text = "word ".repeat(400);
        for strat in [STRATEGY_FIXED, STRATEGY_RECURSIVE, STRATEGY_SENTENCE] {
            let chunks = chunk_text(&text, 50, 0, strat, None);
            assert!(chunks.len() > 1, "strategy {strat} should split");
            assert_eq!(chunks.first().unwrap().0, 0);
            assert_eq!(chunks.last().unwrap().1, text.len());
        }
    }

    #[test]
    fn effective_max_never_zero() {
        let s = settings(STRATEGY_FIXED, 0, 0, 0);
        assert!(effective_max(&s, 1) >= 1);
        assert!(effective_max(&s, 0) >= 1);
    }

    #[test]
    fn chunk_chars_spans_stay_in_bounds_multibyte() {
        let text = "🦀x ".repeat(300);
        for &max in &[10usize, 50, 200] {
            for &ov in &[0usize, 5] {
                let chunks = chunk_chars(&text, max, ov);
                for (a, b) in &chunks {
                    assert!(a <= b && *b <= text.len());
                    assert!(text.is_char_boundary(*a) && text.is_char_boundary(*b));
                }
                assert_eq!(chunks.last().unwrap().1, text.len());
            }
        }
    }
}
