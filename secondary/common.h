// Copyright (c) 2020-2021, Manticore Software LTD (https://manticoresearch.com)
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

#include <string>
#include <array>

namespace SI
{
	static const int LIB_VERSION = 1;
	static const uint32_t STORAGE_VERSION = 1;

	enum class AttrType_e : uint32_t
	{
		NONE,
		UINT32,
		TIMESTAMP,
		INT64,
		BOOLEAN,
		FLOAT,
		STRING,
		UINT32SET,
		INT64SET
	};

	struct ColumnInfo_t
	{
		AttrType_e	m_eType { AttrType_e::NONE };
		int			m_iSrcAttr { -1 };
		int			m_iAttr { -1 };
		std::string m_sName;
		bool		m_bEnabled { true };
	};

	enum class Packing_e : uint32_t
	{
		ROW,
		ROW_BLOCK,
		ROW_BLOCKS_LIST,
		ROW_FULLSCAN, // FIXME!!! handle large length as full scan row iterator

		TOTAL
	};

	/// known collations
	enum class Collation_e : uint32_t
	{
		LIBC_CI,
		LIBC_CS,
		UTF8_GENERAL_CI,
		BINARY,

		TOTAL
	};


using StrHash_fn = uint64_t (*) ( const uint8_t * pStr, int iLen );

StrHash_fn GetHashFn ( Collation_e eCollation );

} // namespace SI
