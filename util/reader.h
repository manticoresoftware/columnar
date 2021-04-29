// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
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

#ifndef _reader_
#define _reader_

#include "util.h"
#include "assert.h"

namespace columnar
{

class FileReader_c
{
public:
							FileReader_c() = default;
	explicit				FileReader_c ( int iFD );
							~FileReader_c() { Close(); }

	bool					Open ( const std::string & sName, std::string & sError );
	void					Close();

	int64_t					GetPos() const { return m_iFilePos+m_tPtr; }
	int						GetFD() const { return m_iFD; }

	void					Read ( uint8_t * pData, size_t tLen );

	FORCE_INLINE uint8_t	Read_uint8()
	{
		if ( m_tPtr>=m_tUsed && !ReadToBuffer() )
			return 0;

		assert ( m_tPtr<m_tUsed );
		return m_pData[m_tPtr++];
	}

	FORCE_INLINE uint16_t	Read_uint16()   { return ReadValue<uint16_t>(); }
	FORCE_INLINE uint32_t	Read_uint32()   { return ReadValue<uint32_t>(); }
	FORCE_INLINE uint64_t	Read_uint64()   { return ReadValue<uint64_t>(); }
	std::string				Read_string();

	uint32_t				Unpack_uint32();
	uint64_t				Unpack_uint64();

	FORCE_INLINE bool		IsError() const { return m_bError; }
	std::string				GetError() const { return m_sError; }

	FORCE_INLINE bool		ReadFromBuffer ( uint8_t * & pData, size_t tLen )
	{
		if ( m_tPtr+tLen > m_tUsed )
			return false;

		pData = m_pData.get()+m_tPtr;
		m_tPtr += tLen;
		return true;
	}

	FORCE_INLINE void		Seek ( int64_t iPos )
	{
		assert ( iPos>=0 );

		if ( iPos>=m_iFilePos && iPos<m_iFilePos+(int64_t)m_tUsed ) // seek inside the buffer?
			m_tPtr = iPos - m_iFilePos;
		else
		{
			m_iFilePos = iPos;
			m_tPtr = m_tUsed = 0;
		}
	}

private:
	static const size_t DEFAULT_SIZE = 1048576;

	int         m_iFD = -1;
	bool        m_bOpened = false;
	std::string m_sFile;

	std::unique_ptr<uint8_t[]> m_pData;
	size_t      m_tSize = DEFAULT_SIZE;
	size_t      m_tUsed = 0;
	size_t      m_tPtr = 0;

	int64_t     m_iFilePos = 0;

	bool        m_bError = false;
	std::string m_sError;

	bool		ReadToBuffer();

	FORCE_INLINE void CreateBuffer()
	{
		if ( m_pData )
			return;

		m_pData = std::unique_ptr<uint8_t[]> ( new uint8_t[m_tSize] );
	}

	FORCE_INLINE void CopyTail ( uint8_t * & pDst, size_t & tLen )
	{
		if ( m_tUsed<=m_tPtr )
			return;

		int iToCopy = int ( m_tUsed-m_tPtr );
		memcpy ( pDst, m_pData.get() + m_tPtr, iToCopy );
		m_tPtr += iToCopy;
		pDst += iToCopy;
		tLen -= iToCopy;
	}

	template <typename T>
	FORCE_INLINE T ReadValue()
	{
		T tValue;
		Read ( (uint8_t*)&tValue, sizeof(T) );
		return IsError() ? (T)0 : tValue;
	}
};

} // namespace columnar

#endif // reader