// Copyright (c) 2021-2022, Manticore Software LTD (https://manticoresearch.com)
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

#include "accessortraits.h"

namespace columnar
{

class Checker_c : public Checker_i
{
public:
					Checker_c ( const AttributeHeader_i & tHeader, util::FileReader_c * pReader, Reporter_fn & fnProgress, Reporter_fn & fnError );

	bool			Check() override;

protected:
	const AttributeHeader_i &		m_tHeader;
	std::unique_ptr<util::FileReader_c>	m_pReader;
	Reporter_fn	&					m_fnProgress;
	Reporter_fn	&					m_fnError;
	uint32_t						m_uBlockId = INVALID_BLOCK_ID;
	uint32_t						m_uChecked = 0;

	virtual bool	CheckBlockHeader ( uint32_t uBlockId ) = 0;
};


void	CheckStorage ( const std::string & sFilename, uint32_t uNumRows, Reporter_fn & fnError, Reporter_fn & fnProgress );

bool	CheckString ( util::FileReader_c & tReader, int iMinLength, int iMaxLength, const std::string & sMessage, Reporter_fn & fnError );
bool	CheckInt32 ( util::FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, Reporter_fn & fnError );
bool	CheckInt32 ( util::FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, int & iValue, Reporter_fn & fnError );
bool	CheckInt32Packed ( util::FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, Reporter_fn & fnError );
bool	CheckInt32Packed ( util::FileReader_c & tReader, int iMin, int iMax, const std::string & sMessage, int & iValue, Reporter_fn & fnError );
bool	CheckInt64 ( util::FileReader_c & tReader, int64_t iMin, int64_t iMax, const std::string & sMessage, Reporter_fn & fnError );
bool	CheckInt64 ( util::FileReader_c & tReader, int64_t iMin, int64_t iMax, const std::string & sMessage, int64_t & iValue, Reporter_fn & fnError );

} // namespace columnar
