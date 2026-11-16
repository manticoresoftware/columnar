// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
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

#include "columnar.h"
#include "builder.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf ( stderr, "FAILED line %d: %s\n", __LINE__, #x ); return false; } } while (false)

static bool TestRange ( common::AttrType_e eType, int64_t iMin, int64_t iMax, const std::string & sFile )
{
	common::Schema_t tSchema = { { "value", eType } };
	std::string sError;
	std::unique_ptr<columnar::Builder_i> pBuilder ( CreateColumnarBuilder ( tSchema, sFile, 65536, sError ) );
	CHECK(pBuilder);
	// Multiple leaves and a partial final leaf; extremes must come from the root.
	const int iDocs = 2051;
	for ( int i=0; i<iDocs; ++i )
		pBuilder->SetAttr ( 0, i==iDocs-1 ? iMax : iMin );

	CHECK ( pBuilder->Done(sError) );
	pBuilder.reset();
	std::unique_ptr<columnar::Columnar_i> pReader ( CreateColumnarStorageReader ( sFile, iDocs, false, sError ) );
	CHECK(pReader);
	common::Filter_t tFilter;
	tFilter.m_sName = "value";
	tFilter.m_eType = common::FilterType_e::RANGE;
	tFilter.m_iMinValue = iMin;
	tFilter.m_iMaxValue = iMax;
	CHECK ( pReader->IsFilterDegenerate(tFilter) );
	tFilter.m_bLeftClosed = false;
	CHECK ( !pReader->IsFilterDegenerate(tFilter) );
	tFilter.m_bLeftUnbounded = true;
	CHECK ( pReader->IsFilterDegenerate(tFilter) );
	tFilter.m_bRightClosed = false;
	CHECK ( !pReader->IsFilterDegenerate(tFilter) );
	tFilter.m_bLeftClosed = true;
	tFilter.m_bLeftUnbounded = false;
	tFilter.m_bRightUnbounded = true;
	CHECK ( pReader->IsFilterDegenerate(tFilter) );
	tFilter.m_bExclude = true;
	CHECK ( !pReader->IsFilterDegenerate(tFilter) );
	tFilter.m_bExclude = false;
	tFilter.m_bRightUnbounded = false;
	tFilter.m_bRightClosed = true;
	if ( iMax>iMin )
	{
		tFilter.m_iMaxValue = iMax-1;
		CHECK ( !pReader->IsFilterDegenerate(tFilter) );
	}

	tFilter.m_eType = common::FilterType_e::VALUES;
	tFilter.m_dValues = {0,1};
	CHECK ( pReader->IsFilterDegenerate(tFilter)==(eType==common::AttrType_e::BOOLEAN) );
	tFilter.m_bExclude = true;
	CHECK ( !pReader->IsFilterDegenerate(tFilter) );
	tFilter.m_sName = "missing";
	CHECK ( !pReader->IsFilterDegenerate(tFilter) );
	return true;
}

static bool TestUnsupported ( const std::string & sFile )
{
	common::Schema_t tSchema = { { "str", common::AttrType_e::STRING }, { "mva", common::AttrType_e::INT64SET },
		{ "flt", common::AttrType_e::FLOAT } };
	std::string sError;
	std::unique_ptr<columnar::Builder_i> pBuilder ( CreateColumnarBuilder ( tSchema, sFile, 65536, sError ) );
	CHECK(pBuilder);
	const int64_t dValues[] = { 10, 20 };
	pBuilder->SetAttr ( 0, (const uint8_t*)"text", 4 );
	pBuilder->SetAttr ( 1, dValues, 2 );
	pBuilder->SetAttr ( 2, int64_t(0) );
	CHECK ( pBuilder->Done(sError) );
	pBuilder.reset();
	std::unique_ptr<columnar::Columnar_i> pReader ( CreateColumnarStorageReader ( sFile, 1, false, sError ) );
	CHECK(pReader);
	common::Filter_t tFilter;
	tFilter.m_eType = common::FilterType_e::RANGE;
	tFilter.m_iMinValue = INT64_MIN;
	tFilter.m_iMaxValue = INT64_MAX;
	for ( const auto & tAttr : tSchema )
	{
		tFilter.m_sName = tAttr.m_sName;
		CHECK ( !pReader->IsFilterDegenerate(tFilter) );
	}

	return true;
}

int main()
{
	auto tFile = std::filesystem::temp_directory_path() / ( "columnar-degenerate-" + std::to_string ( std::chrono::steady_clock::now().time_since_epoch().count() ) + ".spc" );
	const auto sFile = tFile.string();
	bool bOk = TestRange ( common::AttrType_e::UINT32, 0, UINT32_MAX, sFile )
		&& TestRange ( common::AttrType_e::TIMESTAMP, 10, 30, sFile )
		&& TestRange ( common::AttrType_e::INT64, INT64_MIN, INT64_MAX, sFile )
		&& TestRange ( common::AttrType_e::INT64, -7, -7, sFile )
		&& TestRange ( common::AttrType_e::BOOLEAN, 0, 1, sFile )
		&& TestUnsupported(sFile);
	std::filesystem::remove(tFile);
	return bOk ? 0 : 1;
}
