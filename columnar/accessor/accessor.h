// Copyright (c) 2020-2025, Manticore Software LTD (https://manticoresearch.com)
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
#include "builder.h"
#include "attributeheader.h"

namespace columnar
{

// fixme: add bitmaps
class MatchingBlocks_c
{
public:
						MatchingBlocks_c() { m_dBlocks.reserve(1024); }

	FORCE_INLINE void	Add ( int iBlock ) { m_dBlocks.push_back(iBlock); }
	FORCE_INLINE int	GetBlock ( int iBlock ) const { return m_dBlocks[iBlock]; }
	FORCE_INLINE int	GetNumBlocks() const { return (int)m_dBlocks.size(); }
	FORCE_INLINE int	Find ( int iStartBlock, int iValue );

private:
	std::vector<int>	m_dBlocks;
};


int MatchingBlocks_c::Find ( int iStartBlock, int iValue )
{
	auto tFound = std::lower_bound ( m_dBlocks.begin()+iStartBlock, m_dBlocks.end(), iValue );
	if ( tFound==m_dBlocks.end() )
		return (int)m_dBlocks.size();

	return tFound-m_dBlocks.begin();
}


using SharedBlocks_c = std::shared_ptr<MatchingBlocks_c>;

class Analyzer_i : public common::BlockIterator_i
{
public:
	virtual void	Setup ( SharedBlocks_c & pBlocks, uint32_t uTotalDocs ) = 0;
};


class Checker_i
{
public:
	virtual			~Checker_i() = default;

	virtual bool	Check() = 0;
};


bool	CheckEmptySpan ( uint32_t * pRowID, uint32_t * pRowIdStart, util::Span_T<uint32_t> & dRowIdBlock );

} // namespace columnar
