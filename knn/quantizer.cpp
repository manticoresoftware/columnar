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

#include "quantizer.h"

#include <float.h>
#include <algorithm>
#include <cmath>

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

	void	Train ( const util::Span_T<float> & dPoint ) override;
	void	Encode ( const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) override;
	const QuantizationSettings_t & GetSettings() override;

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
	virtual void		FinalizeTraining();
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


void ScalarQuantizer8Bit_c::Train ( const util::Span_T<float> & dPoint )
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


void ScalarQuantizer8Bit_c::FinalizeTraining()
{
	assert(m_bTrained);
	if ( m_bFinalized )
		return;

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
}


void ScalarQuantizer8Bit_c::Encode ( const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized )
{
	FinalizeTraining();

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
	FinalizeTraining();
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

	void	Encode ( const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) override;

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


void ScalarQuantizer4Bit_c::Encode ( const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized )
{
	FinalizeTraining();

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
	void	Encode ( const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized ) override;

private:
	HNSWSimilarity_e m_eSimilarity = HNSWSimilarity_e::L2;
};

template <bool COSINE>
void ScalarQuantizer1Bit_T<COSINE>::Encode ( const util::Span_T<float> & dPoint, std::vector<uint8_t> & dQuantized )
{
	FinalizeTraining();

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

ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, const QuantizationSettings_t & tQuantSettings, HNSWSimilarity_e eSimilarity )
{
	switch ( eQuantization )
	{
	case Quantization_e::BIT1:
//		if ( eSimilarity==HNSWSimilarity_e::COSINE )
			return new ScalarQuantizer1Bit_T<true>(tQuantSettings);
//		else
//			return new ScalarQuantizer1Bit_T<false>(tQuantSettings);

	case Quantization_e::BIT4:	return new ScalarQuantizer4Bit_c(tQuantSettings);
	case Quantization_e::BIT8:	return new ScalarQuantizer8Bit_c(tQuantSettings);
	default:					return nullptr;
	}
}


ScalarQuantizer_i * CreateQuantizer ( Quantization_e eQuantization, HNSWSimilarity_e eSimilarity )
{
	switch ( eQuantization )
	{
	case Quantization_e::BIT1:
//		if ( eSimilarity==HNSWSimilarity_e::COSINE )
			return new ScalarQuantizer1Bit_T<true>;
//		else
//			return new ScalarQuantizer1Bit_T<false>;

	case Quantization_e::BIT4:	return new ScalarQuantizer4Bit_c;
	case Quantization_e::BIT8:	return new ScalarQuantizer8Bit_c;
	default:					return nullptr;
	}
}

} // namespace knn
