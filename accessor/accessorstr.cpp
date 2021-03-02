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

#include "accessorstr.h"
#include "accessortraits.h"
#include "builderstr.h"
#include "reader.h"

namespace columnar
{

class StoredSubblock_Str_c
{
public:
	void		ReadHeader ( FileReader_c & tReader, int iValues );
	void		SetHashFlags ( bool bHaveHashes, bool bNeedHashes );

	template <bool PACK>
	inline int		ReadValue ( const uint8_t * & pValue, FileReader_c & tReader, int iIdInSubblock );
	inline int		GetValueLength ( int iIdInSubblock ) const;
	inline uint64_t	GetHash ( int iIdInSubblock ) const;

protected:
	SpanResizeable_T<uint64_t>	m_dCumulativeLengths;
	std::vector<uint64_t>		m_dHashes;
	std::vector<uint8_t>		m_dValue;
	int64_t						m_tFirstValueOffset = 0;
	int							m_iLastReadId = -1;
	bool						m_bHaveHashes = false;
	bool						m_bNeedHashes = false;

	inline void	ReadHashes ( FileReader_c & tReader, int iValues );
};


void StoredSubblock_Str_c::ReadHashes ( FileReader_c & tReader, int iValues )
{
	if ( !m_bHaveHashes )
		return;

	int iTotalHashSize = iValues*sizeof(m_dHashes[0]);
	if ( m_bNeedHashes )
	{
		m_dHashes.resize(iValues);
		tReader.Read ( (uint8_t*)m_dHashes.data(), iTotalHashSize );
	}
	else
		tReader.Seek ( tReader.GetPos() + iTotalHashSize );
}


void StoredSubblock_Str_c::SetHashFlags ( bool bHaveHashes, bool bNeedHashes )
{
	m_bHaveHashes = bHaveHashes;
	m_bNeedHashes = bHaveHashes && bNeedHashes;
}


void StoredSubblock_Str_c::ReadHeader ( FileReader_c & tReader, int iValues )
{
	ReadHashes ( tReader, iValues );

	m_dCumulativeLengths.resize(iValues);
	m_dCumulativeLengths[0] = tReader.Unpack_uint64();
	for ( int i = 1; i<iValues; i++ )
		m_dCumulativeLengths[i] = tReader.Unpack_uint64() + m_dCumulativeLengths[i-1];

	m_tFirstValueOffset = tReader.GetPos();

	m_iLastReadId = -1;
}


int StoredSubblock_Str_c::GetValueLength ( int iIdInSubblock ) const
{
	uint64_t uLength = m_dCumulativeLengths[iIdInSubblock];
	if ( iIdInSubblock>0 )
		uLength -= m_dCumulativeLengths[iIdInSubblock-1];

	return (int)uLength;
}


uint64_t StoredSubblock_Str_c::GetHash ( int iIdInSubblock ) const
{
	return m_dHashes[iIdInSubblock];
}

template <bool PACK>
int StoredSubblock_Str_c::ReadValue ( const uint8_t * & pValue, FileReader_c & tReader, int iIdInSubblock )
{
	int iLength = GetValueLength(iIdInSubblock);

	int64_t tOffset = m_tFirstValueOffset;
	if ( iIdInSubblock>0 )
		tOffset += m_dCumulativeLengths[iIdInSubblock-1];

	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInSubblock )
		tReader.Seek(tOffset);

	m_iLastReadId = iIdInSubblock;

	if ( PACK )
	{
		uint8_t * pData = nullptr;
		std::tie ( pValue, pData ) = ByteCodec_c::PackData ( (size_t)iLength );
		tReader.Read ( pData, iLength );
	}
	else
	{
		// try to read without copying first
		if ( !tReader.ReadFromBuffer ( pValue, iLength ) )
		{
			// can't read directly from reader's buffer? read to a temp buffer then
			m_dValue.resize(iLength);
			tReader.Read ( m_dValue.data(), iLength );
			pValue = m_dValue.data();
		}
	}

	return iLength;
}

//////////////////////////////////////////////////////////////////////////

class StoredSubblock_Str_PFOR_c : public StoredSubblock_Str_c
{
public:
	void		ReadHeader ( FileReader_c & tReader, IntCodec_i & tCodec, int iValues );

private:
	SpanResizeable_T<uint32_t>	m_dTmp;
};


void StoredSubblock_Str_PFOR_c::ReadHeader ( FileReader_c & tReader, IntCodec_i & tCodec, int iValues )
{
	ReadHashes ( tReader, iValues );

	uint32_t uSubblockSize = (uint32_t)tReader.Unpack_uint64();

	// the logic is that if we need hashes, we don't need string lengths/values
	// maybe there's a case when we need both? we'll need separate flags in that case
	if ( !m_bNeedHashes )
		DecodeValues_Delta_PFOR ( m_dCumulativeLengths, tReader, tCodec, m_dTmp, uSubblockSize, false );

	m_tFirstValueOffset = tReader.GetPos();
	m_iLastReadId = -1;
}

//////////////////////////////////////////////////////////////////////////

struct StoredBlock_StrConstLen_t
{
	int64_t					m_tHashOffset = 0;
	int64_t					m_tValuesOffset = 0;
	size_t					m_tValueLength = 0;
	int						m_iLastReadId = -1;
	std::vector<uint8_t>	m_dValue;

	inline void		ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveStringHashes );

	template <bool PACK>
	inline int		ReadValue ( const uint8_t * & pValue, FileReader_c & tReader, int iIdInBlock );

	inline uint64_t	GetHash ( FileReader_c & tReader, int iIdInBlock );
};


void StoredBlock_StrConstLen_t::ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveStringHashes )
{
	size_t tLength = tReader.Unpack_uint64();

	if ( bHaveStringHashes )
	{
		m_tHashOffset = tReader.GetPos();
		m_tValuesOffset = m_tHashOffset + iValues*sizeof(uint64_t);
	}
	else
		m_tValuesOffset = tReader.GetPos();

	m_tValueLength = tLength;
	m_dValue.resize(m_tValueLength);

	m_iLastReadId = -1;
}

template <bool PACK>
int StoredBlock_StrConstLen_t::ReadValue ( const uint8_t * & pValue, FileReader_c & tReader, int iIdInBlock )
{
	// non-sequental read or first read?
	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInBlock )
	{
		int64_t tOffset = m_tValuesOffset + int64_t(iIdInBlock)*m_tValueLength;
		tReader.Seek ( tOffset );
	}

	m_iLastReadId = iIdInBlock;

	if ( PACK )
	{
		uint8_t * pData = nullptr;
		std::tie ( pValue, pData ) = ByteCodec_c::PackData(m_tValueLength);
		tReader.Read ( pData, m_tValueLength );
	}
	else
	{
		// try to read without copying first
		if ( !tReader.ReadFromBuffer ( pValue, m_tValueLength ) )
		{
			// can't read directly from reader's buffer? read to a temp buffer then
			m_dValue.resize(m_tValueLength);
			tReader.Read ( m_dValue.data(), m_tValueLength );
			pValue = m_dValue.data();
		}
	}

	return (int)m_tValueLength;
}


uint64_t StoredBlock_StrConstLen_t::GetHash ( FileReader_c & tReader, int iIdInBlock )
{
	// we assume that we are reading either hashes or values but not both at the same time
	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInBlock )
	{
		int64_t tOffset = m_tHashOffset + sizeof(uint64_t)*iIdInBlock;
		tReader.Seek(tOffset);
	}

	m_iLastReadId = iIdInBlock;

	return tReader.Read_uint64();
}

//////////////////////////////////////////////////////////////////////////

struct StoredBlock_Str_t
{
	int			m_iNumValues = 0;
	int			m_iSubblockId = -1;
	int64_t		m_tValuesOffset = 0;

	SpanResizeable_T<uint64_t> m_dOffsets;

	void			Setup ( int iBlockValues, int64_t tValuesOffset );
	uint32_t		GetNumSubblockValues ( int iSubblockId, int iSubblockSize ) const;
};


void StoredBlock_Str_t::Setup ( int iBlockValues, int64_t tValuesOffset )
{
	m_iNumValues = iBlockValues;
	m_tValuesOffset = tValuesOffset;
	m_iSubblockId = -1;
}


uint32_t StoredBlock_Str_t::GetNumSubblockValues ( int iSubblockId, int iSubblockSize ) const
{
	if ( m_iNumValues==DOCS_PER_BLOCK )
		return iSubblockSize;

	if ( iSubblockId<m_dOffsets.size()-1 )
		return iSubblockSize;

	int iLeftover = m_iNumValues & (iSubblockSize-1);
	return iLeftover ? iLeftover : iSubblockSize;
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrDeltaPFOR_c : public StoredBlock_Str_t
{
public:
	StoredSubblock_Str_PFOR_c	m_tSubblock;

								StoredBlock_StrDeltaPFOR_c ( const std::string & sCodec32, const std::string & sCodec64 );

	inline void					ReadHeader ( FileReader_c & tReader, int iBlockValues, bool bHaveHashes, bool bNeedHashes );
	inline void					ReadSubblock ( int iSubblockId, FileReader_c & tReader, int iSubblocksize );

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<uint32_t>	m_dTmp;
};


StoredBlock_StrDeltaPFOR_c::StoredBlock_StrDeltaPFOR_c ( const std::string & sCodec32, const std::string & sCodec64 )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) )
{}


void StoredBlock_StrDeltaPFOR_c::ReadHeader ( FileReader_c & tReader, int iBlockValues, bool bHaveHashes, bool bNeedHashes )
{
	uint32_t uSubblockSize = tReader.Unpack_uint32();
	DecodeValues_Delta_PFOR ( m_dOffsets, tReader, *m_pCodec, m_dTmp, uSubblockSize, false );

	int64_t tValuesOffset = tReader.GetPos();
	Setup ( iBlockValues, tValuesOffset );

	m_tSubblock.SetHashFlags ( bHaveHashes, bNeedHashes );
}


void StoredBlock_StrDeltaPFOR_c::ReadSubblock ( int iSubblockId, FileReader_c & tReader, int iSubblocksize )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;
	tReader.Seek ( m_tValuesOffset+m_dOffsets[iSubblockId] );

	m_tSubblock.ReadHeader ( tReader, *m_pCodec, GetNumSubblockValues ( m_iSubblockId, iSubblocksize ) );
}

//////////////////////////////////////////////////////////////////////////

class Iterator_String_c : public Iterator_i, public StoredBlockTraits_t
{
public:
				Iterator_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints );

	uint32_t		AdvanceTo ( uint32_t tRowID ) final;

	int64_t	Get() final;

	int			Get ( const uint8_t * & pData, bool bPack ) final;
	int			GetLength() const final;

	uint64_t	GetStringHash() final;
	bool		HaveStringHashes() const final;

private:
	SubblockCalc_t					m_tSubblockCalc;
	const AttributeHeader_i &		m_tHeader;
	IteratorHints_t					m_tHints;
	std::unique_ptr<FileReader_c>	m_pReader;
	StrPacking_e					m_ePacking = StrPacking_e::CONSTLEN;
	StoredBlock_StrConstLen_t		m_tBlockConstLen;
	StoredBlock_StrDeltaPFOR_c		m_tBlockDeltaPFOR;

	const uint8_t *			m_pResult = nullptr;
	size_t					m_tValueLength = 0;

	void (Iterator_String_c::*m_fnReadValue)() = nullptr;
	void (Iterator_String_c::*m_fnReadValuePacked)() = nullptr;
	void (Iterator_String_c::*m_fnReadSubblockHeader)() = nullptr;
	int (Iterator_String_c::*m_fnGetValueLength)() const = nullptr;
	uint64_t (Iterator_String_c::*m_fnGetHash)() = nullptr;

	inline void	SetCurBlock ( uint32_t uBlockId );

	void		ReadHeader_ConstLen() {}		// no subblock header for constlen
	template <bool PACK>
	void		ReadValue_ConstLen();
	int			GetValueLen_ConstLen() const;
	uint64_t	GetHash_ConstLen();

	void		ReadHeader_DeltaPFOR();
	template <bool PACK>
	void		ReadValue_DeltaPFOR();
	int			GetValueLen_DeltaPFOR() const;

	uint64_t	GetHash_DeltaPFOR();
};


Iterator_String_c::Iterator_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints )
	: StoredBlockTraits_t ( tHeader.GetSettings().m_iSubblockSize )
	, m_tSubblockCalc ( tHeader.GetSettings().m_iSubblockSize )
	, m_tHeader ( tHeader )
	, m_tHints ( tHints )
	, m_pReader ( pReader )
	, m_tBlockDeltaPFOR ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64 )
{
	assert(pReader);
}


void Iterator_String_c::SetCurBlock ( uint32_t uBlockId )
{
	m_pReader->Seek ( m_tHeader.GetBlockOffset(uBlockId) );
	m_ePacking = (StrPacking_e)m_pReader->Unpack_uint32();

	switch ( m_ePacking )
	{
	case StrPacking_e::CONSTLEN:
		m_fnReadValue			= &Iterator_String_c::ReadValue_ConstLen<false>;
		m_fnReadValuePacked		= &Iterator_String_c::ReadValue_ConstLen<true>;
		m_fnReadSubblockHeader	= &Iterator_String_c::ReadHeader_ConstLen;
		m_fnGetValueLength		= &Iterator_String_c::GetValueLen_ConstLen;
		m_fnGetHash				= &Iterator_String_c::GetHash_ConstLen;
		m_tBlockConstLen.ReadHeader ( *m_pReader, m_tHeader.GetNumDocs(uBlockId), m_tHeader.HaveStringHashes() );
		break;

	case StrPacking_e::DELTA_PFOR:
		m_fnReadValue			= &Iterator_String_c::ReadValue_DeltaPFOR<false>;
		m_fnReadValuePacked		= &Iterator_String_c::ReadValue_DeltaPFOR<true>;
		m_fnReadSubblockHeader	= &Iterator_String_c::ReadHeader_DeltaPFOR;
		m_fnGetValueLength		= &Iterator_String_c::GetValueLen_DeltaPFOR;
		m_fnGetHash				= &Iterator_String_c::GetHash_DeltaPFOR;
		m_tBlockDeltaPFOR.ReadHeader ( *m_pReader, m_tHeader.GetNumDocs(uBlockId), m_tHeader.HaveStringHashes(), m_tHints.m_bNeedStringHashes );
		break;

	default:
		assert ( 0 && "Packing not implemented yet" );
		break;
	}

	m_tRequestedRowID = INVALID_ROW_ID;
	m_pResult = nullptr;

	SetBlockId ( uBlockId, m_tHeader.GetNumDocs(uBlockId) );
}

template <bool PACK>
void Iterator_String_c::ReadValue_ConstLen()
{
	m_tValueLength = m_tBlockConstLen.ReadValue<PACK> ( m_pResult, *m_pReader, m_iIdInBlock );
}


int Iterator_String_c::GetValueLen_ConstLen() const
{
	return (int)m_tBlockConstLen.m_tValueLength;
}


uint64_t Iterator_String_c::GetHash_ConstLen()
{
	return m_tBlockConstLen.GetHash ( *m_pReader, m_iIdInBlock );
}


void Iterator_String_c::ReadHeader_DeltaPFOR()
{
	m_iSubblockId = m_tSubblockCalc.GetSubblockId(m_iIdInBlock);
	m_iValueIdInSubblock = m_tSubblockCalc.GetValueIdInSubblock(m_iIdInBlock);
	m_tBlockDeltaPFOR.ReadSubblock ( m_iSubblockId, *m_pReader, m_iSubblockSize );
}

template <bool PACK>
void Iterator_String_c::ReadValue_DeltaPFOR()
{
	m_tValueLength = m_tBlockDeltaPFOR.m_tSubblock.ReadValue<PACK> ( m_pResult, *m_pReader, m_iValueIdInSubblock );
}


int Iterator_String_c::GetValueLen_DeltaPFOR() const
{
	return m_tBlockDeltaPFOR.m_tSubblock.GetValueLength(m_iValueIdInSubblock);
}


uint64_t Iterator_String_c::GetHash_DeltaPFOR()
{
	return m_tBlockDeltaPFOR.m_tSubblock.GetHash(m_iValueIdInSubblock);
}


uint32_t	Iterator_String_c::AdvanceTo ( uint32_t tRowID )
{
	if ( m_tRequestedRowID==tRowID ) // might happen on GetLength/Get calls
		return tRowID;

	uint32_t uBlockId = RowId2BlockId(tRowID);
	if ( uBlockId!=m_uBlockId )
		SetCurBlock(uBlockId);

	m_tRequestedRowID = tRowID;
	m_iIdInBlock = m_tRequestedRowID-m_tStartBlockRowId;

	assert(m_fnReadSubblockHeader);
	(*this.*m_fnReadSubblockHeader)();

	return tRowID;
}


int64_t Iterator_String_c::Get()
{
	assert ( 0 && "INTERNAL ERROR: requesting int from string iterator" );
	return 0;
}


int Iterator_String_c::Get ( const uint8_t * & pData, bool bPack )
{
	if ( bPack )
	{
		assert(m_fnReadValuePacked);
		(*this.*m_fnReadValuePacked)();
	}
	else
	{
		assert(m_fnReadValue);
		(*this.*m_fnReadValue)();
	}

	pData = m_pResult;
	m_pResult = nullptr;

	return (int)m_tValueLength;
}


int Iterator_String_c::GetLength() const
{
	assert(m_fnGetValueLength);
	return (*this.*m_fnGetValueLength)();
}


uint64_t Iterator_String_c::GetStringHash()
{
	assert(m_fnGetHash);
	return (*this.*m_fnGetHash)();
}


bool Iterator_String_c::HaveStringHashes() const
{
	return m_tHeader.HaveStringHashes();
}

//////////////////////////////////////////////////////////////////////////

Iterator_i * CreateIteratorStr ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints )
{
	return new Iterator_String_c ( tHeader, pReader, tHints );
}

} // namespace columnar