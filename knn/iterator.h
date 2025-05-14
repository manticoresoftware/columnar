// Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
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

#include "knn.h"

namespace knn
{

class KNNIndex_i
{
public:
	virtual			~KNNIndex_i() = default;
	virtual void	Search ( std::vector<DocDist_t> & dResults, const util::Span_T<float> & dData, int iResults, int iEf, std::vector<uint8_t> & dQuantized ) const = 0;
};

Iterator_i * CreateIterator ( KNNIndex_i & tIndex, const util::Span_T<float> & dData, int iResults, int iEf );

} // namespace knn
