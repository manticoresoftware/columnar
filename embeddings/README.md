# manticore-knn-embeddings
Proof of Concept to use Rust in building lib for generating text embeddings


## How to build rust library

```bash
cargo build --lib --release
```

## How to build examples/test.cpp

```bash
g++ -o test examples/test.cpp -Ltarget/release -lmanticore_knn_embeddings -I. -lpthread -ldl -std=c++17
```

## Testing

Some integration tests download model files into a cache directory if they are missing. You can
override the cache location with environment variables:

- `MANTICORE_TEST_CACHE`: preferred cache path for tests
- `MANTICORE_CACHE_PATH`: fallback cache path for tests

If neither is set, tests use `./.cache/manticore` under the repo.
