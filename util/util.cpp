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

#include "util.h"
#include <stdexcept>
#include <errno.h>
#include <cmath>
#include <limits>

#ifdef _MSC_VER
	#include <io.h>
#else
	#include <unistd.h>
#endif

namespace util
{

int CalcNumBits ( uint64_t uNumber )
{
    int iNumBits = 0;
    while ( uNumber )
    {
        uNumber >>= 1;
        iNumBits++;
    }

    return iNumBits;
}

/////////////////////////////////////////////////////////////////////

class ScopedFile_c
{
public:
				ScopedFile_c ( const std::string & sFile, int iFlags );
				~ScopedFile_c();

	bool		Open ( std::string & sError );
	int			GetFD() const { return m_iFD; }

private:
	std::string	m_sFile;
	int			m_iFD = -1;
	int			m_iFlags = 0;
};


ScopedFile_c::ScopedFile_c ( const std::string & sFile, int iFlags )
	: m_sFile ( sFile )
	, m_iFlags ( iFlags )
{}


ScopedFile_c::~ScopedFile_c()
{
	if ( m_iFD>=0 )
		::close(m_iFD);
}


bool ScopedFile_c::Open ( std::string & sError )
{
	assert ( m_iFD<0 );
	m_iFD = ::open ( m_sFile.c_str(), m_iFlags, 0644 );
	if ( m_iFD<0 )
	{
		sError = FormatStr ( "error opening '%s': %s; flags: %d", m_sFile.c_str(), strerror(errno), m_iFlags );
		return false;
	}

	return true;
}

/////////////////////////////////////////////////////////////////////

bool CopySingleFile ( const std::string & sSource, const std::string & sDest, std::string & sError, int iMode )
{
	const int BUFFER_SIZE = 1048576;
	std::unique_ptr<uint8_t[]> pData = std::unique_ptr<uint8_t[]>( new uint8_t[BUFFER_SIZE] );

	ScopedFile_c tSrc ( sSource, O_RDONLY | O_BINARY );
	ScopedFile_c tDst ( sDest, O_CREAT | O_RDWR | O_APPEND | O_BINARY );

	if ( !tSrc.Open(sError) ) return false;
	if ( !tDst.Open(sError) ) return false;

	int64_t iRead = 0;
	while ( ( iRead = ::read ( tSrc.GetFD(), pData.get(), BUFFER_SIZE ) ) > 0 )
		if ( ::write ( tDst.GetFD(), pData.get(), (uint32_t)iRead )<0 )
		{
			iRead = -1;
			break;
		}

	if ( iRead<0 )
	{
		sError = FormatStr ( "error copying '%s' to '%s': %s", sSource.c_str(), sDest.c_str(), strerror(errno) );
		return false;
	}

	return true;
}


static void Seek ( int iFD, int64_t iOffset )
{
#ifdef _MSC_VER
	_lseeki64 ( iFD, iOffset, SEEK_SET );
#else
	lseek ( iFD, iOffset, SEEK_SET );
#endif
}

/////////////////////////////////////////////////////////////////////

bool FileWriter_c::Open ( const std::string & sFile, bool bNewFile, bool bAppend, bool bTmp, std::string & sError )
{
	assert ( m_iFD<0 );
	assert ( !m_pData );

	int iFlags = O_CREAT | O_RDWR | O_BINARY;
	if ( bAppend )
		iFlags |= O_APPEND;
	if ( bNewFile )
		iFlags |= O_TRUNC;

	m_sFile = sFile;
	m_pData = std::unique_ptr<uint8_t[]> ( new uint8_t[m_tSize] );
	m_iFD = ::open ( sFile.c_str(), iFlags, 0644 );
	if ( m_iFD<0 )
	{
		sError = FormatStr ( "error creating '%s': %s", sFile.c_str(), strerror(errno) );
		return false;
	}

	m_tUsed = 0;
	m_iFilePos = 0;
	m_bError = false;
	m_sError = "";
	m_bTemporary = bTmp;

	return true;
}

bool FileWriter_c::Open ( const std::string & sFile, std::string & sError )
{
	assert ( m_iFD<0 );
	assert ( !m_pData );

	m_sFile = sFile;
	m_pData = std::unique_ptr<uint8_t[]> ( new uint8_t[m_tSize] );
	m_iFD = ::open ( sFile.c_str(), O_CREAT | O_RDWR | O_TRUNC | O_BINARY, 0644 );
	if ( m_iFD<0 )
	{
		sError = FormatStr ( "error creating '%s': %s", sFile.c_str(), strerror(errno) );
		return false;
	}

	m_tUsed = 0;
	m_iFilePos = 0;
	m_bError = false;
	m_sError = "";

	return true;
}


void FileWriter_c::Close()
{
	if ( m_iFD<0 )
		return;

	Flush();
	::close(m_iFD);
	m_iFD = -1;
}


void FileWriter_c::Unlink()
{
	Close();
	::unlink ( m_sFile.c_str() );
}


void FileWriter_c::Write ( const uint8_t * pData, size_t tLength )
{
	const uint8_t * pSrc = pData;
	while ( tLength )
	{
		size_t tToWrite = std::min ( tLength, m_tSize );
		if ( m_tUsed+tToWrite > m_tSize )
			Flush();

		assert(m_pData);
		memcpy ( m_pData.get() + m_tUsed, pSrc, tToWrite );
		m_tUsed += tToWrite;
		pSrc += tToWrite;
		tLength -= tToWrite;
	}
}


void FileWriter_c::SeekAndWrite ( int64_t iOffset, uint64_t uValue )
{
	int64_t iOldPos = GetPos();

	Flush();

	Seek ( iOffset );
	int iRes = ::write ( m_iFD, &uValue, sizeof(uValue) );
	if ( iRes<0 )
	{
		m_sError = FormatStr ( "write error in '%s': %d (%s)", m_sFile.c_str(), errno, strerror(errno) );
		m_bError = true;
	}

	Seek ( iOldPos );
}

void FileWriter_c::Seek ( int64_t iOffset )
{
	Flush();
	util::Seek ( m_iFD, iOffset );
	m_iFilePos = iOffset;
}

void FileWriter_c::Write_string ( const std::string & sStr )
{
	size_t tLen = sStr.length();
	Write_uint32 ( (uint32_t)tLen );
	Write ( (const uint8_t *)sStr.c_str(), tLen );
}


void FileWriter_c::Flush()
{
	assert ( m_iFD>=0 );

	int iRes = ::write ( m_iFD, m_pData.get(), (uint32_t)m_tUsed );
	if ( iRes<0 )
	{
		m_sError = FormatStr ( "write error in '%s': %d (%s)", m_sFile.c_str(), errno, strerror(errno) );
		m_bError = true;
	}

	m_iFilePos += m_tUsed;
	m_tUsed = 0;
}

FileWriter_c::~FileWriter_c()
{
	if ( m_bTemporary )
		Unlink();

	Close();
}

/////////////////////////////////////////////////////////////////////

MemWriter_c::MemWriter_c ( std::vector<uint8_t> & dData )
	: m_dData ( dData )
{}


void MemWriter_c::Write ( const uint8_t * pData, size_t tSize )
{
	if ( !tSize )
		return;

	size_t tOldSize = m_dData.size();
	m_dData.resize ( tOldSize+tSize );
	memcpy ( &m_dData[tOldSize], pData, tSize );
}

bool FloatEqual ( float fA, float fB )
{
    return std::fabs ( fA - fB )<=std::numeric_limits<float>::epsilon();
}

} // namespace util
