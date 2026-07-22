# Embedding strategy FFI (embeddings lib → Manticore daemon)

Embeddings lib **v8**. The one embedding call, `make_vect_embeddings`, takes an
optional `ChunkSettings*` that selects how a document becomes one or many
vectors. Cardinality is carried as **data** in the return (a per-document
offsets sidecar), so a single method covers both 1-vector and N-vector
strategies — no second method.

## FFI

```c
constexpr uint32_t STRATEGY_TRUNCATE  = 0;  // 1 vector/doc
constexpr uint32_t STRATEGY_MEAN      = 1;  // 1 vector/doc
constexpr uint32_t STRATEGY_FIXED     = 2;  // N vectors/doc
constexpr uint32_t STRATEGY_RECURSIVE = 3;  // N vectors/doc
constexpr uint32_t STRATEGY_SENTENCE  = 4;  // N vectors/doc

struct ChunkSettings {
  uint32_t strategy;        // one of STRATEGY_*
  uint32_t max_tokens;      // chunk size in tokens; 0 = model max
  uint32_t overlap_tokens;  // token overlap between chunks; 0 = none
  uint32_t max_chunks;      // cap on chunks/doc; 0 = unlimited (overflow merges into last)
};

// settings == nullptr  ⇒  truncate (today's behavior)
FloatVecResult make_vect_embeddings(
    const TextModelWrapper*, const StringItem* texts, uintptr_t count,
    const ChunkSettings*, int32_t threads);
```

### Return: flat vectors + per-row offsets (cardinality is data)

```c
struct FloatVecResult {
  char            *m_szError;
  const FloatVec  *m_tEmbedding;   // FLAT: every document's vectors concatenated, `len` total
  uintptr_t        len;
  uintptr_t        cap;
  const uintptr_t *m_pRowOffsets;  // length rows+1; doc i = m_tEmbedding[off[i] .. off[i+1]]
  uintptr_t        rows;           // number of input documents
  uintptr_t        offsets_cap;
};
```

Read document `i`'s vectors as `m_tEmbedding[m_pRowOffsets[i] .. m_pRowOffsets[i+1]]`.
- `truncate`/`mean` → one vector/doc, so `len == rows == count` and offsets are
  `[0, 1, …, rows]` (you may just index `m_tEmbedding[i]`).
- `fixed`/`recursive`/`sentence` → N vectors/doc; `len` = total chunks, and the
  offsets group them per document.

Free with `free_vec_result` (it frees the offsets too). Load-time check:
`EmbedLib.version == 8`.

## Strategies

| strategy | val | output | what it does |
|---|---|---|---|
| **truncate** | 0 | 1 vec/doc | embed the first `max_tokens` tokens (rest dropped). `max_tokens`/`overlap`/`max_chunks` ignored. |
| **mean** | 1 | 1 vec/doc | split (recursive, token-aware) → embed every chunk → **average** into one L2-normalized vector. Whole document, no tail loss. |
| **fixed** | 2 | N vecs/doc | split into fixed `max_tokens`-token windows; one vector per chunk. |
| **recursive** | 3 | N vecs/doc | split on a separator hierarchy (paragraph → line → space) ≤ `max_tokens`; one vector per chunk. |
| **sentence** | 4 | N vecs/doc | UAX-29 sentence segmentation (ES-style), grouped to `max_tokens`; one vector per chunk. |

- `max_tokens = 0` → the model's own max input length.
- `overlap_tokens` → token overlap between chunks (multi-vector + mean).
- `max_chunks` → cap chunks/doc; overflow merges the tail into the last chunk.
- Local models chunk on the model's real subword tokens; remote API models
  (OpenAI/Voyage/Jina) chunk by a char/byte heuristic (no local tokenizer).

## Calling it

```cpp
// non-chunked field & queries: pass nullptr → truncate
pFuncs->make_vect_embeddings( &model, items.data(), items.size(), nullptr, iThreads );

// any strategy, taken from a table option:
ChunkSettings tCfg { STRATEGY_SENTENCE, /*max_tokens*/ 256, /*overlap*/ 0, /*max_chunks*/ 0 };
FloatVecResult tRes = pFuncs->make_vect_embeddings( &model, items.data(), items.size(), &tCfg, iThreads );
// doc i owns vectors tRes.m_tEmbedding[ tRes.m_pRowOffsets[i] .. tRes.m_pRowOffsets[i+1] ]
```

The daemon owns the SQL/DDL surface (e.g. a per-`float_vector`-field option),
parses it into a `ChunkSettings`, and passes it on every embed call. Queries are
short: pass `nullptr` so they are never chunked.

## Daemon side (next phase, not this lib)

The lib already returns N vectors/doc for `fixed`/`recursive`/`sentence` via the
offsets sidecar. To *use* them the daemon needs N-vectors-per-row storage +
max-over-chunks search; `truncate`/`mean` (1 vector/doc) work with today's
storage unchanged.
