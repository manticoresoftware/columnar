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


class StrHashReader_c
{
public:
	FORCE_INLINE void		ReadHashesWithNullMap ( FileReader_c & tReader, int iValues, int iNumHashes );
	FORCE_INLINE void		ReadHashes ( FileReader_c & tReader, int iValues, bool bNeedHashes );
	FORCE_INLINE uint64_t	GetHash ( int iId ) const { return m_dHashes[iId]; }
	FORCE_INLINE Span_T<uint64_t> GetAllHashes() { return m_dHashes; }

private:
	SpanResizeable_T<uint32_t>	m_dNullMap;
	SpanResizeable_T<uint64_t>	m_dHashes;
	SpanResizeable_T<uint32_t>	m_dTmp;
};


void StrHashReader_c::ReadHashesWithNullMap ( FileReader_c & tReader, int iValues, int iNumHashes )
{
	assert ( iValues==128 );
	m_dTmp.resize ( iValues >> 5 );
	m_dNullMap.resize(iValues);
	tReader.Read ( (uint8_t*)m_dTmp.data(), m_dTmp.size()*sizeof(m_dTmp[0]) );
	BitUnpack128 ( m_dTmp, m_dNullMap, 1 );
	tReader.Read ( (uint8_t*)m_dHashes.data(), iNumHashes*sizeof(uint64_t) );

	const uint32_t * pNullMap = m_dNullMap.end()-1;
	uint64_t * pDstMin = m_dHashes.data();
	uint64_t * pDst = pDstMin+iValues-1;
	uint64_t * pSrc = pDstMin+iNumHashes-1;
	while ( pDst>=pDstMin )
	{
		if ( *pNullMap )
			*pDst = *pSrc--;
		else
			*pDst = 0;

		pNullMap--;
		pDst--;
	}
}


void StrHashReader_c::ReadHashes ( FileReader_c & tReader, int iValues, bool bNeedHashes )
{
	int iNumHashes = tReader.Read_uint8();
	bool bHaveNullMap = iValues!=iNumHashes;
	size_t tTotalHashSize = iNumHashes*sizeof(uint64_t);

	if ( !bNeedHashes )
	{
		size_t tOffsetToData = tTotalHashSize + ( bHaveNullMap ? ( iValues>>3 ) : 0 );
		tReader.Seek ( tReader.GetPos() + tOffsetToData );
		return;
	}

	m_dHashes.resize(iValues);
	if ( bHaveNullMap )
		ReadHashesWithNullMap ( tReader, iValues, iNumHashes );
	else
		tReader.Read ( (uint8_t*)m_dHashes.data(), tTotalHashSize );
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrConst_c : public StrHashReader_c
{
	using BASE = StrHashReader_c;

public:
	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes );
	template <bool PACK>
	FORCE_INLINE Span_T<uint8_t> GetValue();
	FORCE_INLINE int		GetValueLength() const { return (int)m_dValue.size(); }
	FORCE_INLINE uint64_t	GetHash() const { return BASE::GetHash(0); }

private:
	std::vector<uint8_t>	m_dValue;
	std::vector<uint8_t>	m_dValuePacked;
};


void StoredBlock_StrConst_c::ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes )
{
	if ( bHaveHashes )
		ReadHashes ( tReader, 1, bNeedHashes );

	int iLength = tReader.Unpack_uint32();
	m_dValue.resize(iLength);
	tReader.Read ( m_dValue.data(), iLength );

	ByteCodec_c::PackData ( m_dValuePacked, Span_T<uint8_t>(m_dValue) );
}

template <bool PACK>
Span_T<uint8_t> StoredBlock_StrConst_c::GetValue()
{
	if ( PACK )
	{
		uint8_t * pData = new uint8_t [ m_dValuePacked.size() ];
		memcpy ( pData, m_dValuePacked.data(), m_dValuePacked.size() );
		return { pData, 0 };
	}

	return m_dValue;
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrConstLen_c
{
public:
									StoredBlock_StrConstLen_c ( int iSubblockSize );

	FORCE_INLINE void				ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveStringHashes );
	template <bool PACK>
	FORCE_INLINE Span_T<uint8_t>	ReadValue ( FileReader_c & tReader, int iIdInBlock );
	FORCE_INLINE int				GetValueLength() const { return (int)m_tValuesOffset; }
	FORCE_INLINE uint64_t			GetHash ( FileReader_c & tReader, int iIdInBlock );

	FORCE_INLINE void				ReadSubblock ( int iSubblockIdInBlock, int iSubblockValues, FileReader_c & tReader );
	FORCE_INLINE Span_T<uint64_t>	GetAllHashes() { return m_dHashes; }
	FORCE_INLINE Span_T<uint64_t>	GetAllValueLengths() { return m_dLengths; }
	FORCE_INLINE Span_T<Span_T<uint8_t>> & ReadAllSubblockValues ( int iSubblockIdInBlock, int iSubblockValues, FileReader_c & tReader );

private:
	int							m_iSubblockSize = 0;
	int64_t						m_tHashOffset = 0;
	int64_t						m_tValuesOffset = 0;
	size_t						m_tValueLength = 0;
	int							m_iLastReadId = -1;
	std::vector<uint8_t>		m_dValue;

	SpanResizeable_T<uint64_t>	m_dHashes;
	SpanResizeable_T<uint64_t>	m_dLengths;
	SpanResizeable_T<uint8_t>	m_dAllValues;
	SpanResizeable_T<Span_T<uint8_t>> m_dAllValuePtrs;
};


StoredBlock_StrConstLen_c::StoredBlock_StrConstLen_c ( int iSubblockSize )
	: m_iSubblockSize ( iSubblockSize )
{
	m_dLengths.resize(iSubblockSize);
}


void StoredBlock_StrConstLen_c::ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveStringHashes )
{
	size_t tLength = tReader.Unpack_uint64();

	if ( bHaveStringHashes )
	{
		m_tHashOffset = tReader.GetPos();
		m_tValuesOffset = m_tHashOffset + iValues*sizeof(uint64_t);
	}
	else
		m_tValuesOffset = tReader.GetPos();

	for ( auto & i : m_dLengths )
		i = tLength;

	m_tValueLength = tLength;
	m_dValue.resize(m_tValueLength);

	m_iLastReadId = -1;
}

template <bool PACK>
Span_T<uint8_t> StoredBlock_StrConstLen_c::ReadValue ( FileReader_c & tReader, int iIdInBlock )
{
	// non-sequental read or first read?
	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInBlock )
	{
		int64_t tOffset = m_tValuesOffset + int64_t(iIdInBlock)*m_tValueLength;
		tReader.Seek ( tOffset );
	}

	m_iLastReadId = iIdInBlock;

	uint8_t * pValue = nullptr;
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

	return { pValue, m_tValueLength };
}


uint64_t StoredBlock_StrConstLen_c::GetHash ( FileReader_c & tReader, int iIdInBlock )
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


void StoredBlock_StrConstLen_c::ReadSubblock ( int iSubblockIdInBlock, int iSubblockValues, FileReader_c & tReader )
{
	int iIdInBlock = iSubblockIdInBlock*m_iSubblockSize;
	m_dHashes.resize(iSubblockValues);
	tReader.Seek ( m_tHashOffset + int64_t(iIdInBlock)*sizeof(uint64_t) );
	tReader.Read ( (uint8_t*)m_dHashes.data(), m_dHashes.size()*sizeof(m_dHashes[0]) );
}


Span_T<Span_T<uint8_t>> & StoredBlock_StrConstLen_c::ReadAllSubblockValues ( int iSubblockIdInBlock, int iSubblockValues, FileReader_c & tReader )
{
	int iIdInBlock = iSubblockIdInBlock*m_iSubblockSize;
	tReader.Seek ( m_tValuesOffset + int64_t(iIdInBlock)*m_tValueLength );

	uint64_t uTotalLength = m_tValueLength*iSubblockValues;
	uint8_t * pAllData = nullptr;
	if ( !tReader.ReadFromBuffer ( pAllData, uTotalLength ) )
	{
		// can't read directly from reader's buffer? read to a temp buffer then
		m_dAllValues.resize(uTotalLength);
		tReader.Read ( m_dAllValues.data(), uTotalLength );
		pAllData = m_dAllValues.data();
	}

	m_dAllValuePtrs.resize ( m_dLengths.size() );
	Span_T<uint8_t> * pValueSpan = m_dAllValuePtrs.data();
	uint8_t * pValue = pAllData;
	for ( int i = 0; i < iSubblockValues; i++ )
	{
		*pValueSpan = { pValue, m_tValueLength };
		pValue += m_tValueLength;
		pValueSpan++;
	}

	return m_dAllValuePtrs;
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrTable_c : public StrHashReader_c
{
	using BASE = StrHashReader_c;

public:
							StoredBlock_StrTable_c ( const std::string & sCodec32, const std::string & sCodec64, int iSubblockSize );

	FORCE_INLINE void		ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveHashes, bool bNeedHashes );
	FORCE_INLINE void		ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader );
	FORCE_INLINE int		GetValueLength ( int iIdInSubblock ) const	{ return m_dTableValueLengths[m_dValueIndexes[iIdInSubblock]]; }
	template <bool PACK>
	FORCE_INLINE Span_T<uint8_t> GetValue ( int iIdInSubblock );
	FORCE_INLINE uint64_t	GetHash ( int iIdInSubblock ) const			{ return BASE::GetHash ( m_dValueIndexes[iIdInSubblock] ); }

	FORCE_INLINE int		GetTableSize() const						{ return (int)m_dTableValueLengths.size(); }
	FORCE_INLINE uint64_t	GetTableValueHash ( int iId ) const			{ return BASE::GetHash(iId); }
	FORCE_INLINE int		GetTableValueLength ( int iId ) const		{ return m_dTableValueLengths[iId]; }
	FORCE_INLINE Span_T<const uint8_t> GetTableValue ( int iId ) const	{ return Span_T<const uint8_t> ( m_dTableValues[iId].data(), m_dTableValues[iId].size() ); }
	FORCE_INLINE Span_T<uint32_t> GetValueIndexes()						{ return m_tValuesRead; }

private:
	std::vector<std::vector<uint8_t>>	m_dTableValues;
	SpanResizeable_T<uint32_t>			m_dTableValueLengths;
	SpanResizeable_T<uint32_t> 			m_dTmp;
	std::vector<uint32_t>				m_dValueIndexes;
	std::vector<uint32_t>				m_dEncoded;
	std::unique_ptr<IntCodec_i>			m_pCodec;
	Span_T<uint32_t>					m_tValuesRead;

	int64_t		m_iValuesOffset = 0;
	int			m_iSubblockId = -1;
	int			m_iBits = 0;
};


StoredBlock_StrTable_c::StoredBlock_StrTable_c ( const std::string & sCodec32, const std::string & sCodec64, int iSubblockSize )
	: m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) ) 
{
	m_dValueIndexes.resize(iSubblockSize);
}


void StoredBlock_StrTable_c::ReadHeader ( FileReader_c & tReader, int iValues, bool bHaveHashes, bool bNeedHashes )
{
	m_dTableValues.resize ( tReader.Read_uint8() );
	if ( bHaveHashes )
		ReadHashes ( tReader, (int)m_dTableValues.size(), bNeedHashes ); // read or skip hashes depending on bNeedHashes

	uint64_t uTotalSizeOfValues = tReader.Unpack_uint64();
	uint32_t uTotalSizeOfLengths = tReader.Unpack_uint32();

	// we read either hashes or lengths+values, but not both at the same time
	if ( !(bHaveHashes && bNeedHashes) )
	{
		DecodeValues_Delta_PFOR ( m_dTableValueLengths, tReader, *m_pCodec, m_dTmp, uTotalSizeOfLengths, false );
		for ( size_t i = 0; i < m_dTableValues.size(); i++ )
		{
			auto & tValue = m_dTableValues[i];
			tValue.resize ( m_dTableValueLengths[i] );
			tReader.Read ( tValue.data(), tValue.size() );
		}
	}
	else
	{
		tReader.Seek ( tReader.GetPos() + uTotalSizeOfLengths + uTotalSizeOfValues );	// skip lengths and values
		m_dTableValueLengths.resize(iValues);
	}

	m_iBits = CalcNumBits ( m_dTableValues.size() );
	m_dEncoded.resize ( ( m_dValueIndexes.size() >> 5 ) * m_iBits );

	m_iValuesOffset = tReader.GetPos();
	m_iSubblockId = -1;
}


void StoredBlock_StrTable_c::ReadSubblock ( int iSubblockId, int iNumValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;

	size_t uPackedSize = m_dEncoded.size()*sizeof ( m_dEncoded[0] );
	tReader.Seek ( m_iValuesOffset + uPackedSize*iSubblockId );
	tReader.Read ( (uint8_t*)m_dEncoded.data(), uPackedSize );
	BitUnpack128 ( m_dEncoded, m_dValueIndexes, m_iBits );

	m_tValuesRead = { m_dValueIndexes.data(), (size_t)iNumValues };
}

template <bool PACK>
Span_T<uint8_t> StoredBlock_StrTable_c::GetValue ( int iIdInSubblock )
{
	uint8_t * pValue = nullptr;
	uint32_t uLen = PackValue<uint8_t,PACK> ( m_dTableValues [ m_dValueIndexes[iIdInSubblock] ], pValue ); 
	return {pValue, uLen};
}

//////////////////////////////////////////////////////////////////////////

class StoredBlock_StrGeneric_c : public StrHashReader_c
{
public:
									StoredBlock_StrGeneric_c ( const std::string & sCodec32, const std::string & sCodec64 ) : m_pCodec ( CreateIntCodec ( sCodec32, sCodec64 ) ) {}

	FORCE_INLINE void				ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes );
	FORCE_INLINE void				ReadSubblock ( int iSubblockId, int iSubblockValues, FileReader_c & tReader );
	template <bool PACK>
	FORCE_INLINE Span_T<uint8_t>	ReadValue ( int iIdInSubblock, FileReader_c & tReader );
	FORCE_INLINE int				GetValueLength ( int iIdInSubblock ) const { return m_dLengths[iIdInSubblock]; }

	FORCE_INLINE Span_T<uint64_t>	GetAllValueLengths() { return m_dLengths; }
	FORCE_INLINE Span_T<Span_T<uint8_t>> & ReadAllSubblockValues ( int iSubblockId, FileReader_c & tReader );

private:
	std::unique_ptr<IntCodec_i>	m_pCodec;
	SpanResizeable_T<uint32_t>	m_dTmp;
	SpanResizeable_T<uint64_t>	m_dOffsets;
	SpanResizeable_T<uint64_t>	m_dCumulativeLengths;
	SpanResizeable_T<uint64_t>	m_dLengths;
	SpanResizeable_T<uint8_t>	m_dValue;

	SpanResizeable_T<uint8_t>	m_dAllValues;	// stores all values as a single blob. used by analyzers (when hashes are unavailable)
	SpanResizeable_T<Span_T<uint8_t>> m_dAllValuePtrs;

	int		m_iSubblockId = -1;
	int64_t	m_tValuesOffset = 0;
	bool	m_bHaveHashes = false;
	bool	m_bNeedHashes = false;
	int64_t	m_iFirstValueOffset = 0;
	int		m_iLastReadId = -1;
	bool	m_bValuesRead = false;
};


void StoredBlock_StrGeneric_c::ReadHeader ( FileReader_c & tReader, bool bHaveHashes, bool bNeedHashes )
{
	uint32_t uSubblockSize = tReader.Unpack_uint32();
	DecodeValues_Delta_PFOR ( m_dOffsets, tReader, *m_pCodec, m_dTmp, uSubblockSize, false );

	m_tValuesOffset = tReader.GetPos();
	m_bHaveHashes = bHaveHashes;
	m_bNeedHashes = bHaveHashes && bNeedHashes;
}


void StoredBlock_StrGeneric_c::ReadSubblock ( int iSubblockId, int iSubblockValues, FileReader_c & tReader )
{
	if ( m_iSubblockId==iSubblockId )
		return;

	m_iSubblockId = iSubblockId;
	tReader.Seek ( m_tValuesOffset+m_dOffsets[iSubblockId] );

	if ( m_bHaveHashes )
		ReadHashes ( tReader, iSubblockValues, m_bNeedHashes ); // read or skip hashes depending on m_bNeedHashes

	uint32_t uSubblockSize = (uint32_t)tReader.Unpack_uint64();

	// the logic is that if we need hashes, we don't need string lengths/values
	// maybe there's a case when we need both? we'll need separate flags in that case
	if ( !m_bNeedHashes )
	{
		DecodeValues_PFOR ( m_dLengths, tReader, *m_pCodec, m_dTmp, uSubblockSize );
		m_dCumulativeLengths.resize ( m_dLengths.size() );
		memcpy ( m_dCumulativeLengths.data(), m_dLengths.data(), m_dLengths.size()*sizeof(m_dLengths[0]) );
		ComputeInverseDeltas ( m_dCumulativeLengths, true );
	}
	else
		m_dLengths.resize(iSubblockValues);

	m_iFirstValueOffset = tReader.GetPos();
	m_iLastReadId = -1;
	m_bValuesRead = false;
}

template <bool PACK>
Span_T<uint8_t> StoredBlock_StrGeneric_c::ReadValue ( int iIdInSubblock, FileReader_c & tReader )
{
	int iLength = GetValueLength(iIdInSubblock);

	int64_t iOffset = m_iFirstValueOffset;
	if ( iIdInSubblock>0 )
		iOffset += m_dCumulativeLengths[iIdInSubblock-1];

	if ( m_iLastReadId==-1 || m_iLastReadId+1!=iIdInSubblock )
		tReader.Seek(iOffset);

	m_iLastReadId = iIdInSubblock;
	uint8_t * pValue = nullptr;

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

	return { pValue, size_t(iLength) };
}


Span_T<Span_T<uint8_t>> & StoredBlock_StrGeneric_c::ReadAllSubblockValues ( int iSubblockId, FileReader_c & tReader )
{
	if ( m_bValuesRead )
		return m_dAllValuePtrs;

	m_bValuesRead = true;
	tReader.Seek(m_iFirstValueOffset);

	uint64_t uTotalLength = m_dCumulativeLengths.back();
	uint8_t * pAllData = nullptr;
	if ( !tReader.ReadFromBuffer ( pAllData, uTotalLength ) )
	{
		// can't read directly from reader's buffer? read to a temp buffer then
		m_dAllValues.resize(uTotalLength);
		tReader.Read ( m_dAllValues.data(), uTotalLength );
		pAllData = m_dAllValues.data();
	}

	m_dAllValuePtrs.resize ( m_dLengths.size() );
	Span_T<uint8_t> * pValueSpan = m_dAllValuePtrs.data();
	uint8_t * pValue = pAllData;
	for ( size_t i = 0; i < m_dLengths.size(); i++ )
	{
		*pValueSpan = { pValue, m_dLengths[i] };
		pValue += pValueSpan->size();
		pValueSpan++;
	}

	return m_dAllValuePtrs;
}

//////////////////////////////////////////////////////////////////////////

class Accessor_String_c : public StoredBlockTraits_t
{
	using BASE = StoredBlockTraits_t;

public:
									Accessor_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, bool bNeedHashes );

	FORCE_INLINE void				SetCurBlock ( uint32_t uBlockId );

protected:
	const AttributeHeader_i &		m_tHeader;
	std::unique_ptr<FileReader_c>	m_pReader;
	StrPacking_e					m_ePacking = StrPacking_e::CONSTLEN;

	StoredBlock_StrConst_c			m_tBlockConst;
	StoredBlock_StrConstLen_c		m_tBlockConstLen;
	StoredBlock_StrTable_c			m_tBlockTable;
	StoredBlock_StrGeneric_c		m_tBlockGeneric;

	bool							m_bNeedHashes = true;

	Span_T<uint8_t>					m_tResult;

	void (Accessor_String_c::*m_fnReadValue)() = nullptr;
	void (Accessor_String_c::*m_fnReadValuePacked)() = nullptr;
	int (Accessor_String_c::*m_fnGetValueLength)() = nullptr;
	uint64_t (Accessor_String_c::*m_fnGetHash)() = nullptr;

	template <bool PACK> void ReadValue_Const()		{ m_tResult = m_tBlockConst.GetValue<PACK>(); }
	int			GetValueLen_Const()					{ return m_tBlockConst.GetValueLength(); }
	uint64_t	GetHash_Const()						{ return m_tBlockConst.GetHash(); }

	template <bool PACK> void ReadValue_ConstLen()	{ m_tResult = m_tBlockConstLen.ReadValue<PACK> ( *m_pReader, m_tRequestedRowID-m_tStartBlockRowId ); }
	int			GetValueLen_ConstLen()				{ return m_tBlockConstLen.GetValueLength(); }
	uint64_t	GetHash_ConstLen()					{ return m_tBlockConstLen.GetHash ( *m_pReader, m_tRequestedRowID-m_tStartBlockRowId ); }

	template <bool PACK> void ReadValue_Table()		{ m_tResult = m_tBlockTable.template GetValue<PACK>( ReadSubblock(m_tBlockTable) ); }
	int			GetValueLen_Table()					{ return m_tBlockTable.GetValueLength ( ReadSubblock(m_tBlockTable) ); }
	uint64_t	GetHash_Table()						{ return m_tBlockTable.GetHash ( ReadSubblock(m_tBlockTable) ); }

	template <bool PACK> void ReadValue_Generic()	{ m_tResult = m_tBlockGeneric.template ReadValue<PACK>( ReadSubblock(m_tBlockGeneric), *m_pReader ); }
	int			GetValueLen_Generic()				{ return m_tBlockGeneric.GetValueLength ( ReadSubblock(m_tBlockGeneric) ); }
	uint64_t	GetHash_Generic()					{ return m_tBlockGeneric.GetHash ( ReadSubblock(m_tBlockGeneric) ); }

	template <typename T>
	FORCE_INLINE int ReadSubblock ( T & tSubblock );
};


Accessor_String_c::Accessor_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, bool bNeedHashes )
	: StoredBlockTraits_t ( tHeader.GetSettings().m_iSubblockSize )
	, m_tHeader ( tHeader )
	, m_pReader ( pReader )
	, m_tBlockTable ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64, tHeader.GetSettings().m_iSubblockSize )
	, m_tBlockConstLen ( tHeader.GetSettings().m_iSubblockSize )
	, m_tBlockGeneric ( tHeader.GetSettings().m_sCompressionUINT32, tHeader.GetSettings().m_sCompressionUINT64 )
	, m_bNeedHashes ( bNeedHashes )
{
	assert(pReader);
}


void Accessor_String_c::SetCurBlock ( uint32_t uBlockId )
{
	m_pReader->Seek ( m_tHeader.GetBlockOffset(uBlockId) );
	m_ePacking = (StrPacking_e)m_pReader->Unpack_uint32();

	switch ( m_ePacking )
	{
	case StrPacking_e::CONST:
		m_fnReadValue			= &Accessor_String_c::ReadValue_Const<false>;
		m_fnReadValuePacked		= &Accessor_String_c::ReadValue_Const<true>;
		m_fnGetValueLength		= &Accessor_String_c::GetValueLen_Const;
		m_fnGetHash				= &Accessor_String_c::GetHash_Const;
		m_tBlockConst.ReadHeader ( *m_pReader, m_tHeader.HaveStringHashes(), m_bNeedHashes );
		break;

	case StrPacking_e::CONSTLEN:
		m_fnReadValue			= &Accessor_String_c::ReadValue_ConstLen<false>;
		m_fnReadValuePacked		= &Accessor_String_c::ReadValue_ConstLen<true>;
		m_fnGetValueLength		= &Accessor_String_c::GetValueLen_ConstLen;
		m_fnGetHash				= &Accessor_String_c::GetHash_ConstLen;
		m_tBlockConstLen.ReadHeader ( *m_pReader, m_tHeader.GetNumDocs(uBlockId), m_tHeader.HaveStringHashes() );
		break;

	case StrPacking_e::TABLE:
		m_fnReadValue			= &Accessor_String_c::ReadValue_Table<false>;
		m_fnReadValuePacked		= &Accessor_String_c::ReadValue_Table<true>;
		m_fnGetValueLength		= &Accessor_String_c::GetValueLen_Table;
		m_fnGetHash				= &Accessor_String_c::GetHash_Table;
		m_tBlockTable.ReadHeader ( *m_pReader, m_tHeader.GetNumDocs(uBlockId), m_tHeader.HaveStringHashes(), m_bNeedHashes );
		break;

	case StrPacking_e::GENERIC:
		m_fnReadValue			= &Accessor_String_c::ReadValue_Generic<false>;
		m_fnReadValuePacked		= &Accessor_String_c::ReadValue_Generic<true>;
		m_fnGetValueLength		= &Accessor_String_c::GetValueLen_Generic;
		m_fnGetHash				= &Accessor_String_c::GetHash_Generic;
		m_tBlockGeneric.ReadHeader ( *m_pReader, m_tHeader.HaveStringHashes(), m_bNeedHashes );
		break;

	default:
		assert ( 0 && "Packing not implemented yet" );
		break;
	}

	m_tRequestedRowID = INVALID_ROW_ID;
	m_tResult = { nullptr, 0 };
	
	SetBlockId ( uBlockId, m_tHeader.GetNumDocs(uBlockId) );
}

template <typename T>
int Accessor_String_c::ReadSubblock ( T & tSubblock )
{
	uint32_t uIdInBlock = m_tRequestedRowID - m_tStartBlockRowId;
	int iSubblockId = StoredBlockTraits_t::GetSubblockId(uIdInBlock);
	tSubblock.ReadSubblock ( iSubblockId, StoredBlockTraits_t::GetNumSubblockValues(iSubblockId), *m_pReader );
	return GetValueIdInSubblock(uIdInBlock);
}

//////////////////////////////////////////////////////////////////////////

class Iterator_String_c : public Iterator_i, public Accessor_String_c
{
public:
				Iterator_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints );

	uint32_t	AdvanceTo ( uint32_t tRowID ) final;

	int64_t		Get() final						{ assert ( 0 && "INTERNAL ERROR: requesting int from string iterator" ); return 0; }

	int			Get ( const uint8_t * & pData ) final;
	uint8_t *	GetPacked() final;
	int			GetLength() final;

	uint64_t	GetStringHash() final;
	bool		HaveStringHashes() const final	{ return m_tHeader.HaveStringHashes(); }
};


Iterator_String_c::Iterator_String_c ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints )
	: Accessor_String_c ( tHeader, pReader, tHints.m_bNeedStringHashes )
{}


uint32_t Iterator_String_c::AdvanceTo ( uint32_t tRowID )
{
	if ( m_tRequestedRowID==tRowID ) // might happen on GetLength/Get calls
		return tRowID;

	uint32_t uBlockId = RowId2BlockId(tRowID);
	if ( uBlockId!=m_uBlockId )
		SetCurBlock(uBlockId);

	m_tRequestedRowID = tRowID;

	return tRowID;
}


int Iterator_String_c::Get ( const uint8_t * & pData )
{
	assert(m_fnReadValue);
	(*this.*m_fnReadValue)();

	pData = m_tResult.data();
	size_t tLength = m_tResult.size();
	m_tResult = { nullptr, 0 };

	return (int)tLength;
}


uint8_t * Iterator_String_c::GetPacked()
{
	assert(m_fnReadValuePacked);
	(*this.*m_fnReadValuePacked)();

	uint8_t * pData = m_tResult.data();
	m_tResult = { nullptr, 0 };

	return pData;
}


int Iterator_String_c::GetLength()
{
	assert(m_fnGetValueLength);
	return (*this.*m_fnGetValueLength)();
}


uint64_t Iterator_String_c::GetStringHash()
{
	assert(m_fnGetHash);
	return (*this.*m_fnGetHash)();
}

//////////////////////////////////////////////////////////////////////////

template <bool USE_HASHES, bool EQ>
class AnalyzerBlock_Str_T : public Filter_t
{
public:
				AnalyzerBlock_Str_T ( uint32_t & tRowID ) : m_tRowID ( tRowID ) {}

	void		Setup ( const Filter_t & tSettings );

protected:
	uint32_t &	m_tRowID;
	std::vector<uint64_t> m_dHashes;

	template <bool SINGLEVALUE, typename GETVALUE>
	FORCE_INLINE bool CompareStrings ( int iId, uint64_t uHash, uint64_t uLength, GETVALUE && fnGetValue );
};

template <bool USE_HASHES, bool EQ>
void AnalyzerBlock_Str_T<USE_HASHES,EQ>::Setup ( const Filter_t & tSettings )
{
	*(Filter_t*)this = tSettings;

	if ( USE_HASHES )
	{
		m_dHashes.resize ( tSettings.m_dStringValues.size() );
		for ( size_t i = 0; i < m_dHashes.size(); i++ )
		{
			int iLen = (int)tSettings.m_dStringValues[i].size();
			m_dHashes[i] = iLen ? m_fnCalcStrHash ( tSettings.m_dStringValues[i].data(), iLen, STR_HASH_SEED ) : 0;
		}
	}
}

template <bool USE_HASHES, bool EQ>
template <bool SINGLEVALUE, typename GETVALUE>
bool AnalyzerBlock_Str_T<USE_HASHES,EQ>::CompareStrings ( int iId, uint64_t uHash, uint64_t uLength, GETVALUE && fnGetValue )
{
	if ( USE_HASHES )
	{
		if ( SINGLEVALUE )
			return ( m_dHashes[0]==uHash ) ^ (!EQ);
		else
		{
			for ( auto i : m_dHashes )
				if ( i==uHash )
					return true ^ (!EQ);
		}
	}
	else
	{
		assert ( m_fnStrCmp );
		if ( SINGLEVALUE )
		{
			auto & tString = m_dStringValues[0];
			if ( tString.size()==uLength )
			{
				auto dValue = fnGetValue(iId);
				return !m_fnStrCmp ( { tString.data(), (int)tString.size() }, { dValue.data(), (int)dValue.size() }, false ) ^ (!EQ);
			}
		}
		else
		{
			for ( const auto & i : m_dStringValues )
			{
				if ( i.size()!=uLength )
					continue;

				auto dValue = fnGetValue(iId);
				if ( !m_fnStrCmp ( { i.data(), (int)i.size() }, { dValue.data(), (int)dValue.size() }, false ) )
					return true ^ (!EQ);
			}
		}
	}

	return false ^ (!EQ);
}

//////////////////////////////////////////////////////////////////////////

template <bool USE_HASHES, bool EQ>
class AnalyzerBlock_Str_Const_T : public AnalyzerBlock_Str_T<USE_HASHES,EQ>
{
	using BASE = AnalyzerBlock_Str_T<USE_HASHES,EQ>;
	using BASE::AnalyzerBlock_Str_T;

public:
	FORCE_INLINE bool	SetupNextBlock ( StoredBlock_StrConst_c & tBlock );
	FORCE_INLINE int	ProcessSubblock ( uint32_t * & pRowID, int iNumValues );
};

template <bool USE_HASHES, bool EQ>
bool AnalyzerBlock_Str_Const_T<USE_HASHES,EQ>::SetupNextBlock ( StoredBlock_StrConst_c & tBlock )
{
	assert ( m_eType==FilterType_e::STRINGS );
	return BASE::template CompareStrings<false> ( 0, tBlock.GetHash(), tBlock.GetValueLength(), [&tBlock](int){ return tBlock.GetValue<false>(); } );
}

template <bool USE_HASHES, bool EQ>
int AnalyzerBlock_Str_Const_T<USE_HASHES,EQ>::ProcessSubblock ( uint32_t * & pRowID, int iNumValues )
{
	uint32_t tRowID = BASE::m_tRowID;

	// FIXME! use SSE here
	for ( int i = 0; i < iNumValues; i++ )
		*pRowID++ = tRowID++;

	BASE::m_tRowID = tRowID;
	return iNumValues;
}

//////////////////////////////////////////////////////////////////////////

template <bool USE_HASHES, bool EQ>
class AnalyzerBlock_Str_Table_T : public AnalyzerBlock_Str_T<USE_HASHES,EQ>
{
	using BASE = AnalyzerBlock_Str_T<USE_HASHES,EQ>;
	using BASE::AnalyzerBlock_Str_T;

public:
	FORCE_INLINE int	ProcessSubblock ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes );
	FORCE_INLINE bool	SetupNextBlock ( const StoredBlock_StrTable_c & tBlock );

private:
	std::array<bool, UCHAR_MAX> m_dMap;
};

template <bool USE_HASHES, bool EQ>
int AnalyzerBlock_Str_Table_T<USE_HASHES,EQ>::ProcessSubblock ( uint32_t * & pRowID, const Span_T<uint32_t> & dValueIndexes )
{
	uint32_t tRowID = BASE::m_tRowID;

	for ( auto i : dValueIndexes )
	{
		if ( m_dMap[i] )
			*pRowID++ = tRowID;

		tRowID++;
	}

	BASE::m_tRowID = tRowID;
	return (int)dValueIndexes.size();
}

template <bool USE_HASHES, bool EQ>
bool AnalyzerBlock_Str_Table_T<USE_HASHES,EQ>::SetupNextBlock ( const StoredBlock_StrTable_c & tBlock )
{
	assert ( m_eType==FilterType_e::STRINGS );
	bool bAnythingMatches = false;

	for ( int i = 0; i < tBlock.GetTableSize(); i++ )
	{
		m_dMap[i] = BASE::template CompareStrings<false> ( i, tBlock.GetTableValueHash(i), tBlock.GetTableValueLength(i), [&tBlock]( int iValue ){ return tBlock.GetTableValue(iValue); } );
		bAnythingMatches |= m_dMap[i];
	}

	return bAnythingMatches;
}

//////////////////////////////////////////////////////////////////////////

template <bool USE_HASHES, bool EQ>
class AnalyzerBlock_Str_Values_T : public AnalyzerBlock_Str_T<USE_HASHES,EQ>
{
	using BASE = AnalyzerBlock_Str_T<USE_HASHES,EQ>;
	using BASE::AnalyzerBlock_Str_T;

public:
	template <bool SINGLEVALUE, typename READVALUE>
	FORCE_INLINE int	ProcessSubblock_Values ( uint32_t * & pRowID, const Span_T<uint64_t> & dHashes, const Span_T<uint64_t> & dLengths, READVALUE && fnReadValue );
};

template <bool USE_HASHES, bool EQ>
template <bool SINGLEVALUE, typename READVALUE>
int AnalyzerBlock_Str_Values_T<USE_HASHES,EQ>::ProcessSubblock_Values ( uint32_t * & pRowID, const Span_T<uint64_t> & dHashes, const Span_T<uint64_t> & dLengths, READVALUE && fnReadValue )
{
	uint32_t tRowID = BASE::m_tRowID;

	for ( size_t i = 0; i < dHashes.size(); i++ )
	{
		if ( BASE::template CompareStrings<SINGLEVALUE> ( (int)i, dHashes[i], dLengths[i], fnReadValue ) )
			*pRowID++ = tRowID;

		tRowID++;
	}

	BASE::m_tRowID = tRowID;
	return (int)dHashes.size();
}

//////////////////////////////////////////////////////////////////////////

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
class Analyzer_String_T : public Analyzer_T<HAVE_MATCHING_BLOCKS>, public Accessor_String_c
{
	using ANALYZER = Analyzer_T<HAVE_MATCHING_BLOCKS>;
	using ACCESSOR = Accessor_String_c;

public:
				Analyzer_String_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings );

	bool		GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) final;

private:
	AnalyzerBlock_Str_Const_T<USE_HASHES,EQ>	m_tBlockConst;
	AnalyzerBlock_Str_Table_T<USE_HASHES,EQ>	m_tBlockTable;
	AnalyzerBlock_Str_Values_T<USE_HASHES,EQ>	m_tBlockValues;

	const Filter_t &				m_tSettings;

	typedef int (Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::*ProcessSubblock_fn)( uint32_t * & pRowID, int iSubblockIdInBlock );
	std::array<ProcessSubblock_fn, to_underlying ( StrPacking_e::TOTAL )> m_dProcessingFuncs;
	ProcessSubblock_fn				m_fnProcessSubblock = nullptr;

	void		SetupPackingFuncs();

	int			ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock );
	int			ProcessSubblockTable ( uint32_t * & pRowID, int iSubblockIdInBlock );
	template<bool SINGLEVALUE> int	ProcessSubblockConstLen ( uint32_t * & pRowID, int iSubblockIdInBlock );
	template<bool SINGLEVALUE> int	ProcessSubblockGeneric ( uint32_t * & pRowID, int iSubblockIdInBlock );

	bool		MoveToBlock ( int iNextBlock ) final;
};

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::Analyzer_String_T ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings )
	: ANALYZER ( tHeader.GetSettings().m_iSubblockSize )
	, ACCESSOR ( tHeader, pReader, true )
	, m_tBlockConst ( ANALYZER::m_tRowID )
	, m_tBlockTable ( ANALYZER::m_tRowID )
	, m_tBlockValues ( ANALYZER::m_tRowID )
	, m_tSettings ( tSettings )
{
	m_tBlockConst.Setup(m_tSettings);
	m_tBlockTable.Setup(m_tSettings);
	m_tBlockValues.Setup(m_tSettings);

	SetupPackingFuncs();
}

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
bool Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
{
	return ANALYZER::GetNextRowIdBlock ( (ACCESSOR&)*this, dRowIdBlock, [this] ( uint32_t * & pRowID, int iSubblockIdInBlock ){ return (*this.*m_fnProcessSubblock) ( pRowID, iSubblockIdInBlock ); } );
}

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
void Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::SetupPackingFuncs()
{
	auto & dFuncs = m_dProcessingFuncs;
	for ( auto & i : dFuncs )
		i = nullptr;

	// doesn't depend on filter type; just fills result with rowids
	dFuncs [ to_underlying ( StrPacking_e::CONST ) ] = &Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockConst;

	// doesn't depend on filter type too; work off pre-calculated array
	dFuncs [ to_underlying ( StrPacking_e::TABLE ) ] = &Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockTable;

	switch ( m_tSettings.m_eType )
	{
	case FilterType_e::STRINGS:
		if ( m_tSettings.m_dStringValues.size()==1 )
		{
			dFuncs [ to_underlying ( StrPacking_e::CONSTLEN ) ]	= &Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockConstLen<true>;
			dFuncs [ to_underlying ( StrPacking_e::GENERIC ) ]	= &Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockGeneric<true>;
		}
		else
		{
			dFuncs [ to_underlying ( StrPacking_e::CONSTLEN ) ]	= &Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockConstLen<false>;
			dFuncs [ to_underlying ( StrPacking_e::GENERIC ) ]	= &Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockGeneric<false>;
		}
		break;

	default:
		assert ( 0 && "Unsupported filter type" );
		break;
	}
}

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
int Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockConst ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	return m_tBlockConst.ProcessSubblock ( pRowID, ACCESSOR::GetNumSubblockValues(iSubblockIdInBlock) );
}

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
int Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockTable ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockTable.ReadSubblock ( iSubblockIdInBlock, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock), *ACCESSOR::m_pReader );
	return m_tBlockTable.ProcessSubblock ( pRowID, ACCESSOR::m_tBlockTable.GetValueIndexes() );
}

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
template <bool SINGLEVALUE>
int Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockConstLen ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	int iNumSubblockValues = StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock);
	ACCESSOR::m_tBlockConstLen.ReadSubblock ( iSubblockIdInBlock, iNumSubblockValues, *ACCESSOR::m_pReader );

	// the idea is to postpone value reading to the point when all other options (hashes, lengths) are exhausted
	return m_tBlockValues.template ProcessSubblock_Values<SINGLEVALUE> ( pRowID, ACCESSOR::m_tBlockConstLen.GetAllHashes(), ACCESSOR::m_tBlockConstLen.GetAllValueLengths(),
		[iSubblockIdInBlock,iNumSubblockValues,this]( int iValue )
		{
			auto dValues = ACCESSOR::m_tBlockConstLen.ReadAllSubblockValues ( iSubblockIdInBlock, iNumSubblockValues, *ACCESSOR::m_pReader );
			return dValues[iValue];
		} );
}

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
template <bool SINGLEVALUE>
int Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::ProcessSubblockGeneric ( uint32_t * & pRowID, int iSubblockIdInBlock )
{
	ACCESSOR::m_tBlockGeneric.ReadSubblock ( iSubblockIdInBlock, StoredBlockTraits_t::GetNumSubblockValues(iSubblockIdInBlock), *m_pReader );

	// the idea is to postpone value reading to the point when all other options (hashes, lengths) are exhausted
	return m_tBlockValues.template ProcessSubblock_Values<SINGLEVALUE> ( pRowID, ACCESSOR::m_tBlockGeneric.GetAllHashes(), ACCESSOR::m_tBlockGeneric.GetAllValueLengths(),
		[iSubblockIdInBlock,this]( int iValue )
		{
			auto dValues = ACCESSOR::m_tBlockGeneric.ReadAllSubblockValues ( iSubblockIdInBlock, *ACCESSOR::m_pReader );
			return dValues[iValue];
		} );
}

template <bool USE_HASHES, bool HAVE_MATCHING_BLOCKS, bool EQ>
bool Analyzer_String_T<USE_HASHES,HAVE_MATCHING_BLOCKS,EQ>::MoveToBlock ( int iNextBlock )
{
	while(true)
	{
		ANALYZER::StartBlockProcessing ( (ACCESSOR&)*this, iNextBlock );

		if ( ACCESSOR::m_ePacking!=StrPacking_e::CONST && ACCESSOR::m_ePacking!=StrPacking_e::TABLE )
			break;

		if ( ACCESSOR::m_ePacking==StrPacking_e::CONST )
		{
			if ( m_tBlockConst.SetupNextBlock ( ACCESSOR::m_tBlockConst ) )
				break;
		}
		else
		{
			if ( m_tBlockTable.SetupNextBlock ( ACCESSOR::m_tBlockTable ) )
				break;
		}

		if ( !ANALYZER::RewindToNextBlock ( (ACCESSOR&)*this, iNextBlock ) )
			return false;
	}

	m_fnProcessSubblock = m_dProcessingFuncs [ to_underlying ( ACCESSOR::m_ePacking ) ];
	assert ( m_fnProcessSubblock );

	return true;
}

//////////////////////////////////////////////////////////////////////////

Iterator_i * CreateIteratorStr ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const IteratorHints_t & tHints )
{
	return new Iterator_String_c ( tHeader, pReader, tHints );
}


Analyzer_i * CreateAnalyzerStr ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings, bool bHaveMatchingBlocks )
{
	bool bUseHashes = tHeader.HaveStringHashes() && tSettings.m_fnCalcStrHash;
	bool bEq = !tSettings.m_bExclude;
	int iIndex = 4*( bUseHashes ? 1 : 0 ) + 2*( bHaveMatchingBlocks ? 1 : 0 ) + ( bEq ? 1 : 0 );

	switch ( iIndex )
	{
	case 0:	return new Analyzer_String_T<false,false,false> ( tHeader, pReader, tSettings );
	case 1:	return new Analyzer_String_T<false,false,true>  ( tHeader, pReader, tSettings );
	case 2:	return new Analyzer_String_T<false,true, false> ( tHeader, pReader, tSettings );
	case 3:	return new Analyzer_String_T<false,true, true>  ( tHeader, pReader, tSettings );
	case 4:	return new Analyzer_String_T<true, false,false> ( tHeader, pReader, tSettings );
	case 5:	return new Analyzer_String_T<true, false,true>  ( tHeader, pReader, tSettings );
	case 6:	return new Analyzer_String_T<true, true, false> ( tHeader, pReader, tSettings );
	case 7:	return new Analyzer_String_T<true, true, true>  ( tHeader, pReader, tSettings );
	default: return nullptr;
	}
}

} // namespace columnar