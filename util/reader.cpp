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
	#include <unistd.h>
	#define struct_stat        struct stat
#endif


namespace columnar
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


FileReader_c::FileReader_c ( int iFD )
	: m_iFD ( iFD )
{
	assert ( iFD>=0 );
}


bool FileReader_c::Open ( const std::string & sName, std::string & sError )
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
	if ( m_iFD<0 )
	{
		m_sError = FormatStr ( "invalid FD: %d", m_iFD );
		return -1;
	}

	struct_stat tStat;
	if ( fstat ( m_iFD, &tStat )<0 )
	{
		m_sError = FormatStr ( "fstat failed for %d: '%s'", m_iFD, strerror(errno) );
		return -1;
	}

	return tStat.st_size;
}

} // namespace columnar
