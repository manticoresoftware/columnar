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

// This file is a part of the common headers (API).
// If you make any significant changes to this file, you MUST bump the LIB_VERSION.

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

static const int LIB_VERSION = 13;
static const uint32_t STORAGE_VERSION = 8;

class Index_i
{
public:
	virtual				~Index_i() = default;

	virtual bool		CreateIterators ( std::vector<common::BlockIterator_i *> & dIterators, const common::Filter_t & tFilter, const common::RowidRange_t * pBounds, uint32_t uMaxValues, int64_t iRsetSize, int iCutoff, std::string & sError ) const = 0;
	virtual bool		CalcCount ( uint32_t & uCount, const common::Filter_t & tFilter, uint32_t uMaxValues, std::string & sError ) const = 0;
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
}
