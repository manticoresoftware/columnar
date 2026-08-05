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

#include "util_private.h"
#include <cassert>

namespace util
{

template <typename D>
class ReaderBase_T
{
public:
	int64_t					GetPos() const			{ return m_iFilePos+m_tPtr; }
	size_t					GetBufferSize() const	{ return m_tSize; }
	const std::string &		GetFilename() const		{ return m_sFile; }

	template <typename T> void Read ( T & tValue )	{ static_cast<D*>(this)->Read ( (uint8_t *)&tValue, sizeof(tValue) ); }

	FORCE_INLINE uint8_t	Read_uint8()
	{
		if ( m_tPtr>=m_tUsed && !Refill() )
			return 0;

		assert ( m_tPtr<m_tUsed );
		return m_pBuf[m_tPtr++];
	}

	FORCE_INLINE uint16_t	Read_uint16()   { return ReadValue<uint16_t>(); }
	FORCE_INLINE uint32_t	Read_uint32()   { return ReadValue<uint32_t>(); }
 	FORCE_INLINE uint64_t	Read_uint64()   { return ReadValue<uint64_t>(); }

	std::string				Read_string()
	{
		uint32_t uLen = Read_uint32();
		if ( !uLen )
			return "";

		std::unique_ptr<char[]> pBuffer ( new char[uLen+1] );
		static_cast<D*>(this)->Read ( (uint8_t*)pBuffer.get(), uLen );
		pBuffer[uLen] = '\0';
		return std::string ( pBuffer.get() );
	}

	FORCE_INLINE uint32_t	Unpack_uint32()	{ return ByteCodec_c::Unpack_uint32 ( [this](){ return Read_uint8(); } ); }
	FORCE_INLINE uint64_t	Unpack_uint64()	{ return ByteCodec_c::Unpack_uint64 ( [this](){ return Read_uint8(); } ); }

	FORCE_INLINE bool		IsError() const { return m_bError; }
	std::string				GetError() const { return m_sError; }

	FORCE_INLINE bool		ReadFromBuffer ( uint8_t * & pData, size_t tLen )
	{
		if ( m_tPtr+tLen > m_tUsed )
			return false;

		pData = m_pBuf+m_tPtr;
		m_tPtr += tLen;
		return true;
	}

	FORCE_INLINE void		Seek ( int64_t iPos )
	{
		assert ( iPos>=0 );

		if ( iPos>=m_iFilePos && iPos<m_iFilePos+(int64_t)m_tUsed ) // seek inside the current window?
			m_tPtr = iPos - m_iFilePos;
		else
		{
			m_iFilePos = iPos;
			m_tPtr = m_tUsed = 0;
		}
	}

protected:
	static const size_t DEFAULT_SIZE = 65536;

	std::string m_sFile;

	uint8_t *   m_pBuf = nullptr;			// active window base: owned heap buffer (file) or mmap base (mapped)
	size_t      m_tSize = DEFAULT_SIZE;		// chunk/window size: refill chunk (file) or whole file (mapped)
	size_t      m_tUsed = 0;
	size_t      m_tPtr = 0;
	int64_t     m_iFilePos = 0;

	bool        m_bError = false;
	std::string m_sError;

	FORCE_INLINE bool Refill() { return static_cast<D*>(this)->DoRefill(); }

	template <typename T>
	FORCE_INLINE T ReadValue()
	{
		T tValue;
		static_cast<D*>(this)->Read ( (uint8_t*)&tValue, sizeof(T) );
		return IsError() ? (T)0 : tValue;
	}
};


class FileReader_c : public ReaderBase_T<FileReader_c>
{
public:
	static constexpr bool	IS_MAPPED = false;

							FileReader_c() = default;
	explicit				FileReader_c ( int iFD, size_t tBufferSize = DEFAULT_SIZE );
							~FileReader_c() { Close(); }

	bool					Open ( const std::string & sName, std::string & sError );
	bool					Open ( const std::string & sName, int iBufSize, std::string & sError );
	void					Close();

	int						GetFD() const			{ return m_iFD; }
	int64_t					GetFileSize();

	std::shared_ptr<FileReader_c> Clone() const { return std::make_shared<FileReader_c> ( m_iFD, GetBufferSize() ); }

	using ReaderBase_T<FileReader_c>::Read;
	void					Read ( uint8_t * pData, size_t tLen );
	bool					DoRefill();

private:
	int         m_iFD = -1;
	bool        m_bOpened = false;
	std::unique_ptr<uint8_t[]> m_pData;

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
		memcpy ( pDst, m_pBuf + m_tPtr, iToCopy );
		m_tPtr += iToCopy;
		pDst += iToCopy;
		tLen -= iToCopy;
	}
};


class MappedReader_c : public ReaderBase_T<MappedReader_c>
{
public:
	static constexpr bool	IS_MAPPED = true;

	MappedReader_c ( uint8_t * pMap, int64_t iSize )
	{
		m_pBuf = pMap;
		m_tSize = m_tUsed = (size_t)iSize;
	}

	std::shared_ptr<MappedReader_c> Clone() const { return std::make_shared<MappedReader_c> ( m_pBuf, (int64_t)m_tUsed ); }

	FORCE_INLINE void Seek ( int64_t iPos )
	{
		assert ( iPos>=0 );
		assert ( (size_t)iPos<=m_tUsed && "corrupt offset: seek past the end of the mapping" );
		m_tPtr = (size_t)iPos;
	}

	FORCE_INLINE const uint8_t * GetCurPtrAndSkip ( size_t tLen )
	{
		assert ( m_tPtr<=m_tUsed );
		assert ( tLen<=m_tUsed-m_tPtr && "corrupt length: block extends past the end of the mapping" );

		const uint8_t * pRes = m_pBuf + m_tPtr;
		m_tPtr += tLen;
		return pRes;
	}

	using ReaderBase_T<MappedReader_c>::Read;

	FORCE_INLINE void Read ( uint8_t * pData, size_t tLen )
	{
		if ( m_tPtr+tLen > m_tUsed )
		{
			m_bError = true;
			m_sError = "read past end of mapped file";
			return;
		}

		memcpy ( pData, m_pBuf+m_tPtr, tLen );
		m_tPtr += tLen;
	}

	FORCE_INLINE bool DoRefill()
	{
		m_bError = true;
		m_sError = "read past end of mapped file";
		return false;
	}
};


int PreadWrapper ( int iFD, void * pBuf, size_t tCount, int64_t iOff );

void ReadVectorPacked ( std::vector<uint64_t> & dData, FileReader_c & tReader );

template<typename VEC, typename RD>
FORCE_INLINE void ReadVectorLen32 ( VEC & dData, RD & tReader )
{
	uint32_t uLen =  tReader.Unpack_uint32();
	dData.resize(uLen);
	tReader.Read ( (uint8_t *)( dData.data() ), sizeof ( dData[0] ) * uLen );
}

template<typename VEC, typename RD>
FORCE_INLINE void ReadVectorLen32 ( VEC & dData, RD & tReader, size_t tTail )
{
	uint32_t uLen =  tReader.Unpack_uint32();
	dData.resize_with_padding ( uLen, tTail );
	tReader.Read ( (uint8_t *)( dData.data() ), sizeof ( dData[0] ) * uLen );
}

template<typename T, typename RD>
FORCE_INLINE void SkipVectorLen32 ( RD & tReader )
{
	uint32_t uLen =  tReader.Unpack_uint32();
	tReader.Seek ( tReader.GetPos() + uLen*sizeof(T) );
}

template<typename VEC, typename RD>
void ReadVectorData ( VEC & dData, RD & tReader )
{
	size_t uLen = dData.size();
	tReader.Read ( (uint8_t *)( dData.data() ), sizeof ( dData[0] ) * uLen );
}

template<typename RD>
FORCE_INLINE Span_T<uint32_t> ReadCompressedSpan ( RD & tReader, SpanResizeable_T<uint32_t> & dScratch, size_t tWords, bool bZeroCopyOk )
{
	assert ( tWords<=UINT32_MAX );
	static_assert ( sizeof(size_t)>=sizeof(uint64_t), "ReadCompressedSpan assumes a 64-bit size_t" );
	const size_t tBytes = tWords*sizeof(uint32_t);

	if constexpr ( RD::IS_MAPPED )
	{
		if ( bZeroCopyOk )
			return Span_T<uint32_t> ( (uint32_t*)tReader.GetCurPtrAndSkip ( tBytes ), tWords );
	}

	dScratch.resize_with_padding ( tWords, ( SVB_PADDING_BYTES + sizeof(uint32_t)-1 ) / sizeof(uint32_t) );
	tReader.Read ( (uint8_t*)dScratch.data(), tBytes );
	return Span_T<uint32_t> ( dScratch.data(), tWords );
}

template<typename RD>
FORCE_INLINE Span_T<uint32_t> ReadCompressedSpanLen32 ( RD & tReader, SpanResizeable_T<uint32_t> & dScratch, bool bZeroCopyOk )
{
	uint32_t uLen = tReader.Unpack_uint32();
	return ReadCompressedSpan ( tReader, dScratch, uLen, bZeroCopyOk );
}

int64_t GetFileSize ( int iFD, std::string * sError );
bool IsFileExists ( const std::string & sName );

class MappedBuffer_i
{
public:
					MappedBuffer_i() = default;
	virtual			~MappedBuffer_i() = default;

	virtual bool	Open ( const std::string & sFile, bool bWrite, std::string & sError ) = 0;
	virtual void	Close () {};

	virtual void *	GetPtr () const = 0;
	virtual size_t	GetLengthBytes () const = 0;
	virtual const char * GetFileName() const = 0;
};

MappedBuffer_i * CreateMappedBuffer();


template < typename T >
class MappedBuffer_T
{
public:

					MappedBuffer_T () = default;
					~MappedBuffer_T() { Reset(); }

	bool			Open ( const std::string & sFile, bool bWrite, std::string & sError ) { return m_pBuf->Open ( sFile, bWrite, sError ); }
	void			Reset() { m_pBuf->Close(); }

	const char *	GetFileName() const	{ return m_pBuf->GetFileName(); }
	T *				data() const		{ return (T *)m_pBuf->GetPtr(); }
	T *				begin() const		{ return (T *)m_pBuf->GetPtr(); }
	T *				end() const			{ return (T *)m_pBuf->GetPtr() + size(); }
	size_t			size() const		{ return m_pBuf->GetLengthBytes() / sizeof(T); }

private:
	std::unique_ptr<MappedBuffer_i> m_pBuf { CreateMappedBuffer() };
};

} // namespace util
