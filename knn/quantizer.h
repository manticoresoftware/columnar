// Copyright (c) 2025, Manticore Software LTD (https://manticoresearch.com)
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

struct QuantizationSettings_t
{
	float	m_fMin = 0.0f;
	float	m_fMax = 0.0f;
};


class ScalarQuantizer_i
{
public:
	virtual			~ScalarQuantizer_i() = default;

	virtual void	Train ( const util::Span_T<float> & dPoint ) = 0;
	virtual void	Encode ( const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) = 0;
	virtual const QuantizationSettings_t & GetSettings() = 0;

};

ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, const QuantizationSettings_t & tQuantSettings );
ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization );

} // namespace knn
