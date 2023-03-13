// Copyright (c) 2023, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
//
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "util.h"

#if defined(USE_SIMDE)
#define SIMDE_ENABLE_NATIVE_ALIASES 1
#include <simde/x86/sse4.1.h>
#elif _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

inline int FillWithIncreasingValues ( uint32_t *& pRowID, size_t uNumValues, uint32_t & tRowID )
{
    __m128i tAdd = _mm_set_epi32 ( tRowID + 3, tRowID + 2, tRowID + 1, tRowID) ;
    size_t uValuesInBlocks = (uNumValues >> 2) << 2;
    uint32_t *pRowIDMax = pRowID + uValuesInBlocks;
    while (pRowID < pRowIDMax)
    {
        _mm_storeu_si128 ( (__m128i *)pRowID, tAdd );
        tAdd = _mm_add_epi32(tAdd, _mm_set1_epi32(4));
        pRowID += 4;
    }

    size_t uValuesLeft = uNumValues - uValuesInBlocks;
    pRowIDMax = pRowID + uValuesLeft;
    while ( pRowID < pRowIDMax )
        *pRowID++ = tRowID++;

    return (int)uNumValues;
}
