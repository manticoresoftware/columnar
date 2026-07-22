// Auto-generated file. Do not edit.

#ifndef MANTICORESEARCH_TEXT_EMBEDDINGS_H
#define MANTICORESEARCH_TEXT_EMBEDDINGS_H

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

/// Strategy, mirrored as a `u32` across the FFI in [`ChunkSettings`].
constexpr static const uint32_t STRATEGY_TRUNCATE = 0;

constexpr static const uint32_t STRATEGY_MEAN = 1;

constexpr static const uint32_t STRATEGY_FIXED = 2;

constexpr static const uint32_t STRATEGY_RECURSIVE = 3;

constexpr static const uint32_t STRATEGY_SENTENCE = 4;

struct TextModelResult {
  void *m_pModel;
  char *m_szError;
};

using LoadModelFn = TextModelResult(*)(const char*,
                                       uintptr_t,
                                       const char*,
                                       uintptr_t,
                                       const char*,
                                       uintptr_t,
                                       const char*,
                                       uintptr_t,
                                       int32_t,
                                       bool);

using FreeModelResultFn = void(*)(TextModelResult);

struct FloatVec {
  const float *ptr;
  uintptr_t len;
  uintptr_t cap;
};

/// Embedding result for one `make_vect_embeddings` call.
///
/// `m_tEmbedding` is a FLAT array of `len` vectors — every input document's
/// vectors concatenated. `m_pRowOffsets` (length `rows + 1`) groups them per
/// input document, Arrow-style: document `i` owns
/// `m_tEmbedding[m_pRowOffsets[i] .. m_pRowOffsets[i + 1]]`. For the v1
/// strategies (truncate / mean) every document yields exactly one vector, so
/// `len == rows` and the offsets are `[0, 1, ..., rows]`. The sidecar lets a
/// future multi-vector strategy return N vectors per document through this same
/// struct — no second method, cardinality carried as data.
///
struct FloatVecResult {
  char *m_szError;
  const FloatVec *m_tEmbedding;
  uintptr_t len;
  uintptr_t cap;
  const uintptr_t *m_pRowOffsets;
  uintptr_t rows;
  uintptr_t offsets_cap;
};

using TextModelWrapper = void*;

struct StringItem {
  const char *ptr;
  uintptr_t len;
};

/// Chunking parameters. `#[repr(C)]` — passed straight across the FFI by the
/// daemon, which owns the DDL surface and validates against the model.
struct ChunkSettings {
  /// One of the `STRATEGY_*` constants.
  uint32_t strategy;
  /// Target chunk size in tokens. `0` ⇒ use the model's max. Always clamped to
  /// the model's real input limit.
  uint32_t max_tokens;
  /// Token overlap between consecutive chunks. `0` ⇒ none.
  uint32_t overlap_tokens;
  /// Hard cap on chunks per document. `0` ⇒ unlimited. Overflow merges the
  /// tail into the last chunk (matches OpenSearch's `max_chunk_limit`).
  uint32_t max_chunks;
};

using MakeVectEmbeddingsFn = FloatVecResult(*)(const TextModelWrapper*,
                                               const StringItem*,
                                               uintptr_t,
                                               const ChunkSettings*,
                                               int32_t);

using FreeVecResultFn = void(*)(FloatVecResult);

using GetLenFn = uintptr_t(*)(const TextModelWrapper*);

using ValidateApiKeyFn = char*(*)(const TextModelWrapper*);

/// Function pointer type for freeing strings returned by validate_api_key().
///
/// Required for proper memory management in FFI: validate_api_key() returns a Rust-allocated
/// CString via CString::into_raw(). The C++ caller receives ownership and must call free_string()
/// to deallocate the memory, otherwise it will leak. This follows the standard Rust FFI pattern
/// for returning owned strings to C/C++.
using FreeStringFn = void(*)(char*);

struct EmbedLib {
  uintptr_t version;
  const char *version_str;
  LoadModelFn load_model;
  FreeModelResultFn free_model_result;
  MakeVectEmbeddingsFn make_vect_embeddings;
  FreeVecResultFn free_vec_result;
  GetLenFn get_hidden_size;
  GetLenFn get_max_input_size;
  ValidateApiKeyFn validate_api_key;
  FreeStringFn free_string;
};

extern "C" {

const EmbedLib *GetLibFuncs();

} // extern "C"

#endif // MANTICORESEARCH_TEXT_EMBEDDINGS_H
