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

// This file is a part of the common headers (API).
// If you make any significant changes to this file, you MUST bump the LIB_VERSION in secondary.h

#pragma once

#include "util/util.h"
#include "common/schema.h"

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

} // namespace SI


extern "C"
{
	DLLEXPORT SI::Builder_i * CreateBuilder ( const common::Schema_t & tSchema, size_t tMemoryLimit, const std::string & sFile, size_t tBufferSize, std::string & sError );
}