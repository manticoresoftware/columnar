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

#ifdef _MSC_VER
	#include <intrin.h>
#endif

namespace util
{

template <typename T=uint32_t>
class BitVec_T
{
public:
	explicit BitVec_T ( int iSize ) { Resize(iSize); }

	FORCE_INLINE bool BitGet ( int iBit )
	{
		if ( !GetDataLen() )
			return false;

		assert ( iBit>=0 && iBit<m_iSize );
		return ( ( m_dData [ iBit>>SHIFT ] & ( ( (T)1 )<<( iBit&MASK ) ) )!=0 );
	}

	FORCE_INLINE void BitSet ( int iBit )
	{
		if ( !GetDataLen() )
			return;

		assert ( iBit>=0 && iBit<m_iSize );
		m_dData [ iBit>>SHIFT ] |= ( (T)1 )<<( iBit&MASK );
	}

	void Invert ( int iMinBit=-1, int iMaxBit=-1 )
	{
		if ( !m_iSize )
			return;

		if ( iMinBit<0 )
			iMinBit = 0;

		if ( iMaxBit<0 )
			iMaxBit = m_iSize-1;

		int iMinId = iMinBit>>SHIFT;
		int iMaxId = (iMaxBit+SIZEBITS)>>SHIFT;
		for ( int i = iMinId; i<iMaxId; i++ )
			m_dData[i] = ~m_dData[i];

		if ( ( iMinId<<SHIFT ) != iMinBit )
		{
			int iFirstBit = iMinId << SHIFT;
			m_dData[iMinId] &= ~((T(1) << ( iMinBit-iFirstBit )) - 1);
		}

		if ( ( iMaxId<<SHIFT ) != iMaxBit )
		{
			int iFirstBit = (iMaxId-1) << SHIFT;
			m_dData[iMaxId-1] &= (T(1) << ( iMaxBit-iFirstBit+1 )) - 1;
		}
	}

	template <typename RESULT>
	void Fetch ( int & iIterator, int iBase, RESULT * & pRes, RESULT * pMax )
	{
		assert ( sizeof(T)==8 );	// this func should not be used with 32-bit based bitmaps

		const T * pDataStart = &m_dData.front();
		const T * pData = pDataStart + iIterator;
		const T * pDataMax = pDataStart + GetDataLen();

		pMax -= SIZEBITS;
		assert ( pMax>=pRes );

		RESULT tOutStart = ( iIterator << SHIFT ) + iBase;
		for ( ; pRes<pMax && pData<pDataMax; pData++ )
		{
			uint64_t tVal = *pData;
			while ( tVal )
			{
				unsigned long uIdx;
#ifdef _MSC_VER
				::_BitScanForward64 ( &uIdx, tVal );
#else
				uIdx = __builtin_ctzll(tVal);
#endif
				*pRes++ = uIdx + tOutStart;

				tVal ^= tVal & (~tVal + 1);
			}

			tOutStart += 64;
		}

		iIterator = pData - pDataStart;
	}

	int Scan ( int iStart )
	{
		if ( iStart>=m_iSize )
			return m_iSize;

		const T * pData = &m_dData.front();
		int iIndex = iStart>>SHIFT;
		T uMask = ~( ( T(1)<<( iStart&MASK ) )-1 );
		if ( pData[iIndex] & uMask )
			return (iIndex<<SHIFT) + ScanBit ( pData[iIndex], iStart&MASK );

		iIndex++;
		while ( iIndex<(int)GetDataLen() && !pData[iIndex] )
			iIndex++;

		if ( iIndex>=(int)GetDataLen() )
			return m_iSize;

		return (iIndex<<SHIFT) + ScanBit ( pData[iIndex], 0 );
	}

	void SetAllBits() { std::fill ( m_dData.begin(), m_dData.end(), (T)0xffffffffffffffffULL ); }
	void Resize ( int iSize )
	{
		m_iSize = iSize;
		m_iDataLen = ( iSize+SIZEBITS-1 )/SIZEBITS;
		if ( m_iDataLen > m_dData.size() )
			m_dData = std::vector<T> ( m_iDataLen, 0 );
	}

	int	GetLength() const { return m_iSize; }
	const std::vector<T> & GetData() const { return m_dData; }

private:
	static const size_t	SIZEBITS = sizeof(T)*8;
	static const T		MASK = T(sizeof(T)*8 - 1);
	static constexpr T	SHIFT = T(Log2(SIZEBITS)-1);

	std::vector<T>	m_dData;
	int				m_iSize = 0;
	int				m_iDataLen = 0;

	FORCE_INLINE int ScanBit ( T tData, int iStart )
	{
		for ( int i = iStart; i < SIZEBITS; i++ )
			if ( tData & ( (T)1<<i ) )
				return i;

		return -1;
	}

	FORCE_INLINE int GetDataLen() const { return m_iDataLen; }
};

using BitVec_c = BitVec_T<>;

} // namespace util
