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

#include "common.h"
#include "util/codec.h"
#include "common/filter.h"
#include "common/blockiterator.h"
#include <memory>

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
	BlockIter_t ( const ApproxPos_t & tFrom, uint64_t uVal, uint64_t uBlocksCount, int iValuesPerBlock );
};


class BlockReader_i
{
public:
	virtual			~BlockReader_i() = default;

	virtual void	CreateBlocksIterator ( const BlockIter_t & tIt, std::vector<common::BlockIterator_i *> & dRes ) = 0;
	virtual void	CreateBlocksIterator ( const BlockIter_t & tIt, const common::Filter_t & tVal, std::vector<common::BlockIterator_i *> & dRes ) = 0;
};


struct Settings_t;
BlockReader_i * CreateBlockReader ( int iFD, const ColumnInfo_t & tCol, const Settings_t & tSettings, uint64_t uBlockBaseOff, const common::RowidRange_t * pBounds );
BlockReader_i * CreateRangeReader ( int iFD, const ColumnInfo_t & tCol, const Settings_t & tSettings, uint64_t uBlockBaseOff, const common::RowidRange_t * pBounds );

} // namespace SI
