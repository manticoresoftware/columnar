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
#include <cfloat>
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
	struct IPMetrics_t
	{
		float m_fQuality;
		float m_fVecMinusCentroidNorm;
		float m_fVecDotCentroid;
	};

	size_t				m_uDimPadded = 0;
	HNSWSimilarity_e	m_eSimilarity = HNSWSimilarity_e::COSINE;
	float				m_fSqrtDim = 0.0f;

	SpanResizeable_T<float>		m_dVecMinusCentroid;
	SpanResizeable_T<uint8_t>	m_dQuantized;

	static void		Pack ( const Span_T<float> & dVector, Span_T<uint8_t> & dPacked );
	static int		Quantize ( const Span_T<float> & dVector, float fMin, float fRange, SpanResizeable_T<uint8_t> & dQuantized );
	FORCE_INLINE static void Transpose ( const Span_T<uint8_t> & dQuantized, size_t uDim, Span_T<uint8_t> & dTransposed );

	float			ComputeQuality ( int iOriginalLength, const Span_T<float> & dVecMinusCentroidNormalized, const Span_T<uint8_t> & dPacked ) const;
	float			QuantizeVecL2 ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, Span_T<uint8_t> & dResult );
	IPMetrics_t		QuantizeVecIP ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, Span_T<uint8_t> & dResult );

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
	for ( size_t i = 0; i < dVector.size(); i++ )
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


BinaryQuantizer_c::IPMetrics_t BinaryQuantizer_c::QuantizeVecIP ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, Span_T<uint8_t> & dResult )
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
	return { fQuality, fVecMinusCentroidNorm, fVecDotCentroid };
}


void BinaryQuantizer_c::Quantize1Bit ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, std::vector<uint8_t> & dResult )
{
	size_t uDataSize = ( ( dVector.size()+7 ) >> 3 );
	size_t uHeaderSize = m_eSimilarity==HNSWSimilarity_e::L2 ? sizeof(float)*3 : sizeof(float)*4;
	dResult.resize ( uHeaderSize + uDataSize );
	Span_T<uint8_t> dData { dResult.data()+uHeaderSize, uDataSize };
	auto pHeader = (float*)dResult.data();

	std::vector<float> dCorrections;
	switch ( m_eSimilarity )
	{
	case HNSWSimilarity_e::L2:
		*pHeader++ = VecDist ( dVector, dCentroid );
		*pHeader++ = QuantizeVecL2 ( dVector, dCentroid, dData );
		*pHeader++ = PopCnt(dData);
		break;

	case HNSWSimilarity_e::IP:
	case HNSWSimilarity_e::COSINE:
		{
			IPMetrics_t tMetrics = QuantizeVecIP ( dVector, dCentroid, dData );
			*pHeader++ = tMetrics.m_fQuality;
			*pHeader++ = tMetrics.m_fVecMinusCentroidNorm;
			*pHeader++ = tMetrics.m_fVecDotCentroid;
			*pHeader++ = PopCnt(dData);
		}
		break;

	default:
		assert ( 0 && "Unsupported similarity" );
		break;
	}
}


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


void BinaryQuantizer_c::Quantize4Bit ( const Span_T<float> & dVector, const std::vector<float> & dCentroid, std::vector<uint8_t> & dResult )
{
	assert ( dVector.size()==dCentroid.size() );

	m_dVecMinusCentroid.resize ( dVector.size() );

	float fDistanceToCentroidSq = 0.0f;
	for ( size_t i = 0; i < dVector.size(); i++ )
	{
		float fDiff = dVector[i] - dCentroid[i];
		fDistanceToCentroidSq += fDiff*fDiff;
		m_dVecMinusCentroid[i] = fDiff;
	}

	float fVecMinusCentroidNorm = 0.0f;
	float fVecDotCentroid = 0.0f;
	if ( m_eSimilarity!=HNSWSimilarity_e::L2 )
	{
		fVecMinusCentroidNorm = VecNormalize(m_dVecMinusCentroid);
		fVecDotCentroid = VecDot ( dVector, dCentroid );
	}

	float fMin, fMax;
	VecMinMax ( m_dVecMinusCentroid, fMin, fMax );
	float fRange = ( fMax - fMin ) / 15.0f;

	int iQuantizedSum = Quantize ( m_dVecMinusCentroid, fMin, fRange, m_dQuantized );
	PadToDim(m_dQuantized);

	size_t uDataSize = dVector.size() >> 1;
	size_t uHeaderSize = sizeof(float)*6;
	dResult.resize ( uHeaderSize + uDataSize );

	auto pHeader = (float *)dResult.data();
	*pHeader++ = iQuantizedSum;
	*pHeader++ = fDistanceToCentroidSq;
	*pHeader++ = fMin;
	*pHeader++ = fRange;
	*pHeader++ = fVecMinusCentroidNorm;
	*pHeader++ = fVecDotCentroid;

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
