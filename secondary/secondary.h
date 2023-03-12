// Copyright (c) 2020-2023, Manticore Software LTD (https://manticoresearch.com)
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
#include "common/schema.h"

namespace util
{
	class FileReader_c;
	class FileWriter_c;
}

namespace common
{
	struct Filter_t;
	struct RowidRange_t;
	class BlockIterator_i;
}

namespace SI
{

static const int LIB_VERSION = 7;
static const uint32_t STORAGE_VERSION = 7;

struct ColumnInfo_t
{
	common::AttrType_e m_eType = common::AttrType_e::NONE;
	std::string m_sName;
	uint32_t	m_uCountDistinct = 0;
	bool		m_bEnabled = true;

	void		Load ( util::FileReader_c & tReader );
	void		Save ( util::FileWriter_c & tWriter ) const; 
};


struct Settings_t
{
	std::string	m_sCompressionUINT32 = "streamvbyte";
	std::string	m_sCompressionUINT64 = "fastpfor128";

	void		Load ( util::FileReader_c & tReader );
	void		Save ( util::FileWriter_c & tWriter ) const;
};


class Index_i
{
public:
	virtual				~Index_i() = default;

	virtual bool		CreateIterators ( std::vector<common::BlockIterator_i *> & dIterators, const common::Filter_t & tFilter, const common::RowidRange_t * pBounds, uint32_t uMaxValues, int64_t iRsetSize, int iCutoff, std::string & sError ) const = 0;
	virtual bool		CalcCount ( uint32_t & uCount, const common::Filter_t & tFilter, std::string & sError ) const = 0;
	virtual uint32_t	GetNumIterators ( const common::Filter_t & tFilter ) const = 0;
	virtual bool		IsEnabled ( const std::string & sName ) const = 0;
	virtual int64_t		GetCountDistinct ( const std::string & sName ) const = 0;
	virtual bool		SaveMeta ( std::string & sError ) = 0;
	virtual void		ColumnUpdated ( const char * sName ) = 0;
};

class Builder_i;

} // namespace SI

extern "C"
{
	DLLEXPORT SI::Index_i *		CreateSecondaryIndex ( const char * sFile, std::string & sError );

	DLLEXPORT int				GetSecondaryLibVersion();
	DLLEXPORT const char *		GetSecondaryLibVersionStr();
	DLLEXPORT int				GetSecondaryStorageVersion();
}