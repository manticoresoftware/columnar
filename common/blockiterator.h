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

namespace common
{

struct IteratorDesc_t
{
	std::string m_sAttr;
	std::string m_sType;
};


class BlockIterator_i
{
public:
	virtual				~BlockIterator_i() = default;

	virtual bool		HintRowID ( uint32_t tRowID ) = 0;
	virtual bool		GetNextRowIdBlock ( util::Span_T<uint32_t> & dRowIdBlock ) = 0;
	virtual int64_t		GetNumProcessed() const = 0;

	virtual void		AddDesc ( std::vector<IteratorDesc_t> & dDesc ) const = 0;
};


} // namespace common
