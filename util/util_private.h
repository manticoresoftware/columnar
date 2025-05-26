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

#include "util.h"

#include <cmath>
#include <limits>

#if defined(USE_SIMDE)
	#define SIMDE_ENABLE_NATIVE_ALIASES 1
	#include <simde/x86/sse4.2.h>
#elif _MSC_VER
	#include <intrin.h>
#else
	#include <x86intrin.h>
	#include <nmmintrin.h>
#endif

namespace util
{

const uint64_t STR_HASH_SEED = 0xCBF29CE484222325ULL;

class ByteCodec_c
{
public:
	template <typename T>
	static FORCE_INLINE uint8_t * PackData ( const Span_T<T> & dData )
	{
		size_t tSizeBytes = dData.size()*sizeof(T);
		uint8_t * pResult = nullptr;
		uint8_t * pData = nullptr;
		std::tie ( pResult, pData ) = PackData(tSizeBytes);
		memcpy ( pData, dData.begin(), tSizeBytes );
		return pResult;
	}

	static FORCE_INLINE std::pair<uint8_t *,uint8_t *> PackData ( size_t tDataLen )
	{
		uint8_t dPacked[16];
		uint8_t * pPacked = dPacked;
		int iPackedLen = Pack_uint64 ( pPacked, tDataLen );
		uint8_t * pResult = new uint8_t[iPackedLen + tDataLen];
		memcpy ( pResult, dPacked, iPackedLen );
		return { pResult, pResult+iPackedLen };
	}

	static FORCE_INLINE void PackData ( std::vector<uint8_t> & dPacked, const Span_T<uint8_t> & dData )
	{
		dPacked.resize ( dData.size() + CalcPackedLen (dData.size() ) );
		uint8_t * p = dPacked.data();
		Pack_uint64 ( p, dData.size() );
		memcpy ( p, dData.begin(), dData.size() );
	}

	template <typename T>
	static FORCE_INLINE int CalcPackedLen ( T tValue )
	{
		int iNumBytes = 1;
		uint64_t uValue = (uint64_t)tValue;
		uValue >>= 7;
		while ( uValue )
		{
			uValue >>= 7;
			iNumBytes++;
		}

		return iNumBytes;
	}

	static FORCE_INLINE int Pack_uint32 ( uint8_t * & pOut, uint32_t uValue )
	{
		return EncodeValue ( pOut, uValue );
	}

	static FORCE_INLINE int Pack_uint64 ( uint8_t * & pOut, uint64_t uValue )
	{
		return EncodeValue ( pOut, uValue );
	}

	template <typename GET>
	static FORCE_INLINE uint32_t Unpack_uint32 ( GET && fnGetByte )
	{
		return DecodeValue<uint32_t>(fnGetByte);
	}

	template <typename GET>
	static FORCE_INLINE uint64_t Unpack_uint64 ( GET && fnGetByte )
	{
		return DecodeValue<uint64_t>(fnGetByte);
	}

	template <typename T>
	static FORCE_INLINE int EncodeValue ( uint8_t * & pOut, T tValue )
	{
		int iNumBytes = CalcPackedLen(tValue);
		for ( int i = iNumBytes-1; i>=0; i-- )
		{
			uint8_t uEncoded = ( tValue >> ( 7*i ) ) & 0x7f;
			if ( i )
				uEncoded |= 0x80;

			*pOut++ = uEncoded;
		}

		return iNumBytes;
	}

private:
	template <typename T, typename GET>
	static FORCE_INLINE T DecodeValue ( GET && fnGetByte )
	{
		uint8_t uByte = fnGetByte();
		T tValue = 0;
		while ( uByte & 0x80 )
		{
			tValue = ( tValue << 7 ) | ( uByte & 0x7f );
			uByte = fnGetByte();
		}

		return ( tValue << 7 ) | uByte;
	}
};


class FileWriterTraits_c
{
public:
	std::string GetFilename() const { return m_sFile; }

	bool        IsError() const		{ return m_bError; }
	std::string GetError() const	{ return m_sError; }

protected:
	int         m_iFD = -1;
	int64_t     m_iFilePos = 0;
	bool		m_bTemporary = false; // whatever to unlink file at writer destructor

	std::string	m_sFile;
	bool        m_bError = false;
	std::string m_sError;

	int			GetFileFlags ( bool bNewFile, bool bAppend ) const;
};


class FileWriter_c : public FileWriterTraits_c
{
public:
				~FileWriter_c();

	bool        Open ( const std::string & sFile, bool bNewFile, bool bAppend, bool bTmp, std::string & sError );
	bool        Open ( const std::string & sFile, std::string & sError );
	void        Close();
	void        Unlink();
	void		SetBufferSize ( size_t tBufferSize );

	template <typename T> void Write ( const T & tValue ) { Write ( (const uint8_t *)&tValue, sizeof(tValue) ); }
	void        Write ( const uint8_t * pData, size_t tLength );
	void        SeekAndWrite ( int64_t iOffset, uint64_t uValue );
	void        Seek ( int64_t iOffset );
	void        Write_string ( const std::string & sStr );
	void        Write_uint8 ( uint8_t uValue )      { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }
	void        Write_uint16 ( uint16_t uValue )    { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }
	void        Write_uint32 ( uint32_t uValue )    { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }
	void        Write_uint64 ( uint64_t uValue )    { Write ( (uint8_t*)&uValue, sizeof(uValue) ); }

	void        Pack_uint32 ( uint32_t uValue ) { PackValue(uValue); }
	void        Pack_uint64 ( uint64_t uValue ) { PackValue(uValue); }

	int64_t     GetPos() const { return m_iFilePos + (int64_t)m_tUsed; }

private:
	static const size_t DEFAULT_SIZE = 1048576;

	std::unique_ptr<uint8_t[]> m_pData;

	size_t      m_tSize = DEFAULT_SIZE;
	size_t      m_tUsed = 0;

	void        Flush();

	template <typename T>
	void PackValue ( T uValue )
	{
		uint8_t dPacked[16];
		uint8_t * pOut = dPacked;
		int iPackedLen = ByteCodec_c::EncodeValue ( pOut, uValue );
		Write ( dPacked, iPackedLen );
	}
};


class FileWriterNonBuffered_c : public FileWriterTraits_c
{
public:
				~FileWriterNonBuffered_c();

	bool        Open ( const std::string & sFile, bool bNewFile, bool bAppend, bool bTmp, std::string & sError );
	void        Close();
	void        Unlink();

	void        Write ( const uint8_t * pData, size_t tLength );
	void        Seek ( int64_t iOffset );
	int64_t     GetPos() const { return m_iFilePos; }
};


class MemWriter_c
{
public:
	MemWriter_c ( std::vector<uint8_t> & dData );

	void    Write ( const uint8_t * pData, size_t tSize );
	int64_t GetPos() const { return (int64_t)m_dData.size(); }

	void    Write_uint8 ( uint8_t uValue ) { m_dData.push_back(uValue); }
	void    Write_uint16 ( uint16_t uValue ) { WriteValue(uValue); }
	void    Write_uint32 ( uint64_t uValue ) { WriteValue(uValue); }
	void    Write_uint64 ( uint64_t uValue ) { WriteValue(uValue); }

	void    Pack_uint32 ( uint32_t uValue ) { PackValue(uValue); }
	void    Pack_uint64 ( uint64_t uValue ) { PackValue(uValue); }

private:
	std::vector<uint8_t> & m_dData;

	template <typename T>
	void    WriteValue ( T tValue ) { Write ( (const uint8_t*)&tValue, sizeof(tValue) ); }

	template <typename T>
	void PackValue ( T uValue )
	{
		uint8_t dPacked[16];
		uint8_t * pOut = dPacked;
		int iPackedLen = ByteCodec_c::EncodeValue ( pOut, uValue );
		size_t tOldSize = m_dData.size();
		m_dData.resize ( tOldSize+iPackedLen );
		memcpy ( &m_dData[tOldSize], dPacked, iPackedLen );
	}
};

template<typename ... Args>
std::string FormatStr ( const std::string & sFormat, Args ... args )
{
	int iSize = snprintf ( nullptr, 0, sFormat.c_str(), args ... ) + 1;
	if ( iSize<=0 )
		return "";

	std::unique_ptr<char[]> pBuf( new char[iSize] );
	snprintf ( pBuf.get(), iSize, sFormat.c_str(), args ... );
	return std::string ( pBuf.get(), pBuf.get()+iSize-1 );
}


inline uint32_t FloatToUint ( float fValue )
{
	union { float m_fValue; uint32_t m_uValue; } tUnion;
	tUnion.m_fValue = fValue;
	return tUnion.m_uValue;
}


inline float UintToFloat ( uint32_t uValue )
{
	union { float m_fValue; uint32_t m_uValue; } tUnion;
	tUnion.m_uValue = uValue;
	return tUnion.m_fValue;
}

template <typename T>
constexpr auto to_underlying(T t) noexcept
{
	return static_cast<typename std::underlying_type<T>::type>(t);
}

template <typename T>
inline T to_type ( int64_t iValue )
{
	return (T)iValue;
}

template <>
inline float to_type<float> ( int64_t iValue )
{
	return UintToFloat ( (uint32_t)iValue );
}

template<class T, class CONTAINER>
const T * binary_search ( const CONTAINER & dValues, const T & tValue )
{
	auto tFirst = dValues.begin();
	auto tLast = dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, tValue );
	if ( tFound==tLast || tValue < *tFound )
		return nullptr;

	return &(*tFound);
}

template<typename VEC, typename T>
int binary_search_idx ( const VEC & dValues, const T & tValue )
{
	auto tFirst = dValues.begin();
	auto tLast = dValues.end();
	auto tFound = std::lower_bound ( tFirst, tLast, tValue );

	if ( tFound!=tLast && tValue==*tFound )
		return tFound-tFirst;

	return -1;
}

template<typename VEC>
void VectorReset ( VEC & dData )
{
	dData.clear();
	dData.shrink_to_fit();
}

template<typename VEC, typename WRITER>
void WriteVector ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename VEC, typename WRITER>
void WriteVectorLen ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Pack_uint64 ( dData.size() );
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename VEC, typename WRITER>
void WriteVectorLen32 ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Pack_uint32 ( (uint32_t)dData.size() );
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename VEC, typename WRITER>
void WriteVectorRawLen32 ( const VEC & dData, WRITER & tWriter )
{
	tWriter.Write_uint32 ( (uint32_t)dData.size() );
	tWriter.Write ( (const uint8_t *)dData.data(), sizeof ( dData[0] ) * dData.size() );
}

template<typename WRITER>
void WriteVectorPacked ( const std::vector<uint64_t> & dData, WRITER & tWriter )
{
	tWriter.Pack_uint32 ( (uint32_t)dData.size() );
	for ( uint64_t tVal : dData )
		tWriter.Pack_uint64 ( tVal );
}

template <typename T>
constexpr int Log2 ( T tValue )
{
	int iBits = 0;
	while ( tValue )
	{
		tValue >>= 1;
		iBits++;
	}

	return iBits;
}

int     CalcNumBits ( uint64_t uNumber );
bool    CopySingleFile ( const std::string & sSource, const std::string & sDest, std::string & sError, int iMode, size_t tBufferSize=1048576 );
bool	FloatEqual ( float fA, float fB );

FORCE_INLINE float VecCalcNorm ( const Span_T<float> & dData )
{
	size_t uSize = dData.size();
	size_t i = 0;
	float fNorm = 0.0f;

#if defined (USE_AVX2)
	__m256 iNormVec = _mm256_setzero_ps();
	size_t uLimit = uSize & ~7;
	for ( ; i < uLimit; i += 8 )
	{
		__m256 iVec = _mm256_loadu_ps ( &dData[i] );
		iNormVec = _mm256_fmadd_ps ( iVec, iVec, iNormVec );
	}

	__m128 iLow = _mm256_extractf128_ps ( iNormVec, 0 );
	__m128 iHigh = _mm256_extractf128_ps ( iNormVec, 1 );
	__m128 iSum = _mm_add_ps ( iLow, iHigh );

	__m128 iShuf = _mm_movehdup_ps(iSum);
	__m128 iSums = _mm_add_ps ( iSum, iShuf );
	iShuf = _mm_movehl_ps ( iShuf, iSums );
	iSums = _mm_add_ss ( iSums, iShuf );
	fNorm = _mm_cvtss_f32(iSums);
#endif

	for ( ; i < uSize; i++ )
		fNorm += dData[i]*dData[i];

	return sqrtf(fNorm);
}

FORCE_INLINE float VecNormalize ( Span_T<float> & dData )
{
	float fNorm = VecCalcNorm(dData);
	float fDiv = 1.0f / (fNorm + 1e-30f);
	size_t i = 0;
	size_t uSize = dData.size();

#if defined (USE_AVX2)
	__m256 iDivVec = _mm256_set1_ps(fDiv);
	size_t uLimit = uSize & ~7;
	for ( ; i < uLimit; i += 8 )
	{
		__m256 iVec = _mm256_loadu_ps ( &dData[i] );
		__m256 iResult = _mm256_fmadd_ps ( iVec, iDivVec, _mm256_setzero_ps() );
		_mm256_storeu_ps ( &dData[i], iResult );
	}
#endif

	for ( ; i < uSize; i++ )
		dData[i] *= fDiv;

	return fNorm;
}

template <typename T1, typename T2>
FORCE_INLINE float VecDot ( const T1 & dVec1, const T2 & dVec2 )
{
	assert ( dVec1.size()==dVec2.size() );
	const size_t uSize = dVec1.size();
	size_t i = 0;
	float fDot = 0.0f;

#if defined (USE_AVX2)
	__m256 iDotVec = _mm256_setzero_ps();
	size_t uLimit = uSize & ~7;
	for ( ; i < uLimit; i += 8 )
	{
		__m256 iVec1 = _mm256_loadu_ps ( &dVec1[i] );
		__m256 iVec2 = _mm256_loadu_ps ( &dVec2[i] );
		iDotVec = _mm256_fmadd_ps ( iVec1, iVec2, iDotVec );
	}

	__m128 iLow = _mm256_extractf128_ps ( iDotVec, 0 );
	__m128 iHigh = _mm256_extractf128_ps ( iDotVec, 1 );
	__m128 iSum = _mm_add_ps ( iLow, iHigh );

	__m128 iShuf = _mm_movehdup_ps(iSum);
	__m128 iSums = _mm_add_ps ( iSum, iShuf );
	iShuf = _mm_movehl_ps ( iShuf, iSums );
	iSums = _mm_add_ss ( iSums, iShuf );
	fDot = _mm_cvtss_f32 ( iSums );
#endif

	for ( ; i < uSize; i++ )
		fDot += dVec1[i]*dVec2[i];

	return fDot;
}

FORCE_INLINE void VecMinMax ( const Span_T<float> & dVec, float & fMin, float & fMax )
{
	const size_t uSize = dVec.size();
	fMin = FLT_MAX;
	fMax = -FLT_MAX;
	size_t i = 0;

#if defined (USE_AVX2)
	__m256 iMinVec = _mm256_set1_ps(FLT_MAX);
	__m256 iMaxVec = _mm256_set1_ps(-FLT_MAX);
	size_t uLimit = uSize & ~7;
	for ( ; i < uLimit; i += 8 )
	{
		__m256 iVec = _mm256_loadu_ps ( &dVec[i] );
		iMinVec = _mm256_min_ps ( iMinVec, iVec );
		iMaxVec = _mm256_max_ps ( iMaxVec, iVec );
	}

	__m128 iLow = _mm256_extractf128_ps ( iMinVec, 0 );
	__m128 iHigh = _mm256_extractf128_ps ( iMinVec, 1 );
	__m128 iMin = _mm_min_ps ( iLow, iHigh );
	iLow = _mm_movehdup_ps(iMin);
	iMin = _mm_min_ps ( iMin, iLow );
	iLow = _mm_movehl_ps ( iLow, iMin );
	iMin = _mm_min_ss ( iMin, iLow );
	fMin = _mm_cvtss_f32(iMin);

	iLow = _mm256_extractf128_ps ( iMaxVec, 0 );
	iHigh = _mm256_extractf128_ps ( iMaxVec, 1 );
	__m128 iMax = _mm_max_ps ( iLow, iHigh );
	iLow = _mm_movehdup_ps(iMax);
	iMax = _mm_max_ps ( iMax, iLow );
	iLow = _mm_movehl_ps ( iLow, iMax );
	iMax = _mm_max_ss ( iMax, iLow );
	fMax = _mm_cvtss_f32(iMax);
#endif

	for ( ; i < uSize; i++ )
	{
		fMin = std::min ( fMin, dVec[i] );
		fMax = std::max ( fMax, dVec[i] );
	}
}

template <typename T1, typename T2>
FORCE_INLINE float VecDist ( const T1 & dVec1, const T2 & dVec2 )
{
	return sqrtf ( VecDistSq ( dVec1, dVec2 ) );
}

template <typename T1, typename T2>
FORCE_INLINE float VecDistSq ( const T1 & dVec1, const T2 & dVec2 )
{
	assert ( dVec1.size()==dVec2.size() );

	float fDist = 0.0f;
	for ( size_t i = 0; i < dVec1.size(); i++ )
	{
		float fDiff = dVec1[i]-dVec2[i];
		fDist += fDiff * fDiff;
	}

	return fDist;
}

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

} // namespace util