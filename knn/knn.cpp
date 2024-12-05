// Copyright (c) 2023-2024, Manticore Software LTD (https://manticoresearch.com)
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
#include "util/reader.h"

#include <unordered_map>
#include <algorithm>

#include "hnswlib.h"

namespace knn
{

using namespace util;

// not member functions because there's no need to expose them in knn.h
static void LoadSettings ( IndexSettings_t & tSettings, FileReader_c & tReader )
{
	tSettings.m_iDims				= tReader.Read_uint32();
	tSettings.m_eHNSWSimilarity		= (HNSWSimilarity_e)tReader.Read_uint32();
	tSettings.m_iHNSWM				= tReader.Read_uint32();
	tSettings.m_iHNSWEFConstruction	= tReader.Read_uint32();
}


static void SaveSettings ( const IndexSettings_t & tSettings, FileWriter_c & tWriter )
{
	tWriter.Write_uint32 ( tSettings.m_iDims );
	tWriter.Write_uint32 ( (int)tSettings.m_eHNSWSimilarity );
	tWriter.Write_uint32 ( tSettings.m_iHNSWM );
	tWriter.Write_uint32 ( tSettings.m_iHNSWEFConstruction );
}

/////////////////////////////////////////////////////////////////////

class HNSWDist_c
{
public:
			HNSWDist_c ( int iDim, HNSWSimilarity_e eSimilarity );

	hnswlib::SpaceInterface<float> * GetSpaceInterface();

protected:
	hnswlib::InnerProductSpace	m_tSpaceIP;
	hnswlib::L2Space			m_tSpaceL2;
	int							m_iDim = 0;
	HNSWSimilarity_e			m_eSimilarity = HNSWSimilarity_e::L2;
};

HNSWDist_c::HNSWDist_c ( int iDim, HNSWSimilarity_e eSimilarity )
	: m_tSpaceIP ( iDim )
	, m_tSpaceL2 ( iDim )
	, m_iDim ( iDim )
	, m_eSimilarity ( eSimilarity )
{}


hnswlib::SpaceInterface<float> * HNSWDist_c::GetSpaceInterface()
{
	switch ( m_eSimilarity )
	{
	case HNSWSimilarity_e::IP:
	case HNSWSimilarity_e::COSINE:  return &m_tSpaceIP;
	case HNSWSimilarity_e::L2:		return &m_tSpaceL2;
	default:
		assert ( 0 && "Unknown similarity" );
		return nullptr;
	}
}

/////////////////////////////////////////////////////////////////////

class Distance_c : public Distance_i, public HNSWDist_c
{
public:
			Distance_c ( const knn::IndexSettings_t & tSettings );

	float	CalcDist ( const util::Span_T<float> & dPoint1, const util::Span_T<float> & dPoint2 ) const override;

private:
	hnswlib::DISTFUNC<float>	m_fnDistFunc;
	void *						m_pDistFuncParam = nullptr;
};


Distance_c::Distance_c ( const knn::IndexSettings_t & tSettings )
	: HNSWDist_c ( tSettings.m_iDims, tSettings.m_eHNSWSimilarity )
{
	hnswlib::SpaceInterface<float> * pSpace = GetSpaceInterface();
	m_fnDistFunc = pSpace->get_dist_func();
	m_pDistFuncParam = pSpace->get_dist_func_param();
}


float Distance_c::CalcDist ( const util::Span_T<float> & dPoint1, const util::Span_T<float> & dPoint2 ) const
{
	assert ( dPoint1.size()==m_iDim && dPoint2.size()==m_iDim );
	assert ( m_fnDistFunc );
	return m_fnDistFunc ( dPoint1.data(), dPoint2.data(), m_pDistFuncParam );
}

/////////////////////////////////////////////////////////////////////

class HNSWIndex_c : public HNSWDist_c, public KNNIndex_i
{
public:
			HNSWIndex_c ( const std::string & sName, int64_t iNumElements, const knn::IndexSettings_t & tSettings );

	bool	AddDoc ( const Span_T<float> & dData, std::string & sError );
	bool	Load ( FileReader_c & tReader, std::string & sError )	{ return m_pAlg->loadIndex ( tReader, GetSpaceInterface(), sError ); }
	const std::string &	GetName() const								{ return m_sName; }
	const knn::IndexSettings_t & GetSettings() const				{ return m_tSettings; }
	void	Search ( std::vector<DocDist_t> & dResults, const Span_T<float> & dData, int iResults, int iEf ) const override;

private:
	std::string				m_sName;
	knn::IndexSettings_t	m_tSettings;
	std::unique_ptr<hnswlib::HierarchicalNSW<float>> m_pAlg;
	uint32_t				m_tRowID = 0;
	SpanResizeable_T<float>	m_dNormalized;
};


HNSWIndex_c::HNSWIndex_c ( const std::string & sName, int64_t iNumElements, const knn::IndexSettings_t & tSettings )
	: HNSWDist_c ( tSettings.m_iDims, tSettings.m_eHNSWSimilarity )
	, m_sName ( sName )
	, m_tSettings ( tSettings )
{
	m_pAlg = std::make_unique<hnswlib::HierarchicalNSW<float>>( GetSpaceInterface(), iNumElements, tSettings.m_iHNSWM, tSettings.m_iHNSWEFConstruction );
	m_dNormalized.resize ( tSettings.m_iDims );
}


bool HNSWIndex_c::AddDoc ( const Span_T<float> & dData, std::string & sError )
{
	if ( dData.size()!=(size_t)m_tSettings.m_iDims )
	{
		sError = FormatStr ( "HNSW error: data has %ull values, index '%s' needs %d values", dData.size(), m_sName.c_str(), m_tSettings.m_iDims );
		return false;
	}

	Span_T<float> dToAdd = dData;
	if ( m_tSettings.m_eHNSWSimilarity==HNSWSimilarity_e::COSINE )
	{
		memcpy ( m_dNormalized.data(), dData.data(), dData.size()*sizeof(dData[0] ) );
		NormalizeVec(m_dNormalized);
		dToAdd = m_dNormalized;
	}

	m_pAlg->addPoint ( (void*)dToAdd.data(), (size_t)m_tRowID++ );
	return true;
}


void HNSWIndex_c::Search ( std::vector<DocDist_t> & dResults, const Span_T<float> & dData, int iResults, int iEf ) const
{
	size_t iSearchEf = iEf;
	auto tPQ = m_pAlg->searchKnn ( dData.begin(), iResults, nullptr, &iSearchEf );
	dResults.resize(0);
	dResults.reserve ( tPQ.size() );
	while ( !tPQ.empty() )
	{
		dResults.push_back ( { (uint32_t)tPQ.top().second, tPQ.top().first } );
		tPQ.pop();
	}
}

/////////////////////////////////////////////////////////////////////

class KNN_c : public KNN_i
{
public:
	bool			Load ( const std::string & sFilename, std::string & sError ) override;
	Iterator_i *	CreateIterator ( const std::string & sName, const Span_T<float> & dData, int iResults, int iEf, std::string & sError ) override;

private:
	std::vector<std::unique_ptr<HNSWIndex_c>>		m_dIndexes;
	std::unordered_map<std::string, HNSWIndex_c*>	m_hIndexes;

	HNSWIndex_c *	GetIndex ( const std::string & sName );
	void			PopulateHash();
};


bool KNN_c::Load ( const std::string & sFilename, std::string & sError )
{
	FileReader_c tReader;
	if ( !tReader.Open ( sFilename, sError ) )
		return false;

	uint32_t uVersion = tReader.Read_uint32();
	if ( uVersion!=STORAGE_VERSION )
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
		LoadSettings ( tSettings, tReader );

		i = std::make_unique<HNSWIndex_c> ( sName, 0, tSettings );
		if ( !i->Load ( tReader, sError ) )
			return false;
	}

	PopulateHash();

	return !tReader.IsError();
}


Iterator_i * KNN_c::CreateIterator ( const std::string & sName, const Span_T<float> & dData, int iResults, int iEf , std::string & sError )
{
	HNSWIndex_c * pIndex = GetIndex(sName);
	if ( !pIndex )
	{
		sError = FormatStr ( "KNN index not found for attribute '%s'", sName.c_str() );
		return nullptr;
	}

	return knn::CreateIterator ( *pIndex, dData, iResults, iEf );
}


HNSWIndex_c * KNN_c::GetIndex ( const std::string & sName )
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

class HNSWIndexBuilder_c : public HNSWDist_c
{
public:
			HNSWIndexBuilder_c ( const AttrWithSettings_t & tAttr, int64_t iNumElements );

	bool	AddDoc ( const util::Span_T<float> & dData, std::string & sError );
	void	Save ( FileWriter_c & tWriter )			{ m_pAlg->saveIndex(tWriter); }
	const AttrWithSettings_t &	GetAttr() const		{ return m_tAttr; }

private:
	AttrWithSettings_t	m_tAttr;
	uint32_t			m_tRowID = 0;
	SpanResizeable_T<float> m_dNormalized;
	std::unique_ptr<hnswlib::HierarchicalNSW<float>> m_pAlg;
};


HNSWIndexBuilder_c::HNSWIndexBuilder_c ( const AttrWithSettings_t & tAttr, int64_t iNumElements )
	: HNSWDist_c ( tAttr.m_iDims, tAttr.m_eHNSWSimilarity )
	, m_tAttr ( tAttr )
{
	m_pAlg = std::make_unique<hnswlib::HierarchicalNSW<float>>( GetSpaceInterface(), iNumElements, m_tAttr.m_iHNSWM, m_tAttr.m_iHNSWEFConstruction );
	m_dNormalized.resize ( tAttr.m_iDims );
}


bool HNSWIndexBuilder_c::AddDoc ( const util::Span_T<float> & dData, std::string & sError )
{
	if ( dData.size()!=m_tAttr.m_iDims )
	{
		sError = FormatStr ( "HNSW error: data has %llu values, index '%s' needs %d values", dData.size(), m_tAttr.m_sName.c_str(), m_tAttr.m_iDims );
		return false;
	}

	Span_T<float> dToAdd = dData;
	if ( m_tAttr.m_eHNSWSimilarity==HNSWSimilarity_e::COSINE )
	{
		memcpy ( m_dNormalized.data(), dData.data(), dData.size()*sizeof(dData[0] ) );
		NormalizeVec(m_dNormalized);
		dToAdd = m_dNormalized;
	}

	m_pAlg->addPoint ( (void*)dToAdd.data(), (size_t)m_tRowID++ );
	return true;
}

/////////////////////////////////////////////////////////////////////

class HNSWBuilder_c : public Builder_i
{
public:
	HNSWBuilder_c ( const Schema_t & tSchema, int64_t iNumElements );

	bool	SetAttr ( int iAttr, const util::Span_T<float> & dData ) override		{ return m_dIndexes[iAttr]->AddDoc ( dData, m_sError ); }
	bool	Save ( const std::string & sFilename, size_t tBufferSize, std::string & sError ) override;
	const std::string & GetError() const override									{ return m_sError; }

private:
	std::vector<std::unique_ptr<HNSWIndexBuilder_c>> m_dIndexes;
	std::string m_sError;
};


HNSWBuilder_c::HNSWBuilder_c ( const Schema_t & tSchema, int64_t iNumElements )
{
	for ( const auto & i : tSchema )
		m_dIndexes.push_back ( std::make_unique<HNSWIndexBuilder_c> ( i, iNumElements ) );
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


knn::Builder_i * CreateKNNBuilder ( const knn::Schema_t & tSchema, int64_t iNumElements )
{
	return new knn::HNSWBuilder_c ( tSchema, iNumElements );
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
