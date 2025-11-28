# manticore-knn-embeddings

## How to build rust library

```bash
cargo build --lib --release
```

## How to build examples/test.cpp

```bash
g++ -o test examples/test.cpp -Ltarget/release -lmanticore_knn_embeddings -I. -lpthread -ldl -std=c++17
```

