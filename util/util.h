// Copyright (c) 2020-2024, Manticore Software LTD (https://manticoresearch.com)
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
// If you make any significant changes to this file, you MUST bump the LIB_VERSION in columnar.h or secondary.h

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <array>
#include <memory>
#include <type_traits>
#include <fcntl.h>
#include <climits>
#include <cassert>

namespace util
{

#ifdef _MSC_VER
	#define DLLEXPORT __declspec(dllexport)
#else
	#define O_BINARY 0
	#define DLLEXPORT
#endif

#ifndef FORCE_INLINE
	#ifndef NDEBUG
		#define FORCE_INLINE inline
	#else
		#ifdef _MSC_VER
			#define FORCE_INLINE __forceinline
		#else
			#if defined (__cplusplus) || defined (__STDC_VERSION__) && __STDC_VERSION__ >= 199901L   /* C99 */
				#ifdef __GNUC__
					#define FORCE_INLINE inline __attribute__((always_inline))
				#else
					#define FORCE_INLINE inline
				#endif
			#else
				#define FORCE_INLINE
			#endif
		#endif
	#endif
#endif

template<typename T>
class Span_T
{
public:
	Span_T() = default;

	Span_T ( T * pData, size_t tLength )
		: m_pData ( pData )
		, m_tLength ( tLength )
	{}

	Span_T ( std::vector<T> & dVec )
		: m_pData ( dVec.data() )
		, m_tLength ( dVec.size() )
	{}

	T *     data() const    { return m_pData; }
	T &     front() const   { return *m_pData; }
	T &     back() const    { return *(m_pData+m_tLength-1); }
	T *     begin() const   { return m_pData; }
	T *     end() const     { return m_pData+m_tLength; }
	size_t  size() const    { return m_tLength; }
	bool    empty() const   { return m_tLength==0; }
	T & operator [] ( size_t i )
	{
		assert ( i < m_tLength );
		return m_pData[i];
	}

	const T & operator [] ( size_t i ) const
	{
		assert ( i < m_tLength );
		return m_pData[i];
	}

protected:
	T *		m_pData = nullptr;
	size_t	m_tLength = 0; 
};


template<typename T>
class SpanResizeable_T : public Span_T<T>
{
	using BASE = Span_T<T>;

public:
	FORCE_INLINE void resize ( size_t tLength )
	{
		if ( tLength>m_tMaxLength )
		{
			m_tMaxLength = tLength;
			m_dData.resize(m_tMaxLength);
			BASE::m_pData = m_dData.data();
		}

		BASE::m_tLength = tLength;
	}

	FORCE_INLINE void resize ( size_t tLength, T tValue )
	{
		if ( tLength>m_tMaxLength )
		{
			m_tMaxLength = tLength;
			m_dData.resize ( m_tMaxLength, tValue );
			BASE::m_pData = m_dData.data();
		}
		else
			memset ( BASE::m_pData + tLength, 0, m_tMaxLength - tLength );

		BASE::m_tLength = tLength;
	}

private:
	std::vector<T>	m_dData;
	size_t			m_tMaxLength = 0;
};

} // namespace util
