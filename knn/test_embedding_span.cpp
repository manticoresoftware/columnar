// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
// Licensed under the Apache License, Version 2.0.

#include "embeddings.h"

#include <cstdio>

int main()
{
	const std::string sText = "a\xD0\x96\xF0\x9F\x99\x82z"; // a, Cyrillic Zhe, emoji, z
	const uint64_t dBoundaries[] = { 0, 1, 3, 7, 8 };
	const uint64_t dContinuationBytes[] = { 2, 4, 5, 6 };

	for ( uint64_t uOffset : dBoundaries )
		if ( !knn::IsUtf8CodepointBoundary ( sText, uOffset ) )
		{
			std::fprintf ( stderr, "FAILED: rejected UTF-8 boundary %llu\n", (unsigned long long)uOffset );
			return 1;
		}

	for ( uint64_t uOffset : dContinuationBytes )
		if ( knn::IsUtf8CodepointBoundary ( sText, uOffset ) )
		{
			std::fprintf ( stderr, "FAILED: accepted UTF-8 continuation byte %llu\n", (unsigned long long)uOffset );
			return 1;
		}

	if ( knn::IsUtf8CodepointBoundary ( sText, sText.size()+1 ) )
	{
		std::fprintf ( stderr, "FAILED: accepted an out-of-range offset\n" );
		return 1;
	}

	return 0;
}