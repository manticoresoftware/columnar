# Changelog

# Version 2.2.4

### Minor changes

* [Commit c6db](https://github.com/manticoresoftware/columnar/commit/c6dbbcbf277ac35f398637980bb57398a4434dbc) Implemented better analyzer rewinding

### Bugfixes
* [Commit 357e](https://github.com/manticoresoftware/columnar/commit/357eab2d7b93759e31927b1bdf62b119ed2d2db2) Fixed bitmap union selection logic
* [Commit 4c90](https://github.com/manticoresoftware/columnar/commit/4c90bc0f11b8b5dddc2db365f4197e3812f20356) Fixup integer filters before creating integer analyzers
* [Commit fea4](https://github.com/manticoresoftware/columnar/commit/fea449a36f45a436712f581f1589111b8ef637a1) Added an analyzer fastpath when all table values pass the filter

# Version 2.2.0

### Major new features
* Added the ability to fetch the number of documents corresponding to a given filter without using iterators
* Significantly improved the performance of secondary indexes with rowid filtering
* Added cutoff support to analyzers
* Significantly improved the performance of secondary indexes with non-selective range filters

### Minor changes
* Сhanged PGM resolution for better estimates

* [Commit 0abc](https://github.com/manticoresoftware/columnar/commit/0abc7246) Inlined some functions
* [Commit c45d](https://github.com/manticoresoftware/columnar/commit/c45ddf7b) Added inlines; changed codec interface; changed default 64bit codec to fastpfor256
* [Commit 86b3](https://github.com/manticoresoftware/columnar/commit/86b3af30) Added exclude filters to CalcCount
* [Commit 5ccf](https://github.com/manticoresoftware/columnar/commit/5ccffa0c) Changed columnar iterator interface to single-call
* [Commit f7f5](https://github.com/manticoresoftware/columnar/commit/f7f54d93) Reduced partial minmax eval depth

### Bugfixes
* [Commit 4f42](https://github.com/manticoresoftware/columnar/commit/1310c8af37398c42cfc010c24f07d146793b4f42) Fixed a crash caused by buffer overflow when encoding integer data
* [Commit 7653](https://github.com/manticoresoftware/columnar/commit/76530db2f74072ea7787cb7d41124b1117ed014f) Fixed a crash caused by using a string filter without a hash func
* [Issue #20](https://github.com/manticoresoftware/columnar/issues/20) Fixed a crash on indexing zero-length MVA attributes
* [Commit 102d](https://github.com/manticoresoftware/columnar/commit/102d67c3) Bitmap iterator now rewinds only forward
* [Commit 24e7](https://github.com/manticoresoftware/columnar/commit/24e76dd9) Fixed float range filters vs negative values
* [Commit e447](https://github.com/manticoresoftware/columnar/commit/e447ec88) Fixed header integrity checks
* [Commit 3c0b](https://github.com/manticoresoftware/columnar/commit/3c0b089c) Fixed bitmap iterator description on empty result sets
* [Commit 4b21](https://github.com/manticoresoftware/columnar/commit/4b21f461) Clamp iterator esitmates for FilterType_e::VALUES

# Version 2.0.4

### Bugfixes

* [Issue #1054](https://github.com/manticoresoftware/manticoresearch/issues/1054) Bug on empty string condition
