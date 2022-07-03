// Copyright (c) 2020-2022, Manticore Software LTD (https://manticoresearch.com)
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

#include "accessor.h"

#include "columnar.h"
#include <algorithm>

namespace columnar
{

bool CheckEmptySpan ( uint32_t * pRowID, uint32_t * pRowIdStart, util::Span_T<uint32_t> & dRowIdBlock )
{
	if ( pRowID==pRowIdStart )
		return false;

	dRowIdBlock = { pRowIdStart, size_t(pRowID-pRowIdStart) };
	return true;
}

} // namespace columnar
