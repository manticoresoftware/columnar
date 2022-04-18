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

#include "sidx.h"

#include "codec.h"
#include "delta.h"
#include "pgm.h"

#include <unordered_map>
#include <vector>

namespace SI
{
	using namespace columnar;

	static const int g_iReaderBufSize { 256 };
	static const int g_iBlocksReaderBufSize { 1024 };

	class SITrait_c : public Index_i
	{
	public:
		bool Setup ( const std::string & sFile, std::string & sError );
		ColumnInfo_t GetColumn ( const char * sName ) const override;
		bool GetValsRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<RowidIterator_i *> & dRes ) const override;
		bool GetRangeRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<RowidIterator_i *> & dRes ) const override;

		bool SaveMeta ( std::string & sError ) override;
		void ColumnUpdated ( const char * sName ) override;

		FilterContext_i * CreateFilterContext () const override;

		Collation_e GetCollation() const override;
		uint64_t GetHash ( const char * sVal ) const override;

	private:

		std::string m_sCompressionUINT32;
		std::string m_sCompressionUINT64;
		int m_iValuesPerBlock = { 1 };

		uint64_t m_uMetaOff { 0 };
		uint64_t m_uNextMetaOff { 0 };

		std::vector<ColumnInfo_t> m_dAttrs;
		bool m_bUpdated { false };
		std::unordered_map<std::string, int> m_hAttrs;
		std::vector<uint64_t> m_dBlockStartOff;			// per attribute vector of offsets to every block of values-rows-meta
		std::vector<uint64_t> m_dBlocksCount;			// per attribute vector of blocks count
		std::vector<std::shared_ptr<PGM_i>> m_dIdx;
		int64_t m_iBlocksBase = 0;						// start of offsets at file

		std::string m_sFileName;

		Collation_e m_eCollation;
		StrHash_fn m_fnHash { nullptr };
	};

	bool SITrait_c::Setup ( const std::string & sFile, std::string & sError )
	{
		columnar::FileReader_c tReader;
		if ( !tReader.Open ( sFile, sError ) )
			return false;

		int iVersion = tReader.Read_uint32();
		if ( iVersion>LIB_VERSION )
		{
			sError = FormatStr ( "Unable to load inverted index: %s is v.%d, binary is v.%d", sFile.c_str(), iVersion, LIB_VERSION );
			return false;
		}
		
		m_sFileName = sFile;
		m_uMetaOff = tReader.Read_uint64();
		
		tReader.Seek ( m_uMetaOff );

		// raw non packed data first
		m_uNextMetaOff = tReader.Read_uint64();
		int iAttrsCount = tReader.Read_uint32();

		BitVec_t dAttrsEnabled ( iAttrsCount );
		ReadVectorData ( dAttrsEnabled.m_dData, tReader );

		m_sCompressionUINT32 = tReader.Read_string();
		m_sCompressionUINT64 = tReader.Read_string();
		m_eCollation = (Collation_e)tReader.Read_uint32();
		m_iValuesPerBlock = tReader.Read_uint32();

		m_dAttrs.resize ( iAttrsCount );
		for ( int i=0; i<iAttrsCount; i++ )
		{
			ColumnInfo_t & tAttr = m_dAttrs[i];
			tAttr.m_sName = tReader.Read_string();
			tAttr.m_iSrcAttr = tReader.Unpack_uint32();
			tAttr.m_iAttr = tReader.Unpack_uint32();
			tAttr.m_eType = (AttrType_e)tReader.Unpack_uint32();
			tAttr.m_bEnabled = dAttrsEnabled.BitGet ( i );
		}

		ReadVectorPacked ( m_dBlockStartOff, tReader );
		ComputeInverseDeltas ( m_dBlockStartOff, true );
		ReadVectorPacked ( m_dBlocksCount, tReader );

		m_dIdx.resize ( m_dAttrs.size() );
		for ( int i=0; i<m_dIdx.size(); i++ )
		{
			const ColumnInfo_t & tCol = m_dAttrs[i];
			switch ( tCol.m_eType )
			{
				case AttrType_e::UINT32:
				case AttrType_e::TIMESTAMP:
				case AttrType_e::UINT32SET:
					m_dIdx[i].reset ( new PGM_T<uint32_t>() );
					break;

				case AttrType_e::FLOAT:
					m_dIdx[i].reset ( new PGM_T<float>() );
					break;

				case AttrType_e::STRING:
					m_dIdx[i].reset ( new PGM_T<uint64_t>() );
					break;

				case AttrType_e::INT64:
				case AttrType_e::INT64SET:
					m_dIdx[i].reset ( new PGM_T<int64_t>() );
					break;

				default:
					sError = FormatStr ( "Unknown attribute '%s'(%d) with type %d", tCol.m_sName.c_str(), i, tCol.m_eType );
					return false;
			}

			int64_t iPgmLen = tReader.Unpack_uint64();
			int64_t iPgmEnd = tReader.GetPos() + iPgmLen;
			m_dIdx[i]->Load ( tReader );
			if ( tReader.GetPos()!=iPgmEnd )
			{
				sError = FormatStr ( "Out of bounds on loading PGM for attribute '%s'(%d), end expected %ll got %ll", tCol.m_sName.c_str(), i, iPgmEnd, tReader.GetPos() );
				return false;
			}

			assert ( tCol.m_iAttr==i );
			m_hAttrs.insert ( { tCol.m_sName, tCol.m_iAttr } );
		}

		m_iBlocksBase = tReader.GetPos();

		if ( tReader.IsError() )
		{
			sError = tReader.GetError();
			return false;
		}

		m_fnHash = GetHashFn ( m_eCollation );

		return true;
	}

	ColumnInfo_t SITrait_c::GetColumn ( const char * sName ) const
	{
		auto tIt = m_hAttrs.find ( sName );
		if ( tIt==m_hAttrs.end() )
			return ColumnInfo_t();
		
		return m_dAttrs[tIt->second];
	}

	bool SITrait_c::SaveMeta ( std::string & sError )
	{
		if ( !m_bUpdated || !m_dAttrs.size() )
			return true;

		BitVec_t dAttrEnabled ( m_dAttrs.size() );
		for ( int i=0; i<m_dAttrs.size(); i++ )
		{
			const ColumnInfo_t & tAttr = m_dAttrs[i];
			if ( tAttr.m_bEnabled )
				dAttrEnabled.BitSet ( i );
		}

		FileWriter_c tDstFile;
		if ( !tDstFile.Open ( m_sFileName, false, false, false, sError ) )
			return false;

		// seek to meta offset and skip attrbutes count
		tDstFile.Seek ( m_uMetaOff + sizeof(uint64_t) + sizeof(uint32_t) );
		WriteVector ( dAttrEnabled.m_dData, tDstFile );
		return true;
	}
	
	void SITrait_c::ColumnUpdated ( const char * sName )
	{
		auto tIt = m_hAttrs.find ( sName );
		if ( tIt==m_hAttrs.end() )
			return;
		
		ColumnInfo_t & tCol = m_dAttrs[tIt->second];
		m_bUpdated |= tCol.m_bEnabled; // already disabled indexes should not cause flush
		tCol.m_bEnabled = false;
	}

	Collation_e SITrait_c::GetCollation() const
	{
		return m_eCollation;
	}

	uint64_t SITrait_c::GetHash ( const char * sVal ) const
	{
		int iLen = ( sVal ? strlen ( sVal ) : 0 );
		return m_fnHash ( (const uint8_t *)sVal, iLen );
	}

	typedef std::shared_ptr<IntCodec_i> SharedIntCodec_t;

	class FilterContext_c : public FilterContext_i
	{
	public:
		FilterContext_c ( const std::string & sCodec32, const std::string & sCodec64 )
		{
			m_pCodec.reset ( CreateIntCodec ( sCodec32, sCodec64 ) );
		}
		virtual ~FilterContext_c() {}

		SharedIntCodec_t m_pCodec { nullptr };
	};

	FilterContext_i * SITrait_c::CreateFilterContext () const
	{
		return new FilterContext_c ( m_sCompressionUINT32, m_sCompressionUINT64 );
	}

	static uint64_t GetValueBlock ( uint64_t uPos, int iValuesPerBlock )
	{
		return uPos / iValuesPerBlock;
	}

	struct BlockIter_t
	{
		uint64_t m_uVal { 0 };

		uint64_t m_iPos { 0 };
		uint64_t m_iStart { 0 };
		uint64_t m_iLast { 0 };
		
		BlockIter_t() = default;
		BlockIter_t ( const ApproxPos_t & tFrom, uint64_t uVal, uint64_t uBlocksCount, int iValuesPerBlock )
			: m_uVal ( uVal )
		{
			m_iStart = GetValueBlock ( tFrom.m_iLo, iValuesPerBlock );
			m_iPos = GetValueBlock ( tFrom.m_iPos, iValuesPerBlock ) - m_iStart;
			m_iLast = GetValueBlock ( tFrom.m_iHi, iValuesPerBlock );

			if ( m_iStart+m_iPos>=uBlocksCount )
				m_iPos = uBlocksCount - 1 - m_iStart;
			if ( m_iLast>=uBlocksCount )
				m_iLast = uBlocksCount - m_iStart;
		}
	};

	struct FindValueResult_t
	{
		int m_iMatchedItem { -1 };
		int m_iCmp { 0 };
	};

	class BlockReader_c
	{
	public:
		BlockReader_c ( SharedIntCodec_t & pCodec, uint64_t uBlockBaseOff, const RowidRange_t & tBounds )
			: m_pCodec ( pCodec )
			, m_uBlockBaseOff ( uBlockBaseOff )
			, m_tBounds ( tBounds )
		{
			assert ( m_pCodec.get() );
		}
		virtual ~BlockReader_c() = default;

		bool Open ( const std::string & sFileName, std::string & sError );
		void CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<RowidIterator_i *> & dRes );
		const std::string & GetWarning() const { return m_sWarning; }

	protected:
		std::shared_ptr<columnar::FileReader_c> m_pFileReader { nullptr };
		SharedIntCodec_t m_pCodec { nullptr };
		std::string m_sWarning;

		SpanResizeable_T<uint32_t> m_dTypes;
		SpanResizeable_T<uint32_t> m_dRowStart;

		SpanResizeable_T<uint32_t> m_dBufTmp;
		
		uint64_t m_uBlockBaseOff { 0 };
		std::vector<uint64_t> m_dBlockOffsets;
		int m_iLoadedBlock { -1 };
		int m_iStartBlock { -1 };
		int64_t m_iOffPastValues { -1 };

		const RowidRange_t m_tBounds;

		// interface for value related methods
		virtual void LoadValues () = 0;
		virtual FindValueResult_t FindValue ( uint64_t uRefVal ) const = 0;

		RowidIterator_i * CreateIterator ( int iItem );
		int BlockLoadCreateIterator ( int iBlock, uint64_t uVal, std::vector<RowidIterator_i *> & dRes );
	};

	template<typename VALUE, bool FLOAT_VALUE>
	class BlockReader_T : public BlockReader_c
	{
	public:
		BlockReader_T ( SharedIntCodec_t & pCodec, uint64_t uBlockBaseOff, const RowidRange_t & tBounds ) : BlockReader_c ( pCodec, uBlockBaseOff, tBounds ) {}

	private:
		SpanResizeable_T<VALUE> m_dValues;

		void LoadValues () override;
		FindValueResult_t FindValue ( uint64_t uRefVal ) const override;
	};

	static BlockReader_c * CreateBlockReader ( AttrType_e eType, SharedIntCodec_t & pCodec, uint64_t uBlockBaseOff, const RowidRange_t & tBounds )
	{
		switch ( eType )
		{
			case AttrType_e::UINT32:
			case AttrType_e::TIMESTAMP:
			case AttrType_e::UINT32SET:
				return new BlockReader_T<uint32_t, false> ( pCodec, uBlockBaseOff, tBounds );
				break;

			case AttrType_e::FLOAT:
				return new BlockReader_T<uint32_t, true> ( pCodec, uBlockBaseOff, tBounds );
				break;

			case AttrType_e::STRING:
			case AttrType_e::INT64:
			case AttrType_e::INT64SET:
				return new BlockReader_T<uint64_t, false> ( pCodec, uBlockBaseOff, tBounds );
				break;

			default: return nullptr;
		}
	}

	bool BlockReader_c::Open ( const std::string & sFileName, std::string & sError )
	{
		m_pFileReader.reset ( new columnar::FileReader_c() );
		return m_pFileReader->Open ( sFileName, g_iReaderBufSize, sError );
	}

	int BlockReader_c::BlockLoadCreateIterator ( int iBlock, uint64_t uVal, std::vector<RowidIterator_i *> & dRes )
	{
		if ( iBlock!=-1 )
		{
			m_pFileReader->Seek ( m_dBlockOffsets[iBlock] );
			LoadValues();
			m_iLoadedBlock = m_iStartBlock + iBlock;
		}

		auto [iItem, iCmp] = FindValue ( uVal );
		if ( iItem!=-1 )
			dRes.emplace_back ( CreateIterator ( iItem ) );

		return iCmp;
	}

	void BlockReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<RowidIterator_i *> & dRes )
	{
		m_iStartBlock = tIt.m_iStart;

		// load offsets of all blocks for the range
		m_dBlockOffsets.resize ( tIt.m_iLast - tIt.m_iStart + 1 );
		m_pFileReader->Seek ( m_uBlockBaseOff + tIt.m_iStart * sizeof ( uint64_t) );
		for ( int iBlock=0; iBlock<m_dBlockOffsets.size(); iBlock++ )
			m_dBlockOffsets[iBlock] = m_pFileReader->Read_uint64();

		// first check already loadded block in case it fits the range and it is not the best block that will be checked
		int iLastBlockChecked = -1;
		if ( m_iLoadedBlock!=m_iStartBlock+tIt.m_iPos && m_iStartBlock>=m_iLoadedBlock && m_iLoadedBlock<=tIt.m_iLast )
		{
			// if current block fits - exit even no matches
			if ( BlockLoadCreateIterator ( -1, tIt.m_uVal, dRes )==0 )
				return;

			iLastBlockChecked = m_iLoadedBlock;
		}

		// if best block fits - exit even no matches
		if ( BlockLoadCreateIterator ( tIt.m_iPos, tIt.m_uVal, dRes )==0 )
			return;

		for ( int iBlock=0; iBlock<=tIt.m_iLast - tIt.m_iStart; iBlock++ )
		{
			if ( iBlock==tIt.m_iPos || ( iLastBlockChecked!=-1 && m_iStartBlock+iBlock==iLastBlockChecked ) )
				continue;

			int iCmp = BlockLoadCreateIterator ( iBlock, tIt.m_uVal, dRes );

			// stop ckecking blocks in case
			// - found block where the value in values range
			// - checked block with the greater value
			if ( iCmp==0 || iCmp>0 )
				return;
		}
	}

	bool SITrait_c::GetValsRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<RowidIterator_i *> & dRes ) const
	{
		const ColumnInfo_t & tCol = tArgs.m_tCol;
		const Span_T<uint64_t> & dVals = tArgs.m_dVals;

		if ( tCol.m_eType==AttrType_e::NONE )
		{
			sError = FormatStr( "invalid attribute %s(%d) type %d", tCol.m_sName.c_str(), tCol.m_iSrcAttr, tCol.m_eType );
			return false;
		}

		if ( !pCtx )
		{
			sError = "empty filter context";
			return false;
		}

		// m_dBlockStartOff is 0based need to set to start of offsets vector
		uint64_t uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[tCol.m_iAttr];
		uint64_t uBlocksCount = m_dBlocksCount[tCol.m_iAttr];

		std::unique_ptr<BlockReader_c> pBlockReader { CreateBlockReader ( tCol.m_eType, ( (FilterContext_c *)pCtx )->m_pCodec, uBlockBaseOff, tArgs.m_tBounds ) } ;
		if ( !pBlockReader->Open ( m_sFileName, sError ) )
			return false;

		std::vector<BlockIter_t> dBlocksIt;
 		for ( const uint64_t uVal : dVals )
			dBlocksIt.emplace_back ( BlockIter_t ( m_dIdx[tCol.m_iAttr]->Search ( uVal ), uVal, uBlocksCount, m_iValuesPerBlock ) );

		// sort by block start offset
		std::sort ( dBlocksIt.begin(), dBlocksIt.end(), [] ( const BlockIter_t & tA, const BlockIter_t & tB ) { return tA.m_iStart<tB.m_iStart; } );

		for ( int i=0; i<dBlocksIt.size(); i++ )
			pBlockReader->CreateBlocksIterator ( dBlocksIt[i], dRes );

		sError = pBlockReader->GetWarning();

		return true;
	}

	struct FindRangeResult_t
	{
		int m_iMatchedItem { -1 };
		int m_iLeft { 0 };
		int m_iCmp { 0 };
	};

	class RangeReader_c
	{
	public:
		RangeReader_c ( SharedIntCodec_t & pCodec, uint64_t uBlockBaseOff, const RowidRange_t & tBounds )
			: m_pCodec ( pCodec )
			, m_uBlockBaseOff ( uBlockBaseOff )
			, m_tBounds ( tBounds )
		{
			assert ( m_pCodec.get() );
		}
		virtual ~RangeReader_c() = default;

		bool Open ( const std::string & sFileName, std::string & sError );
		void CreateBlocksIterator ( const BlockIter_t & tIt, const FilterRange_t & tVal, std::vector<RowidIterator_i *> & dRes );
		const std::string & GetWarning() const { return m_sWarning; }

	protected:
		std::shared_ptr<columnar::FileReader_c> m_pOffReader { nullptr };
		std::shared_ptr<columnar::FileReader_c> m_pBlockReader { nullptr };
		std::string m_sWarning;

		SharedIntCodec_t m_pCodec { nullptr };

		SpanResizeable_T<uint32_t> m_dTypes;
		SpanResizeable_T<uint32_t> m_dRowStart;

		SpanResizeable_T<uint32_t> m_dBufTmp;
		
		uint64_t m_uBlockBaseOff { 0 };
		const RowidRange_t m_tBounds;

		// interface for value related methods
		virtual int LoadValues () = 0;
		virtual bool EvalRangeValue ( int iItem, const FilterRange_t & tRange ) const = 0;
		virtual int CmpBlock ( const FilterRange_t & tRange ) const = 0;

		RowidIterator_i * CreateIterator ( int iItem, bool bLoad );
	};

	template<typename T>
	bool EvalRange ( T tVal, const FilterRange_t & tRange )
	{
		if ( tRange.m_bOpenLeft )
			return ( tRange.m_bHasEqualMax ? ( tVal<=tRange.m_iMax ) : ( tVal<tRange.m_iMax ) );

		if ( tRange.m_bOpenRight )
			return ( tRange.m_bHasEqualMin ? ( tVal>=tRange.m_iMin ) : ( tVal>tRange.m_iMin ) );

		bool bMinMatched = ( tRange.m_bHasEqualMin ? ( tVal>=tRange.m_iMin ) : ( tVal>tRange.m_iMin ) );
		bool bMaxMatched = ( tRange.m_bHasEqualMax ? ( tVal<=tRange.m_iMax ) : ( tVal<tRange.m_iMax ) );

		return ( bMinMatched && bMaxMatched );
	}


	template<>
	bool EvalRange<float> ( float fVal, const FilterRange_t & tRange )
	{
		if ( tRange.m_bOpenLeft )
			return ( tRange.m_bHasEqualMax ? ( fVal<=tRange.m_fMax ) : ( fVal<tRange.m_fMax ) );

		if ( tRange.m_bOpenRight )
			return ( tRange.m_bHasEqualMin ? ( fVal>=tRange.m_fMin ) : ( fVal>tRange.m_fMin ) );

		bool bMinMatched = ( tRange.m_bHasEqualMin ? ( fVal>=tRange.m_fMin ) : ( fVal>tRange.m_fMin ) );
		bool bMaxMatched = ( tRange.m_bHasEqualMax ? ( fVal<=tRange.m_fMax ) : ( fVal<tRange.m_fMax ) );

		return ( bMinMatched && bMaxMatched );
	}

	template<typename T>
	struct Interval_T
	{
        T m_tStart;
        T m_tEnd;

		Interval_T() = default;
		Interval_T ( T tStart, T tEnd )
			: m_tStart ( tStart )
			, m_tEnd ( tEnd )
		{}

		bool operator< ( const Interval_T & tOther ) const
		{
            return ( m_tStart<tOther.m_tStart || ( m_tStart==tOther.m_tStart && m_tEnd<tOther.m_tEnd ) );
        }

        bool Overlaps ( const Interval_T & tOther ) const
		{
            return ( m_tStart<=tOther.m_tEnd && tOther.m_tStart<=m_tEnd );
        }
	};

	template<>
    bool Interval_T<float>::operator< ( const Interval_T<float> & tOther ) const
	{
        return ( m_tStart<tOther.m_tStart || ( FloatEqual ( m_tStart, tOther.m_tStart ) && m_tEnd<tOther.m_tEnd ) );
    }

	template<typename T>
	int CmpRange ( T tStart, T tEnd, const FilterRange_t & tRange )
	{
		Interval_T<T> tIntBlock ( tStart, tEnd );

		Interval_T<T> tIntRange;
		if ( std::is_floating_point<T>::value )
			tIntRange = Interval_T<T> ( tRange.m_fMin, tRange.m_fMax );
		else
			tIntRange = Interval_T<T> ( tRange.m_iMin, tRange.m_iMax );

		if ( tRange.m_bOpenLeft )
			tIntRange.m_tStart = std::numeric_limits<T>::min();
		if ( tRange.m_bOpenRight )
			tIntRange.m_tEnd = std::numeric_limits<T>::max();

		if ( tIntBlock.Overlaps ( tIntRange ) )
			return 0;

		return ( tIntBlock<tIntRange ? -1 : 1 );
	}

	template<typename VEC>
	void DecodeBlock ( VEC & dDst, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, columnar::FileReader_c & tReader )
	{
		dBuf.resize ( 0 );
		ReadVectorLen32 ( dBuf, tReader );
		pCodec->Decode ( dBuf, dDst );
		ComputeInverseDeltas ( dDst, true );
	}

	template<typename VEC>
	void DecodeBlockWoDelta ( VEC & dDst, IntCodec_i * pCodec, SpanResizeable_T<uint32_t> & dBuf, columnar::FileReader_c & tReader )
	{
		dBuf.resize ( 0 );
		ReadVectorLen32 ( dBuf, tReader );
		pCodec->Decode ( dBuf, dDst );
	}

	template<typename STORE_VALUE, typename DST_VALUE>
	class RangeReader_T : public RangeReader_c
	{
	public:
		RangeReader_T ( SharedIntCodec_t & pCodec, uint64_t uBlockBaseOff, const RowidRange_t & tBounds ) : RangeReader_c ( pCodec, uBlockBaseOff, tBounds ) {}

	private:
		SpanResizeable_T<STORE_VALUE> m_dValues;

		int LoadValues () override
		{
			DecodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
			return m_dValues.size();
		}

		bool EvalRangeValue ( int iItem, const FilterRange_t & tRange ) const override
		{
			if ( std::is_floating_point<DST_VALUE>::value )
			{
				return EvalRange<float> ( UintToFloat ( m_dValues[iItem] ), tRange );
			} else
			{
				return EvalRange<DST_VALUE> ( (DST_VALUE)m_dValues[iItem], tRange );
			}
		}

		int CmpBlock ( const FilterRange_t & tRange ) const override
		{
			if ( std::is_floating_point<DST_VALUE>::value )
			{
				return CmpRange<float> ( UintToFloat ( m_dValues.front() ), UintToFloat ( m_dValues.back() ), tRange );
			} else
			{
				return CmpRange<DST_VALUE> ( m_dValues.front(), m_dValues.back(), tRange );
			}
		}
	};

	bool RangeReader_c::Open ( const std::string & sFileName, std::string & sError )
	{
		m_pOffReader.reset ( new columnar::FileReader_c() );
		m_pBlockReader.reset ( new columnar::FileReader_c() );
		return ( m_pOffReader->Open ( sFileName, g_iReaderBufSize, sError ) && m_pBlockReader->Open ( sFileName, g_iReaderBufSize, sError ) );
	}

	void RangeReader_c::CreateBlocksIterator ( const BlockIter_t & tIt, const FilterRange_t & tRange, std::vector<RowidIterator_i *> & dRes )
	{
		int iBlockCur = tIt.m_iStart;
		m_pOffReader->Seek ( m_uBlockBaseOff + iBlockCur * sizeof ( uint64_t) );

		int iValCur = 0;
		int iValCount = 0;
		int iBlockItCreated = -1;

		// warmup
		for ( ; iBlockCur<=tIt.m_iLast; iBlockCur++ )
		{
			uint64_t uBlockOff = m_pOffReader->Read_uint64();
			m_pBlockReader->Seek ( uBlockOff );
			
			iValCount = LoadValues();

			int iCmpLast = CmpBlock ( tRange );
			if ( iCmpLast==1 )
				break;
			if ( iCmpLast==-1 )
				continue;

			for ( iValCur=0; iValCur<iValCount; iValCur++ )
			{
				if ( EvalRangeValue ( iValCur, tRange ) )
				{
					dRes.emplace_back ( CreateIterator ( iValCur, true ) );
					iBlockItCreated = iBlockCur;
					iValCur++;
					break;
				}
			}

			// get into search in case current block has values matched
			if ( iBlockItCreated!=-1 )
				break;
		}

		if ( iBlockItCreated==-1 )
			return;

		// check end block value vs EvalRange then add all values from block as iterators
		// for openleft find start via linear scan
		// cases :
		// - whole block
		// - skip left part of values in block 
		// - stop on scan all values in block
		// FIXME!!! stop checking on range passed values 


		for ( ;; )
		{
			if ( iValCur<iValCount )
			{
				// case: all values till the end of the block matched
				if ( EvalRangeValue ( iValCount-1, tRange ) )
				{
					for ( ; iValCur<iValCount; iValCur++ )
					{
						dRes.emplace_back ( CreateIterator ( iValCur, iBlockItCreated!=iBlockCur ) );
						iBlockItCreated = iBlockCur;
					}
				} else // case: values only inside the block matched, need to check every value
				{
					for ( ; iValCur<iValCount; iValCur++ )
					{
						if ( !EvalRangeValue ( iValCur, tRange ) )
							return;

						dRes.emplace_back ( CreateIterator ( iValCur, iBlockItCreated!=iBlockCur ) );
						iBlockItCreated = iBlockCur;
					}

					break;
				}
			}

			iBlockCur++;
			if ( iBlockCur>tIt.m_iLast )
				break;

			uint64_t uBlockOff = m_pOffReader->Read_uint64();
			m_pBlockReader->Seek ( uBlockOff );
			
			iValCount = LoadValues();
			iValCur = 0;
			assert ( iValCount );
			
			// matching is over
			if ( !EvalRangeValue ( 0, tRange ) )
				break;
		}
	}

	static RangeReader_c * CreateRangeReader ( AttrType_e eType, SharedIntCodec_t & pCodec, uint64_t uBlockBaseOff, const RowidRange_t & tBounds )
	{
		switch ( eType )
		{
			case AttrType_e::UINT32:
			case AttrType_e::TIMESTAMP:
			case AttrType_e::UINT32SET:
				return new RangeReader_T<uint32_t, uint32_t> ( pCodec, uBlockBaseOff, tBounds );
				break;

			case AttrType_e::FLOAT:
				return new RangeReader_T<uint32_t, float> ( pCodec, uBlockBaseOff, tBounds );
				break;

			case AttrType_e::INT64:
			case AttrType_e::INT64SET:
				return new RangeReader_T<uint64_t, int64_t> ( pCodec, uBlockBaseOff, tBounds );
				break;

			default: return nullptr;
		}
	}

	bool SITrait_c::GetRangeRows ( const FilterArgs_t & tArgs, std::string & sError, FilterContext_i * pCtx, std::vector<RowidIterator_i *> & dRes ) const
	{
		const ColumnInfo_t & tCol = tArgs.m_tCol;
		const FilterRange_t & tVal = tArgs.m_tRange;

		if ( tCol.m_eType==AttrType_e::NONE )
		{
			sError = FormatStr( "invalid attribute %s(%d) type %d", tCol.m_sName.c_str(), tCol.m_iSrcAttr, tCol.m_eType );
			return false;
		}

		if ( !pCtx )
		{
			sError = "empty filter context";
			return false;
		}

		uint64_t uBlockBaseOff = m_iBlocksBase + m_dBlockStartOff[tCol.m_iAttr];
		uint64_t uBlocksCount = m_dBlocksCount[tCol.m_iAttr];

		const bool bFloat = ( tCol.m_eType==AttrType_e::FLOAT );

		ApproxPos_t tPos { 0, 0, ( uBlocksCount - 1 ) * m_iValuesPerBlock };
		if ( tVal.m_bOpenRight )
		{
			ApproxPos_t tFound =  ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMin ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMin ) );
			tPos.m_iPos = tFound.m_iPos;
			tPos.m_iLo = tFound.m_iLo;
		} else if ( tVal.m_bOpenLeft )
		{
			ApproxPos_t tFound = ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMax ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMax ) );
			tPos.m_iPos = tFound.m_iPos;
			tPos.m_iHi = tFound.m_iHi;
		} else
		{
			ApproxPos_t tFoundMin =  ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMin ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMin ) );
			ApproxPos_t tFoundMax =  ( bFloat ? m_dIdx[tCol.m_iAttr]->Search ( FloatToUint ( tVal.m_fMax ) ) : m_dIdx[tCol.m_iAttr]->Search ( tVal.m_iMax ) );
			tPos.m_iLo = std::min ( tFoundMin.m_iLo, tFoundMax.m_iLo );
			tPos.m_iPos = std::min ( tFoundMin.m_iPos, tFoundMax.m_iPos );
			tPos.m_iHi = std::max ( tFoundMin.m_iHi, tFoundMax.m_iHi );
		}

		BlockIter_t tPosIt ( tPos, 0, uBlocksCount, m_iValuesPerBlock );

		std::unique_ptr<RangeReader_c> pReader { CreateRangeReader ( tCol.m_eType, ( (FilterContext_c *)pCtx)->m_pCodec, uBlockBaseOff, tArgs.m_tBounds ) } ;
		if ( !pReader->Open ( m_sFileName, sError ) )
			return false;

		pReader->CreateBlocksIterator ( tPosIt, tVal, dRes );
		sError = pReader->GetWarning();

		return true;
	}

	class RowidIterator_c : public RowidIterator_i
	{
	public:
		RowidIterator_c ( Packing_e eType, uint64_t uRowStart, std::shared_ptr<columnar::FileReader_c> & pReader, SharedIntCodec_t & pCodec, const RowidRange_t & tBounds );
		~RowidIterator_c() override {}

		bool	HintRowID ( uint32_t tRowID ) override;
		bool	GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock ) override;
		int64_t	GetNumProcessed() const override;
		int GetPacking() const override { return (int)m_eType; }

	private:
		Packing_e m_eType = Packing_e::TOTAL;
		uint64_t m_uRowStart = 0;
		std::shared_ptr<columnar::FileReader_c> m_pReader { nullptr };
		SharedIntCodec_t m_pCodec { nullptr };
		uint64_t m_uLastOff { 0 };
		const RowidRange_t m_tBounds;

		bool m_bStarted = false;
		bool m_bStopped = false;

		uint32_t m_uRowMin = 0;
		uint32_t m_uRowMax = 0;
		uint32_t m_uCurBlock = 0;
		uint32_t m_uBlocksCount = 0;
		SpanResizeable_T<uint32_t> m_dRowsDecoded;
		std::vector<uint32_t> m_dRowsRaw;

		bool	StartBlock ( Span_T<uint32_t> & dRowIdBlock );
		bool	NextBlock ( Span_T<uint32_t> & dRowIdBlock );
		void	DecodeRowsBlock();
	};

	static RowidIterator_i * CreateRowidIterator ( Packing_e eType, uint64_t uRowStart, std::shared_ptr<columnar::FileReader_c> & pReader, SharedIntCodec_t & pCodec, const RowidRange_t & tBounds, bool bCreateReader, std::string & sError )
	{
		std::shared_ptr<columnar::FileReader_c> tBlocksReader { nullptr };
		if ( bCreateReader && eType==Packing_e::ROW_BLOCKS_LIST )
		{
			tBlocksReader.reset ( new columnar::FileReader_c() );
			if ( !tBlocksReader->Open ( pReader->GetFilename(), g_iBlocksReaderBufSize, sError ) )
			{
				tBlocksReader.reset();
			} else
			{
				tBlocksReader->Seek ( pReader->GetPos() );
			}
		}

		return new RowidIterator_c ( eType, uRowStart, ( tBlocksReader ? tBlocksReader : pReader ), pCodec, tBounds );
	}

	RowidIterator_i * BlockReader_c::CreateIterator ( int iItem )
	{
		if ( m_iOffPastValues!=-1 )
		{
			// seek right after values to load the rest of the block content as only values could be loaded
			m_pFileReader->Seek ( m_iOffPastValues );
			m_iOffPastValues = -1;

			DecodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );

			bool bLenDelta = !!m_pFileReader->Read_uint8();
			if ( bLenDelta )
			{
				DecodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
			} else
			{
				DecodeBlockWoDelta ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
			}
		}

		return CreateRowidIterator ( (Packing_e)m_dTypes[iItem], m_dRowStart[iItem], m_pFileReader, m_pCodec, m_tBounds, m_sWarning.empty(), m_sWarning );
	}

	RowidIterator_i * RangeReader_c::CreateIterator ( int iItem, bool bLoad )
	{
		if ( bLoad )
		{
			DecodeBlockWoDelta ( m_dTypes, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );

			bool bLenDelta = !!m_pBlockReader->Read_uint8();
			if ( bLenDelta )
			{
				DecodeBlock ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
			} else
			{
				DecodeBlockWoDelta ( m_dRowStart, m_pCodec.get(), m_dBufTmp, *m_pBlockReader.get() );
			}
		}

		return CreateRowidIterator ( (Packing_e)m_dTypes[iItem], m_dRowStart[iItem], m_pBlockReader, m_pCodec, m_tBounds, m_sWarning.empty(), m_sWarning );
	}

	template<typename VALUE, bool FLOAT_VALUE>
	void BlockReader_T<VALUE, FLOAT_VALUE>::LoadValues ()
	{
		DecodeBlock ( m_dValues, m_pCodec.get(), m_dBufTmp, *m_pFileReader.get() );
		m_iOffPastValues = m_pFileReader->GetPos();
	}

	template<>
	FindValueResult_t BlockReader_T<uint32_t, false>::FindValue ( uint64_t uRefVal ) const
	{
		uint32_t uVal = uRefVal;
		int iItem = binary_search_idx ( m_dValues, uVal );
		if ( iItem!=-1 )
			return FindValueResult_t { iItem, 0 };

		if ( !m_dValues.size() || ( m_dValues.size() && m_dValues.front()<=uVal && uVal<=m_dValues.back() ) )
			return FindValueResult_t { -1, 0 };

		return FindValueResult_t { -1, m_dValues.back()<uVal ? 1 : -1 };
	}

	struct Int64ValueCmp_t
	{
	   bool operator()( const uint64_t & uVal1, const uint64_t & uVal2 ) const
	   {
		   return ( (int64_t)uVal1<(int64_t)uVal2 );
	   }
	};

	template<>
	FindValueResult_t BlockReader_T<uint64_t, false>::FindValue ( uint64_t uRefVal ) const
	{
		const auto tFirst = m_dValues.begin();
		const auto tLast = m_dValues.end();
		auto tFound = std::lower_bound ( tFirst, tLast, uRefVal, Int64ValueCmp_t() );
		if ( tFound!=tLast && *tFound==uRefVal )
			return FindValueResult_t { (int)( tFound-tFirst ), 0 };

		if ( !m_dValues.size() || ( m_dValues.size() && (int64_t)m_dValues.front()<=(int64_t)uRefVal && (int64_t)uRefVal<=(int64_t)m_dValues.back() ) )
			return FindValueResult_t { -1, 0 };

		return FindValueResult_t { -1, (int64_t)m_dValues.back()<(int64_t)uRefVal ? 1 : -1 };
	}

	struct FloatValueCmp_t
	{
	   float AsFloat ( const uint32_t & tVal ) const { return UintToFloat ( tVal ); }
	   float AsFloat ( const float & fVal ) const { return fVal; }

	   template< typename T1, typename T2 >
	   bool operator()( const T1 & tVal1, const T2 & tVal2 ) const
	   {
		   return IsLess ( tVal1, tVal2 );
	   }

	   template< typename T1, typename T2 >
	   bool IsLess ( const T1 & tVal1, const T2 & tVal2 ) const
	   {
		   return AsFloat( tVal1 ) < AsFloat( tVal2 );
	   }
	};

	template<>
	FindValueResult_t BlockReader_T<uint32_t, true>::FindValue ( uint64_t uRefVal ) const
	{
		float fVal = UintToFloat ( uRefVal );
		const auto tFirst = m_dValues.begin();
		const auto tLast = m_dValues.end();
		auto tFound = std::lower_bound ( tFirst, tLast, fVal, FloatValueCmp_t() );

		if ( tFound!=tLast && FloatEqual ( *tFound, fVal ) )
			return FindValueResult_t { (int)( tFound-tFirst ), 0 };

		if ( !m_dValues.size() || ( m_dValues.size() && m_dValues.front()<=fVal && fVal<=m_dValues.back() ) )
			return FindValueResult_t { -1, 0 };

		return FindValueResult_t { -1, m_dValues.back()<fVal ? 1 : -1 };
	}

	RowidIterator_c::RowidIterator_c ( Packing_e eType, uint64_t uRowStart, std::shared_ptr<columnar::FileReader_c> & pReader, SharedIntCodec_t & pCodec, const RowidRange_t & tBounds )
		: m_eType ( eType )
		, m_uRowStart ( uRowStart )
		, m_pReader ( pReader )
		, m_pCodec ( pCodec )
		, m_tBounds ( tBounds )
	{
		m_uLastOff = m_pReader->GetPos();
	}

	bool RowidIterator_c::HintRowID ( uint32_t tRowID )
	{
		return !m_bStopped;
	}

	bool RowidIterator_c::GetNextRowIdBlock ( Span_T<uint32_t> & dRowIdBlock )
	{
		if ( m_bStopped )
			return false;
		if ( !m_bStarted )
			return StartBlock ( dRowIdBlock );

		return NextBlock ( dRowIdBlock );
	}

	int64_t	RowidIterator_c::GetNumProcessed() const
	{
		return 0;
	}

	bool RowidIterator_c::StartBlock ( Span_T<uint32_t> & dRowIdBlock )
	{
		m_bStarted = true;
		switch ( m_eType )
		{
		case Packing_e::ROW:
			m_bStopped = true;
			m_uRowMin = m_uRowMax = m_uRowStart;
			if ( !m_tBounds.m_bHasRange || ( m_tBounds.m_uMin<=m_uRowStart && m_uRowStart<=m_tBounds.m_uMax ) )
			{
				m_dRowsDecoded.resize ( 1 );
				m_dRowsDecoded[0] = m_uRowStart;
			}
			break;

		case Packing_e::ROW_BLOCK:
			m_pReader->Seek ( m_uLastOff + m_uRowStart );
			// FIXME!!! block length larger dRowIdBlock
			m_bStopped = true;
			DecodeRowsBlock();
			break;

		case Packing_e::ROW_BLOCKS_LIST:
			m_pReader->Seek ( m_uLastOff + m_uRowStart );
			// FIXME!!! block length larger dRowIdBlock
			m_uCurBlock = 1;
			m_uBlocksCount = m_pReader->Unpack_uint32();
			DecodeRowsBlock();
			dRowIdBlock = Span_T<uint32_t> ( m_dRowsDecoded );
			// decode could produce empty block in case it out of @rowid range
			while ( !m_bStopped && dRowIdBlock.empty() )
				NextBlock ( dRowIdBlock );
			break;

		default:				// FIXME!!! handle ROW_FULLSCAN
			m_bStopped = true;
			break;
		}

		dRowIdBlock = Span_T<uint32_t> ( m_dRowsDecoded );
		return ( !dRowIdBlock.empty() );
	}

	bool RowidIterator_c::NextBlock ( Span_T<uint32_t> & dRowIdBlock )
	{
		assert ( m_bStarted && !m_bStopped );
		assert ( m_eType==Packing_e::ROW_BLOCKS_LIST );

		if ( m_uCurBlock>=m_uBlocksCount )
		{
			m_bStopped = true;
			return false;
		}

		m_uCurBlock++;
		// reader is shared among multiple blocks - need to keep offset after last operation
		m_pReader->Seek ( m_uLastOff );
		DecodeRowsBlock();
		dRowIdBlock = Span_T<uint32_t> ( m_dRowsDecoded );

		return ( !dRowIdBlock.empty() );
	}

	void RowidIterator_c::DecodeRowsBlock()
	{
		m_dRowsRaw.resize ( 0 );
		m_dRowsDecoded.resize ( 0 );

		m_uRowMin = m_pReader->Unpack_uint32();
		m_uRowMax = m_pReader->Unpack_uint32() + m_uRowMin;

		// should rows block be skipped or unpacked
		Interval_T<uint32_t> tBlockRange ( m_uRowMin, m_uRowMax );
		Interval_T<uint32_t> tRowidBounds ( m_tBounds.m_uMin, m_tBounds.m_uMax );
		if ( m_tBounds.m_bHasRange && !tRowidBounds.Overlaps ( tBlockRange ) )
		{
			uint32_t uLen =  m_pReader->Unpack_uint32();
			m_uLastOff = m_pReader->GetPos() + sizeof ( m_dRowsRaw[0] ) * uLen;
			return;
		}

		ReadVectorLen32 ( m_dRowsRaw, *m_pReader );
		
		m_pCodec->Decode ( m_dRowsRaw, m_dRowsDecoded );
		ComputeInverseDeltas ( m_dRowsDecoded, true );

		m_uLastOff = m_pReader->GetPos();
	}
}

SI::Index_i * CreateSecondaryIndex ( const char * sFile, std::string & sError )
{
	std::unique_ptr<SI::SITrait_c> pIdx ( new SI::SITrait_c );

	if ( !pIdx->Setup ( sFile, sError ) )
		return nullptr;

	return pIdx.release();
}

