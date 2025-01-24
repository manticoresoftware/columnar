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

# Version 2.7.0

- [Commit 89ed74a3](https://github.com/manticoresoftware/columnar/commit/89ed74a3d767a4a9dfdfe20d7c954fbc36c5ab72) - Fixed a crash caused by mismatched filter and secondary index types.
- [Commit 4223c525](https://github.com/manticoresoftware/columnar/commit/4223c525aed2cfb704ae9a0b439e5fac034913d0) - Implemented the NOTNULL filter type for secondary indexes.
- [Commit 020c82ed](https://github.com/manticoresoftware/columnar/commit/020c82ede0903f898a685cae0b5d8fcb19027771) - Fixed exclude filter handling in columnar accessor for table encoding.
- [Commit 3fb88e65](https://github.com/manticoresoftware/columnar/commit/3fb88e65fa6575a40d80cbf96b45ad3383b39c46) - Fixed issues with full-scan (NOTNULL) filters on strings.
- [Commit b707d5b0](https://github.com/manticoresoftware/columnar/commit/b707d5b0eec0383cdae12730d36eb8a25bc26ce2) - Added native exclude filter handling using bitmaps.
- [Commit bd59d083](https://github.com/manticoresoftware/columnar/commit/bd59d083eec5f6debcf190b69cedc303683553da) - Fixed issues with bitmap inversion.
- [Commit ba9e283b](https://github.com/manticoresoftware/columnar/commit/ba9e283b2f0e8a60756af69b0a0d8c21e2263099) - Switched to the hnsw library to fix issues when loading multiple KNN indexes.
- [Commit 89120fa7](https://github.com/manticoresoftware/columnar/commit/89120fa7ead9b2770f7ddc3912807e6e6bcca1f3) - Resolved another bitmap inversion issue.
- [Commit edadc694](https://github.com/manticoresoftware/columnar/commit/edadc694c68d6022bdd13134263667430a42cc1d) - Addressed additional issues with bitmap inversion.
- [Commit 3ff21a80](https://github.com/manticoresoftware/columnar/commit/3ff21a80357dcca80b021b4827524d9ba63f11e6) - Fixed incorrectly enabled secondary indexes for JSON attribute fields affected by updates.
- [Commit 47da6760](https://github.com/manticoresoftware/columnar/commit/47da6760aa8b32b2ef9d82f3a55666e7d0dbdf30) - Added support for fetching index metadata.