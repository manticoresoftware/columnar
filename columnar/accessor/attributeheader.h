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

#include "columnar.h"

namespace util
{
	class FileReader_c;
}

namespace columnar
{

class AttributeHeader_i
{
public:
	virtual						~AttributeHeader_i() = default;

	virtual const std::string &	GetName() const = 0;
	virtual common::AttrType_e	GetType() const = 0;
	virtual const Settings_t &	GetSettings() const = 0;

	virtual uint32_t			GetNumDocs() const = 0;
	virtual int					GetNumBlocks() const = 0;
	virtual uint32_t			GetNumDocs ( int iBlock ) const = 0;
	virtual uint64_t			GetBlockOffset ( int iBlock ) const = 0;

	virtual int					GetNumMinMaxLevels() const = 0;
	virtual int					GetNumMinMaxBlocks ( int iLevel ) const = 0;
	virtual std::pair<int64_t,int64_t> GetMinMax ( int iLevel, int iBlock ) const = 0;

	virtual bool				Load ( util::FileReader_c & tReader, std::string & sError ) = 0;
	virtual bool				Check ( util::FileReader_c & tReader, Reporter_fn & fnError ) = 0;
};


AttributeHeader_i * CreateAttributeHeader ( common::AttrType_e eType, uint32_t uTotalDocs, std::string & sError );

} // namespace columnar
