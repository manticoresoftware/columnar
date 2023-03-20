# Changelog

# Version X

### Major new features
* Added the ability to fetch the number of documents corresponding to a given filter without using iterators.

### Bugfixes
* [Commit 4f42](https://github.com/manticoresoftware/columnar/commit/1310c8af37398c42cfc010c24f07d146793b4f42) Fixed a crash caused by buffer overflow when encoding integer data
* [Commit 7653](https://github.com/manticoresoftware/columnar/commit/76530db2f74072ea7787cb7d41124b1117ed014f) Fixed a crash caused by using a string filter without a hash func
* [Commit 014f] Fixed a crash caused by using a string filter without a hash func
* [Issue #20](https://github.com/manticoresoftware/columnar/issues/20) Fixed a crash on indexing zero-length MVA attributes

# Version 2.0.4

### Bugfixes

* [Issue #1054](https://github.com/manticoresoftware/manticoresearch/issues/1054) Bug on empty string condition
