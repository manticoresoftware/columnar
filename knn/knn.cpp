// Copyright (c) 2023-2025, Manticore Software LTD (https://manticoresearch.com)
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

#include "knn.h"
#include "iterator.h"
#include "embeddings.h"
#include "quantizer.h"
#include "termination.h"
#include "space.h"
#include "util/reader.h"
#include "util_private.h"

#include <unordered_map>
#include <algorithm>

namespace knn
{

using namespace util;

// not member functions because there's no need to expose them in knn.h
static void LoadSettings ( IndexSettings_t & tSettings, FileReader_c & tReader, uint32_t uVersion )
{
	tSettings.m_iDims				= tReader.Read_uint32();
	tSettings.m_eHNSWSimilarity		= (HNSWSimilarity_e)tReader.Read_uint32();
	if ( uVersion>=2 )
		tSettings.m_eQuantization	= (Quantization_e)tReader.Read_uint32();

	tSettings.m_iHNSWM				= tReader.Read_uint32();
	tSettings.m_iHNSWEFConstruction	= tReader.Read_uint32();

	if ( uVersion>=4 )
		tSettings.m_bMulti			= !!tReader.Read_uint32();
}


static void LoadQuantizationSettings ( QuantizationSettings_t & tSettings, FileReader_c & tReader, uint32_t uVersion )
{
	if ( uVersion<2 )
		return;

	tSettings.m_fMin = UintToFloat ( tReader.Read_uint32() );
	tSettings.m_fMax = UintToFloat ( tReader.Read_uint32() );
	tSettings.m_fK = UintToFloat ( tReader.Read_uint32() );
	tSettings.m_fB = UintToFloat ( tReader.Read_uint32() );

	if ( uVersion>=3 )
	{
		tSettings.m_dCentroid.resize ( tReader.Read_uint32() );
		for ( auto & i : tSettings.m_dCentroid )
			i = UintToFloat ( tReader.Read_uint32() );
	}
}


static void SaveSettings ( const IndexSettings_t & tSettings, FileWriter_c & tWriter )
{
	tWriter.Write_uint32 ( tSettings.m_iDims );
	tWriter.Write_uint32 ( (int)tSettings.m_eHNSWSimilarity );
	tWriter.Write_uint32 ( (int)tSettings.m_eQuantization );
	tWriter.Write_uint32 ( tSettings.m_iHNSWM );
	tWriter.Write_uint32 ( tSettings.m_iHNSWEFConstruction );
	tWriter.Write_uint32 ( tSettings.m_bMulti ? 1 : 0 );
}


static void SaveQuantizationSettings ( const QuantizationSettings_t & tSettings, FileWriter_c & tWriter )
{
	tWriter.Write_uint32 ( FloatToUint ( tSettings.m_fMin ) );
	tWriter.Write_uint32 ( FloatToUint ( tSettings.m_fMax ) );
	tWriter.Write_uint32 ( FloatToUint ( tSettings.m_fK ) );
	tWriter.Write_uint32 ( FloatToUint ( tSettings.m_fB ) );

	tWriter.Write_uint32 ( tSettings.m_dCentroid.size() );
	for ( auto & i : tSettings.m_dCentroid )
		tWriter.Write_uint32 ( FloatToUint(i) );
}

/////////////////////////////////////////////////////////////////////

class HNSWFilterWrapper_c : public hnswlib::BaseFilterFunctor
{
public:
				HNSWFilterWrapper_c ( KNNFilter_i * pFilter, const uint32_t * pVidToRowid = nullptr ) : m_pFilter ( pFilter ), m_pVidToRowid ( pVidToRowid ) {}
	virtual		~HNSWFilterWrapper_c() = default;

	bool		operator() ( hnswlib::labeltype id ) override	{ return m_pFilter->IsAllowed ( m_pVidToRowid ? m_pVidToRowid[id] : (uint32_t)id ); }
	long long	getFilterCount() const override;
	void		SetVectorsPerDoc ( float fVectorsPerDoc )		{ m_fVectorsPerDoc = fVectorsPerDoc; }

private:
	KNNFilter_i *		m_pFilter = nullptr;
	const uint32_t *	m_pVidToRowid = nullptr;
	float				m_fVectorsPerDoc = 1.0f;
};


long long HNSWFilterWrapper_c::getFilterCount() const
{
	int64_t iCount = m_pFilter->GetFilterCount();
	if ( iCount<0 )
		return -1;						// unknown cardinality, scaling is meaningless

	// hnswlib weighs this against vectors; the daemon's estimate counts documents, hence the scale
	return (long long)( iCount*m_fVectorsPerDoc );
}

/////////////////////////////////////////////////////////////////////

class HNSWDist_c
{
public:
			HNSWDist_c ( int iDim, HNSWSimilarity_e eSimilarity, Quantization_e eQuantization, bool bBuild );

protected:
	int							m_iDim = 0;
	HNSWSimilarity_e			m_eSimilarity = HNSWSimilarity_e::L2;
	Quantization_e				m_eQuantization = Quantization_e::NONE;
	std::unique_ptr<Space_i>	m_pSpace;

private:
	Space_i * CreateSpaceInterface ( bool bBuild ) const;
};

HNSWDist_c::HNSWDist_c ( int iDim, HNSWSimilarity_e eSimilarity, Quantization_e eQuantization, bool bBuild )
	: m_iDim ( iDim )
	, m_eSimilarity ( eSimilarity )
	, m_eQuantization ( eQuantization )
	, m_pSpace ( CreateSpaceInterface(bBuild) )
{}


Space_i * HNSWDist_c::CreateSpaceInterface ( bool bBuild ) const
{
	switch ( m_eSimilarity )
	{
	case HNSWSimilarity_e::IP:
	case HNSWSimilarity_e::COSINE:
		switch ( m_eQuantization )
		{
		case Quantization_e::BIT1:	return new IPSpaceBinaryFloat_c ( m_iDim, bBuild );
		case Quantization_e::BIT1SIMPLE: return new IPSpace1BitFloat_c(m_iDim);
		case Quantization_e::BIT8:	return new IPSpace8BitFloat_c(m_iDim);
		default:					return new IPSpace32BitFloat_c(m_iDim);
		}

	case HNSWSimilarity_e::L2:
		switch ( m_eQuantization )
		{
		case Quantization_e::BIT1:	return new L2SpaceBinaryFloat_c ( m_iDim, bBuild );
		case Quantization_e::BIT1SIMPLE: return new L2Space1BitFloat_c(m_iDim);
		case Quantization_e::BIT8:	return new L2Space8BitFloat_c(m_iDim);
		default:					return new L2Space32BitFloat_c(m_iDim);
		}

	default:
		assert ( 0 && "Unknown similarity" );
		return nullptr;
	}
}

/////////////////////////////////////////////////////////////////////

static FORCE_INLINE void PrefetchVec ( const void * pVec )
{
	assert(pVec);
	_mm_prefetch ( (const char *)pVec, _MM_HINT_T0 );
}


class Distance_c : public Distance_i, public HNSWDist_c
{
public:
			Distance_c ( const knn::IndexSettings_t & tSettings );

	float		CalcDist ( const util::Span_T<float> & dPoint1, const util::Span_T<float> & dPoint2 ) const override;
	DistFunc_fn	GetDistFunc() const override		{ return m_fnDistFunc; }
	void *		GetDistFuncParam() const override	{ return m_pDistFuncParam; }
	void		CalcDistBatch ( const void * pAnchor, const util::Span_T<const void *> & dVectors, const util::Span_T<float> & dDistances ) const override;

private:
	hnswlib::DISTFUNC<float> m_fnDistFunc;
	void *				m_pDistFuncParam = nullptr;
	DistFuncId_e		m_eDistFuncId = DistFuncId_e::NONE;
};


Distance_c::Distance_c ( const knn::IndexSettings_t & tSettings )
	: HNSWDist_c ( tSettings.m_iDims, tSettings.m_eHNSWSimilarity, tSettings.m_eQuantization, false )
{
	m_fnDistFunc = m_pSpace->get_dist_func();
	m_pDistFuncParam = m_pSpace->get_dist_func_param();
	m_eDistFuncId = m_pSpace->GetDistFuncId();
}


float Distance_c::CalcDist ( const util::Span_T<float> & dPoint1, const util::Span_T<float> & dPoint2 ) const
{
	assert ( dPoint1.size()==m_iDim && dPoint2.size()==m_iDim );
	assert ( m_fnDistFunc );
	return m_fnDistFunc ( dPoint1.data(), dPoint2.data(), (size_t)-1, (size_t)-1, m_pDistFuncParam );
}

/////////////////////////////////////////////////////////////////////

class HNSWIndex_i : public KNNIndex_i
{
public:
	virtual bool	Load ( FileReader_c & tReader, std::string & sError ) = 0;
	virtual const std::string &	GetName() const = 0;
};


class HNSWIndex_c : public HNSWDist_c, public HNSWIndex_i
{
public:
			HNSWIndex_c ( const std::string & sName, int64_t iNumElements, const knn::IndexSettings_t & tSettings, const QuantizationSettings_t & tQuantSettings, ScalarQuantizer_i * pQuantizer );

	bool	Load ( FileReader_c & tReader, std::string & sError ) override;
	const std::string &	GetName() const override	{ return m_sName; }
	void	Search ( std::vector<DocDist_t> & dResults, const Span_T<float> & dData, int64_t iResults, int iEf, std::vector<uint8_t> & dQuantized, int64_t * pDistanceComputations = nullptr, KNNFilter_i * pFilter = nullptr, HNSWTerminationPolicy_e ePolicy = HNSWTerminationPolicy_e::NONE ) const override;
	bool	ShouldUseFullscan ( int64_t iResults, int iEf, int64_t iFilterCount ) const override;

private:
	std::string											m_sName;
	std::unique_ptr<hnswlib::HierarchicalNSW<float>>	m_pAlg;
	std::unique_ptr<ScalarQuantizer_i>					m_pQuantizer;
	DistFuncId_e										m_eDistFuncId = DistFuncId_e::NONE;
	bool												m_bMulti = false;
	std::vector<uint32_t>								m_dVidToRowid;				// [vector id] -> rowid. Empty in scalar mode
	float												m_fVectorsPerDoc = 1.0f;	// average vectors per document, used to convert the daemon's document-space filter estimates into the vector space for the hnsw
};


HNSWIndex_c::HNSWIndex_c ( const std::string & sName, int64_t iNumElements, const knn::IndexSettings_t & tSettings, const QuantizationSettings_t & tQuantSettings, ScalarQuantizer_i * pQuantizer )
	: HNSWDist_c ( tSettings.m_iDims, tSettings.m_eHNSWSimilarity,  tSettings.m_eQuantization, false )
	, m_sName ( sName )
	, m_pQuantizer ( pQuantizer )
	, m_bMulti ( tSettings.m_bMulti )
{
	m_pSpace->SetQuantizationSettings(*pQuantizer);
	m_eDistFuncId = m_pSpace->GetDistFuncId();
	m_pAlg = std::make_unique<hnswlib::HierarchicalNSW<float>>( m_pSpace.get(), iNumElements, tSettings.m_iHNSWM, tSettings.m_iHNSWEFConstruction );
}


bool HNSWIndex_c::Load ( FileReader_c & tReader, std::string & sError )
{
	if ( m_bMulti )
	{
		const uint64_t uNumVectors = tReader.Read_uint64();
		m_dVidToRowid.resize ( (size_t)uNumVectors );
		for ( auto & i : m_dVidToRowid )
			i = tReader.Read_uint32();
	}

	if ( !m_pAlg->loadIndex ( tReader, m_pSpace.get(), sError ) )
		return false;

	assert ( !m_bMulti || m_dVidToRowid.size()==m_pAlg->cur_element_count );

	if ( !m_dVidToRowid.empty() )
	{
		// graph needs the map because it needs to collect one result per doc (group)
		m_pAlg->setGroupMap ( m_dVidToRowid.data() );

		const int64_t iDocs = (int64_t)m_dVidToRowid.back() + 1;
		m_fVectorsPerDoc = float ( (double)m_dVidToRowid.size() / (double)iDocs );
	}

	return true;
}


bool HNSWIndex_c::ShouldUseFullscan ( int64_t iResults, int iEf, int64_t iFilterCount ) const
{
	// iFilterCount is a doc estimate; need to conver it to vectors first
	if ( iFilterCount>=0 )
		iFilterCount = (int64_t)( iFilterCount*m_fVectorsPerDoc );

	return m_pAlg->shouldBypassHnswForFilteredSearch ( iResults, (long long)iFilterCount, iEf );
}


static void ExtractResults ( std::vector<std::pair<float, hnswlib::labeltype>> && dRaw, std::vector<DocDist_t> & dResults )
{
	dResults.resize(0);
	dResults.reserve ( dRaw.size() );
	for ( auto & tRes : dRaw )
		dResults.push_back ( { (uint32_t)tRes.second, tRes.first } );
}

template<float (*DIST_FN)(const void *, const void *, size_t, size_t, const void *)>
class DistFnDispatch_c
{
public:
	static FORCE_INLINE float Eval ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam )
	{
		return DIST_FN ( pVect1, pVect2, uRowID1, uRowID2, pParam );
	}

	static FORCE_INLINE void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		fDistA = DIST_FN ( pVect1, pVect2A, uRowID1, uRowID2A, pParam );
		fDistB = DIST_FN ( pVect1, pVect2B, uRowID1, uRowID2B, pParam );
	}
};

using IPBinaryGenericDistFn_c = DistFnDispatch_c<&IPBinaryFloatDistanceGeneric>;
class IPFloatDistFn_c : public DistFnDispatch_c<&IPFloatDistance>
{
public:
	static FORCE_INLINE void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		IPFloatDistanceBatch2 ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};

#if !defined(USE_SIMDE)
class IPBinarySIMD16DistFn_c : public DistFnDispatch_c<&IPBinaryFloatDistanceSIMD16>
{
public:
	static FORCE_INLINE void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		IPBinaryFloatDistanceSIMD16Batch2 ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};

class IPBinarySIMD16ResidualsDistFn_c : public DistFnDispatch_c<&IPBinaryFloatDistanceSIMD16Residuals>
{
public:
	static FORCE_INLINE void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		IPBinaryFloatDistanceSIMD16ResidualsBatch2 ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};
#endif

using L2BinaryGenericDistFn_c = DistFnDispatch_c<&L2BinaryFloatDistanceGeneric>;
class L2FloatDistFn_c : public DistFnDispatch_c<&L2FloatDistance>
{
public:
	static FORCE_INLINE void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		L2FloatDistanceBatch2 ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};

#if !defined(USE_SIMDE)
class L2BinarySIMD16DistFn_c : public DistFnDispatch_c<&L2BinaryFloatDistanceSIMD16>
{
public:
	static FORCE_INLINE void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		L2BinaryFloatDistanceSIMD16Batch2 ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};

class L2BinarySIMD16ResidualsDistFn_c : public DistFnDispatch_c<&L2BinaryFloatDistanceSIMD16Residuals>
{
public:
	static FORCE_INLINE void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		L2BinaryFloatDistanceSIMD16ResidualsBatch2 ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};
#endif

// build-mode DistFn classes
using IPBinaryGenericBuildDistFn_c = DistFnDispatch_c<&IPBinaryFloatDistanceGenericBuild>;
using L2BinaryGenericBuildDistFn_c = DistFnDispatch_c<&L2BinaryFloatDistanceGenericBuild>;

#if !defined(USE_SIMDE)
class IPBinarySIMD16BuildDistFn_c : public DistFnDispatch_c<&IPBinaryFloatDistanceSIMD16Build>
{
public:
	static void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		IPBinaryFloatDistanceSIMD16Batch2Build ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};

class IPBinarySIMD16ResidualsBuildDistFn_c : public DistFnDispatch_c<&IPBinaryFloatDistanceSIMD16ResidualsBuild>
{
public:
	static void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		IPBinaryFloatDistanceSIMD16ResidualsBatch2Build ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};

class L2BinarySIMD16BuildDistFn_c : public DistFnDispatch_c<&L2BinaryFloatDistanceSIMD16Build>
{
public:
	static void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		L2BinaryFloatDistanceSIMD16Batch2Build ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};

class L2BinarySIMD16ResidualsBuildDistFn_c : public DistFnDispatch_c<&L2BinaryFloatDistanceSIMD16ResidualsBuild>
{
public:
	static void Eval2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB )
	{
		L2BinaryFloatDistanceSIMD16ResidualsBatch2Build ( pVect1, pVect2A, pVect2B, uRowID1, uRowID2A, uRowID2B, pParam, fDistA, fDistB );
	}
};
#endif

template <typename DISTFN>
static void CalcDistBatchT ( const void * pAnchor, const util::Span_T<const void *> & dVectors, const util::Span_T<float> & dDistances, const void * pParam )
{
	const size_t uCount = dVectors.size();
	const void * const * pVecs = dVectors.data();
	float * pOut = dDistances.data();
	const size_t uUnusedRow = (size_t)-1;

	size_t i = 0;
	const size_t uPairLimit = uCount & ~size_t(1);
	for ( ; i < uPairLimit; i += 2 )
	{
		if ( i+2 < uCount )	PrefetchVec ( pVecs[i+2] );
		if ( i+3 < uCount )	PrefetchVec ( pVecs[i+3] );
		DISTFN::Eval2 ( pAnchor, pVecs[i], pVecs[i+1], uUnusedRow, uUnusedRow, uUnusedRow, pParam, pOut[i], pOut[i+1] );
	}

	for ( ; i < uCount; i++ )	// odd tail
		pOut[i] = DISTFN::Eval ( pAnchor, pVecs[i], uUnusedRow, uUnusedRow, pParam );
}


void Distance_c::CalcDistBatch ( const void * pAnchor, const util::Span_T<const void *> & dVectors, const util::Span_T<float> & dDistances ) const
{
	assert ( dVectors.size()==dDistances.size() );

	switch ( m_eDistFuncId )
	{
	case DistFuncId_e::IP_FLOAT32:			CalcDistBatchT<IPFloatDistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );			break;
	case DistFuncId_e::IP_BINARY_GENERIC:	CalcDistBatchT<IPBinaryGenericDistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );	break;
	case DistFuncId_e::L2_FLOAT32:			CalcDistBatchT<L2FloatDistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );			break;
	case DistFuncId_e::L2_BINARY_GENERIC:	CalcDistBatchT<L2BinaryGenericDistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );	break;

#if !defined(USE_SIMDE)
	case DistFuncId_e::IP_BINARY_SIMD16:			CalcDistBatchT<IPBinarySIMD16DistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );			break;
	case DistFuncId_e::IP_BINARY_SIMD16_RESIDUALS:	CalcDistBatchT<IPBinarySIMD16ResidualsDistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );	break;
	case DistFuncId_e::L2_BINARY_SIMD16:			CalcDistBatchT<L2BinarySIMD16DistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );			break;
	case DistFuncId_e::L2_BINARY_SIMD16_RESIDUALS:	CalcDistBatchT<L2BinarySIMD16ResidualsDistFn_c> ( pAnchor, dVectors, dDistances, m_pDistFuncParam );	break;
#endif

	default:
		assert ( m_fnDistFunc );
		{
			const void * const * pVecs = dVectors.data();
			float * pOut = dDistances.data();
			for ( size_t i = 0; i < dVectors.size(); i++ )
				pOut[i] = m_fnDistFunc ( pAnchor, pVecs[i], (size_t)-1, (size_t)-1, m_pDistFuncParam );
		}
		break;
	}
}


template <typename DistFn = void>
static void RunSearchPath ( const hnswlib::HierarchicalNSW<float> & tAlg, std::vector<DocDist_t> & dResults, const void * pData, int64_t iResults, HNSWFilterWrapper_c * pFilter, size_t * pSearchEf, int iSearchPath )
{
	switch ( iSearchPath )
	{
	case 0:
	case 1:
		ExtractResults ( tAlg.template searchKnn<hnswlib::NoopTerminationState, false, DistFn> ( pData, iResults, pFilter, pSearchEf ), dResults );
		break;

	case 2:
		ExtractResults ( tAlg.template searchKnn<TerminationQuantile_c, false, DistFn> ( pData, iResults, pFilter, pSearchEf ), dResults );
		break;

	case 3:
		ExtractResults ( tAlg.template searchKnn<TerminationQuantileL2_c, false, DistFn> ( pData, iResults, pFilter, pSearchEf ), dResults );
		break;

	case 4:
	case 5:
		ExtractResults ( tAlg.template searchKnn<hnswlib::NoopTerminationState, true, DistFn> ( pData, iResults, pFilter, pSearchEf ), dResults );
		break;

	case 6:
		ExtractResults ( tAlg.template searchKnn<TerminationQuantile_c, true, DistFn> ( pData, iResults, pFilter, pSearchEf ), dResults );
		break;

	case 7:
		ExtractResults ( tAlg.template searchKnn<TerminationQuantileL2_c, true, DistFn> ( pData, iResults, pFilter, pSearchEf ), dResults );
		break;

	default:
		assert ( 0 );
		break;
	}
}


void HNSWIndex_c::Search ( std::vector<DocDist_t> & dResults, const Span_T<float> & dData, int64_t iResults, int iEf, std::vector<uint8_t> & dQuantized, int64_t * pDistanceComputations, KNNFilter_i * pFilter, HNSWTerminationPolicy_e ePolicy ) const
{
	if ( pDistanceComputations )
		*pDistanceComputations = 0;

	if ( !m_pAlg->cur_element_count )
		return;

	const void * pData = dData.begin();
	if ( m_pQuantizer )
	{
		std::vector<uint8_t> dUnusedQuantizedForQuery;
		m_pQuantizer->Encode ( 0, dData, dQuantized, dUnusedQuantizedForQuery );
		pData = dQuantized.data();
	}

	std::unique_ptr<HNSWFilterWrapper_c> pFilterWrapper;
	if ( pFilter )
	{
		pFilterWrapper = std::make_unique<HNSWFilterWrapper_c> ( pFilter, m_dVidToRowid.empty() ? nullptr : m_dVidToRowid.data() );
		pFilterWrapper->SetVectorsPerDoc ( m_fVectorsPerDoc );
	}

	size_t iSearchEf = iEf;
	long iBeforeDistanceComputations = 0;
	if ( pDistanceComputations )
		iBeforeDistanceComputations = m_pAlg->metric_distance_computations.load();

	bool bCollectMetrics = !!pDistanceComputations;
	bool bQuantile = ePolicy==HNSWTerminationPolicy_e::QUANTILE && iResults > 10;	// disable early termination for k<=10
	bool bL2 = m_eSimilarity==HNSWSimilarity_e::L2;
	int iSearchPath = bCollectMetrics*4 + bQuantile*2 + bL2;

	switch ( m_eDistFuncId )
	{
	case DistFuncId_e::NONE:
		RunSearchPath<> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;

	case DistFuncId_e::IP_FLOAT32:
		RunSearchPath<IPFloatDistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;

	case DistFuncId_e::IP_BINARY_GENERIC:
		RunSearchPath<IPBinaryGenericDistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;

#if !defined(USE_SIMDE)
	case DistFuncId_e::IP_BINARY_SIMD16:
		RunSearchPath<IPBinarySIMD16DistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;

	case DistFuncId_e::IP_BINARY_SIMD16_RESIDUALS:
		RunSearchPath<IPBinarySIMD16ResidualsDistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;
#endif

	case DistFuncId_e::L2_FLOAT32:
		RunSearchPath<L2FloatDistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;

	case DistFuncId_e::L2_BINARY_GENERIC:
		RunSearchPath<L2BinaryGenericDistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;

#if !defined(USE_SIMDE)
	case DistFuncId_e::L2_BINARY_SIMD16:
		RunSearchPath<L2BinarySIMD16DistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;

	case DistFuncId_e::L2_BINARY_SIMD16_RESIDUALS:
		RunSearchPath<L2BinarySIMD16ResidualsDistFn_c> ( *m_pAlg, dResults, pData, iResults, pFilterWrapper.get(), &iSearchEf, iSearchPath );
		break;
#endif

	default:
		assert ( 0 );
		break;
	}

	if ( pDistanceComputations )
	{
		long iAfterDistanceComputations = m_pAlg->metric_distance_computations.load();
		*pDistanceComputations = std::max ( 0L, iAfterDistanceComputations - iBeforeDistanceComputations );
	}
}

/////////////////////////////////////////////////////////////////////

class KNN_c : public KNN_i
{
public:
	bool			Load ( const std::string & sFilename, std::string & sError ) override;
	Iterator_i *	CreateIterator ( const std::string & sName, const Span_T<float> & dData, int iResults, int iEf, KNNFilter_i * pFilter, HNSWTerminationPolicy_e ePolicy, bool bCollectMetrics, std::string & sError ) override;
	bool			ShouldUseFullscan ( const std::string & sName, int64_t iResults, int iEf, int64_t iFilterCount ) override;

private:
	std::vector<std::unique_ptr<HNSWIndex_i>>		m_dIndexes;
	std::unordered_map<std::string, HNSWIndex_i*>	m_hIndexes;

	HNSWIndex_i *	GetIndex ( const std::string & sName );
	void			PopulateHash();
};


bool KNN_c::Load ( const std::string & sFilename, std::string & sError )
{
	FileReader_c tReader;
	if ( !tReader.Open ( sFilename, sError ) )
		return false;

	uint32_t uVersion = tReader.Read_uint32();
	if ( uVersion < 2 || uVersion > STORAGE_VERSION )
	{
		sError = FormatStr ( "Unable to load KNN index: %s is v.%d, binary is v.%d", sFilename.c_str(), uVersion, STORAGE_VERSION );
		return false;
	}

	int iNumIndexes = tReader.Read_uint32();
	m_dIndexes.resize(iNumIndexes);

	for ( auto & i : m_dIndexes )
	{
		std::string sName = tReader.Read_string();
		knn::IndexSettings_t tSettings;
		LoadSettings ( tSettings, tReader, uVersion );

		if ( uVersion==2 && tSettings.m_eQuantization!=Quantization_e::NONE )
		{
			sError = FormatStr ( "Unable to load KNN index with quantization: %s is v.%d, binary is v.%d", sFilename.c_str(), uVersion, STORAGE_VERSION );
			return false;
		}
		
		QuantizationSettings_t tQuantSettings;
		if ( tSettings.m_eQuantization!=Quantization_e::NONE )
			LoadQuantizationSettings ( tQuantSettings, tReader, uVersion );

		i = std::make_unique<HNSWIndex_c> ( sName, 0, tSettings, tQuantSettings, CreateQuantizer ( tSettings.m_eQuantization, tQuantSettings, tSettings.m_eHNSWSimilarity ) );
		if ( !i->Load ( tReader, sError ) )
			return false;
	}

	PopulateHash();

	return !tReader.IsError();
}


Iterator_i * KNN_c::CreateIterator ( const std::string & sName, const Span_T<float> & dData, int iResults, int iEf, KNNFilter_i * pFilter, HNSWTerminationPolicy_e ePolicy, bool bCollectMetrics, std::string & sError )
{
	HNSWIndex_i * pIndex = GetIndex(sName);
	if ( !pIndex )
	{
		sError = FormatStr ( "KNN index not found for attribute '%s'", sName.c_str() );
		return nullptr;
	}

	return knn::CreateIterator ( *pIndex, dData, iResults, iEf, bCollectMetrics, pFilter, ePolicy );
}


bool KNN_c::ShouldUseFullscan ( const std::string & sName, int64_t iResults, int iEf, int64_t iFilterCount )
{
	HNSWIndex_i * pIndex = GetIndex(sName);
	if ( !pIndex )
		return false;

	return pIndex->ShouldUseFullscan ( iResults, iEf, iFilterCount );
}


HNSWIndex_i * KNN_c::GetIndex ( const std::string & sName )
{
	const auto & tFound = m_hIndexes.find(sName);
	return tFound==m_hIndexes.end() ? nullptr : tFound->second;
}


void KNN_c::PopulateHash()
{
	for ( auto & i : m_dIndexes )
		m_hIndexes.insert ( { i->GetName(), i.get() } );
}

/////////////////////////////////////////////////////////////////////

class HNSWIndexBuilder_i
{
public:
	virtual			~HNSWIndexBuilder_i() = default;

	virtual void	Train ( uint32_t uRowID, const util::Span_T<float> & dData ) = 0;
	virtual bool	FinalizeTraining ( std::string & sError ) = 0;
	virtual bool	AddDoc ( uint32_t uRowID, const util::Span_T<float> & dData, BuildContext_t & tBuildCtx, std::string & sError ) = 0;
	virtual void	Save ( FileWriter_c & tWriter ) = 0;
	virtual const AttrWithSettings_t & GetAttr() const = 0;
	virtual const QuantizationSettings_t & GetQuantizationSettings() const = 0;
};


class HNSWIndexBuilder_c : public HNSWIndexBuilder_i, public HNSWDist_c
{
public:
			HNSWIndexBuilder_c ( const AttrWithSettings_t & tAttr, int64_t iNumElements, ScalarQuantizer_i * pQuantizer );

	void	Train ( uint32_t uRowID, const util::Span_T<float> & dData ) override;
	bool	FinalizeTraining ( std::string & sError ) override;
	bool	AddDoc ( uint32_t uRowID, const util::Span_T<float> & dData, BuildContext_t & tBuildCtx, std::string & sError ) override;
	void	Save ( FileWriter_c & tWriter ) override;
	const AttrWithSettings_t & GetAttr() const override						{ return m_tAttr; }
	const QuantizationSettings_t & GetQuantizationSettings() const override { return m_pQuantizer->GetSettings(); }

private:
	using AddPoint_fn = void (*) ( hnswlib::HierarchicalNSW<float> &, const void *, uint32_t );

	template <typename DistFn>
	static void	AddPointTyped ( hnswlib::HierarchicalNSW<float> & tAlg, const void * pVec, uint32_t uVecID ) { tAlg.template addPoint<DistFn, false> ( pVec, (size_t)uVecID, -1 ); }
	static void	AddPointFallback ( hnswlib::HierarchicalNSW<float> & tAlg, const void * pVec, uint32_t uVecID ) { tAlg.addPoint ( pVec, (size_t)uVecID ); }
	AddPoint_fn SelectAddPointFn() const;

	bool	AddVector ( uint32_t uVecID, const util::Span_T<float> & dVec, BuildContext_t & tBuildCtx );

	AttrWithSettings_t								m_tAttr;
	std::unique_ptr<ScalarQuantizer_i>				m_pQuantizer;
	std::unique_ptr<hnswlib::HierarchicalNSW<float>> m_pAlg;
	AddPoint_fn										m_fnAddPoint = AddPointFallback;

	bool											m_bCountOverflow = false;	// a row held more vectors than a 32-bit id can address
	std::vector<uint32_t>							m_dCounts;					// [rowid] -> num vectors in that row
	std::vector<int64_t>							m_dBase;					// [rowid] -> first vector id of that row
};


HNSWIndexBuilder_c::AddPoint_fn HNSWIndexBuilder_c::SelectAddPointFn() const
{
	switch ( m_pSpace->GetDistFuncId() )
	{
	case DistFuncId_e::IP_FLOAT32:					return AddPointTyped<IPFloatDistFn_c>;
	case DistFuncId_e::L2_FLOAT32:					return AddPointTyped<L2FloatDistFn_c>;
	case DistFuncId_e::IP_BINARY_GENERIC:			return AddPointTyped<IPBinaryGenericBuildDistFn_c>;
	case DistFuncId_e::L2_BINARY_GENERIC:			return AddPointTyped<L2BinaryGenericBuildDistFn_c>;

#if !defined(USE_SIMDE)
	case DistFuncId_e::IP_BINARY_SIMD16:			return AddPointTyped<IPBinarySIMD16BuildDistFn_c>;
	case DistFuncId_e::IP_BINARY_SIMD16_RESIDUALS:	return AddPointTyped<IPBinarySIMD16ResidualsBuildDistFn_c>;
	case DistFuncId_e::L2_BINARY_SIMD16:			return AddPointTyped<L2BinarySIMD16BuildDistFn_c>;
	case DistFuncId_e::L2_BINARY_SIMD16_RESIDUALS:	return AddPointTyped<L2BinarySIMD16ResidualsBuildDistFn_c>;
#endif

	default:
		return AddPointFallback;
	}
}


HNSWIndexBuilder_c::HNSWIndexBuilder_c ( const AttrWithSettings_t & tAttr, int64_t iNumElements, ScalarQuantizer_i * pQuantizer )
	: HNSWDist_c ( tAttr.m_iDims, tAttr.m_eHNSWSimilarity, tAttr.m_eQuantization, true )
	, m_tAttr ( tAttr )
	, m_pQuantizer ( pQuantizer )
{
	// we don't know total number of vectors in multi mode, so we can't allocate the graph yet - do it in FinalizeTraining()
	if ( m_tAttr.m_bMulti )
		m_dCounts.resize ( (size_t)iNumElements, 0 );
	else
		m_pAlg = std::make_unique<hnswlib::HierarchicalNSW<float>>( m_pSpace.get(), iNumElements, m_tAttr.m_iHNSWM, m_tAttr.m_iHNSWEFConstruction );

	m_fnAddPoint = SelectAddPointFn();
}


void HNSWIndexBuilder_c::Train ( uint32_t uRowID, const util::Span_T<float> & dData )
{
	if ( m_tAttr.m_bMulti )
	{
		assert ( uRowID<m_dCounts.size() );
		assert ( m_tAttr.m_iDims>0 && !( dData.size() % (size_t)m_tAttr.m_iDims ) );

		const size_t uCount = dData.size() / (size_t)m_tAttr.m_iDims;
		if ( uCount>(size_t)UINT32_MAX )
			m_bCountOverflow = true;

		m_dCounts[uRowID] = (uint32_t)uCount;

		// train the quantizer per vector slot
		if ( m_pQuantizer )
			for ( size_t i = 0; i < dData.size(); i += (size_t)m_tAttr.m_iDims )
				m_pQuantizer->Train ( { dData.data()+i, (size_t)m_tAttr.m_iDims } );

		return;
	}

	if ( m_pQuantizer )
		m_pQuantizer->Train(dData);
}


bool HNSWIndexBuilder_c::FinalizeTraining ( std::string & sError )
{
	if ( m_tAttr.m_bMulti )
	{
		int64_t iTotal = 0;
		m_dBase.resize ( m_dCounts.size()+1 );
		for ( size_t i = 0; i < m_dCounts.size(); i++ )
		{
			m_dBase[i] = iTotal;
			iTotal += m_dCounts[i];
		}

		m_dBase[m_dCounts.size()] = iTotal;

		if ( m_bCountOverflow || iTotal > (int64_t)UINT32_MAX )
		{
			sError = FormatStr ( "HNSW error: index '%s' needs %lld vectors but at most %u are addressable per chunk; store fewer vectors per document, or split the table so chunks stay smaller", m_tAttr.m_sName.c_str(), (long long)iTotal, (unsigned)UINT32_MAX );
			return false;
		}

		m_pAlg = std::make_unique<hnswlib::HierarchicalNSW<float>>( m_pSpace.get(), iTotal, m_tAttr.m_iHNSWM, m_tAttr.m_iHNSWEFConstruction );

		// nothing to encode if there are no vectors at all; skip sizing the quantizer temp buffer
		if ( m_pQuantizer && iTotal )
			m_pQuantizer->SetTotalVectors(iTotal);
	}

	if ( !m_pQuantizer )
		return true;

	if ( m_pQuantizer->IsFinalized() )
		return true;

	if ( !m_pQuantizer->FinalizeTraining ( sError ) )
		return false;

	m_pSpace->SetQuantizationSettings ( *m_pQuantizer );
	return true;
}


bool HNSWIndexBuilder_c::AddVector ( uint32_t uVecID, const util::Span_T<float> & dVec, BuildContext_t & tBuildCtx )
{
	Span_T<float> dToAdd = dVec;
	if ( m_tAttr.m_eHNSWSimilarity==HNSWSimilarity_e::COSINE )
	{
		// PER VECTOR. Normalizing a whole multi-vector row as one long vector would scale every slot
		// by the wrong magnitude and silently corrupt every cosine distance in the index.
		tBuildCtx.m_dNormalized.resize ( dVec.size() );
		memcpy ( tBuildCtx.m_dNormalized.data(), dVec.data(), dVec.size()*sizeof(dVec[0]) );
		VecNormalize ( tBuildCtx.m_dNormalized );
		dToAdd = tBuildCtx.m_dNormalized;
	}

	const void * pVec = nullptr;
	if ( m_pQuantizer )
	{
		m_pQuantizer->Encode ( uVecID, dToAdd, tBuildCtx.m_dQuantized, tBuildCtx.m_dQuantizedForQuery );
		pVec = (void*)tBuildCtx.m_dQuantized.data();
	}
	else
		pVec = (void*)dToAdd.data();

	m_fnAddPoint ( *m_pAlg, pVec, uVecID );
	return true;
}


bool HNSWIndexBuilder_c::AddDoc ( uint32_t uRowID, const util::Span_T<float> & dData, BuildContext_t & tBuildCtx, std::string & sError )
{
	assert ( !m_pQuantizer || m_pQuantizer->IsFinalized() );

	const size_t uDims = (size_t)m_tAttr.m_iDims;

	if ( m_tAttr.m_bMulti )
	{
		if ( dData.size() % uDims )
		{
			sError = FormatStr ( "HNSW error: data has %llu values, index '%s' needs a multiple of %d", dData.size(), m_tAttr.m_sName.c_str(), m_tAttr.m_iDims );
			return false;
		}

		const size_t uCount = dData.size() / uDims;
		assert ( uRowID+1<m_dBase.size() );
		assert ( m_dBase[uRowID] + (int64_t)uCount == m_dBase[uRowID+1] );	// Train() saw the same row

		const int64_t iBase = m_dBase[uRowID];
		for ( size_t i = 0; i < uCount; i++ )
			if ( !AddVector ( (uint32_t)( iBase + (int64_t)i ), { dData.data() + i*uDims, uDims }, tBuildCtx ) )
				return false;

		return true;
	}

	if ( dData.size()!=uDims )
	{
		sError = FormatStr ( "HNSW error: data has %llu values, index '%s' needs %d values", dData.size(), m_tAttr.m_sName.c_str(), m_tAttr.m_iDims );
		return false;
	}

	return AddVector ( uRowID, dData, tBuildCtx );
}


void HNSWIndexBuilder_c::Save ( FileWriter_c & tWriter )
{
	if ( m_pQuantizer )
		m_pQuantizer->FinalizeEncoding();

	if ( m_tAttr.m_bMulti )
	{
		const int64_t iTotal = m_dBase.empty() ? 0 : m_dBase.back();
		tWriter.Write_uint64 ( (uint64_t)iTotal );

		for ( size_t uRowID = 0; uRowID < m_dCounts.size(); uRowID++ )
			for ( uint32_t i = 0; i < m_dCounts[uRowID]; i++ )
				tWriter.Write_uint32 ( (uint32_t)uRowID );
	}

	m_pAlg->saveIndex(tWriter);
}

/////////////////////////////////////////////////////////////////////

class HNSWBuilder_c : public Builder_i
{
public:
			HNSWBuilder_c ( const Schema_t & tSchema, int64_t iNumElements, const std::string & sTmpFilename );

	void	Train ( int iAttr, uint32_t uRowID, const util::Span_T<float> & dData ) override								{ m_dIndexes[iAttr]->Train ( uRowID, dData ); }
	bool	SetAttr ( int iAttr, uint32_t uRowID, const util::Span_T<float> & dData, BuildContext_t & tBuildCtx ) override	{ return m_dIndexes[iAttr]->AddDoc ( uRowID, dData, tBuildCtx, tBuildCtx.m_sError ); }
	bool	FinalizeTraining ( std::string & sError ) override;
	bool	Save ( const std::string & sFilename, size_t tBufferSize, std::string & sError ) override;

private:
	std::vector<std::unique_ptr<HNSWIndexBuilder_i>> m_dIndexes;
};


HNSWBuilder_c::HNSWBuilder_c ( const Schema_t & tSchema, int64_t iNumElements, const std::string & sTmpFilename )
{
	int iFile = 0;
	for ( const auto & i : tSchema )
		m_dIndexes.push_back ( std::make_unique<HNSWIndexBuilder_c> ( i, iNumElements, CreateQuantizer ( i.m_eQuantization, i.m_eHNSWSimilarity, iNumElements, FormatStr ( "%s.%d", sTmpFilename.c_str(), iFile++ ) ) ) );
}


bool HNSWBuilder_c::FinalizeTraining ( std::string & sError )
{
	for ( auto & i : m_dIndexes )
		if ( !i->FinalizeTraining(sError) )
			return false;

	return true;
}


bool HNSWBuilder_c::Save ( const std::string & sFilename, size_t tBufferSize, std::string & sError )
{
	FileWriter_c tWriter;
	tWriter.SetBufferSize(tBufferSize);
	if ( !tWriter.Open ( sFilename, sError ) )
		return false;

	tWriter.Write_uint32 ( STORAGE_VERSION );
	tWriter.Write_uint32 ( (uint32_t)m_dIndexes.size() );
	for ( auto & i : m_dIndexes )
	{
		tWriter.Write_string ( i->GetAttr().m_sName );
		SaveSettings ( i->GetAttr(), tWriter );
		if ( i->GetAttr().m_eQuantization != Quantization_e::NONE )
			SaveQuantizationSettings ( i->GetQuantizationSettings(), tWriter );

		i->Save(tWriter);
	}

	tWriter.Close();
	return !tWriter.IsError();
}

} // namespace knn


knn::Distance_i * CreateDistanceCalc ( const knn::IndexSettings_t & tSettings )
{
	return new knn::Distance_c(tSettings);
}


knn::KNN_i * CreateKNN()
{
	return new knn::KNN_c;
}


knn::Builder_i * CreateKNNBuilder ( const knn::Schema_t & tSchema, int64_t iNumElements, const std::string & sTmpFilename )
{
	return new knn::HNSWBuilder_c ( tSchema, iNumElements, sTmpFilename );
}


knn::EmbeddingsLib_i * LoadEmbeddingsLib ( const std::string & sLibPath, std::string & sError )
{
	return knn::LoadEmbeddingsLib ( sLibPath, sError );
}


int GetKNNLibVersion()
{
	return knn::LIB_VERSION;
}


extern const char * LIB_VERSION;
const char * GetKNNLibVersionStr()
{
	return LIB_VERSION;
}
