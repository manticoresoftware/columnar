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

// This file is a part of the common headers (API).
// If you make any significant changes to this file, you MUST bump the LIB_VERSION.

#pragma once

#include "knn.h"
#include <functional>

namespace knn
{

struct QuantizationSettings_t
{
	float	m_fMin = 0.0f;
	float	m_fMax = 0.0f;
	float	m_fK = 0.0f;
	float	m_fB = 0.0f;

	std::vector<float> m_dCentroid;
};

struct Binary4BitFactors_t
{
	float	m_fQuantizedSum;
	float	m_fDistanceToCentroidSq;
	float	m_fMin;
	float	m_fRange;
	float	m_fVecMinusCentroidNorm;
	float	m_fVecDotCentroid;
};

struct Binary1BitFactorsL2_t
{
	float	m_fDistanceToCentroid;
	float	m_fVectorMagnitude;
	float	m_fPopCnt;
};

struct Binary1BitFactorsIP_t
{
	float	m_fQuality;
	float	m_fVecMinusCentroidNorm;
	float	m_fVecDocCentroid;
	float	m_fPopCnt;
};

class ScalarQuantizer_i
{
public:
	virtual			~ScalarQuantizer_i() = default;

	virtual void	Train ( const util::Span_T<float> & dPoint ) = 0;
	virtual bool	FinalizeTraining ( std::string & sError ) = 0;
	virtual void	Encode ( uint32_t uRowID, const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) = 0;
	virtual void	FinalizeEncoding() = 0;
	virtual const QuantizationSettings_t & GetSettings() = 0;

	virtual std::function<const uint8_t *(uint32_t)> GetPoolFetcher() const = 0;
};

ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, const QuantizationSettings_t & tQuantSettings, HNSWSimilarity_e eSimilarity );
ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, HNSWSimilarity_e eSimilarity, int64_t iNumElements, const std::string & sTmpFilename );

} // namespace knn
