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

#include <vector>
#include <string>

namespace common
{

enum class AttrType_e : uint32_t
{
	NONE,
	UINT32,
	TIMESTAMP,
	INT64,
	UINT64,
	BOOLEAN,
	FLOAT,
	STRING,
	UINT32SET,
	INT64SET,

	TOTAL
};

using StringHash_fn = uint64_t (*)( const uint8_t * pStr, int iLen, uint64_t uPrev );

struct SchemaAttr_t
{
	std::string		m_sName;
	AttrType_e		m_eType = AttrType_e::NONE;
	StringHash_fn	m_fnCalcHash = nullptr;
};

using Schema_t = std::vector<SchemaAttr_t>;

} // namespace common
