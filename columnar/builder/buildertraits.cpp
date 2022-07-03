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

#include "buildertraits.h"

namespace columnar
{

using namespace util;
using namespace common;


AttributeHeaderBuilder_c::AttributeHeaderBuilder_c ( const Settings_t & tSettings, const std::string & sName, AttrType_e eType )
	: m_sName ( sName )
	, m_eType ( eType )
	, m_tSettings ( tSettings )
{}


bool AttributeHeaderBuilder_c::Save ( FileWriter_c & tWriter, int64_t & tBaseOffset, std::string & sError )
{
	m_tSettings.Save(tWriter);

	tWriter.Write_string(m_sName);

	// store base offset to correct it later
	tBaseOffset = tWriter.GetPos();

	tWriter.Write_uint64 ( 0 ); // stub
	tWriter.Pack_uint32 ( (uint32_t)m_dBlocks.size() );
	int64_t tPrevOffset = 0;

	// no offset for 1st block
	for ( size_t i=1; i < m_dBlocks.size(); i++ )
	{
		tWriter.Pack_uint64 ( m_dBlocks[i]-tPrevOffset );
		tPrevOffset = m_dBlocks[i];
	}

	return !tWriter.IsError();
}

} // namespace columnar