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

#include "util/util.h"
#include "common.h"
#include "builder.h"
#include "iterator.h"

extern "C"
{
	DLLEXPORT SI::Index_i *		CreateSecondaryIndex ( const char * sFile, std::string & sError );
	DLLEXPORT SI::Builder_i *	CreateBuilder ( const std::vector<SI::SourceAttrTrait_t> & dSrcAttrs, int iMemoryLimit, SI::Collation_e eCollation, const char * sFile, std::string & sError );

	DLLEXPORT void CollationInit ( const std::array<SI::StrHash_fn, (size_t)SI::Collation_e::TOTAL> & dCollations );

	DLLEXPORT int				GetSecondaryLibVersion();
	DLLEXPORT const char *		GetSecondaryLibVersionStr();
	DLLEXPORT int				GetSecondaryStorageVersion();
}
