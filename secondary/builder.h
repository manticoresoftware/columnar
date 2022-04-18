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

#include "common.h"

namespace SI
{
class Builder_i
{
public:
	virtual			~Builder_i() = default;

	virtual void	SetRowID ( uint32_t tRowID ) = 0;
	virtual void	SetAttr ( int iAttr, int64_t tAttr ) = 0;
	virtual void	SetAttr ( int iAttr, const uint8_t * pData, int iLength ) = 0;
	virtual void	SetAttr ( int iAttr, const int64_t * pData, int iLength ) = 0;

	virtual bool	Done ( std::string & sError ) = 0;
};

struct SourceAttrTrait_t
{
	AttrType_e	m_eType { AttrType_e::NONE };
	int			m_iAttr { -1 };
	std::string m_sName;
};

} // namespace SI
