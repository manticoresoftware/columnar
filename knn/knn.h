// Copyright (c) 2020-2024, Manticore Software LTD (https://manticoresearch.com)
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

// This file is a part of the common headers (API).
// If you make any significant changes to this file, you MUST bump the LIB_VERSION.

#pragma once

#include "util/util.h"
#include "common/schema.h"
#include "common/blockiterator.h"

namespace knn
{

static const int LIB_VERSION = 4;
static const uint32_t STORAGE_VERSION = 2;

enum class HNSWSimilarity_e
{
	L2,
	IP,
	COSINE
};

enum class Quantization_e
{
	NONE,
	BIT1,
	BIT4,
	BIT8
};

struct IndexSettings_t
{
	int					m_iDims = 0;
	HNSWSimilarity_e	m_eHNSWSimilarity = HNSWSimilarity_e::L2;
	Quantization_e		m_eQuantization = Quantization_e::NONE;
	int					m_iHNSWM = 16;
	int					m_iHNSWEFConstruction = 200;
};

struct AttrWithSettings_t : public common::SchemaAttr_t, public IndexSettings_t {};
using Schema_t = std::vector<AttrWithSettings_t>;

struct DocDist_t
{
	uint32_t	m_tRowID;
	float		m_fDist;
};

class Distance_i
{
public:
	virtual			~Distance_i() = default;

	virtual	float	CalcDist ( const util::Span_T<float> & dPoint1, const util::Span_T<float> & dPoint2 ) const = 0;
};

class Iterator_i : public common::BlockIterator_i
{
public:
	virtual util::Span_T<const DocDist_t> GetData() const = 0;
};

class KNN_i
{
public:
	virtual			~KNN_i() = default;

	virtual bool	Load ( const std::string & sFilename, std::string & sError ) = 0;
	virtual Iterator_i * CreateIterator ( const std::string & sName, const util::Span_T<float> & dData, int iResults, int iEf, std::string & sError ) = 0;
};

class Builder_i
{
public:
	virtual			~Builder_i() = default;

	virtual void	Train ( int iAttr, const util::Span_T<float> & dData ) = 0;
	virtual bool	SetAttr ( int iAttr, const util::Span_T<float> & dData ) = 0;
	virtual bool	Save ( const std::string & sFilename, size_t tBufferSize, std::string & sError ) = 0;
	virtual const std::string & GetError() const = 0;
};

} // namespace knn


extern "C"
{
	DLLEXPORT knn::Distance_i *			CreateDistanceCalc ( const knn::IndexSettings_t & tSettings );
	DLLEXPORT knn::KNN_i *				CreateKNN();
	DLLEXPORT knn::Builder_i *			CreateKNNBuilder ( const knn::Schema_t & tSchema, int64_t iNumElements );
	DLLEXPORT int						GetKNNLibVersion();
	DLLEXPORT const char *				GetKNNLibVersionStr();
}
