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

#include "builder.h"
#include "secondary.h"
#include "blockreader.h"
#include "codec.h"
#include "delta.h"
#include "pgm.h"
#include "bitvec.h"

#include <queue>

// FastPFOR
#include "fastpfor.h"

#ifdef _MSC_VER
	#include <io.h>
#else
	#include <unistd.h>
#endif

using namespace util;
using namespace common;

namespace SI
{

#define BUILD_PRINT_VALUES 0
#define VALUES_PER_BLOCK 128
#define ROWIDS_PER_BLOCK 1024

/////////////////////////////////////////////////////////////////////

class SIWriter_i
{
public:
						SIWriter_i() = default;
	virtual				~SIWriter_i() = default;

	virtual bool		Setup ( const std::string & sSrcFile, uint64_t iFileSize, std::vector<uint64_t> & dOffset, std::string & sError ) = 0;
	virtual bool		Process ( FileWriter_c & tDstFile, FileWriter_c & tTmpBlocksOff, const std::string & sPgmValuesName, std::string & sError ) = 0;
	virtual const std::vector<uint8_t> & GetPGM() = 0;
	virtual uint32_t	GetCountDistinct() const = 0;
	virtual uint64_t	GetMin() const = 0;
	virtual uint64_t	GetMax() const = 0;
};


class RawWriter_i
{
public:
					RawWriter_i() = default;
	virtual			~RawWriter_i() = default;

	virtual bool	Setup ( const std::string & sFile, const SchemaAttr_t & tAttr, int iAttr, std::string & sError ) = 0;
	virtual int		GetItemSize () const = 0;
	virtual void	SetItemsCount ( size_t tSize ) = 0;

	virtual void	SetAttr ( uint32_t tRowID, int64_t tAttr ) = 0;
	virtual void	SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength ) = 0;
	virtual void	SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength ) = 0;

	virtual void	Flush () = 0;
	virtual void	Done() = 0;

	virtual SIWriter_i * GetWriter ( std::string & sError ) = 0;
	virtual std::string GetFilename() const = 0;
};

template<typename VALUE>
struct RawValue_T
{
	VALUE m_tValue = 0;
	uint32_t m_tRowid = 0;

	RawValue_T () = default;
	RawValue_T ( VALUE tVal, uint32_t tRowid )
		: m_tValue ( tVal )
		, m_tRowid ( tRowid )
	{}
};

template<typename VALUE>
bool RawValueCmp ( const VALUE & tA, const VALUE & tB )
{
	return ( tA.m_tValue==tB.m_tValue ? tA.m_tRowid<tB.m_tRowid : tA.m_tValue<tB.m_tValue );
}

template<>
bool RawValueCmp< RawValue_T<float> > ( const RawValue_T<float> & tA, const RawValue_T<float> & tB )
{
	if ( tA.m_tValue<tB.m_tValue )
		return true;
	if ( tA.m_tValue>tB.m_tValue )
		return false;

	return ( tA.m_tRowid<tB.m_tRowid );
}

template<typename VALUE>
class RawWriter_T : public RawWriter_i
{
	using RawValue_t = RawValue_T<VALUE>;

public:
			RawWriter_T ( const Settings_t & tSettings ) : m_tSettings(tSettings) {}

	bool	Setup ( const std::string & sFile, const SchemaAttr_t & tAttr, int iAttr, std::string & sError ) final;
	int		GetItemSize() const final { return sizeof ( m_dRows[0] ); }
	void	SetItemsCount ( size_t tSize ) final { m_dRows.reserve(tSize); }
	void	Flush() final;
	void	Done() final;
	void	SetAttr ( uint32_t tRowID, int64_t tAttr ) final;
	void	SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength ) final;
	void	SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength ) final;

	SIWriter_i *	GetWriter ( std::string & sError ) final;
	std::string		GetFilename() const final { return m_tFile.GetFilename(); }

private:
	Settings_t				m_tSettings;
	std::vector<RawValue_t>	m_dRows; // value, rowid
	std::vector<uint64_t>	m_dOffset;
	FileWriterNonBuffered_c	m_tFile;
	SchemaAttr_t			m_tAttr;
	uint64_t				m_iFileSize = 0;
};

template<typename VALUE>
bool RawWriter_T<VALUE>::Setup ( const std::string & sFile, const SchemaAttr_t & tAttr, int iAttr, std::string & sError )
{
	m_tAttr = tAttr;
	std::string sFilename = FormatStr ( "%s.%d.tmp", sFile.c_str(), iAttr );
	return m_tFile.Open ( sFilename, true, true, false, sError );
}

template<typename VALUE>
void RawWriter_T<VALUE>::Flush()
{
	size_t iBytesLen = sizeof( m_dRows[0] ) * m_dRows.size();
	if ( !iBytesLen )
		return;

	std::sort ( m_dRows.begin(), m_dRows.end(), RawValueCmp<RawValue_t> );

	m_dOffset.emplace_back ( m_tFile.GetPos() );
	m_tFile.Write ( (const uint8_t *)m_dRows.data(), iBytesLen );

	m_dRows.resize ( 0 ); 
}

template<typename VALUE>
void RawWriter_T<VALUE>::Done()
{
	Flush();
	m_iFileSize = m_tFile.GetPos();
	m_tFile.Close();
	VectorReset ( m_dRows );
}

// raw int writer
template<>
inline void RawWriter_T<uint32_t>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	m_dRows.emplace_back ( RawValue_T<uint32_t> { (uint32_t)tAttr, tRowID } );
}

template<>
inline void RawWriter_T<uint32_t>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

template<>
inline void RawWriter_T<int64_t>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

// raw int64 writer
template<>
inline void RawWriter_T<int64_t>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	m_dRows.emplace_back ( RawValue_T<int64_t> { (int64_t)tAttr, tRowID } );
}

// raw string writer
template<>
inline void RawWriter_T<uint64_t>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	assert ( m_tAttr.m_fnCalcHash );
	m_dRows.emplace_back ( RawValue_T<uint64_t> { ( iLength ? m_tAttr.m_fnCalcHash ( pData, iLength, STR_HASH_SEED ) : 0 ), tRowID } );
}

template<>
inline void RawWriter_T<uint64_t>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

template<>
inline void RawWriter_T<uint64_t>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to int packer" );
}

// raw MVA32 writer
template<>
inline void RawWriter_T<uint32_t>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	for ( int i=0; i<iLength; i++ )
		m_dRows.emplace_back ( RawValue_T<uint32_t> { (uint32_t)pData[i], tRowID } );
}

// raw MVA64 writer
template<>
inline void RawWriter_T<int64_t>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	for ( int i=0; i<iLength; i++ )
		m_dRows.emplace_back ( RawValue_T<int64_t> { pData[i], tRowID } );
}

// raw float writer
template<>
inline void RawWriter_T<float>::SetAttr ( uint32_t tRowID, int64_t tAttr )
{
	m_dRows.emplace_back ( RawValue_T<float> { UintToFloat ( tAttr ), tRowID } );
}

template<>
inline void RawWriter_T<float>::SetAttr ( uint32_t tRowID, const uint8_t * pData, int iLength )
{
	assert ( 0 && "INTERNAL ERROR: sending string to float packer" );
}

// raw floatvec writer
template<>
inline void RawWriter_T<float>::SetAttr ( uint32_t tRowID, const int64_t * pData, int iLength )
{
	for ( int i=0; i<iLength; i++ )
		m_dRows.emplace_back ( RawValue_T<float> { UintToFloat ( pData[i] ), tRowID } );
}

/////////////////////////////////////////////////////////////////////

template<typename VEC>
static void EncodeRowsBlock ( VEC & dSrcRows, uint32_t iOff, uint32_t iCount, IntCodec_i * pCodec, std::vector<uint32_t> & dBufRows, MemWriter_c & tWriter, bool bWriteSize )
{
	Span_T<uint32_t> dRows ( dSrcRows.data() + iOff, iCount );
	if ( FastPForLib::needPaddingTo128Bits( dRows.begin() ) )
	{
		memmove ( dSrcRows.data(), dSrcRows.data() + iOff, sizeof(dSrcRows[0]) * iCount );
		dRows = Span_T<uint32_t> ( dSrcRows.data(), iCount );
	}

	pCodec->EncodeDelta ( dRows, dBufRows );

	if ( bWriteSize )
		WriteVectorLen32 ( dBufRows, tWriter );
	else
		WriteVector ( dBufRows, tWriter );
}

template<typename T, typename WRITER>
void EncodeBlock ( std::vector<T> & dSrc, IntCodec_i * pCodec, std::vector<uint32_t> & dBuf, WRITER & tWriter )
{
	Span_T<T> tSpan (dSrc);
	pCodec->EncodeDelta ( tSpan, dBuf );
	WriteVectorLen32 ( dBuf, tWriter );
}

template<typename VEC>
void EncodeBlockWoDelta ( VEC & dSrc, IntCodec_i * pCodec, std::vector<uint32_t> & dBuf, FileWriter_c & tWriter )
{
	dBuf.resize ( 0 );

	pCodec->Encode ( dSrc, dBuf );
	WriteVectorLen32 ( dBuf, tWriter );
}

template<typename VALUE>
void WriteRawValues ( const std::vector<VALUE> & dSrc, FileWriter_c & tWriter ) = delete;

template<>
void WriteRawValues<> ( const std::vector<uint32_t> & dSrc, FileWriter_c & tWriter )
{
	for ( uint32_t uVal : dSrc )
		tWriter.Write_uint32 ( uVal );
}

template<>
void WriteRawValues<> ( const std::vector<uint64_t> & dSrc, FileWriter_c & tWriter )
{
	for ( uint64_t uVal : dSrc )
		tWriter.Write_uint64 ( uVal );
}

/////////////////////////////////////////////////////////////////////

template<typename SRC_VALUE, typename VALUE>
class RowWriter_T
{
public:
				RowWriter_T ( FileWriter_c * pBlocksOff, FileWriter_c * pPGMVals, const Settings_t & tSettings );

	void		Done ( FileWriter_c & tWriter )	{ FlushBlock ( tWriter ); }
	void		AddValue ( const RawValue_T<VALUE> & tBin );
	void		NextValue ( const RawValue_T<VALUE> & tBin, FileWriter_c & m_tDstFile );
	uint32_t	GetCountDistinct() const	{ return m_uTotalValues; }
	SRC_VALUE	GetMin() const				{ return m_tMin; }
	SRC_VALUE	GetMax() const				{ return m_tMax; }

private:
	static const bool IS_FLOAT_VALUE = std::is_floating_point<SRC_VALUE>::value;

	std::vector<VALUE>		m_dValues;
	std::vector<uint32_t>	m_dTypes;
	std::vector<uint32_t>	m_dCount;
	std::vector<uint32_t>	m_dRowStart;
	std::vector<uint32_t>	m_dMin;
	std::vector<uint32_t>	m_dMax;
	std::vector<uint32_t>	m_dRows;
	std::vector<uint32_t>	m_dMinMax;
	std::vector<uint32_t>	m_dBlockOffsets;

	std::vector<uint32_t>	m_dBufTmp;
	std::vector<uint8_t>	m_dRowsPacked;
	std::vector<uint8_t>	m_dTmp;
	VALUE					m_tLastValue = 0;
	SRC_VALUE				m_tMin = (SRC_VALUE)0;
	SRC_VALUE				m_tMax = (SRC_VALUE)0;
	uint32_t				m_uTotalValues = 0;

	std::unique_ptr<IntCodec_i>	m_pCodec { nullptr };

	FileWriter_c *			m_pBlocksOff = nullptr;
	FileWriter_c *			m_pPGMVals = nullptr;

	void	FlushValue ( FileWriter_c & tWriter );
	void	WriteSingleRow ( int iItem, uint32_t uSrcRowsStart );
	void	WriteSingleBlock ( int iItem, uint32_t uSrcRowsStart, uint32_t uSrcRowsCount, MemWriter_c & tBlockWriter );
	void	WriteBlockList ( int iItem, uint32_t uSrcRowsStart, uint32_t uSrcRowsCount, MemWriter_c & tBlockWriter );
	void	ResetData();
	void	FlushBlock ( FileWriter_c & tWriter );
	void	DetermineMinMax();
};

template<typename SRC_VALUE, typename VALUE>
RowWriter_T<SRC_VALUE, VALUE>::RowWriter_T ( FileWriter_c * pBlocksOff, FileWriter_c * pPGMVals, const Settings_t & tSettings )
	: m_pBlocksOff ( pBlocksOff )
	, m_pPGMVals ( pPGMVals )
{
	m_dValues.reserve ( VALUES_PER_BLOCK );
	m_dRowStart.reserve ( VALUES_PER_BLOCK );
	m_dRows.reserve ( VALUES_PER_BLOCK * 16 );

	m_dBufTmp.reserve ( VALUES_PER_BLOCK );
	m_dRowsPacked.reserve ( VALUES_PER_BLOCK * 16 );

	m_pCodec.reset ( CreateIntCodec ( tSettings.m_sCompressionUINT32, tSettings.m_sCompressionUINT64 ) );
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::AddValue ( const RawValue_T<VALUE> & tBin )
{
	m_dRowStart.push_back ( (uint32_t)m_dRows.size() );

	m_dValues.push_back ( tBin.m_tValue );
	m_dRows.push_back ( tBin.m_tRowid );
	m_tLastValue = tBin.m_tValue;
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::NextValue ( const RawValue_T<VALUE> & tBin, FileWriter_c & m_tDstFile )
{
	// collect row-list
	// or flush and store new value
	if ( IS_FLOAT_VALUE && ( FloatEqual ( UintToFloat ( m_tLastValue ), UintToFloat ( tBin.m_tValue ) ) ) )
		m_dRows.push_back ( tBin.m_tRowid );
	else if ( !IS_FLOAT_VALUE && m_tLastValue==tBin.m_tValue ) 
		m_dRows.push_back ( tBin.m_tRowid );
	else
	{
		FlushValue(m_tDstFile);
		AddValue(tBin);
	}
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::FlushValue ( FileWriter_c & tWriter )
{
	if ( m_dValues.size()<VALUES_PER_BLOCK )
		return;

	FlushBlock ( tWriter );
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::WriteSingleRow ( int iItem, uint32_t uSrcRowsStart )
{
	m_dTypes[iItem] = (uint32_t)Packing_e::ROW;
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::WriteSingleBlock ( int iItem, uint32_t uSrcRowsStart, uint32_t uSrcRowsCount, MemWriter_c & tBlockWriter )
{
	m_dTypes[iItem] = (uint32_t)Packing_e::ROW_BLOCK;
	EncodeRowsBlock ( m_dRows, uSrcRowsStart, (int)uSrcRowsCount, m_pCodec.get(), m_dBufTmp, tBlockWriter, true );
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::WriteBlockList ( int iItem, uint32_t uSrcRowsStart, uint32_t uSrcRowsCount, MemWriter_c & tBlockWriter )
{
	m_dTypes[iItem] = (uint32_t)Packing_e::ROW_BLOCKS_LIST;

	int iBlocks = (int)( ( uSrcRowsCount + ROWIDS_PER_BLOCK - 1 ) / ROWIDS_PER_BLOCK );
	m_dMinMax.resize(iBlocks*2);
	for ( int iBlock=0; iBlock<iBlocks; iBlock++ )
	{
		uint32_t uSrcStart = uSrcRowsStart + iBlock*ROWIDS_PER_BLOCK;
		uint32_t uSrcCount = iBlock<iBlocks-1 ? ROWIDS_PER_BLOCK : (uint32_t)( uSrcRowsCount - ( iBlock * ROWIDS_PER_BLOCK ) );

		m_dMinMax[iBlock*2]		= m_dRows[uSrcStart];
		m_dMinMax[iBlock*2+1]	= m_dRows[uSrcStart + uSrcCount - 1];
	}

	tBlockWriter.Pack_uint32(iBlocks);
	EncodeBlock ( m_dMinMax, m_pCodec.get(), m_dBufTmp, tBlockWriter );

	// encode blocks to temporary memory storage
	m_dBlockOffsets.resize(iBlocks);
	m_dTmp.resize(0);
	MemWriter_c tTmpWriter ( m_dTmp );
	for ( int iBlock=0; iBlock<iBlocks; iBlock++ )
	{
		uint32_t uSrcStart = uSrcRowsStart + iBlock*ROWIDS_PER_BLOCK;
		uint32_t uSrcCount = iBlock<iBlocks-1 ? ROWIDS_PER_BLOCK : (uint32_t)( uSrcRowsCount - ( iBlock * ROWIDS_PER_BLOCK ) );

		EncodeRowsBlock ( m_dRows, uSrcStart, uSrcCount, m_pCodec.get(), m_dBufTmp, tTmpWriter, false );
		int64_t iPos = tTmpWriter.GetPos();
		assert ( !(iPos % 4) );
		m_dBlockOffsets[iBlock] = iPos>>2;
	}

	EncodeBlock ( m_dBlockOffsets, m_pCodec.get(), m_dBufTmp, tBlockWriter );
	tBlockWriter.Write ( &m_dTmp.front(), m_dTmp.size()  );
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::ResetData()
{
	m_dValues.resize(0);
	m_dTypes.resize(0);
	m_dRowStart.resize(0);
	m_dCount.resize(0);
	m_dMin.resize(0);
	m_dMax.resize(0);
	m_dRows.resize(0);
	m_dRowsPacked.resize(0);
	m_dTmp.resize(0);
	m_dMinMax.resize(0);
	m_dBlockOffsets.resize(0);
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::FlushBlock ( FileWriter_c & tWriter )
{
	assert ( m_dValues.size()==m_dRowStart.size() );
	if ( !m_dValues.size() )
		return;

	const uint32_t iValues = (uint32_t)m_dValues.size();
	// FIXME!!! set flags: IsValsAsc \ IsValsDesc and CalcDelta with these flags or skip delta encoding
	//assert ( std::is_sorted ( m_dValues.begin(), m_dValues.end() ) );

	// FIXME!!! pack per block meta

	DetermineMinMax();
	m_uTotalValues += iValues;

	// pack rows
	MemWriter_c tBlockWriter ( m_dRowsPacked );
	m_dTypes.resize ( iValues );
	m_dMin.resize ( iValues );
	m_dMax.resize ( iValues );
	m_dCount.resize ( iValues );
	for ( size_t iItem=0; iItem<iValues; iItem++)
	{
		uint32_t uSrcRowsStart = m_dRowStart[iItem];
		size_t uSrcRowsCount = (  iItem+1<m_dRowStart.size() ? m_dRowStart[iItem+1] - uSrcRowsStart : m_dRows.size() - uSrcRowsStart );

		m_dRowStart[iItem] = (uint32_t)tBlockWriter.GetPos();
		m_dMin[iItem] = m_dRows[uSrcRowsStart];
		m_dMax[iItem] = m_dRows[uSrcRowsStart + uSrcRowsCount - 1];
		m_dCount[iItem] = uSrcRowsCount;

		if ( uSrcRowsCount==1 )
			WriteSingleRow ( iItem, uSrcRowsStart );
		else if ( uSrcRowsCount<=ROWIDS_PER_BLOCK )
			WriteSingleBlock ( iItem, uSrcRowsStart, uSrcRowsCount, tBlockWriter );
		else
			WriteBlockList ( iItem, uSrcRowsStart, uSrcRowsCount, tBlockWriter );
	}

	// write offset to block into temporary file
	m_pBlocksOff->Write_uint64 ( tWriter.GetPos() );
	// write values for PGM builder
	WriteRawValues ( m_dValues, *m_pPGMVals );

	// write into file
	EncodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, tWriter );
	EncodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, tWriter );
	EncodeBlock ( m_dMin, m_pCodec.get(), m_dBufTmp, tWriter );
	EncodeBlock ( m_dMax, m_pCodec.get(), m_dBufTmp, tWriter );
	EncodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, tWriter );
	EncodeBlockWoDelta ( m_dCount, m_pCodec.get(), m_dBufTmp, tWriter );
	WriteVector ( m_dRowsPacked, tWriter );

	ResetData();
}

template<typename SRC_VALUE, typename VALUE>
void RowWriter_T<SRC_VALUE, VALUE>::DetermineMinMax()
{
	if ( m_dValues.empty() )
		return;

	SRC_VALUE tMin, tMax;
	if constexpr  ( IS_FLOAT_VALUE )
	{
		tMin = UintToFloat ( m_dValues[0] );
		tMax = tMin;
		for ( auto i : m_dValues )
		{
			tMin = std::min ( tMin, UintToFloat(i) );
			tMax = std::max ( tMax, UintToFloat(i) );
		}
	}
	else
	{
		tMin = (SRC_VALUE)m_dValues[0];
		tMax = tMin;
		for ( auto i : m_dValues )
		{
			tMin = std::min ( tMin, (SRC_VALUE)i );
			tMax = std::max ( tMax, (SRC_VALUE)i );
		}
	}

	if ( m_uTotalValues )
	{
		m_tMin = std::min ( m_tMin, tMin );
		m_tMax = std::max ( m_tMax, tMax );
	}
	else
	{
		m_tMin = tMin;
		m_tMax = tMax;
	}
}

/////////////////////////////////////////////////////////////////////

template<typename VALUE>
struct BinValue_T : public RawValue_T<VALUE>
{
	FileReader_c * m_pReader = nullptr;
	int64_t m_iBinEnd = 0;

	bool Read ()
	{
		if ( m_pReader->GetPos()>=m_iBinEnd )
			return false;
		
		m_pReader->Read ( (uint8_t *)( this ), sizeof ( RawValue_T<VALUE> ) );
		return true;
	}
};

template<typename VALUE>
struct PQGreater
{
	bool operator() ( const BinValue_T<VALUE> & tA, const BinValue_T<VALUE> & tB ) const;
};

template<typename VALUE>
bool PQGreater<VALUE>::operator() ( const BinValue_T<VALUE> & tA, const BinValue_T<VALUE> & tB ) const
{
	return ( tA.m_tValue==tB.m_tValue ? tA.m_tRowid>tB.m_tRowid : tA.m_tValue>tB.m_tValue );
}

template<>
bool PQGreater<float>::operator() ( const BinValue_T<float> & tA, const BinValue_T<float> & tB ) const
{
	if ( tA.m_tValue>tB.m_tValue )
		return true;
	if (tA.m_tValue<tB.m_tValue)
		return false;
	return ( tA.m_tRowid>tB.m_tRowid );
}

/////////////////////////////////////////////////////////////////////

template<typename SRC_VALUE, typename DST_VALUE>
class SIWriter_T : public SIWriter_i
{
public:
				SIWriter_T ( const Settings_t & tSettings ) : m_tSettings ( tSettings ) {}

	bool		Setup ( const std::string & sSrcFile, uint64_t iFileSize, std::vector<uint64_t> & dOffset, std::string & sError ) final;
	bool		Process ( FileWriter_c & tDstFile, FileWriter_c & tTmpBlocksOff, const std::string & sPgmValuesName, std::string & sError ) final;
	const std::vector<uint8_t> & GetPGM() { return m_dPGM; }
	uint32_t	GetCountDistinct() const final { return m_uCountDistinct; }
	uint64_t	GetMin() const final;
	uint64_t	GetMax() const final;

private:
	static const bool IS_FLOAT_VALUE = std::is_floating_point<SRC_VALUE>::value;

	Settings_t				m_tSettings;
	std::string				m_sSrcName;
	uint64_t				m_iFileSize = 0;
	uint32_t				m_uCountDistinct = 0;
	SRC_VALUE				m_tMin = (SRC_VALUE)0;
	SRC_VALUE				m_tMax = (SRC_VALUE)0;
	std::vector<uint8_t>	m_dPGM;
	std::vector<uint64_t>	m_dOffset;
};

template<typename SRC_VALUE, typename DST_VALUE>
bool SIWriter_T<SRC_VALUE, DST_VALUE>::Setup ( const std::string & sSrcName, uint64_t iFileSize, std::vector<uint64_t> & dOffset, std::string & sError )
{
	m_dOffset = std::move(dOffset);
	m_sSrcName = sSrcName;
	m_iFileSize = iFileSize;

	return true;
}

template<typename SRC_VALUE, typename DST_VALUE>
bool SIWriter_T<SRC_VALUE, DST_VALUE>::Process ( FileWriter_c & tDstFile, FileWriter_c & tTmpBlocksOff, const std::string & sPgmValuesName, std::string & sError )
{
#if BUILD_PRINT_VALUES
	std::cout << m_sSrcName << std::endl;
#endif

	FileWriter_c tTmpValsPGM;
	if ( !tTmpValsPGM.Open ( sPgmValuesName, true, false, true, sError ) )
		return false;

	std::priority_queue< BinValue_T<SRC_VALUE>, std::vector < BinValue_T<SRC_VALUE> >, PQGreater<SRC_VALUE> > dBins;

	std::vector<std::unique_ptr< FileReader_c > > dSrcFile ( m_dOffset.size() );
	for ( int iReader=0; iReader<m_dOffset.size(); iReader++ )
	{
		FileReader_c * pReader = new FileReader_c();
		dSrcFile[iReader].reset ( pReader );

		if ( !pReader->Open ( m_sSrcName, sError ) )
			return false;

		pReader->Seek ( m_dOffset[iReader] );
		// set file chunk end
		int64_t iBinEnd = 0;
		if ( iReader<m_dOffset.size()-1 )
			iBinEnd = m_dOffset[iReader+1];
		else
			iBinEnd = m_iFileSize;

		BinValue_T<SRC_VALUE> tBin;
		tBin.m_pReader = pReader;
		tBin.m_iBinEnd = iBinEnd;
		tBin.Read();

		dBins.push ( tBin );
	}

	RowWriter_T<SRC_VALUE, DST_VALUE> tWriter ( &tTmpBlocksOff, &tTmpValsPGM, m_tSettings );

	// initial fill
	if ( dBins.size() )
	{
		BinValue_T<SRC_VALUE> tBin = dBins.top();
		dBins.pop();
		tWriter.AddValue ( Convert ( tBin ) );
		if ( tBin.Read() )
			dBins.push ( tBin );
	}

	while ( !dBins.empty() )
	{
		BinValue_T<SRC_VALUE> tBin = dBins.top();
		dBins.pop();

		tWriter.NextValue ( Convert ( tBin ), tDstFile );

		if ( tBin.Read() )
			dBins.push ( tBin );
	}

	tWriter.Done(tDstFile);

	m_uCountDistinct	= tWriter.GetCountDistinct();
	m_tMin				= tWriter.GetMin();
	m_tMax				= tWriter.GetMax();

	dSrcFile.clear(); // to free up memory for PGM build phase
	::unlink ( m_sSrcName.c_str() );

	tTmpValsPGM.Close();
	MappedBuffer_T<SRC_VALUE> tMappedPGM;
	if ( !tMappedPGM.Open ( sPgmValuesName, sError ) )
		return false;

	assert ( std::is_sorted ( tMappedPGM.begin(), tMappedPGM.end() ) );
	PGM_T<SRC_VALUE> tInv ( tMappedPGM.begin(), tMappedPGM.end() );
	tInv.Save ( m_dPGM );

#if BUILD_PRINT_VALUES
	for ( int i=0; i<tPGMVals.size(); i++ )
	{
		ApproxPos_t tRes = tInv.Search ( tPGMVals[i] );
		if ( tRes.m_iLo/3!=tRes.m_iHi/3 )
			std::cout << "val[" << i << "] " << (uint32_t)tPGMVals[i] << ", lo " << tRes.m_iLo/3 << ", hi " << tRes.m_iHi/3 << ", pos " << tRes.m_iPos/3 << " " << std::endl;
	}
#endif

	return true;
}

template<typename SRC_VALUE, typename DST_VALUE>
uint64_t SIWriter_T<SRC_VALUE, DST_VALUE>::GetMin() const
{
	if constexpr ( IS_FLOAT_VALUE )
		return FloatToUint(m_tMin);

	return (uint64_t)m_tMin;
}

template<typename SRC_VALUE, typename DST_VALUE>
uint64_t SIWriter_T<SRC_VALUE, DST_VALUE>::GetMax() const
{
	if constexpr ( IS_FLOAT_VALUE )
		return FloatToUint(m_tMax);

	return (uint64_t)m_tMax;
}

/////////////////////////////////////////////////////////////////////

template<typename VALUE>
SIWriter_i * RawWriter_T<VALUE>::GetWriter ( std::string & sError )
{
	std::unique_ptr<SIWriter_i> pWriter { nullptr };
	switch ( m_tAttr.m_eType )
	{
	case AttrType_e::FLOAT:
	case AttrType_e::FLOATVEC:
		pWriter.reset ( new SIWriter_T<float, uint32_t>(m_tSettings) );
		break;

	case AttrType_e::STRING:
		pWriter.reset ( new SIWriter_T<uint64_t, uint64_t>(m_tSettings) );
		break;

	case AttrType_e::INT64:
	case AttrType_e::INT64SET:
		pWriter.reset ( new SIWriter_T<int64_t, uint64_t>(m_tSettings) );
		break;

	default:
		pWriter.reset ( new SIWriter_T<uint32_t, uint32_t>(m_tSettings) );
		break;
	}

	if ( !pWriter->Setup ( m_tFile.GetFilename(), m_iFileSize, m_dOffset, sError ) )
		return nullptr;

	return pWriter.release();
}

/////////////////////////////////////////////////////////////////////

struct ScopedFilesRemoval_t
{
	~ScopedFilesRemoval_t()
	{
		for ( const std::string & sFile : m_dFiles )
		{
			if ( IsFileExists ( sFile ) )
				::unlink ( sFile.c_str() );
		}
	}
	std::vector<std::string> m_dFiles;
};

/////////////////////////////////////////////////////////////////////

class Builder_c final : public Builder_i
{
public:
	virtual ~Builder_c();
	bool	Setup ( const Settings_t & tSettings, const Schema_t & tSchema, size_t tMemoryLimit, const std::string & sFile, size_t tBufferSize, std::string & sError );

	void	SetRowID ( uint32_t tRowID ) final;
	void	SetAttr ( int iAttr, int64_t tAttr ) final;
	void	SetAttr ( int iAttr, const uint8_t * pData, int iLength ) final;
	void	SetAttr ( int iAttr, const int64_t * pData, int iLength ) final;
	bool	Done ( std::string & sError ) final;

private:
	std::string	m_sFile;
	size_t		m_tBufferSize = 0;
	uint32_t	m_tRowID = 0;
	uint32_t	m_uMaxRows = 0;

	std::vector<std::shared_ptr<RawWriter_i>>	m_dRawWriter;
	std::vector<std::shared_ptr<SIWriter_i>>	m_dCidWriter;

	std::vector<ColumnInfo_t>					m_dAttrs;
	ScopedFilesRemoval_t						m_tCleanup;

	void Flush();
	bool WriteMeta ( const std::string & sPgmName, const std::string & sBlocksName, const std::vector<uint64_t> & dBlocksOffStart, const std::vector<uint64_t> & dBlocksCount, uint64_t uMetaOff, std::string & sError ) const;
};

Builder_c::~Builder_c()
{
	// need to close all writers prior to cleanup step
	// in case of error and clean up take place with the opened files
	VectorReset ( m_dRawWriter );
	VectorReset ( m_dCidWriter );
}

bool Builder_c::Setup ( const Settings_t & tSettings, const Schema_t & tSchema, size_t tMemoryLimit, const std::string & sFile, size_t tBufferSize, std::string & sError )
{
	m_sFile = sFile;
	m_tBufferSize = tBufferSize;

	int iAttr = 0;

	for ( const auto & tSrcAttr : tSchema )
	{
		std::shared_ptr<RawWriter_i> pWriter;
		switch ( tSrcAttr.m_eType )
		{
		case AttrType_e::UINT32:
		case AttrType_e::TIMESTAMP:
		case AttrType_e::UINT32SET:
		case AttrType_e::BOOLEAN:
			pWriter.reset ( new RawWriter_T<uint32_t>(tSettings) );
			break;

		case AttrType_e::FLOAT:
		case AttrType_e::FLOATVEC:
			pWriter.reset ( new RawWriter_T<float>(tSettings) );
			break;

		case AttrType_e::STRING:
			pWriter.reset ( new RawWriter_T<uint64_t>(tSettings) );
			break;

		case AttrType_e::INT64:
		case AttrType_e::INT64SET:
			pWriter.reset ( new RawWriter_T<int64_t>(tSettings) );
			break;

		default:
			break;
		}

		if ( !pWriter )
		{
			sError = FormatStr ( "unable to create secondary index for attribute '%s'", tSrcAttr.m_sName.c_str() );
			return false;
		}

		bool bOpened = pWriter->Setup ( sFile, tSrcAttr, iAttr++, sError );
		if ( pWriter ) // should track files and remove all tmp file on any error
			m_tCleanup.m_dFiles.push_back ( pWriter->GetFilename() );

		if ( !bOpened )
			return false;

		m_dRawWriter.push_back ( pWriter );
		ColumnInfo_t tInfo;
		tInfo.m_eType = tSrcAttr.m_eType;
		tInfo.m_sName = tSrcAttr.m_sName;
		m_dAttrs.push_back ( tInfo );
	}

	int iRowSize = 0;
	for ( const auto & pWriter : m_dRawWriter )
		if ( pWriter )
			iRowSize += pWriter->GetItemSize();

	m_uMaxRows = std::min ( std::max ( size_t(10000), tMemoryLimit / iRowSize ), size_t(UINT32_MAX) );

	for ( auto & pWriter : m_dRawWriter )
	{
		if ( !pWriter )
			continue;

		pWriter->SetItemsCount ( m_uMaxRows );
	}

	return true;
}

void Builder_c::SetRowID ( uint32_t tRowID )
{
	m_tRowID = tRowID;

	if ( ( m_tRowID % m_uMaxRows )==0 )
		Flush();
}

void Builder_c::SetAttr ( int iAttr, int64_t tAttr )
{
	if ( iAttr<m_dRawWriter.size() && m_dRawWriter[iAttr] )
		m_dRawWriter[iAttr]->SetAttr ( m_tRowID, tAttr );
}

void Builder_c::SetAttr ( int iAttr, const uint8_t * pData, int iLength )
{
	if ( iAttr<m_dRawWriter.size() && m_dRawWriter[iAttr] )
		m_dRawWriter[iAttr]->SetAttr ( m_tRowID, pData, iLength );
}

void Builder_c::SetAttr ( int iAttr, const int64_t * pData, int iLength )
{
	if ( iAttr<m_dRawWriter.size() && m_dRawWriter[iAttr] )
		m_dRawWriter[iAttr]->SetAttr ( m_tRowID, pData, iLength );
}

bool Builder_c::Done ( std::string & sError )
{
	// flush tail attributes
	for ( auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
			pWriter->Done();
	}

	// create Secondary Index writers
	for ( auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
		{
			SIWriter_i * pCidx = pWriter->GetWriter(sError);
			if ( !pCidx )
				return false;
			m_dCidWriter.emplace_back ( pCidx );
		}
	}

	// free memory
	VectorReset ( m_dRawWriter );

	// pack values into lists
	FileWriter_c tDstFile;
	tDstFile.SetBufferSize(m_tBufferSize);
	if ( !tDstFile.Open ( m_sFile, true, true, false, sError ) )
		return false;

	std::string sBlocksName = m_sFile + ".tmp.meta";
	FileWriter_c tTmpBlocks;
	if ( !tTmpBlocks.Open ( sBlocksName, true, true, true, sError ) )
		return false;

	std::string sPgmName = m_sFile + ".tmp.pgm";
	FileWriter_c tTmpPgm;
	if ( !tTmpPgm.Open ( sPgmName, true, true, true, sError ) )
		return false;

	std::string sPgmValuesName = m_sFile + ".tmp.pgmvalues";

	// reserve space at main file for meta
	tDstFile.Write_uint32 ( STORAGE_VERSION ); // storage version
	tDstFile.Write_uint64 ( 0 ); // offset to meta itself

	std::vector<uint64_t> dBlocksOffStart ( m_dCidWriter.size() );
	std::vector<uint64_t> dBlocksCount ( m_dCidWriter.size() );

	// process raw attributes into column index
	for ( size_t iWriter=0; iWriter<m_dCidWriter.size(); iWriter++ )
	{
		dBlocksOffStart[iWriter] = tTmpBlocks.GetPos();

		auto & pWriter = m_dCidWriter[iWriter];
		if ( !pWriter->Process ( tDstFile, tTmpBlocks, sPgmValuesName, sError ) )
			return false;

		// temp meta
		WriteVectorLen ( pWriter->GetPGM(), tTmpPgm );
	
		m_dAttrs[iWriter].m_uCountDistinct = pWriter->GetCountDistinct();
		m_dAttrs[iWriter].m_tMin = pWriter->GetMin();
		m_dAttrs[iWriter].m_tMax = pWriter->GetMax();

		// clean up used memory
		m_dCidWriter[iWriter] = nullptr;
	}

	int64_t iLastBlock = tTmpBlocks.GetPos();
	for ( size_t iBlock=1; iBlock<dBlocksCount.size(); iBlock++ )
		dBlocksCount[iBlock-1] = ( dBlocksOffStart[iBlock] - dBlocksOffStart[iBlock-1] ) / sizeof ( dBlocksOffStart[iBlock] );

	dBlocksCount.back() = ( iLastBlock - dBlocksOffStart.back() ) / sizeof ( dBlocksOffStart.back() );

	// meta
	uint64_t uMetaOff = tDstFile.GetPos();
	tDstFile.Close();
	// close temp writers
	tTmpBlocks.Close();
	tTmpPgm.Close();

	// write header and meta
	ComputeDeltas ( dBlocksOffStart.data(), (int)dBlocksOffStart.size(), true );
	return WriteMeta ( sPgmName, sBlocksName, dBlocksOffStart, dBlocksCount, uMetaOff, sError );
}

bool Builder_c::WriteMeta ( const std::string & sPgmName, const std::string & sBlocksName, const std::vector<uint64_t> & dBlocksOffStart, const std::vector<uint64_t> & dBlocksCount, uint64_t uMetaOff, std::string & sError ) const
{
	uint64_t uNextMeta = 0;

	{
		FileWriter_c tDstFile;
		if ( !tDstFile.Open ( m_sFile, false, false, false, sError ) )
			return false;

		// put meta offset to the begining
		tDstFile.Seek ( sizeof(uint32_t) );
		tDstFile.Write_uint64 ( uMetaOff );

		// append meta after blocks
		tDstFile.Seek ( uMetaOff );

		tDstFile.Write_uint64 ( uNextMeta ); // link to next meta
		tDstFile.Write_uint32 ( (int)m_dAttrs.size() );
		
		BitVec_c dAttrsEnabled ( m_dAttrs.size() );
		dAttrsEnabled.SetAllBits();
		WriteVector ( dAttrsEnabled.GetData(), tDstFile );

		Settings_t tSettings;
		tSettings.Save(tDstFile);
		tDstFile.Write_uint32 ( VALUES_PER_BLOCK );
		tDstFile.Write_uint32 ( ROWIDS_PER_BLOCK );
		
		// write schema
		for ( const auto & i : m_dAttrs )
			i.Save(tDstFile);

		WriteVectorPacked ( dBlocksOffStart, tDstFile );
		WriteVectorPacked ( dBlocksCount, tDstFile );
	}

	// append pgm indexes after meta
	if ( !CopySingleFile ( sPgmName, m_sFile, sError, 0 ) )
		return false;
	
	// append offsets to blocks
	if ( !CopySingleFile ( sBlocksName, m_sFile, sError, 0 ) )
		return false;

	return true;
}

void Builder_c::Flush()
{
	for ( auto & pWriter : m_dRawWriter )
	{
		if ( pWriter )
			pWriter->Flush();
	}
}


RawValue_T<uint32_t> Convert ( const BinValue_T<uint32_t> & tSrc )
{
	return tSrc;
}


RawValue_T<uint32_t> Convert ( const BinValue_T<float> & tSrc )
{
	RawValue_T<uint32_t> tRes;
	tRes.m_tValue = FloatToUint ( tSrc.m_tValue );
	tRes.m_tRowid = tSrc.m_tRowid;
	return tRes;
}


RawValue_T<uint64_t> Convert ( const BinValue_T<int64_t> & tSrc )
{
	RawValue_T<uint64_t> tRes;
	tRes.m_tValue = (uint64_t)tSrc.m_tValue;
	tRes.m_tRowid = tSrc.m_tRowid;
	return tRes;
}


RawValue_T<uint64_t> Convert ( const BinValue_T<uint64_t> & tSrc )
{
	return tSrc;
}

} // namespace SI


SI::Builder_i * CreateBuilder ( const Schema_t & tSchema, size_t tMemoryLimit, const std::string & sFile, size_t tBufferSize, std::string & sError )
{
	std::unique_ptr<SI::Builder_c> pBuilder ( new SI::Builder_c );
	SI::Settings_t tSettings;
	if ( !pBuilder->Setup ( tSettings, tSchema, tMemoryLimit, sFile, tBufferSize, sError ) )
		return nullptr;

	return pBuilder.release();
}

int GetSecondaryLibVersion()
{
	return SI::LIB_VERSION;
}

extern const char * LIB_VERSION;
const char * GetSecondaryLibVersionStr()
{
	return LIB_VERSION;
}

