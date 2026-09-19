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

// quantized stored vector: header followed by one bit plane per code bit (64-bit words each)
struct QuantCodeFactors_t
{
	float	m_fScale;				// |x-c| / <code, rotated unit residual>
	float	m_fResidualDotCentroid;	// <x-c, c>
	float	m_fResidualNormSq;		// |x-c|^2
	float	m_fCodeSum;				// sum of the codes
};

// 4-bit query form: header followed by 4 code bit planes (64-bit words each)
struct QuantQueryFactors_t
{
	float	m_fMin;
	float	m_fStep;
	float	m_fCodeSum;				// sum of 4-bit codes
	float	m_fDotCentroid;			// <y, c>
	float	m_fResidualNormSq;		// |y-c|^2
};

static const int QUANT_QUERY_BITS = 4;	// query codes always use 4 bits, whatever the stored vectors use

inline size_t QuantWords ( size_t uDim )					{ return ( uDim+63 ) / 64; }
inline size_t QuantDataSize ( size_t uDim, int iBits )		{ return sizeof(QuantCodeFactors_t) + iBits*QuantWords(uDim)*sizeof(uint64_t); }
inline size_t QuantQuerySize ( size_t uDim )				{ return sizeof(QuantQueryFactors_t) + QUANT_QUERY_BITS*QuantWords(uDim)*sizeof(uint64_t); }

class ScalarQuantizer_i
{
public:
	virtual			~ScalarQuantizer_i() = default;

	virtual void	Train ( const util::Span_T<float> & dPoint ) = 0;
	virtual void	SetTotalVectors ( int64_t iTotalVectors ) {}
	virtual bool	FinalizeTraining ( std::string & sError ) = 0;
	virtual bool	IsFinalized () const = 0;
	virtual void	Encode ( uint32_t uRowID, const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized, std::vector<uint8_t> & dQuantizedForQuery ) = 0;
	virtual void	FinalizeEncoding() = 0;
	virtual const QuantizationSettings_t & GetSettings() = 0;

	virtual std::function<const uint8_t *(uint32_t)> GetPoolFetcher() const = 0;

	// re-estimate result distances with the unquantized query (fnStored returns a result's stored form); false = no finer estimate
	virtual bool	RefineDistances ( const util::Span_T<float> & dQuery, bool bL2, const std::function<const uint8_t *(uint32_t)> & fnStored, std::vector<DocDist_t> & dResults ) const { return false; }
};

ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, const QuantizationSettings_t & tQuantSettings, HNSWSimilarity_e eSimilarity );
ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, HNSWSimilarity_e eSimilarity, int64_t iNumElements, const std::string & sTmpFilename );

} // namespace knn
