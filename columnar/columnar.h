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

#include "util/util.h"
#include "common/blockiterator.h"
#include "common/filter.h"
#include "common/schema.h"
#include <functional>

namespace util
{
	class FileReader_c;
}

namespace columnar
{

static const int LIB_VERSION = 16;

class Iterator_i
{
public:
	virtual				~Iterator_i() = default;

	virtual	uint32_t	AdvanceTo ( uint32_t tRowID ) = 0;

	virtual	int64_t		Get() = 0;

	virtual	void		Fetch ( const util::Span_T<uint32_t> & dRowIDs, util::Span_T<int64_t> & dValues ) = 0;

	virtual	int			Get ( const uint8_t * & pData ) = 0;
	virtual	uint8_t *	GetPacked() = 0;
	virtual	int			GetLength() = 0;

	virtual void		AddDesc ( std::vector<common::IteratorDesc_t> & dDesc ) const = 0;
};


using MinMaxVec_t = std::vector<std::pair<int64_t,int64_t>>;

class BlockTester_i
{
public:
	virtual				~BlockTester_i() = default;

	virtual bool		Test ( const MinMaxVec_t & dMinMax ) const = 0;
};


struct IteratorHints_t
{
	bool	m_bNeedStringHashes = false;
};


struct IteratorCapabilities_t
{
	bool	m_bStringHashes = false;
};

using Reporter_fn = std::function<void (const char*)>;

struct Settings_t
{
	int			m_iSubblockSize = 1024;
	std::string	m_sCompressionUINT32 = "streamvbyte";
	std::string	m_sCompressionUINT64 = "fastpfor128";

	void		Load ( util::FileReader_c & tReader );
	void		Save ( util::FileWriter_c & tWriter );
	bool		Check ( util::FileReader_c & tReader, Reporter_fn & fnError );
};


class Columnar_i
{
public:
	virtual					~Columnar_i() = default;

	virtual Iterator_i *	CreateIterator ( const std::string & sName, const IteratorHints_t & tHints, columnar::IteratorCapabilities_t * pCapabilities, std::string & sError ) const = 0;
	virtual std::vector<common::BlockIterator_i *> CreateAnalyzerOrPrefilter ( const std::vector<common::Filter_t> & dFilters, std::vector<int> & dDeletedFilters, const BlockTester_i & tBlockTester ) const = 0;
	virtual int				GetAttributeId ( const std::string & sName ) const = 0;
	virtual common::AttrType_e GetType ( const std::string & sName ) const = 0;

	virtual bool			EarlyReject ( const std::vector<common::Filter_t> & dFilters, const BlockTester_i & tBlockTester ) const = 0;
	virtual bool			IsFilterDegenerate ( const common::Filter_t & tFilter ) const = 0;
};

} // namespace columnar


extern "C"
{
	DLLEXPORT columnar::Columnar_i *	CreateColumnarStorageReader ( const std::string & sFilename, uint32_t uTotalDocs, std::string & sError );
	DLLEXPORT void						CheckColumnarStorage ( const std::string & sFilename, uint32_t uNumRows, columnar::Reporter_fn & fnError, columnar::Reporter_fn & fnProgress );
	DLLEXPORT int						GetColumnarLibVersion();
	DLLEXPORT const char *				GetColumnarLibVersionStr();
	DLLEXPORT int						GetColumnarStorageVersion();
}
