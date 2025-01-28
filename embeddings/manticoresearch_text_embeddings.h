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

using MakeVectEmbeddingsFn = FloatVecResult(*)(const TextModelWrapper*, const StringItem*, uintptr_t);

using FreeVecResultFn = void(*)(FloatVecResult);

using GetLenFn = uintptr_t(*)(const TextModelWrapper*);

struct EmbedLib {
  uintptr_t version;
  LoadModelFn load_model;
  FreeModelResultFn free_model_result;
  MakeVectEmbeddingsFn make_vect_embeddings;
  FreeVecResultFn free_vec_result;
  GetLenFn get_hidden_size;
  GetLenFn get_max_input_size;
};

extern "C" {

const EmbedLib *GetLibFuncs();

} // extern "C"

#endif // MANTICORESEARCH_TEXT_EMBEDDINGS_H
