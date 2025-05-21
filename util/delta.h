// Copyright (c) 2023-2025, Manticore Software LTD (https://manticoresearch.com)
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

#include "util_private.h"

namespace util
{

FORCE_INLINE void	ComputeDeltas ( uint32_t * pData, int iLength, bool bAsc );
FORCE_INLINE void	ComputeDeltas ( uint64_t * pData, int iLength, bool bAsc );
FORCE_INLINE void	ComputeInverseDeltas ( Span_T<uint32_t> & dData, bool bAsc );
FORCE_INLINE void	ComputeInverseDeltas ( Span_T<uint64_t> & dData, bool bAsc );
FORCE_INLINE void	ComputeInverseDeltas ( std::vector<uint32_t> & dData, bool bAsc );
FORCE_INLINE void	ComputeInverseDeltas ( std::vector<uint64_t> & dData, bool bAsc );
FORCE_INLINE void	ComputeInverseDeltasAsc ( Span_T<uint32_t> & dData );
FORCE_INLINE void	ComputeInverseDeltasAsc ( Span_T<uint64_t> & dData );

} // namespace util

#include "delta_impl.h"