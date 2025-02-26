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
#include "quantizer.h"
#include "space.h"
#include "util/reader.h"

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
}


static void LoadQuantizationSettings ( QuantizationSettings_t & tSettings, FileReader_c & tReader, uint32_t uVersion )
{
	if ( uVersion<2 )
		return;

	tSettings.m_fMin = UintToFloat ( tReader.Read_uint32() );
	tSettings.m_fMax = UintToFloat ( tReader.Read_uint32() );
}


static void SaveSettings ( const IndexSettings_t & tSettings, FileWriter_c & tWriter )
{
	tWriter.Write_uint32 ( tSettings.m_iDims );
	tWriter.Write_uint32 ( (int)tSettings.m_eHNSWSimilarity );
	tWriter.Write_uint32 ( (int)tSettings.m_eQuantization );
	tWriter.Write_uint32 ( tSettings.m_iHNSWM );
	tWriter.Write_uint32 ( tSettings.m_iHNSWEFConstruction );
}


static void SaveQuantizationSettings ( const QuantizationSettings_t & tSettings, FileWriter_c & tWriter )
{
	tWriter.Write_uint32 ( FloatToUint ( tSettings.m_fMin ) );
	tWriter.Write_uint32 ( FloatToUint ( tSettings.m_fMax ) );
}

/////////////////////////////////////////////////////////////////////

template<typename T>
class HNSWDist_T
{
public:
			HNSWDist_T ( int iDim, HNSWSimilarity_e eSimilarity, Quantization_e eQuantization );

	hnswlib::SpaceInterface<T> * GetSpaceInterface ( std::string & sError );

protected:
	hnswlib::InnerProductSpace	m_tSpaceIP;
	hnswlib::L2Space			m_tSpaceL2;
	L2Space1Bit_c				m_tSpaceL2Bit1;
	L2Space4Bit_c				m_tSpaceL2Bit4;
	L2Space8Bit_c				m_tSpaceL2Bit8;
	IPSpace1Bit_c				m_tSpaceIPBit1;
	IPSpace4Bit_c				m_tSpaceIPBit4;
	IPSpace8Bit_c				m_tSpaceIPBit8;
	int							m_iDim = 0;
	HNSWSimilarity_e			m_eSimilarity = HNSWSimilarity_e::L2;
	Quantization_e				m_eQuantization = Quantization_e::NONE;
};

template<typename T>
HNSWDist_T<T>::HNSWDist_T ( int iDim, HNSWSimilarity_e eSimilarity, Quantization_e eQuantization )
	: m_tSpaceIP ( iDim )
	, m_tSpaceL2 ( iDim )
	, m_tSpaceL2Bit1 ( iDim )
	, m_tSpaceL2Bit4 ( iDim )
	, m_tSpaceL2Bit8 ( iDim )
	, m_tSpaceIPBit1 ( iDim )
	, m_tSpaceIPBit4 ( iDim )
	, m_tSpaceIPBit8 ( iDim )
	, m_iDim ( iDim )
	, m_eSimilarity ( eSimilarity )
	, m_eQuantization ( eQuantization )
{}

template<>
hnswlib::SpaceInterface<float> * HNSWDist_T<float>::GetSpaceInterface ( std::string & sError )
{
	switch ( m_eSimilarity )
	{
	case HNSWSimilarity_e::IP:
	case HNSWSimilarity_e::COSINE:  return &m_tSpaceIP;
	case HNSWSimilarity_e::L2:		return &m_tSpaceL2;
	default:
		sError = "Unknown similarity";
		return nullptr;
	}
}

template<>
hnswlib::SpaceInterface<int> * HNSWDist_T<int>::GetSpaceInterface ( std::string & sError )
{
	switch ( m_eSimilarity )
	{
	case HNSWSimilarity_e::IP:
	case HNSWSimilarity_e::COSINE:
		switch ( m_eQuantization )
		{
		case Quantization_e::BIT1:	return &m_tSpaceIPBit1;
		case Quantization_e::BIT4:	return &m_tSpaceIPBit4;
		default:					return &m_tSpaceIPBit8;
		}

	case HNSWSimilarity_e::L2:
	{
		switch ( m_eQuantization )
		{
		case Quantization_e::BIT1:	return &m_tSpaceL2Bit1;
		case Quantization_e::BIT4:	return &m_tSpaceL2Bit4;
		default:					return &m_tSpaceL2Bit8;
		}
	}

	default:
		sError = "Unknown similarity";
		return nullptr;
	}
}

/////////////////////////////////////////////////////////////////////

class Distance_c : public Distance_i, public HNSWDist_T<float>
{
public:
			Distance_c ( const knn::IndexSettings_t & tSettings );

	float	CalcDist ( const util::Span_T<float> & dPoint1, const util::Span_T<float> & dPoint2 ) const override;

private:
	hnswlib::DISTFUNC<float>	m_fnDistFunc;
	void *						m_pDistFuncParam = nullptr;
};


Distance_c::Distance_c ( const knn::IndexSettings_t & tSettings )
	: HNSWDist_T<float> ( tSettings.m_iDims, tSettings.m_eHNSWSimilarity, tSettings.m_eQuantization )
{
	std::string sError;
	hnswlib::SpaceInterface<float> * pSpace = GetSpaceInterface(sError);
	assert(pSpace);
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

class HNSWIndex_i : public KNNIndex_i
{
public:
	virtual bool	Load ( FileReader_c & tReader, std::string & sError ) = 0;
	virtual const std::string &	GetName() const = 0;
};

template <typename T>
class HNSWIndex_T : public HNSWDist_T<T>, public HNSWIndex_i
{
	using DIST = HNSWDist_T<T>;

public:
			HNSWIndex_T ( const std::string & sName, int64_t iNumElements, const knn::IndexSettings_t & tSettings, ScalarQuantizer_i * pQuantizer );

	bool	Load ( FileReader_c & tReader, std::string & sError )	override { return m_pAlg->loadIndex ( tReader, m_pSpaceInterface, sError ); }
	const std::string &	GetName() const override	{ return m_sName; }
	void	Search ( std::vector<DocDist_t> & dResults, const Span_T<float> & dData, int iResults, int iEf, std::vector<uint8_t> & dQuantized ) const override;

private:
	std::string										m_sName;
	std::unique_ptr<hnswlib::HierarchicalNSW<T>>	m_pAlg;
	hnswlib::SpaceInterface<T> *					m_pSpaceInterface = nullptr;
	std::unique_ptr<ScalarQuantizer_i>				m_pQuantizer;
};

template <typename T>
HNSWIndex_T<T>::HNSWIndex_T ( const std::string & sName, int64_t iNumElements, const knn::IndexSettings_t & tSettings, ScalarQuantizer_i * pQuantizer )
	: DIST ( tSettings.m_iDims, tSettings.m_eHNSWSimilarity,  tSettings.m_eQuantization )
	, m_sName ( sName )
	, m_pQuantizer ( pQuantizer )
{
	std::string sError;
	m_pSpaceInterface = DIST::GetSpaceInterface(sError);
	assert(m_pSpaceInterface);

	m_pAlg = std::make_unique<hnswlib::HierarchicalNSW<T>>( m_pSpaceInterface, iNumElements, tSettings.m_iHNSWM, tSettings.m_iHNSWEFConstruction );
}

template <typename T>
void HNSWIndex_T<T>::Search ( std::vector<DocDist_t> & dResults, const Span_T<float> & dData, int iResults, int iEf, std::vector<uint8_t> & dQuantized ) const
{
	const void * pData = dData.begin();
	if ( m_pQuantizer )
	{
		m_pQuantizer->Encode ( dData, dQuantized );
		pData = dQuantized.data();
	}

	size_t iSearchEf = iEf;
	auto tPQ = m_pAlg->searchKnn ( pData, iResults, nullptr, &iSearchEf );
	dResults.resize(0);
	dResults.reserve ( tPQ.size() );
	while ( !tPQ.empty() )
	{
		dResults.push_back ( { (uint32_t)tPQ.top().second, (float)tPQ.top().first } );
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
		LoadSettings ( tSettings, tReader, uVersion );
		
		QuantizationSettings_t tQuantSettings;
		if ( tSettings.m_eQuantization!=Quantization_e::NONE )
			LoadQuantizationSettings ( tQuantSettings, tReader, uVersion );

		i = std::make_unique<HNSWIndex_T<int>> ( sName, 0, tSettings, CreateQuantizer ( tSettings.m_eQuantization, tQuantSettings ) );

		if ( !i->Load ( tReader, sError ) )
			return false;
	}

	PopulateHash();

	return !tReader.IsError();
}


Iterator_i * KNN_c::CreateIterator ( const std::string & sName, const Span_T<float> & dData, int iResults, int iEf , std::string & sError )
{
	HNSWIndex_i * pIndex = GetIndex(sName);
	if ( !pIndex )
	{
		sError = FormatStr ( "KNN index not found for attribute '%s'", sName.c_str() );
		return nullptr;
	}

	return knn::CreateIterator ( *pIndex, dData, iResults, iEf );
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

	virtual void	Train ( const util::Span_T<float> & dData ) = 0;
	virtual bool	AddDoc ( const util::Span_T<float> & dData, std::string & sError ) = 0;
	virtual void	Save ( FileWriter_c & tWriter ) = 0;
	virtual const AttrWithSettings_t & GetAttr() const = 0;
	virtual const QuantizationSettings_t & GetQuantizationSettings() const = 0;
};

template <typename T>
class HNSWIndexBuilder_T : public HNSWIndexBuilder_i, public HNSWDist_T<T>
{
	using DIST = HNSWDist_T<T>;

public:
			HNSWIndexBuilder_T ( const AttrWithSettings_t & tAttr, int64_t iNumElements, ScalarQuantizer_i * pQuantizer );

	void	Train ( const util::Span_T<float> & dData ) override;
	bool	AddDoc ( const util::Span_T<float> & dData, std::string & sError ) override;
	void	Save ( FileWriter_c & tWriter ) override						{ m_pAlg->saveIndex(tWriter); }
	const AttrWithSettings_t & GetAttr() const override						{ return m_tAttr; }
	const QuantizationSettings_t & GetQuantizationSettings() const override { return m_pQuantizer->GetSettings(); }

private:
	AttrWithSettings_t			m_tAttr;
	uint32_t					m_tRowID = 0;
	SpanResizeable_T<float>		m_dNormalized;
	std::vector<uint8_t>		m_dQuantized;
	std::unique_ptr<ScalarQuantizer_i>				m_pQuantizer;
	std::unique_ptr<hnswlib::HierarchicalNSW<T>>	m_pAlg;
};

template <typename T>
HNSWIndexBuilder_T<T>::HNSWIndexBuilder_T ( const AttrWithSettings_t & tAttr, int64_t iNumElements, ScalarQuantizer_i * pQuantizer )
	: DIST ( tAttr.m_iDims, tAttr.m_eHNSWSimilarity, tAttr.m_eQuantization )
	, m_tAttr ( tAttr )
	, m_pQuantizer ( pQuantizer )
{
	std::string sError;
	auto pSpaceInterface = DIST::GetSpaceInterface(sError);
	assert(pSpaceInterface);

	m_pAlg = std::make_unique<hnswlib::HierarchicalNSW<T>>( pSpaceInterface, iNumElements, m_tAttr.m_iHNSWM, m_tAttr.m_iHNSWEFConstruction );
	m_dNormalized.resize ( tAttr.m_iDims );
}

template <typename T>
void HNSWIndexBuilder_T<T>::Train ( const util::Span_T<float> & dData )
{
	if ( m_pQuantizer )
		m_pQuantizer->Train(dData);
}

template <typename T>
bool HNSWIndexBuilder_T<T>::AddDoc ( const util::Span_T<float> & dData, std::string & sError )
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

	if ( m_pQuantizer )
	{
		m_pQuantizer->Encode ( dToAdd, m_dQuantized );
		m_pAlg->addPoint ( (void*)m_dQuantized.data(), (size_t)m_tRowID++ );
	}
	else
		m_pAlg->addPoint ( (void*)dToAdd.data(), (size_t)m_tRowID++ );

	return true;
}

/////////////////////////////////////////////////////////////////////

class HNSWBuilder_c : public Builder_i
{
public:
			HNSWBuilder_c ( const Schema_t & tSchema, int64_t iNumElements );

	void	Train ( int iAttr, const util::Span_T<float> & dData ) override			{ m_dIndexes[iAttr]->Train(dData); }
	bool	SetAttr ( int iAttr, const util::Span_T<float> & dData ) override		{ return m_dIndexes[iAttr]->AddDoc ( dData, m_sError ); }
	bool	Save ( const std::string & sFilename, size_t tBufferSize, std::string & sError ) override;
	const std::string & GetError() const override									{ return m_sError; }

private:
	std::vector<std::unique_ptr<HNSWIndexBuilder_i>> m_dIndexes;
	std::string m_sError;
};


HNSWBuilder_c::HNSWBuilder_c ( const Schema_t & tSchema, int64_t iNumElements )
{
	for ( const auto & i : tSchema )
		m_dIndexes.push_back ( std::make_unique<HNSWIndexBuilder_T<int>> ( i, iNumElements, CreateQuantizer(i.m_eQuantization) ) );
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


knn::Builder_i * CreateKNNBuilder ( const knn::Schema_t & tSchema, int64_t iNumElements )
{
	return new knn::HNSWBuilder_c ( tSchema, iNumElements );
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
