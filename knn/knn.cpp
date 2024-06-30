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
#include "util/reader.h"

#include <unordered_map>
#include <algorithm>

#include "hnswlib.h"

#define ANNOYLIB_MULTITHREADED_BUILD
#include "annoy/annoylib.h"
#include "annoy/kissrandom.h"


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
	tSettings.m_iAnnoyNTrees = tReader.Read_uint32();
	tSettings.m_eKnnType = static_cast<KNNType_e>(tReader.Read_uint32());
	tSettings.m_eAnnoyMetric = static_cast<AnnoyMetric_e>(tReader.Read_uint32());
}


static void SaveSettings ( const IndexSettings_t & tSettings, FileWriter_c & tWriter )
{
	tWriter.Write_uint32 ( tSettings.m_iDims );
	tWriter.Write_uint32 ( (int)tSettings.m_eHNSWSimilarity );
	tWriter.Write_uint32 ( tSettings.m_iHNSWM );
	tWriter.Write_uint32 ( tSettings.m_iHNSWEFConstruction );
	tWriter.Write_uint32 ( tSettings.m_iAnnoyNTrees );
	tWriter.Write_uint32 ( static_cast<uint32_t>(tSettings.m_eKnnType) );
	tWriter.Write_uint32 ( static_cast<uint32_t>(tSettings.m_eAnnoyMetric) );
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

class AngularDistance_c : public Distance_i
{
public:
	explicit AngularDistance_c(int iDim);
	float CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const override;

private:
	int m_iDim;
};

AngularDistance_c::AngularDistance_c(int iDim) : m_iDim(iDim)
{
}

float AngularDistance_c::CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const
{
	assert(dPoint1.size() == m_iDim && dPoint2.size() == m_iDim);
	// copied from Annoy::Angular::distance
	float pp = Annoy::dot(dPoint1.data(), dPoint1.data(), m_iDim);
	float qq = Annoy::dot(dPoint2.data(), dPoint2.data(), m_iDim);
	float pq = Annoy::dot(dPoint1.data(), dPoint2.data(), m_iDim);
	float ppqq = pp * qq;
	if (ppqq > 0) return 2.0 - 2.0 * pq / sqrt(ppqq);
	else return 2.0; // cos is 0
}

class EuclideanDistance_c : public Distance_i
{
public:
	explicit EuclideanDistance_c(int iDim);
	float CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const override;

private:
	int m_iDim;
};

EuclideanDistance_c::EuclideanDistance_c(int iDim) : m_iDim(iDim)
{
}

float EuclideanDistance_c::CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const
{
	assert(dPoint1.size() == m_iDim && dPoint2.size() == m_iDim);
	return sqrt(Annoy::euclidean_distance(dPoint1.data(), dPoint2.data(), m_iDim));
}

class ManhattanDistance_c : public Distance_i
{
public:
	explicit ManhattanDistance_c(int iDim);
	float CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const override;

private:
	int m_iDim;
};

ManhattanDistance_c::ManhattanDistance_c(int iDim) : m_iDim(iDim)
{
}

float ManhattanDistance_c::CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const
{
	assert(dPoint1.size() == m_iDim && dPoint2.size() == m_iDim);
	return Annoy::manhattan_distance(dPoint1.data(), dPoint2.data(), m_iDim);
}

class DotDistance_c : public Distance_i
{
public:
	explicit DotDistance_c(int iDim);
	float CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const override;

private:
	int m_iDim;
};

DotDistance_c::DotDistance_c(int iDim) : m_iDim(iDim)
{
}

float DotDistance_c::CalcDist(const util::Span_T<float>& dPoint1, const util::Span_T<float>& dPoint2) const
{
	assert(dPoint1.size() == m_iDim && dPoint2.size() == m_iDim);
	return Annoy::dot(dPoint1.data(), dPoint2.data(), m_iDim);
}

struct AnnoyError_t
{
	~AnnoyError_t();
	void Release();
	char* m_sError = nullptr;
};

AnnoyError_t::~AnnoyError_t()
{
	Release();
}

void AnnoyError_t::Release()
{
	if (m_sError)
	{
		free(m_sError);
	}

	m_sError = nullptr;
}

class AnnoyIndexBuilder_c
{
public:
	AnnoyIndexBuilder_c(const AttrWithSettings_t& tAttr);
	bool	AddDoc(const util::Span_T<float>& dData, std::string& sError);
	bool Save(FileWriter_c& tWriter, std::string& sError);
	const AttrWithSettings_t& GetAttr() const { return m_tAttr; }

private:
	AttrWithSettings_t m_tAttr;
	uint32_t m_tRowID;
	std::unique_ptr<Annoy::AnnoyIndexInterface<uint32_t, float, uint32_t>> m_pAlg;
};

AnnoyIndexBuilder_c::AnnoyIndexBuilder_c(const AttrWithSettings_t& tAttr) : m_tAttr(tAttr), m_tRowID(0)
{
	switch (tAttr.m_eAnnoyMetric)
	{
	case AnnoyMetric_e::ANGULAR: m_pAlg = std::make_unique<Annoy::AnnoyIndex<uint32_t, float, Annoy::Angular, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tAttr.m_iDims); break;
	case AnnoyMetric_e::EUCLIDEAN: m_pAlg = std::make_unique<Annoy::AnnoyIndex<uint32_t, float, Annoy::Euclidean, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tAttr.m_iDims); break;
	case AnnoyMetric_e::MANHATTAN: m_pAlg = std::make_unique<Annoy::AnnoyIndex<uint32_t, float, Annoy::Manhattan, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tAttr.m_iDims); break;
	case AnnoyMetric_e::DOT: m_pAlg = std::make_unique<Annoy::AnnoyIndex<uint32_t, float, Annoy::DotProduct, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tAttr.m_iDims); break;
	default: assert(0 && "Wrong annoy metric");
	}
}

bool AnnoyIndexBuilder_c::AddDoc(const util::Span_T<float>& dData, std::string& sError)
{
	if (dData.size() != m_tAttr.m_iDims)
	{
		sError = FormatStr("Annoy error: data has %llu values, index '%s' needs %d values", dData.size(), m_tAttr.m_sName.c_str(), m_tAttr.m_iDims);
		return false;
	}

	AnnoyError_t err;
	if (!m_pAlg->add_item(m_tRowID++, dData.data(), &err.m_sError))
	{
		sError = err.m_sError ? err.m_sError : "Unknown annoy add_item error";
		return false;
	}

	return true;
}

bool AnnoyIndexBuilder_c::Save(FileWriter_c& tWriter, std::string& sError)
{
	AnnoyError_t err;
	m_pAlg->build(m_tAttr.m_iAnnoyNTrees, -1, &err.m_sError);
	if (err.m_sError)
	{
		sError = err.m_sError;
		return false;
	}

	err.Release();
	Annoy::Writer wrapper(tWriter);
	m_pAlg->save_to_writer(wrapper, &err.m_sError);
	if (err.m_sError)
	{
		sError = err.m_sError;
		return false;
	}

	return true;
}

class AnnoyBuilder_c : public Builder_i
{
public:
	AnnoyBuilder_c(const Schema_t& tSchema);

	bool SetAttr(int iAttr, const util::Span_T<float>& dData) override;
	bool Save(const std::string& sFilename, size_t tBufferSize, std::string& sError) override;
	const std::string& GetError() const override { return m_sError; }

private:
	std::vector<std::unique_ptr<AnnoyIndexBuilder_c>> m_dIndexes;
	std::string m_sError;
};

AnnoyBuilder_c::AnnoyBuilder_c(const Schema_t& tSchema)
{
	for (const auto& i : tSchema)
	{
		m_dIndexes.push_back(std::make_unique<AnnoyIndexBuilder_c>(i));
	}
}

bool AnnoyBuilder_c::SetAttr(int iAttr, const util::Span_T<float>& dData)
{
	return m_dIndexes[iAttr]->AddDoc(dData, m_sError);
}

bool AnnoyBuilder_c::Save(const std::string& sFilename, size_t tBufferSize, std::string& sError)
{
	FileWriter_c tWriter;
	tWriter.SetBufferSize(tBufferSize);
	if (!tWriter.Open(sFilename, sError))
	{
		return false;
	}

	tWriter.Write_uint32(STORAGE_VERSION);
	tWriter.Write_uint32((uint32_t)m_dIndexes.size());
	for (auto& index : m_dIndexes)
  {
    tWriter.Write_string(index->GetAttr().m_sName);
		SaveSettings(index->GetAttr(), tWriter);
		if (!index->Save(tWriter, sError))
		{
			return false;
		}
	}

	return !tWriter.IsError();
}

class AnnoyIndex_c : public KNNIndex_i
{
public:
	AnnoyIndex_c(const std::string& sName, int64_t iNumElements, const knn::IndexSettings_t& tSettings);

	bool Load(FileReader_c &tReader, std::string& sError);
	const std::string& GetName() const { return m_sName; }
	const knn::IndexSettings_t& GetSettings() const { return m_tSettings; }
	void Search(std::vector<DocDist_t>& dResults, const Span_T<float>& dData, int iResults, int iEf) const override;

private:
	std::string				m_sName;
	knn::IndexSettings_t	m_tSettings;
	std::unique_ptr<Annoy::AnnoyIndexInterface<int32_t, float, uint32_t>> m_pAlg;
	uint32_t				m_tRowID = 0;
};

AnnoyIndex_c::AnnoyIndex_c(const std::string& sName, int64_t iNumElements, const knn::IndexSettings_t& tSettings) : m_sName(sName), m_tSettings(tSettings)
{
	switch (tSettings.m_eAnnoyMetric)
	{
	case AnnoyMetric_e::ANGULAR: m_pAlg = std::make_unique<Annoy::AnnoyIndex<int32_t, float, Annoy::Angular, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tSettings.m_iDims); break;
	case AnnoyMetric_e::EUCLIDEAN: m_pAlg = std::make_unique<Annoy::AnnoyIndex<int32_t, float, Annoy::Euclidean, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tSettings.m_iDims); break;
	case AnnoyMetric_e::MANHATTAN: m_pAlg = std::make_unique<Annoy::AnnoyIndex<int32_t, float, Annoy::Manhattan, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tSettings.m_iDims); break;
	case AnnoyMetric_e::DOT: m_pAlg = std::make_unique<Annoy::AnnoyIndex<int32_t, float, Annoy::DotProduct, Annoy::Kiss32Random, Annoy::AnnoyIndexMultiThreadedBuildPolicy>>(tSettings.m_iDims); break;
	default: assert(0 && "Wrong annoy metric");
	}
}

bool AnnoyIndex_c::Load(FileReader_c& tReader, std::string& sError)
{
	Annoy::Reader reader(tReader);
	AnnoyError_t err;
	if (!m_pAlg->load_from_reader(reader, &err.m_sError))
	{
		sError = err.m_sError;
		return false;
	}

	return true;
}

void AnnoyIndex_c::Search(std::vector<DocDist_t>& dResults, const Span_T<float>& dData, int iResults, int iEf) const
{
	std::vector<int> dLabels;
	std::vector<float> dDistances;
	m_pAlg->get_nns_by_vector(dData.data(), iResults, iEf != 0 ? iEf : -1, &dLabels, &dDistances);
	for (int i = 0; i < dLabels.size(); ++i)
	{
		dResults.push_back({ static_cast<uint32_t>(dLabels[i]), dDistances[i] });
	}
}

/////////////////////////////////////////////////////////////////////

class KNN_c : public KNN_i
{
public:
	bool			Load(const std::string& sFilename, std::string& sError) override;
	Iterator_i* CreateIterator(const std::string& sName, const Span_T<float>& dData, int iResults, int iEf, std::string& sError) override;

private:
	std::vector<std::unique_ptr<KNNIndex_i>>		m_dIndexes;
	std::unordered_map<std::string, KNNIndex_i*>	m_hIndexes;

	KNNIndex_i* GetIndex(const std::string& sName);
};


bool KNN_c::Load(const std::string& sFilename, std::string& sError)
{
	FileReader_c tReader;
	if (!tReader.Open(sFilename, sError))
		return false;

	uint32_t uVersion = tReader.Read_uint32();
	if (uVersion != STORAGE_VERSION)
	{
		sError = FormatStr("Unable to load KNN index: %s is v.%d, binary is v.%d", sFilename.c_str(), uVersion, STORAGE_VERSION);
		return false;
	}

	int iNumIndexes = tReader.Read_uint32();
	m_dIndexes.resize(iNumIndexes);

	for (auto& i : m_dIndexes)
	{
		std::string sName = tReader.Read_string();
		knn::IndexSettings_t tSettings;
		LoadSettings(tSettings, tReader);


		switch (tSettings.m_eKnnType)
		{
		case KNNType_e::HNSW:
		{
			auto index = new HNSWIndex_c(sName, 0, tSettings);
			i.reset(index);
			if (!index->Load(tReader, sError))
				return false;
			break;
		}

		case KNNType_e::ANNOY:
		{
			auto index = new AnnoyIndex_c(sName, 0, tSettings);
			i.reset(index);
			if (!index->Load(tReader, sError))
				return false;
			break;
		}
		}

		m_hIndexes.insert({ sName, i.get() });
	}

	return !tReader.IsError();
}


Iterator_i* KNN_c::CreateIterator(const std::string& sName, const Span_T<float>& dData, int iResults, int iEf, std::string& sError)
{
	KNNIndex_i* pIndex = GetIndex(sName);
	if (!pIndex)
	{
		sError = FormatStr("KNN index not found for attribute '%s'", sName.c_str());
		return nullptr;
	}

	return knn::CreateIterator(*pIndex, dData, iResults, iEf);
}


KNNIndex_i* KNN_c::GetIndex(const std::string& sName)
{
	const auto& tFound = m_hIndexes.find(sName);
	return tFound == m_hIndexes.end() ? nullptr : tFound->second;
}
} // namespace knn


knn::Distance_i * CreateDistanceCalc ( const knn::IndexSettings_t & tSettings )
{
	switch (tSettings.m_eKnnType)
	{
	case knn::KNNType_e::HNSW:
		return new knn::Distance_c(tSettings);

	case knn::KNNType_e::ANNOY:
	{
		switch (tSettings.m_eAnnoyMetric)
		{
		case knn::AnnoyMetric_e::ANGULAR: return new knn::AngularDistance_c(tSettings.m_iDims);
		case knn::AnnoyMetric_e::EUCLIDEAN: return new knn::EuclideanDistance_c(tSettings.m_iDims);
		case knn::AnnoyMetric_e::MANHATTAN: return new knn::ManhattanDistance_c(tSettings.m_iDims);
		case knn::AnnoyMetric_e::DOT: return new knn::DotDistance_c(tSettings.m_iDims);
		default:
			assert(0 && "Unknown annoy metric");
			return nullptr;
		}
	}

	default:
		assert(0 && "Unknown knn type");
		return nullptr;
	}
}


knn::KNN_i * CreateKNN()
{
	return new knn::KNN_c;
}


knn::Builder_i * CreateKNNBuilder ( const knn::Schema_t & tSchema, int64_t iNumElements )
{
	switch (tSchema.front().m_eKnnType)
	{
		case knn::KNNType_e::HNSW:
			return new knn::HNSWBuilder_c(tSchema, iNumElements);

		case knn::KNNType_e::ANNOY:
			return new knn::AnnoyBuilder_c(tSchema);

		default:
			assert(0 && "Unknown knn type");
			return nullptr;
	}
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
