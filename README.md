<p align="center">
  <a href="https://manticoresearch.com" target="_blank" rel="noopener">
    <img src="https://manticoresearch.com/manticore-logo-central.svg" width="15%" alt="Manicore Search Logo">
  </a>
</p>

<h1 align="center">
  Manticore Columnar Library
</h1>

Manticore Columnar Library is a column-oriented storage and secondary indexing library, aiming to provide **decent performance with low memory footprint at big data volume**.
When used in combination with [Manticore Search](https://github.com/manticoresoftware/manticoresearch) can be beneficial for those looking for:
1. log analytics including rich free text search capabities (which is missing in e.g. [Clickhouse](https://github.com/ClickHouse/ClickHouse) - great tool for metrics analytics)
2. faster / low resource consumption log/metrics analytics. Since the library and Manticore Search are both written in C++ with low optimizations in mind, in many cases the performance / RAM consumption is better than in Lucene / SOLR / Elasticsearch
3. running log / metric analytics in docker / kubernetes. Manticore Search + the library can work with as little as 30 megabytes of RAM which Elasticsearch / Clickhouse can't. It also starts in less than a second or a few seconds in the worst case. Since the overhead is so little you can afford having more nodes of Manticore Search + the library than Elasticsearch. More nodes and quicker start means higher high availability and agility.
4. powerful SQL for logs/metrics analytics and everything else [Manticore Search](https://github.com/manticoresoftware/manticoresearch) can give you

## Getting started

### Requirements

1. CPU: with SSE >= 4.2
2. Architecture: arm64 or x86_64 
3. OS: Debian-based OS (Debian, Ubuntu, Mint), RHEL-based (RHEL, Centos, Alma, Oracle Linux, Amazon Linux) OS, Windows, MacOS

### Installation from yum/apt repositories
#### Ubuntu, Debian:
```bash
wget https://repo.manticoresearch.com/manticore-repo.noarch.deb
sudo dpkg -i manticore-repo.noarch.deb
sudo apt update
sudo apt install manticore-columnar-lib
```

#### Centos:
```bash
sudo yum install https://repo.manticoresearch.com/manticore-repo.noarch.rpm
sudo yum install manticore-columnar-lib
```

`searchd -v` should include `columnar x.y.z`, e.g.:
```bash
root@srv# searchd -v
Manticore 6.0.2 89c7a5139@230210 (columnar 2.0.0 a7c703d@230130) (secondary 2.0.0 a7c703d@230130)
```

### Basic usage:
1. Read https://manual.manticoresearch.com/Creating_a_table/Data_types#Row-wise-and-columnar-attribute-storages
2. Create plain or real-time table specifying that the columnar storage should be used

## Benchmarks

### Log analytics - 6x faster than Elasticsearch

https://db-benchmarks.com/test-logs10m/#elasticsearch-tuned-vs-manticore-search-columnar-storage

![logs_es_msc](https://db-benchmarks.com/test-logs10m/est_msc.png)

### Log analytics - 1.4x faster than Clickhouse

https://db-benchmarks.com/test-logs10m/#clickhouse-vs-manticore-search-columnar-storage

![logs_es_ch](https://db-benchmarks.com/test-logs10m/ch_msc.png)

### Medium data - 110M Hackernews comments - 5x faster than Elasticsearch

https://db-benchmarks.com/test-hn/#manticore-search-columnar-storage-vs-elasticsearch

![hn_es_msc](https://db-benchmarks.com/test-hn/msc_es.png)

### Medium data - 110M Hackernews comments - 11x faster than Clickhouse

https://db-benchmarks.com/test-hn/#manticore-search-columnar-storage-vs-clickhouse

![hn_msc_ch](https://db-benchmarks.com/test-hn/msc_ch.png)

### Big data - 1.7B NYC taxi rides - 4x faster than Elasticsearch

https://db-benchmarks.com/test-taxi/#manticore-search-vs-elasticsearch

![taxi_ms_es](https://db-benchmarks.com/test-taxi/ms_es.png)

### Big data - 1.7B NYC taxi rides - 1.8x faster than Clickhouse

https://db-benchmarks.com/test-taxi/#manticore-search-vs-clickhouse

![taxi_ms_ch](https://db-benchmarks.com/test-taxi/ms_ch.png)

