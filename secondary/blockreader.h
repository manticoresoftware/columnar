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

#pragma once

#include "util/codec.h"
#include "common/filter.h"
#include "common/blockiterator.h"
#include "builder.h"
#include <memory>

namespace util
{
	class FileReader_c;
	class FileWriter_c;
}

namespace SI
{

class BlockIteratorSize_i;
struct ApproxPos_t;

struct BlockIter_t
{
	uint64_t m_uVal { 0 };

	uint64_t m_iPos { 0 };
	uint64_t m_iStart { 0 };
	uint64_t m_iLast { 0 };

	BlockIter_t() = default;
	BlockIter_t ( const ApproxPos_t & tFrom, uint64_t uVal, uint64_t uBlocksCount, uint32_t uValuesPerBlock );
};


class BlockReader_i
{
public:
	virtual				~BlockReader_i() = default;

	virtual void		CreateBlocksIterator ( const std::vector<BlockIter_t> & dIt, const common::Filter_t & tFilter, std::vector<common::BlockIterator_i *> & dRes ) = 0;
	virtual void		CreateBlocksIterator ( const BlockIter_t & tIt, const common::Filter_t & tFilter, std::vector<common::BlockIterator_i *> & dRes ) = 0;
	virtual uint32_t	CalcValueCount ( const std::vector<BlockIter_t> & dIt ) = 0;
	virtual uint32_t	CalcValueCount ( const BlockIter_t & tIt, const common::Filter_t & tVal ) = 0;
};

struct RsetInfo_t
{
	int64_t		m_iNumIterators = 0;
	uint32_t	m_uRowsCount = 0;
	int64_t		m_iRsetSize = 0;
};


enum class Packing_e : uint32_t
{
	ROW,
	ROW_BLOCK,
	ROW_BLOCKS_LIST,

	TOTAL
};


struct ColumnInfo_t
{
	common::AttrType_e m_eType = common::AttrType_e::NONE;
	std::string m_sName;
	std::string m_sJsonParentName;
	uint32_t	m_uCountDistinct = 0;
	bool		m_bEnabled = true;

	void		Load ( util::FileReader_c & tReader );
	void		Save ( util::FileWriter_c & tWriter ) const; 
};


struct Settings_t
{
	std::string	m_sCompressionUINT32 = "libstreamvbyte";
	std::string	m_sCompressionUINT64 = "fastpfor256";

	void		Load ( util::FileReader_c & tReader, uint32_t uVersion );
	void		Save ( util::FileWriter_c & tWriter ) const;
};


class ReaderFactory_c
{
public:
	ColumnInfo_t 			m_tCol;
	Settings_t 				m_tSettings;
	RsetInfo_t 				m_tRsetInfo;
	int						m_iFD = -1;
	uint32_t				m_uVersion = 0;
	uint64_t				m_uBlockBaseOff = 0;
	uint64_t				m_uBlocksCount = 0;
	uint32_t				m_uValuesPerBlock = 1;
	uint32_t				m_uRowidsPerBlock = 1;
	const common::RowidRange_t * m_pBounds = nullptr;
	int						m_iCutoff = 0;

	BlockReader_i *			CreateBlockReader();
	BlockReader_i *			CreateRangeReader();
};

} // namespace SI
