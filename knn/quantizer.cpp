// Copyright (c) 2025, Manticore Software LTD (https://manticoresearch.com)
// All rights reserved
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
//
// This implementation of binary vector quantization is based on Elasticsearch's Java implementation:
// https://github.com/elastic/elasticsearch/blob/1dd41ec2b683a7b7c9c16af404b842cf85cbd5bc/server/src/main/java/org/elasticsearch/index/codec/vectors/es816/BinaryQuantizer.java
// Modifications copyright (C) 2024 Elasticsearch B.V.
// Original implementation licensed under the Apache License, Version 2.0.
// The algorithm is based on the paper "RaBitQ" (https://arxiv.org/abs/2405.12497)

#include "quantizer.h"

#include "util_private.h"
#include "reader.h"
#include <float.h>
#include <algorithm>
#include <cmath>
#include <numeric>

#ifdef _MSC_VER
	#include <io.h>
#else
	#include <unistd.h>
#endif


using namespace util;

namespace knn
{

class P2QuantileEstimator_c
{
public:
			P2QuantileEstimator_c ( double fQuantile );

	void	Insert ( float fValue );
	bool	Ready() const	{ return m_iCount>=NUM_MARKERS; }
	float	Get() const		{ return m_dQ[2]; } 

private:
	static const int NUM_MARKERS=5;

	double	m_dQ[NUM_MARKERS];  // heights of the markers
	double	m_dN[NUM_MARKERS];  // positions of the markers
	double	m_dNP[NUM_MARKERS]; // desired positions
	double	m_dDN[NUM_MARKERS]; // increments for desired positions
	int		m_iCount = 0;

	void				InitializeMarkers ( float fValue );
	FORCE_INLINE int	FindMarker ( float fValue );
	FORCE_INLINE void	IncrementPositions ( int iMarker );
	FORCE_INLINE void	AdjustMarkerHeights();
};


P2QuantileEstimator_c::P2QuantileEstimator_c ( double fQuantile )
{
	m_dDN[0] = 0.0;
	m_dDN[1] = fQuantile / 2.0;
	m_dDN[2] = fQuantile;
	m_dDN[3] = (1.0 + fQuantile) / 2.0;
	m_dDN[4] = 1.0;
}


void P2QuantileEstimator_c::Insert ( float fValue )
{
	m_iCount++;

	if ( m_iCount<=NUM_MARKERS )
	{
		InitializeMarkers(fValue);
		return;
	}
		
	int iMarker = FindMarker(fValue);
	IncrementPositions(iMarker);
	AdjustMarkerHeights();
}


void P2QuantileEstimator_c::InitializeMarkers ( float fValue )
{
	m_dQ[m_iCount-1] = fValue;
	if ( m_iCount<NUM_MARKERS)
		return;

	std::sort ( m_dQ, m_dQ+NUM_MARKERS );

	for ( int i = 0; i < NUM_MARKERS; i++ )
	{
		m_dN[i] = i+1;
		m_dNP[i] = 1 + (NUM_MARKERS-1) * m_dDN[i];
	}
}


int P2QuantileEstimator_c::FindMarker ( float fValue )
{
	if ( fValue < m_dQ[0] )
	{
		m_dQ[0] = fValue;
		return 0;
	}

	if ( fValue >= m_dQ[NUM_MARKERS-1] )
	{
		m_dQ[NUM_MARKERS-1] = fValue;
		return NUM_MARKERS-2;
	}
		
	for ( int i = 0; i < NUM_MARKERS-1; i++ )
		if ( m_dQ[i]<=fValue && fValue<m_dQ[i+1] )
			return i;

	assert ( 0 && "Unable to find marker" );
	return 0;
}


void P2QuantileEstimator_c::IncrementPositions ( int iMarker )
{
	for ( int i = iMarker + 1; i < NUM_MARKERS; i++ )
		m_dN[i]++;

	for ( int i = 0; i < NUM_MARKERS; i++ )
		m_dNP[i] += m_dDN[i];
}


void P2QuantileEstimator_c::AdjustMarkerHeights()
{
	for ( int i=1; i < NUM_MARKERS-1; i++ )
	{
		double fD = m_dNP[i] - m_dN[i];
		if ( ( fD>=1.0f && m_dN[i+1]-m_dN[i] > 1.0f ) || ( fD<=-1.0f && m_dN[i-1]-m_dN[i] < -1.0f ) )
		{
			int iSign = fD>=1.0f ? 1 : -1;
			double fNewQ = m_dQ[i] + iSign*( m_dQ[i+iSign] - m_dQ[i] )/( m_dN[i+iSign] - m_dN[i] );
			m_dQ[i] = fNewQ;
			m_dN[i] += iSign;
		}
	}
}

///////////////////////////////////////////////////////////////////////////////

class ScalarQuantizer8Bit_c : public ScalarQuantizer_i
{
public:
			ScalarQuantizer8Bit_c();
			ScalarQuantizer8Bit_c( const QuantizationSettings_t & tSettings );

	void	Train ( const Span_T<float> & dPoint ) override;
	bool	FinalizeTraining ( std::string & sError ) override;
	void	Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) override;
	void	FinalizeEncoding() override {}
	const QuantizationSettings_t & GetSettings() override;
	std::function<const uint8_t * (uint32_t)> GetPoolFetcher() const override { return nullptr; }

protected:
	QuantizationSettings_t	m_tSettings;
	P2QuantileEstimator_c	m_tQuantile1 { 0.005 };
	P2QuantileEstimator_c	m_tQuantile2 { 0.995 };
	bool					m_bQuantilesEnabled = false;
	const float				INT_SCALE = 255.0f;
	float	m_fDiff = 0.0f;
	float	m_fAlpha = 0.0f;
	bool	m_bTrained = false;
	bool	m_bFinalized = false;
	size_t	m_uDim = 0;
	size_t	m_uNumTrained = 0;

	FORCE_INLINE float	Scale ( float fValue ) const;
	virtual float		GetIntScale() const { return INT_SCALE; }
};


ScalarQuantizer8Bit_c::ScalarQuantizer8Bit_c()
{
	m_tSettings.m_fMin = FLT_MAX;
	m_tSettings.m_fMax = -FLT_MAX;
}


ScalarQuantizer8Bit_c::ScalarQuantizer8Bit_c ( const QuantizationSettings_t & tSettings )
	: m_tSettings ( tSettings )
{
	m_fDiff = m_tSettings.m_fMax - m_tSettings.m_fMin;
	m_fAlpha = m_fDiff / INT_SCALE;
	m_bTrained = true;
	m_bFinalized = true;
}


void ScalarQuantizer8Bit_c::Train ( const Span_T<float> & dPoint )
{
	assert ( !m_bFinalized );

	for ( auto i : dPoint )
	{
		if ( i < m_tSettings.m_fMin )
			m_tSettings.m_fMin = i;

		if ( i > m_tSettings.m_fMax )
			m_tSettings.m_fMax = i;

		if ( m_bQuantilesEnabled )
		{
			m_tQuantile1.Insert(i);
			m_tQuantile2.Insert(i);
		}
	}

	m_bTrained = true;
	m_uDim = dPoint.size();
	m_uNumTrained += m_uDim;
}


bool ScalarQuantizer8Bit_c::FinalizeTraining ( std::string & sError )
{
	assert(m_bTrained);
	if ( m_bFinalized )
		return true;

	m_bFinalized = true;

	const size_t TRAINED_SIZE_THRESH = 1000;
	if ( m_bQuantilesEnabled && m_uNumTrained>TRAINED_SIZE_THRESH && m_tQuantile1.Ready() && m_tQuantile2.Ready() )
	{
		m_tSettings.m_fMin = std::max ( m_tSettings.m_fMin, m_tQuantile1.Get() );
		m_tSettings.m_fMax = std::min ( m_tSettings.m_fMax, m_tQuantile2.Get() );
	}

	m_fDiff = m_tSettings.m_fMax - m_tSettings.m_fMin;
	m_fAlpha = m_fDiff / GetIntScale();
	m_tSettings.m_fK = -m_fAlpha*m_fAlpha;
	m_tSettings.m_fB = 1.0f - m_tSettings.m_fMin*m_tSettings.m_fMin*m_uDim;

	return true;
}


void ScalarQuantizer8Bit_c::Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized )
{
	assert(m_bFinalized);

	dQuantized.resize ( dPoint.size() + sizeof(float) );
	uint8_t * pQuantized = dQuantized.data() + sizeof(float);

	int iSum = 0;
	for ( size_t i = 0; i < dPoint.size(); i++ )
	{
		float fValue = INT_SCALE * Scale ( dPoint[i] );
		int iValue = (int)std::lround(fValue);
		iSum += iValue;
		*pQuantized++ = std::clamp ( iValue, 0, int(INT_SCALE) );
	}

	*(float*)dQuantized.data() = -iSum*m_tSettings.m_fMin*m_fAlpha;
}


const QuantizationSettings_t & ScalarQuantizer8Bit_c::GetSettings()
{
	assert(m_bFinalized);
	return m_tSettings;
}


float ScalarQuantizer8Bit_c::Scale ( float fValue ) const
{
	if ( m_fDiff==0.0f )
		return 0.0f;

	return ( fValue-m_tSettings.m_fMin ) / m_fDiff;
}

///////////////////////////////////////////////////////////////////////////////

class ScalarQuantizer4Bit_c : public ScalarQuantizer8Bit_c
{
	using ScalarQuantizer8Bit_c::ScalarQuantizer8Bit_c;

public:
			ScalarQuantizer4Bit_c ( const QuantizationSettings_t & tSettings );

	void	Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) override;

protected:
	float		GetIntScale() const override { return INT_SCALE; }

private:
	const float	INT_SCALE = 15.0f;
};


ScalarQuantizer4Bit_c::ScalarQuantizer4Bit_c( const QuantizationSettings_t & tSettings )
	: ScalarQuantizer8Bit_c(tSettings)
{
	m_fDiff = m_tSettings.m_fMax - m_tSettings.m_fMin;
	m_fAlpha = m_fDiff / INT_SCALE;
}


void ScalarQuantizer4Bit_c::Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized )
{
	assert(m_bFinalized);

	dQuantized.resize ( ( ( dPoint.size()+1 ) >> 1 ) + sizeof(float) );
	uint8_t * pQuantized = dQuantized.data() + sizeof(float);

	size_t tSize = dPoint.size();
	int iSum = 0;
	for ( size_t i = 0; i < tSize; i+=2 )
	{
		float fValue = INT_SCALE*Scale(dPoint[i]);
		int iValue = (int)std::lround(fValue);
		iSum += iValue;
		int iLow = std::clamp ( iValue, 0, int(INT_SCALE) );
		int iHigh = 0;
		if ( i+1 < tSize )
		{
			fValue = INT_SCALE*Scale(dPoint[i+1]);
			iValue = (int)std::lround(fValue);
			iSum += iValue;
			iHigh = std::clamp ( iValue, 0, int(INT_SCALE) );
		}

		pQuantized[i>>1] = ( iHigh << 4 ) | iLow;
	}

	*(float*)dQuantized.data() = -iSum*m_tSettings.m_fMin*m_fAlpha;
}

///////////////////////////////////////////////////////////////////////////////

template <bool COSINE>
class ScalarQuantizer1Bit_T : public ScalarQuantizer8Bit_c
{
	using ScalarQuantizer8Bit_c::ScalarQuantizer8Bit_c;

public:
	void	Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) override;

private:
	HNSWSimilarity_e m_eSimilarity = HNSWSimilarity_e::L2;
};

template <bool COSINE>
void ScalarQuantizer1Bit_T<COSINE>::Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized )
{
	assert(m_bFinalized);

	dQuantized.resize ( ( dPoint.size()+7 ) >> 3 );
	size_t uDim = dPoint.size();
	size_t uNumBytes = dQuantized.size();
	size_t uOff = 0;
	for ( size_t i = 0; i < uNumBytes; i++ )
	{
		uint8_t uPacked = 0;
		for ( size_t uBit = 0; uBit < 8; uBit++ )
		{
			if constexpr ( COSINE )
			{
				if ( dPoint[uOff] > 0.0f )
					uPacked |= 1 << uBit;
			}
			else
			{
				float fValue = Scale(dPoint[uOff]);
				if ( fValue > 0.5f )
					uPacked |= 1 << uBit;
			}

			uOff++;
			if ( uOff>=uDim )
				break;
		}

		dQuantized[i] = uPacked;
	}
}

///////////////////////////////////////////////////////////////////////////////

// BinaryQuantizer_c implements binary vector quantization based on Elasticsearch's Java implementation
// in org.elasticsearch.index.codec.vectors.es816.BinaryQuantizer
// Permalink: https://github.com/elastic/elasticsearch/blob/1dd41ec2b683a7b7c9c16af404b842cf85cbd5bc/server/src/main/java/org/elasticsearch/index/codec/vectors/es816/BinaryQuantizer.java
// See: https://arxiv.org/abs/2405.12497 for the RaBitQ paper
class BinaryQuantizer_c
{
public:
			BinaryQuantizer_c ( int iDim, HNSWSimilarity_e eSimilarity );

	void	Quantize1Bit ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, std::vector<uint8_t> & dResult );
	void	Quantize4Bit ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, std::vector<uint8_t> & dResult );

private:
	size_t				m_uDimPadded = 0;
	HNSWSimilarity_e	m_eSimilarity = HNSWSimilarity_e::COSINE;
	float				m_fSqrtDim = 0.0f;

	SpanResizeable_T<float>		m_dVecMinusCentroid;
	SpanResizeable_T<uint8_t>	m_dQuantized;

	static void					Pack ( const Span_T<float> & dVector, Span_T<uint8_t> & dPacked );
	FORCE_INLINE static int		Quantize ( const Span_T<float> & dVector, float fMin, float fRange, SpanResizeable_T<uint8_t> & dQuantized );
	FORCE_INLINE static void	Transpose ( const Span_T<uint8_t> & dQuantized, size_t uDim, Span_T<uint8_t> & dTransposed );

	float					ComputeQuality ( int iOriginalLength, const Span_T<float> & dVecMinusCentroidNormalized, const Span_T<uint8_t> & dPacked ) const;
	float					QuantizeVecL2 ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, Span_T<uint8_t> & dResult );
	Binary1BitFactorsIP_t	QuantizeVecIP ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, Span_T<uint8_t> & dResult );

	template <typename T> FORCE_INLINE void PadToDim ( T & dVec )
	{
		if ( dVec.size() < m_uDimPadded )
			dVec.resize ( m_uDimPadded, 0 );
	}
};

///////////////////////////////////////////////////////////////////////////////

FORCE_INLINE uint32_t PopCnt32 ( uint32_t uVal )
{
#if defined ( USE_SIMDE )
	return __builtin_popcount ( uVal );
#else
	return _mm_popcnt_u32 ( uVal );
#endif
}


FORCE_INLINE uint32_t PopCnt ( const Span_T<uint8_t> & dValues )
{
	int iCount = 0;
	int i = 0;
	const int i4ByteBlocks = dValues.size () >> 2 << 2;
	auto pValues32 = (const uint32_t*)dValues.data();
	for ( ; i < i4ByteBlocks; i += 4 )
		iCount += PopCnt32 ( *pValues32++ );

	for ( ; i < dValues.size (); i++ )
		iCount += PopCnt32 ( dValues[i] & 0xFF );

	return iCount;
}


static int CalcPadding ( int iValue, int iPad )
{
	return ( ( iValue + iPad - 1 ) / iPad ) * iPad;
}


BinaryQuantizer_c::BinaryQuantizer_c ( int iDim, HNSWSimilarity_e eSimilarity )
	: m_uDimPadded ( CalcPadding ( iDim, 64 ) )
	, m_eSimilarity ( eSimilarity )
	, m_fSqrtDim ( std::sqrt(iDim) )
{}


void BinaryQuantizer_c::Pack ( const Span_T<float> & dVector, Span_T<uint8_t> & dPacked )
{
	for ( size_t i = 0; i < dVector.size(); i += 8 )
	{
		uint8_t uByte = 0;
		int iOff = 0;
		for ( int j = 7; j >= 0; j-- )
		{
			if ( dVector[i + j] > 0.0f )
				uByte |= ( 1 << iOff );

			iOff++;
		}

		dPacked[i >> 3] = uByte;
	}
}


int BinaryQuantizer_c::Quantize ( const Span_T<float> & dVector, float fMin, float fRange, SpanResizeable_T<uint8_t> & dQuantized )
{
	dQuantized.resize ( dVector.size() );

	float fDiv = fRange!=0.0f ? 1.0f/fRange : 0.0f;
	int iQuantizedSum = 0;
	int64_t i = 0;

#if defined(USE_AVX2)
	__m256 iMinVec = _mm256_set1_ps(fMin);
	__m256 iDivVec = _mm256_set1_ps(fDiv);
	__m256i iSumVec = _mm256_setzero_si256();

	size_t uLimit = dVector.size() & ~7;
	for ( ; i < uLimit; i += 8 )
	{
		__m256 iVec = _mm256_loadu_ps ( &dVector[i] );
		iVec = _mm256_sub_ps ( iVec, iMinVec );
		iVec = _mm256_mul_ps ( iVec, iDivVec );
		iVec = _mm256_round_ps ( iVec, _MM_FROUND_TO_NEAREST_INT );
		iVec = _mm256_cvtps_epi32(iVec);       
		iVec = _mm256_max_epi32 ( iVec, _mm256_setzero_si256() );
		iVec = _mm256_min_epi32 ( iVec, _mm256_set1_epi32(15) );

		iSumVec = _mm256_add_epi32 ( iSumVec, iVec );

		__m128i iLow = _mm256_castsi256_si128(iVec);
		__m128i iHigh = _mm256_extracti128_si256 ( iVec, 1 );
		__m128i iPack16 = _mm_packs_epi32 ( iLow, iHigh );
		__m128i iPack8 = _mm_packus_epi16 ( iPack16, iPack16 );
		_mm_storel_epi64 ( (__m128i*)(&dQuantized[i]), iPack8 );
	}

	__m128i iSum128 = _mm_add_epi32 ( _mm256_extracti128_si256 ( iSumVec, 0 ), _mm256_extracti128_si256 ( iSumVec, 1 ) );
	iSum128 = _mm_add_epi32 ( iSum128, _mm_srli_si128 ( iSum128, 8) );
	iSum128 = _mm_add_epi32 ( iSum128, _mm_srli_si128 ( iSum128, 4) );
	iQuantizedSum = _mm_extract_epi32 ( iSum128, 0 );
#endif

	for ( ; i < dVector.size(); i++ )
	{
		int iRes =  (int)std::lround ( ( dVector[i] - fMin )*fDiv );
		uint8_t uRes = (uint8_t)std::clamp ( iRes, 0, 15 );
		dQuantized[i] = uRes;
		iQuantizedSum += uRes;
	}

	return iQuantizedSum;
}


float BinaryQuantizer_c::ComputeQuality ( int iOriginalLength, const Span_T<float> & dVecMinusCentroidNormalized, const Span_T<uint8_t> & dPacked ) const
{
	float fRes = 0.0f;
	auto pVecMinusCentroidNormalized = dVecMinusCentroidNormalized.data();
	for ( int i = 0; i < iOriginalLength / 8; i++ )
		for ( int j = 7; j>=0; j-- )
		{
			int iSign = ( dPacked[i] >> j ) & 1;
			fRes += *pVecMinusCentroidNormalized * ( 2*iSign - 1 );
			pVecMinusCentroidNormalized++;
		}

	return fRes / m_fSqrtDim;
}


float BinaryQuantizer_c::QuantizeVecL2 ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, Span_T<uint8_t> & dResult )
{
	m_dVecMinusCentroid.resize ( dVector.size() );
	for ( size_t i = 0; i < m_dVecMinusCentroid.size(); i++ )
		m_dVecMinusCentroid[i] = dVector[i] - dCentroid[i];

	float fNorm = VecCalcNorm(m_dVecMinusCentroid);
	PadToDim(m_dVecMinusCentroid);
	Pack ( m_dVecMinusCentroid, dResult );
	m_dVecMinusCentroid.resize ( dVector.size() );

	for ( float & i : m_dVecMinusCentroid )
		i = std::abs(i) / m_fSqrtDim;

	float fNormalized = std::accumulate ( m_dVecMinusCentroid.begin (), m_dVecMinusCentroid.end (), 0.0f );
	fNormalized /= fNorm;
	return std::isfinite(fNormalized) ? fNormalized : 0.8f;
}


Binary1BitFactorsIP_t BinaryQuantizer_c::QuantizeVecIP ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, Span_T<uint8_t> & dResult )
{
	float fVecDotCentroid = 0.0f;
	m_dVecMinusCentroid.resize ( dVector.size() );
	for ( size_t i = 0; i < dVector.size(); i++ )
	{
		fVecDotCentroid += dVector[i]*dCentroid[i];
		m_dVecMinusCentroid[i] = dVector[i] - dCentroid[i];
	}

	float fVecMinusCentroidNorm = VecCalcNorm(m_dVecMinusCentroid);
	PadToDim(m_dVecMinusCentroid);
	Pack ( m_dVecMinusCentroid, dResult );

	for ( float & i : m_dVecMinusCentroid )
		i /= fVecMinusCentroidNorm;

	float fQuality = ComputeQuality ( dVector.size(), m_dVecMinusCentroid, dResult );
	return { fQuality, fVecMinusCentroidNorm, fVecDotCentroid, (float)PopCnt(dResult) };
}


void BinaryQuantizer_c::Quantize1Bit ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, std::vector<uint8_t> & dResult )
{
	size_t uDataSize = ( ( dVector.size()+7 ) >> 3 );
	size_t uHeaderSize = m_eSimilarity==HNSWSimilarity_e::L2 ? sizeof(Binary1BitFactorsL2_t) : sizeof(Binary1BitFactorsIP_t);
	dResult.resize ( uHeaderSize + uDataSize );
	Span_T<uint8_t> dData { dResult.data()+uHeaderSize, uDataSize };

	std::vector<float> dCorrections;
	switch ( m_eSimilarity )
	{
	case HNSWSimilarity_e::L2:
		{
			auto & tFactors = *(Binary1BitFactorsL2_t*)(dResult.data());
			tFactors.m_fDistanceToCentroid = VecDist ( dVector, dCentroid );
			tFactors.m_fVectorMagnitude = QuantizeVecL2 ( dVector, dCentroid, dData );
			tFactors.m_fPopCnt = PopCnt(dData);
		}
		break;

	case HNSWSimilarity_e::IP:
	case HNSWSimilarity_e::COSINE:
		{
			auto & tFactors = *(Binary1BitFactorsIP_t*)(dResult.data());
			tFactors = QuantizeVecIP ( dVector, dCentroid, dData );
		}
		break;

	default:
		assert ( 0 && "Unsupported similarity" );
		break;
	}
}

#if defined(USE_AVX2)
static const uint8_t g_dBitReverseTable[256] =
{
    0x00, 0x80, 0x40, 0xC0, 0x20, 0xA0, 0x60, 0xE0, 0x10, 0x90, 0x50, 0xD0, 0x30, 0xB0, 0x70, 0xF0,
    0x08, 0x88, 0x48, 0xC8, 0x28, 0xA8, 0x68, 0xE8, 0x18, 0x98, 0x58, 0xD8, 0x38, 0xB8, 0x78, 0xF8,
    0x04, 0x84, 0x44, 0xC4, 0x24, 0xA4, 0x64, 0xE4, 0x14, 0x94, 0x54, 0xD4, 0x34, 0xB4, 0x74, 0xF4,
    0x0C, 0x8C, 0x4C, 0xCC, 0x2C, 0xAC, 0x6C, 0xEC, 0x1C, 0x9C, 0x5C, 0xDC, 0x3C, 0xBC, 0x7C, 0xFC,
    0x02, 0x82, 0x42, 0xC2, 0x22, 0xA2, 0x62, 0xE2, 0x12, 0x92, 0x52, 0xD2, 0x32, 0xB2, 0x72, 0xF2,
    0x0A, 0x8A, 0x4A, 0xCA, 0x2A, 0xAA, 0x6A, 0xEA, 0x1A, 0x9A, 0x5A, 0xDA, 0x3A, 0xBA, 0x7A, 0xFA,
    0x06, 0x86, 0x46, 0xC6, 0x26, 0xA6, 0x66, 0xE6, 0x16, 0x96, 0x56, 0xD6, 0x36, 0xB6, 0x76, 0xF6,
    0x0E, 0x8E, 0x4E, 0xCE, 0x2E, 0xAE, 0x6E, 0xEE, 0x1E, 0x9E, 0x5E, 0xDE, 0x3E, 0xBE, 0x7E, 0xFE,
    0x01, 0x81, 0x41, 0xC1, 0x21, 0xA1, 0x61, 0xE1, 0x11, 0x91, 0x51, 0xD1, 0x31, 0xB1, 0x71, 0xF1,
    0x09, 0x89, 0x49, 0xC9, 0x29, 0xA9, 0x69, 0xE9, 0x19, 0x99, 0x59, 0xD9, 0x39, 0xB9, 0x79, 0xF9,
    0x05, 0x85, 0x45, 0xC5, 0x25, 0xA5, 0x65, 0xE5, 0x15, 0x95, 0x55, 0xD5, 0x35, 0xB5, 0x75, 0xF5,
    0x0D, 0x8D, 0x4D, 0xCD, 0x2D, 0xAD, 0x6D, 0xED, 0x1D, 0x9D, 0x5D, 0xDD, 0x3D, 0xBD, 0x7D, 0xFD,
    0x03, 0x83, 0x43, 0xC3, 0x23, 0xA3, 0x63, 0xE3, 0x13, 0x93, 0x53, 0xD3, 0x33, 0xB3, 0x73, 0xF3,
    0x0B, 0x8B, 0x4B, 0xCB, 0x2B, 0xAB, 0x6B, 0xEB, 0x1B, 0x9B, 0x5B, 0xDB, 0x3B, 0xBB, 0x7B, 0xFB,
    0x07, 0x87, 0x47, 0xC7, 0x27, 0xA7, 0x67, 0xE7, 0x17, 0x97, 0x57, 0xD7, 0x37, 0xB7, 0x77, 0xF7,
    0x0F, 0x8F, 0x4F, 0xCF, 0x2F, 0xAF, 0x6F, 0xEF, 0x1F, 0x9F, 0x5F, 0xDF, 0x3F, 0xBF, 0x7F, 0xFF
};


static FORCE_INLINE uint8_t ReverseBitsUint8 ( uint8_t uByte )
{
    return g_dBitReverseTable[uByte];
}


void BinaryQuantizer_c::Transpose ( const Span_T<uint8_t> & dQuantized, size_t uDim, Span_T<uint8_t> & dTransposed )
{
	const int NUM_BYTES = 32;
	const size_t uDimDiv8 = uDim >> 3;
	auto pQuantized = dQuantized.data();
	
	for (size_t i = 0; i < uDim; i += NUM_BYTES )
	{
		__m256i iData = _mm256_loadu_si256 ( (const __m256i*)pQuantized );
		
		__m256i iSpread0 = _mm256_or_si256 ( _mm256_slli_epi16 ( iData, 4 ), _mm256_and_si256 ( _mm256_srli_epi16 ( iData, 4 ), _mm256_set1_epi8(0x0F) ) );
		__m256i iSpread1 = _mm256_slli_epi16 ( iSpread0, 1 );
		__m256i iSpread2 = _mm256_slli_epi16 ( iSpread1, 1 );
		__m256i iSpread3 = _mm256_slli_epi16 ( iSpread2, 1 );

		uint32_t uMask0 = _mm256_movemask_epi8 ( iSpread0 );
		uint32_t uMask1 = _mm256_movemask_epi8 ( iSpread1 );
		uint32_t uMask2 = _mm256_movemask_epi8 ( iSpread2 );
		uint32_t uMask3 = _mm256_movemask_epi8 ( iSpread3 );

		auto pTransposed = dTransposed.data() + ( i >> 3 );
		pTransposed[0] = ReverseBitsUint8 ( uMask3 & 0xFF );
		pTransposed[1] = ReverseBitsUint8 ( ( uMask3 >> 8 ) & 0xFF );
		pTransposed[2] = ReverseBitsUint8 ( ( uMask3 >> 16 ) & 0xFF );
		pTransposed[3] = ReverseBitsUint8 ( ( uMask3 >> 24 ) & 0xFF );

		pTransposed += uDimDiv8;
		pTransposed[0] = ReverseBitsUint8 ( uMask2 & 0xFF );
		pTransposed[1] = ReverseBitsUint8 ( ( uMask2 >> 8 ) & 0xFF );
		pTransposed[2] = ReverseBitsUint8 ( ( uMask2 >> 16 ) & 0xFF );
		pTransposed[3] = ReverseBitsUint8 ( ( uMask2 >> 24 ) & 0xFF );

		pTransposed += uDimDiv8;
		pTransposed[0] = ReverseBitsUint8 ( uMask1 & 0xFF );
		pTransposed[1] = ReverseBitsUint8 ( ( uMask1 >> 8 ) & 0xFF );
		pTransposed[2] = ReverseBitsUint8 ( ( uMask1 >> 16 ) & 0xFF );
		pTransposed[3] = ReverseBitsUint8 ( ( uMask1 >> 24 ) & 0xFF );

		pTransposed += uDimDiv8;
		pTransposed[0] = ReverseBitsUint8 ( uMask0 & 0xFF);
		pTransposed[1] = ReverseBitsUint8 ( ( uMask0 >> 8 ) & 0xFF );
		pTransposed[2] = ReverseBitsUint8 ( ( uMask0 >> 16 ) & 0xFF );
		pTransposed[3] = ReverseBitsUint8 ( ( uMask0 >> 24 ) & 0xFF );
	
		pQuantized += NUM_BYTES;
	}
}
#else
static FORCE_INLINE void PackHighBitsToByte ( const Span_T<uint8_t> & dIn, uint8_t * pOut )
{
	for ( size_t i = 0; i < dIn.size(); i++ )
	{
		if ( dIn[i] & 128 )
			*pOut |= 1;

		if ( ( i & 7 ) == 7 )
			pOut++;
		else
			*pOut <<= 1;
	}
}


void BinaryQuantizer_c::Transpose ( const Span_T<uint8_t> & dQuantized, size_t uDim, Span_T<uint8_t> & dTransposed )
{
	const int NUM_BYTES = 32;
	const int NUM_64_BIT_VALUES = NUM_BYTES >> 3;
	const size_t uDimDiv8 = uDim >> 3;
	uint8_t dTmp[4] = {0}, dSpreadBits[NUM_BYTES] = {0};
	auto pQuantized = dQuantized.data();
	for ( size_t i = 0; i < uDim; i += NUM_BYTES )
	{
		uint64_t * pVal = (uint64_t*)dSpreadBits;
		for ( int j = 0; j < NUM_BYTES; j += 8 )
		{
			uint64_t uVal64 = *(uint64_t*)&pQuantized[j];
			*pVal++ = ( uVal64 << 4 ) | ( ( uVal64 >> 4 ) & 0x0F0F0F0F0F0F0F0FULL );
		}

		const int iDiv8 = i >> 3;
		for ( int j = 0; j < 4; j++ )
		{
			PackHighBitsToByte ( { dSpreadBits, NUM_BYTES }, dTmp );
			auto pTransposed = dTransposed.data() + ( 3 - j ) * uDimDiv8 + iDiv8;
			for ( int k = 0; k < 4; k++ )
				pTransposed[k] = dTmp[k];

			memset ( dTmp, 0, sizeof(dTmp) );

			uint64_t * pVal = (uint64_t*)dSpreadBits;
			for ( int k = 0; k < NUM_64_BIT_VALUES; k++ )
				*pVal++ <<= 1;
		}

		pQuantized += NUM_BYTES;
	}
}
#endif


void BinaryQuantizer_c::Quantize4Bit ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, std::vector<uint8_t> & dResult )
{
	assert ( dVector.size()==dCentroid.size() );

	m_dVecMinusCentroid.resize ( dVector.size() );

	Binary4BitFactors_t tFactors = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };

	for ( size_t i = 0; i < dVector.size(); i++ )
	{
		float fDiff = dVector[i] - dCentroid[i];
		tFactors.m_fDistanceToCentroidSq += fDiff*fDiff;
		m_dVecMinusCentroid[i] = fDiff;
	}

	if ( m_eSimilarity!=HNSWSimilarity_e::L2 )
	{
		tFactors.m_fVecMinusCentroidNorm = VecNormalize(m_dVecMinusCentroid);
		tFactors.m_fVecDotCentroid = VecDot ( dVector, dCentroid );
	}

	float fMax;
	VecMinMax ( m_dVecMinusCentroid, tFactors.m_fMin, fMax );
	tFactors.m_fRange = ( fMax - tFactors.m_fMin ) / 15.0f;

	tFactors.m_fQuantizedSum = (float)Quantize ( m_dVecMinusCentroid, tFactors.m_fMin, tFactors.m_fRange, m_dQuantized );
	PadToDim(m_dQuantized);

	size_t uDataSize = dVector.size() >> 1;
	size_t uHeaderSize = sizeof(float)*6;
	dResult.resize ( uHeaderSize + uDataSize );

	auto pHeader = (Binary4BitFactors_t *)dResult.data();
	*pHeader++ = tFactors;

	Span_T<uint8_t> dTransposed ( (uint8_t*)pHeader, uDataSize );
	Transpose ( m_dQuantized, m_uDimPadded, dTransposed );
}

///////////////////////////////////////////////////////////////////////////////

template <bool BUILD>
class ScalarQuantizerBinary_T : public ScalarQuantizer_i
{
public:
			ScalarQuantizerBinary_T ( HNSWSimilarity_e eSimilarity, const std::string & sTmpFilename );
			ScalarQuantizerBinary_T ( const QuantizationSettings_t & tSettings, HNSWSimilarity_e eSimilarity );

	void	Train ( const Span_T<float> & dPoint ) override;
	bool	FinalizeTraining ( std::string & sError ) override;
	void	Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) override;
	void	FinalizeEncoding() override;
	const QuantizationSettings_t & GetSettings() override;

	std::function<const uint8_t *(uint32_t)> GetPoolFetcher() const override;

private:
	std::unique_ptr<BinaryQuantizer_c> m_pQuantizer;
	QuantizationSettings_t	m_tSettings;
	HNSWSimilarity_e		m_eSimilarity = HNSWSimilarity_e::COSINE;
	std::string				m_sTmpFilename;
	std::vector<double>		m_dCentroid64;
	std::vector<uint8_t>	m_dQuantizedForQuery;
	MappedBuffer_T<uint8_t>	m_tBuffer4Bit;
	size_t		m_uDim = 0;
	bool		m_bTrained = false;
	bool		m_bFinalized = false;
	size_t		m_uTrainedVecs = 0;
	uint32_t	m_uRowId = 0;
	size_t		m_uQuantized4BitEntrySize = 0;
	int64_t		m_iWritten = 0;
};

template <bool BUILD>
ScalarQuantizerBinary_T<BUILD>::ScalarQuantizerBinary_T ( HNSWSimilarity_e eSimilarity, const std::string & sTmpFilename )
	: m_eSimilarity ( eSimilarity )
	, m_sTmpFilename ( sTmpFilename )
{}

template <bool BUILD>
ScalarQuantizerBinary_T<BUILD>::ScalarQuantizerBinary_T ( const QuantizationSettings_t & tSettings, HNSWSimilarity_e eSimilarity )
	: m_tSettings ( tSettings )
	, m_eSimilarity ( eSimilarity )
{
	m_uDim = tSettings.m_dCentroid.size();
	m_bTrained = true;
	m_bFinalized = true;

	m_pQuantizer = std::make_unique<BinaryQuantizer_c> ( m_uDim, eSimilarity );
}

template <bool BUILD>
void ScalarQuantizerBinary_T<BUILD>::Train ( const Span_T<float> & dPoint )
{
	assert ( !m_bFinalized );
	if ( !m_bTrained )
	{
		m_uDim = dPoint.size();
		m_dCentroid64.resize(m_uDim);
		for ( auto & i : m_dCentroid64 )
			i = 0.0;

		m_bTrained = true;
	}
		
	for ( size_t i = 0; i < dPoint.size(); i++ )
		m_dCentroid64[i] += dPoint[i];

	m_uTrainedVecs++;
}

template <bool BUILD>
void ScalarQuantizerBinary_T<BUILD>::Encode ( const Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized )
{
	assert(m_bFinalized);

	m_pQuantizer->Quantize4Bit ( dPoint, m_tSettings.m_dCentroid, BUILD ? m_dQuantizedForQuery : dQuantized );
	if constexpr ( !BUILD )
		return;

	memcpy ( m_tBuffer4Bit.data() + m_iWritten, m_dQuantizedForQuery.data(), m_dQuantizedForQuery.size() );
	m_iWritten += m_dQuantizedForQuery.size();

	m_pQuantizer->Quantize1Bit ( dPoint, m_tSettings.m_dCentroid, dQuantized );
}

template <bool BUILD>
void ScalarQuantizerBinary_T<BUILD>::FinalizeEncoding()
{
	m_tBuffer4Bit.Reset();
	::unlink ( m_sTmpFilename.c_str() );
}

template <bool BUILD>
const QuantizationSettings_t & ScalarQuantizerBinary_T<BUILD>::GetSettings()
{
	std::string sError;
	// fixme! return error
	bool bRes = FinalizeTraining(sError);
	assert(bRes);
	return m_tSettings;
}

template <bool BUILD>
std::function<const uint8_t *(uint32_t)> ScalarQuantizerBinary_T<BUILD>::GetPoolFetcher() const
{
	if constexpr ( !BUILD )
		return nullptr;

	return [this](uint32_t uKey) -> const uint8_t *
		{
			return m_tBuffer4Bit.data() + uint64_t(uKey)*m_uQuantized4BitEntrySize;
		};
}

template <bool BUILD>
bool ScalarQuantizerBinary_T<BUILD>::FinalizeTraining ( std::string & sError )
{
	assert(m_bTrained);
	if ( m_bFinalized )
		return true;

	m_bFinalized = true;

	for ( auto & i : m_dCentroid64 )
		m_tSettings.m_dCentroid.push_back ( i/m_uTrainedVecs );

	m_pQuantizer = std::make_unique<BinaryQuantizer_c> ( m_uDim, m_eSimilarity );

	// quantize a fake vector to get quantized size
	std::vector<float> dTmp ( m_uDim, 0.0f );
	m_pQuantizer->Quantize4Bit ( dTmp, m_tSettings.m_dCentroid, m_dQuantizedForQuery );
	m_uQuantized4BitEntrySize = m_dQuantizedForQuery.size();

	FILE * pFile = fopen ( m_sTmpFilename.c_str(), "wb" );
	if ( !pFile )
	{
		sError = FormatStr ( "Failed to create file '%s'", m_sTmpFilename.c_str() );
		return false;
	}

	int64_t iTmpFileSize = m_uTrainedVecs*m_uQuantized4BitEntrySize;
	fseek ( pFile, iTmpFileSize-1, SEEK_SET );
	fwrite ( "", 1, 1, pFile );
	fclose ( pFile );

	return m_tBuffer4Bit.Open ( m_sTmpFilename.c_str(), true, sError );
}

///////////////////////////////////////////////////////////////////////////////

ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, const QuantizationSettings_t & tQuantSettings, HNSWSimilarity_e eSimilarity )
{
	switch ( eQuantization )
	{
	case Quantization_e::BIT1:	return new ScalarQuantizerBinary_T<false> ( tQuantSettings, eSimilarity );
	case Quantization_e::BIT4:	return new ScalarQuantizer4Bit_c(tQuantSettings);
	case Quantization_e::BIT8:	return new ScalarQuantizer8Bit_c(tQuantSettings);
	default:					return nullptr;
	}
}



ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, HNSWSimilarity_e eSimilarity, const std::string & sTmpFilename )
{
	switch ( eQuantization )
	{
	case Quantization_e::BIT1:	return new ScalarQuantizerBinary_T<true> ( eSimilarity, sTmpFilename );
	case Quantization_e::BIT4:	return new ScalarQuantizer4Bit_c;
	case Quantization_e::BIT8:	return new ScalarQuantizer8Bit_c;
	default:					return nullptr;
	}
}

} // namespace knn
