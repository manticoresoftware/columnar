# Manticore Columnar Library

⚠️ PLEASE NOTE: This library is currently in beta and should be used in production with caution! The library is being actively developed and data formats can and will be changed.

Manticore Columnar Library is a column-oriented storage library, aiming to provide **decent performance with low memory footprint at big data volume**.
When used in combination with [Manticore Search](https://github.com/manticoresoftware/manticoresearch) can be beneficial for those looking for:
1. log analytics including rich free text search capabities (which is missing in e.g. [Clickhouse](https://github.com/ClickHouse/ClickHouse) - great tool for metrics analytics)
2. faster / low resource consumption log/metrics analytics. Since the library and Manticore Search are both written in C++ with low optimizations in mind in many cases the performance / RAM consumption is better than in Lucene / SOLR / Elasticsearch
3. running log / metric analytics in docker / kubernetes. Manticore Search + the library can work with as little as 30 megabytes of RAM which Elasticsearch / Clickhouse can't. It also starts in less than a second or few seconds in the worst case. Since the overhead is so little you can afford having more nodes of Manticore Search + the library than Elasticsearch. More nodes and quicker start means higher high availability and agility.
4. powerful SQL for logs/metrics analytics and everything else [Manticore Search](https://github.com/manticoresoftware/manticoresearch) can give you

## Getting started

TODO

## Benchmark "Hacker News comments"

Goal: compare Manticore Columnar Library + Manticore Search on mostly analytical queries with:
1. Manticore Search with its traditional storage
2. Elasticsearch version 7.9.1

Dataset: 1,165,439 [Hacker News curated comments](https://zenodo.org/record/45901/) with numeric fields

Infrastructure: 
* Specially dedicated empty server with no noise load
* CPU: 6*2 Intel(R) Core(TM) i7-3930K CPU @ 3.20GHz
* RAM: 64GB
* Storage: HDD (not SSD)
* Software: 
  - docker in privileged mode
  - RAM limit with help of Linux cgroups
  - CPU limit to only 2 virtual cores to test the performance on one physical core
  - restarting each engine before each query, then running 10 queries one by one
  - dropping OS cache before each query
  - capturing: slowest response time (i.e. cold OS cache) and avg(top 80% fastest) ("Fast avg", shown on the pictures)
  - one shard in Elasticsearch, one plain index in Manticore Search
  - no fine-tuning in either of the engines, just default settings + same field data types everywhere
  - heap size for Elasticsearch - 50% of RAM
* The RAM constraints are based on what Manticore Search traditional storage requires: 
  - 30MB - ~1/3 of the minimum requirement for Manticore Search with the traditional storage for good performance in this case (89MB)
  - 100MB - enough for all the attributes (89MB) to be put in RAM
  - 1024MB - enough for all the index files (972MB) to be put in RAM

### Results:

#### Elasticsearch vs Manticore with 30MB RAM limit - Elasticsearch failed on start
![hn_small_es_ms_30MB](benchmarks/hn_small_es_ms_30MB.png)

#### Elasticsearch vs Manticore with 100MB RAM limit - Elasticsearch failed on start
![hn_small_es_ms_100MB](benchmarks/hn_small_es_ms_100MB.png)

#### Elasticsearch vs Manticore with 1024MB RAM limit - Elasticsearch is 6.51x slower
![hn_small_es_ms_1024MB](benchmarks/hn_small_es_ms_1024MB.png)

#### Manticore GA vs Manticore + Columnar with 30MB RAM limit - the columnar lib is 129.45x faster
![hn_small_es_ms_1024MB](benchmarks/hn_small_ma_co_30MB.png)

#### Manticore GA vs Manticore + Columnar with 100MB RAM limit - the columnar lib is 1.43x slower
![hn_small_es_ms_1024MB](benchmarks/hn_small_ma_co_100MB.png)

#### Manticore GA vs Manticore + Columnar with 1024MB RAM limit - the columnar lib is 1.43x slower
![hn_small_es_ms_1024MB](benchmarks/hn_small_ma_co_1024MB.png)

