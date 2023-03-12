# Changelog

# Version X

### Major new features
* Added the ability to fetch the number of documents corresponding to a given filter without using iterators.

### Bugfixes
* [Commit 4f42](https://github.com/manticoresoftware/columnar/commit/1310c8af37398c42cfc010c24f07d146793b4f42) Fixed a crash caused by buffer overflow when encoding integer data
* [Commit 014f] Fixed a crash caused by using a string filter without a hash func

# Version 2.0.4

### Bugfixes

* [Issue #1054](https://github.com/manticoresoftware/manticoresearch/issues/1054) Bug on empty string condition
