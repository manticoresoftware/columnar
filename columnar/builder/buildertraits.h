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

#pragma once

#include "builder.h"
#include "util.h"
#include "delta.h"
#include "codec.h"
#include <cassert>

namespace columnar
{

static const uint32_t	BLOCK_ID_BITS = 16;
static const int		DOCS_PER_BLOCK = 1 << BLOCK_ID_BITS;

class AttributeHeaderBuilder_c
{
public:
				AttributeHeaderBuilder_c ( const Settings_t & tSettings, const std::string & sName, common::AttrType_e eType );

	common::AttrType_e	GetType() const { return m_eType; }
	const		Settings_t & GetSettings() const { return m_tSettings; }
	void		AddBlock ( uint64_t tOffset ) { m_dBlocks.push_back(tOffset); }
	bool		Save ( util::FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError );

private:
	std::string				m_sName;
	common::AttrType_e		m_eType = common::AttrType_e::NONE;
	Settings_t				m_tSettings;

	std::vector<int64_t>	m_dBlocks;
};

class Packer_i
{
public:
	virtual				~Packer_i(){}

	virtual bool		Setup ( const std::string & sFilename, std::string & sError ) = 0;
	virtual void		AddDoc ( int64_t tAttr ) = 0;
	virtual void		AddDoc ( const uint8_t * pData, int iLength ) = 0;
	virtual void		AddDoc ( const int64_t * pData, int iLength ) = 0;
	virtual void		CorrectOffset ( util::FileWriter_c & tWriter, int64_t tBodyOffset ) = 0;
	virtual int64_t		GetBodySize() const = 0;
	virtual void		Done() = 0;
	virtual void		Cleanup() = 0;

	virtual bool		WriteHeader ( util::FileWriter_c & tWriter, std::string & sError ) = 0;
	virtual bool		WriteBody ( const std::string & sDest, std::string & sError ) const = 0;
};

template <typename HEADER>
class PackerTraits_T : public Packer_i
{
public:
					PackerTraits_T ( const Settings_t & tSettings, const std::string & sName, common::AttrType_e eType );

	bool			Setup ( const std::string & sFilename, std::string & sError ) override;
	void			CorrectOffset ( util::FileWriter_c & tWriter, int64_t tBodyOffset ) override;
	int64_t			GetBodySize() const override { return m_iBodySize; }
	void			Done() override;
	bool			WriteHeader ( util::FileWriter_c & tWriter, std::string & sError ) override;
	bool			WriteBody ( const std::string & sDest, std::string & sError ) const override;
	void			Cleanup() override;

	virtual void	Flush() = 0;

protected:
	util::FileWriter_c	m_tWriter;
	int64_t				m_iBaseOffset = 0;
	int64_t				m_iBodySize = 0;

	HEADER				m_tHeader;
};

template <typename HEADER>
PackerTraits_T<HEADER>::PackerTraits_T ( const Settings_t & tSettings, const std::string & sName, common::AttrType_e eType )
	: m_tHeader ( tSettings, sName, eType )
{}

template <typename HEADER>
bool PackerTraits_T<HEADER>::Setup ( const std::string & sFilename, std::string & sError )
{
	return m_tWriter.Open ( sFilename, sError );
}

template <typename HEADER>
void PackerTraits_T<HEADER>::CorrectOffset ( util::FileWriter_c & tWriter, int64_t iBodyOffset )
{
	tWriter.SeekAndWrite ( m_iBaseOffset, iBodyOffset );
}

template <typename HEADER>
void PackerTraits_T<HEADER>::Done()
{
	Flush();
	m_iBodySize = m_tWriter.GetPos();
	m_tWriter.Close();
}

template <typename HEADER>
bool PackerTraits_T<HEADER>::WriteHeader ( util::FileWriter_c & tWriter, std::string & sError )
{
	tWriter.Write_uint32 ( util::to_underlying ( m_tHeader.GetType() ) );
	return m_tHeader.Save ( tWriter, m_iBaseOffset, sError );
}

template <typename HEADER>
bool PackerTraits_T<HEADER>::WriteBody ( const std::string & sDest, std::string & sError ) const
{
	return util::CopySingleFile ( m_tWriter.GetFilename(), sDest, sError, O_CREAT | O_RDWR | O_APPEND | O_BINARY );
}

template <typename HEADER>
void PackerTraits_T<HEADER>::Cleanup()
{
	m_tWriter.Unlink();
}

//////////////////////////////////////////////////////////////////////////

FORCE_INLINE int GetSubblockSize ( int iSubblock, int iNumSubblocks, int iNumValues, int iSubblockSize )
{
	if ( iSubblock==iNumSubblocks-1 )
	{
		int iLeftover = iNumValues % iSubblockSize;
		return iLeftover ? iLeftover : iSubblockSize;
	}

	return iSubblockSize;
}

template <typename T, typename WRITER>
static void WriteValues_Delta_PFOR ( const util::Span_T<T> & dValues, std::vector<T> & dTmpUncompressed, std::vector<uint32_t> & dTmpCompressed, WRITER & tWriter, util::IntCodec_i * pCodec )
{
	dTmpUncompressed.resize ( dValues.size() );
	memcpy ( dTmpUncompressed.data(), dValues.data(), dValues.size()*sizeof ( dValues[0] ) );
	util::ComputeDeltas ( dTmpUncompressed.data(), (int)dTmpUncompressed.size(), true );

	T uMin = dTmpUncompressed[0];
	dTmpUncompressed[0]=0;

	assert(pCodec);
	pCodec->Encode ( dTmpUncompressed, dTmpCompressed );

	// write the length of encoded data
	tWriter.Pack_uint64 ( dTmpCompressed.size()*sizeof ( dTmpCompressed[0] ) + util::ByteCodec_c::CalcPackedLen(uMin) );
	tWriter.Pack_uint64(uMin);
	tWriter.Write ( (const uint8_t*)dTmpCompressed.data(), dTmpCompressed.size()*sizeof ( dTmpCompressed[0] ) );
}

template <typename T, typename WRITER>
static void WriteValues_PFOR ( const util::Span_T<T> & dValues, std::vector<T> & dTmpUncompressed, std::vector<uint32_t> & dTmpCompressed, WRITER & tWriter, util::IntCodec_i * pCodec, bool bWriteLength )
{
	T uMin = dValues[0];
	for ( size_t i = 1; i < dValues.size(); i++ )
		uMin = std::min ( uMin, dValues[i] );

	dTmpUncompressed.resize ( dValues.size() );
	for ( size_t i = 0; i < dValues.size(); i++ )
		dTmpUncompressed[i] = dValues[i]-uMin;

	assert(pCodec);
	pCodec->Encode ( dTmpUncompressed, dTmpCompressed );

	if ( bWriteLength )
		tWriter.Pack_uint64 ( dTmpCompressed.size()*sizeof ( dTmpCompressed[0] ) + util::ByteCodec_c::CalcPackedLen(uMin) );

	tWriter.Pack_uint64(uMin);
	tWriter.Write ( (const uint8_t*)dTmpCompressed.data(), dTmpCompressed.size()*sizeof ( dTmpCompressed[0] ) );
}

template <typename UNIQ_VEC, typename UNIQ_HASH, typename COLLECTED>
void WriteTableOrdinals ( UNIQ_VEC & dUniques, UNIQ_HASH & hUnique, COLLECTED & dCollected, std::vector<uint32_t> & dTableIndexes, std::vector<uint32_t> & dCompressed, int iSubblockSize, util::FileWriter_c & tWriter )
{
	// write the ordinals
	int iBits = util::CalcNumBits ( dUniques.size() );
	dCompressed.resize ( ( dTableIndexes.size()*iBits + 31 ) >> 5 );

	int iId = 0;
	for ( auto i : dCollected )
	{
		auto tFound = hUnique.find(i);
		assert ( tFound!=hUnique.end() );
		assert ( tFound->second<256 );

		dTableIndexes[iId++] = tFound->second;
		if ( iId==iSubblockSize )
		{
			util::BitPack ( dTableIndexes, dCompressed, iBits );
			tWriter.Write ( (uint8_t*)dCompressed.data(), dCompressed.size()*sizeof(dCompressed[0]) );
			iId = 0;
		}
	}

	if ( iId )
	{
		// zero out unused values
		memset ( dTableIndexes.data()+iId, 0, (dTableIndexes.size()-iId)*sizeof(dTableIndexes[0]) );
		util::BitPack ( dTableIndexes, dCompressed, iBits );
		tWriter.Write ( (uint8_t*)dCompressed.data(), dCompressed.size()*sizeof(dCompressed[0]) );
	}
}

} // namespace columnar
