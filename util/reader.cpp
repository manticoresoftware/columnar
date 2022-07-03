// Copyright (c) 2020-2022, Manticore Software LTD (https://manticoresearch.com)
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

#include "reader.h"
#include "assert.h"
#include <errno.h>
#include <sys/stat.h>

#ifdef _MSC_VER
	#define WIN32_LEAN_AND_MEAN
	#define NOMINMAX
	#include <windows.h>
	#include <io.h>
	#define stat		_stat64
	#define fstat		_fstat64
	#define struct_stat	struct _stat64
#else
	#include <sys/mman.h>
	#include <unistd.h>
	#define struct_stat        struct stat
#endif


namespace util
{

#ifdef _MSC_VER
static int PreadWrapper ( int iFD, void * pBuf, size_t tCount, int64_t iOff )
{
	if ( !tCount )
		return 0;

	HANDLE hFile;
	hFile = (HANDLE)_get_osfhandle(iFD);
	if ( hFile==INVALID_HANDLE_VALUE )
		return -1;

	OVERLAPPED tOverlapped;
	memset ( &tOverlapped, 0, sizeof(OVERLAPPED) );
	tOverlapped.Offset = (DWORD)( iOff & 0xFFFFFFFFULL );
	tOverlapped.OffsetHigh = (DWORD)( iOff>>32 );

	DWORD uNumBytesRead;
	if ( !ReadFile ( hFile, pBuf, (DWORD)tCount, &uNumBytesRead, &tOverlapped ) )
		return GetLastError()==ERROR_HANDLE_EOF ? 0 : -1;

	return uNumBytesRead;
}

#else
#if HAVE_PREAD
static int PreadWrapper ( int iFD, void * pBuf, size_t tCount, off_t tOff )
{
	return ::pread ( iFD, pBuf, tCount, tOff );
}
#else
static int PreadWrapper ( int iFD, void * pBuf, size_t tCount, off_t tOff )
{
	if ( lseek ( iFD, tOff, SEEK_SET )==(off_t)-1 )
		return -1;

	return read ( iFD, pBuf, tCount );
}
#endif
#endif	// _MSC_VER


FileReader_c::FileReader_c ( int iFD, size_t tBufferSize )
	: m_iFD ( iFD )
	, m_tSize ( tBufferSize )
{
	assert ( iFD>=0 );
}


bool FileReader_c::Open ( const std::string & sName, std::string & sError )
{
	return Open ( sName, DEFAULT_SIZE, sError );
}

bool FileReader_c::Open ( const std::string & sName, int iBufSise, std::string & sError )
{
#ifdef _MSC_VER
	HANDLE tHandle = CreateFile ( sName.c_str(), GENERIC_READ, FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
	m_iFD = _open_osfhandle ( (intptr_t)tHandle, 0 );
#else
	m_iFD = ::open ( sName.c_str(), O_RDONLY | O_BINARY, 0644 );
#endif

	if ( m_iFD<0 )
    {
        sError = FormatStr ( "error opening '%s': %s", sName.c_str(), strerror(errno) );
        return false;
    }

    m_sFile = sName;
	m_bOpened = true;
	m_tSize = iBufSise;

    return true;
}


void FileReader_c::Close()
{
	if ( !m_bOpened || m_iFD<0 )
		return;

	::close(m_iFD);
	m_iFD = -1;
}


void FileReader_c::Read ( uint8_t * pData, size_t tLen )
{
	uint8_t * pDst = pData;
	while ( tLen > m_tSize )
	{
		CopyTail ( pDst, tLen );
		if ( tLen>0 && !ReadToBuffer() )
			return;
	}

	if ( m_tPtr+tLen > m_tUsed )
	{
		CopyTail ( pDst, tLen );
		if ( !ReadToBuffer() )
			return;
	}

	memcpy ( pDst, m_pData.get()+m_tPtr, tLen );
	m_tPtr += tLen;
}


std::string FileReader_c::Read_string()
{
	uint32_t uLen = Read_uint32();
	if ( !uLen )
		return "";

	std::unique_ptr<char[]> pBuffer ( new char[uLen+1] );
	Read ( (uint8_t*)pBuffer.get(), uLen );
	pBuffer[uLen] = '\0';
	return std::string(pBuffer.get());
}


uint32_t FileReader_c::Unpack_uint32()
{
	return ByteCodec_c::Unpack_uint32 ( [this](){ return Read_uint8(); } );
}


uint64_t FileReader_c::Unpack_uint64()
{
	return ByteCodec_c::Unpack_uint64 ( [this](){ return Read_uint8(); } );
}


bool FileReader_c::ReadToBuffer()
{
	assert ( m_iFD>=0 );

	CreateBuffer();

	int64_t iNewFilePos = m_iFilePos + std::min ( m_tPtr, m_tUsed );
	int iRead = PreadWrapper ( m_iFD, m_pData.get(), m_tSize, iNewFilePos );
	if ( iRead<0 )
	{
		m_tPtr = m_tUsed = 0;
		m_bError = true;
		m_sError = FormatStr ( "read error in '%s': %d (%s)", m_sFile.c_str(), errno, strerror(errno) );
		return false;
	}

	m_tUsed = iRead;
	m_tPtr = 0;
	m_iFilePos = iNewFilePos;

	return true;
}

int64_t FileReader_c::GetFileSize()
{
	return util::GetFileSize ( m_iFD, &m_sError );
}


void ReadVectorPacked ( std::vector<uint64_t> & dData, FileReader_c & tReader )
{
	dData.resize ( tReader.Unpack_uint32() );
	for ( uint64_t & tVal : dData )
		tVal = tReader.Unpack_uint64();
}

struct MappedBufferData_t
{
#if _WIN32
	HANDLE		m_iFD { INVALID_HANDLE_VALUE };
	HANDLE		m_iMap { INVALID_HANDLE_VALUE };
#else
	int			m_iFD { -1 };
#endif

	void * m_pData { nullptr };
	int64_t m_iBytesCount { 0 };
};


bool MMapOpen ( const std::string & sFile, std::string & sError, MappedBufferData_t & tBuf )
{
#if _WIN32
	assert ( tBuf.m_iFD==INVALID_HANDLE_VALUE );
#else
	assert ( tBuf.m_iFD==-1 );
#endif
	assert ( !tBuf.m_pData && !tBuf.m_iBytesCount );

#if _WIN32
	int iAccessMode = GENERIC_READ;
	DWORD uShare = FILE_SHARE_READ | FILE_SHARE_DELETE;

	HANDLE iFD = CreateFile ( sFile.c_str(), iAccessMode, uShare, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
	if ( iFD==INVALID_HANDLE_VALUE )
	{
		sError = FormatStr ( "failed to open file '%s' (errno %u)", sFile.c_str(), ::GetLastError() );
		return false;
	}
	tBuf.m_iFD = iFD;

	LARGE_INTEGER tLen;
	if ( GetFileSizeEx ( iFD, &tLen )==0 )
	{
		sError = FormatStr ( "failed to fstat file '%s' (errno %u)", sFile.c_str(), ::GetLastError() );
		return false;
	}

	tBuf.m_iBytesCount = tLen.QuadPart;

	// mmap fails to map zero-size file
	if ( tBuf.m_iBytesCount>0 )
	{
		tBuf.m_iMap = ::CreateFileMapping ( iFD, NULL, PAGE_READONLY, 0, 0, NULL );
		tBuf.m_pData = ::MapViewOfFile ( tBuf.m_iMap, FILE_MAP_READ, 0, 0, 0 );
		if ( !tBuf.m_pData )
		{
			sError = FormatStr ( "failed to map file '%s': (errno %u, length=%I64u)", sFile.c_str(), ::GetLastError(), tBuf.m_iBytesCount );
			return false;
		}
	}
#else

	int iFD = ::open ( sFile.c_str(), O_RDONLY | O_BINARY, 0644 );
	if ( iFD<0 )
		return false;
	tBuf.m_iFD = iFD;

	tBuf.m_iBytesCount = GetFileSize ( iFD, &sError );
	if ( tBuf.m_iBytesCount<0 )
		return false;

	// mmap fails to map zero-size file
	if ( tBuf.m_iBytesCount>0 )
	{
		tBuf.m_pData = mmap ( NULL, tBuf.m_iBytesCount, PROT_READ, MAP_SHARED, iFD, 0 );
		if ( tBuf.m_pData==MAP_FAILED )
		{
			sError = FormatStr ( "failed to mmap file '%s': %s (length=%lld)", sFile.c_str(), strerror(errno), tBuf.m_iBytesCount );
			return false;
		}
	}
#endif

	return true;
}

void MMapClose ( MappedBufferData_t & tBuf )
{
#if _WIN32
	if ( tBuf.m_pData )
		::UnmapViewOfFile ( tBuf.m_pData );

	if ( tBuf.m_iMap!=INVALID_HANDLE_VALUE )
		::CloseHandle ( tBuf.m_iMap );
	tBuf.m_iMap = INVALID_HANDLE_VALUE;

	if ( tBuf.m_iFD!=INVALID_HANDLE_VALUE )
		::CloseHandle ( tBuf.m_iFD );
	tBuf.m_iFD = INVALID_HANDLE_VALUE;
#else
	if ( tBuf.m_pData )
		::munmap ( tBuf.m_pData, tBuf.m_iBytesCount );

	if ( tBuf.m_iFD!=-1 )
		::close ( tBuf.m_iFD );
	tBuf.m_iFD = -1;
#endif

	tBuf.m_pData = nullptr;
	tBuf.m_iBytesCount = 0;
}

int64_t GetFileSize ( int iFD, std::string * sError )
{
	if ( iFD<0 )
	{
		if ( sError )
			*sError = FormatStr ( "invalid descriptor to fstat '%d'", iFD );
		return -1;
	}

	struct_stat st;
	if ( fstat ( iFD, &st )<0 )
	{
		if ( sError )
			*sError = FormatStr ( "failed to fstat file '%d': '%s'", iFD, strerror(errno) );
		return -1;
	}

	return st.st_size;
}

class MappedBuffer_c : public MappedBuffer_i
{
public:
	MappedBuffer_c() = default;
	virtual ~MappedBuffer_c() override = default;

	bool Open ( const std::string & sFile, std::string & sError ) override
	{
		m_sFileName = sFile;
		return MMapOpen ( sFile, sError, m_tBuf );
	}

	void Close () override
	{
		MMapClose ( m_tBuf );
	}
	
	void * GetPtr () const override
	{
		return m_tBuf.m_pData;
	}

	size_t GetLengthBytes () const override
	{
		return m_tBuf.m_iBytesCount;
	}

	const char * GetFileName() const override
	{
		return m_sFileName.c_str();
	}

	MappedBufferData_t m_tBuf;
	std::string m_sFileName;
};

MappedBuffer_i * MappedBuffer_i::Create()
{
	return new MappedBuffer_c();
}

} // namespace util
