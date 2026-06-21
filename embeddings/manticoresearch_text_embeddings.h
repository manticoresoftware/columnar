// Auto-generated file. Do not edit.

#ifndef MANTICORESEARCH_TEXT_EMBEDDINGS_H
#define MANTICORESEARCH_TEXT_EMBEDDINGS_H

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

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

struct FloatVecResult {
  char *m_szError;
  const FloatVec *m_tEmbedding;
  uintptr_t len;
  uintptr_t cap;
};

using TextModelWrapper = void*;

struct StringItem {
  const char *ptr;
  uintptr_t len;
};

using MakeVectEmbeddingsFn = FloatVecResult(*)(const TextModelWrapper*,
                                               const StringItem*,
                                               uintptr_t,
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

struct ChunkSettings {
  uint32_t strategy;
  uint32_t max_tokens;
  uint32_t overlap_tokens;
  uint32_t max_chunks;
};

struct ChunkSpan {
  uintptr_t start;
  uintptr_t end;
};

struct DocChunks {
  uintptr_t first;
  uintptr_t count;
};

struct ChunkedVecResult {
  char *m_szError;
  const FloatVec *m_tEmbedding;
  uintptr_t emb_len;
  uintptr_t emb_cap;
  const ChunkSpan *m_tSpans;
  uintptr_t spans_cap;
  const DocChunks *m_tDocs;
  uintptr_t docs_len;
  uintptr_t docs_cap;
};

using MakeVectEmbeddingsChunkedFn = ChunkedVecResult(*)(const TextModelWrapper*,
                                                        const StringItem*,
                                                        uintptr_t,
                                                        const ChunkSettings*,
                                                        int32_t);

using FreeChunkedResultFn = void(*)(ChunkedVecResult);

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
  MakeVectEmbeddingsChunkedFn make_vect_embeddings_chunked;
  FreeChunkedResultFn free_chunked_result;
};

extern "C" {

const EmbedLib *GetLibFuncs();

} // extern "C"

#endif // MANTICORESEARCH_TEXT_EMBEDDINGS_H
