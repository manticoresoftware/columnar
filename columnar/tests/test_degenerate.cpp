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
	auto * pBounds = pReader->GetBlockBounds("value");
	CHECK(pBounds);
	int iRows = pBounds->GetLeafRows();
	CHECK ( iRows>1 && iRows<iDocs && iDocs%iRows>1 );
	for ( int iLevel=pBounds->GetNumMinMaxLevels()-1; iLevel>=0; --iLevel, iRows*=2 )
	{
		int iBlocks = (iDocs+iRows-1)/iRows;
		CHECK ( pBounds->GetNumMinMaxBlocks(iLevel)==iBlocks );
		for ( int i=0; i<iBlocks; ++i )
			CHECK ( pBounds->GetMinMax ( iLevel, i )==std::make_pair ( iMin, i+1==iBlocks ? iMax : iMin ) );
	}
	CHECK ( !pReader->GetBlockBounds("missing") );

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


static bool TestSignedTableRanges ( const std::string & sFile )
{
	struct AllBlocks_t : columnar::BlockTester_i
	{
		bool Test ( const columnar::MinMaxVec_t & ) const final { return true; }
	} tAllBlocks;

	common::Schema_t tSchema = { { "value", common::AttrType_e::INT64 } };
	std::string sError;
	std::unique_ptr<columnar::Builder_i> pBuilder ( CreateColumnarBuilder ( tSchema, sFile, 65536, sError ) );
	CHECK(pBuilder);

	// Four distinct values select TABLE packing.
	const int64_t dValues[] = { INT64_MIN, -1, 0, INT64_MAX };
	constexpr uint32_t NUM_DOCS = 512;
	for ( uint32_t i=0; i<NUM_DOCS; ++i )
		pBuilder->SetAttr ( 0, dValues[i%4] );

	CHECK ( pBuilder->Done(sError) );
	pBuilder.reset();

	common::Filter_t tFilter;
	tFilter.m_sName = "value";
	tFilter.m_eType = common::FilterType_e::RANGE;
	tFilter.m_iMinValue = -1;
	tFilter.m_iMaxValue = 0;
	for ( bool bMmap : { false, true } )
	{
		std::unique_ptr<columnar::Columnar_i> pReader ( CreateColumnarStorageReader ( sFile, NUM_DOCS, bMmap, sError ) );
		CHECK(pReader);
		for ( bool bLeftClosed : { true, false } )
			for ( bool bRightClosed : { true, false } )
			{
				tFilter.m_bLeftClosed = bLeftClosed;
				tFilter.m_bRightClosed = bRightClosed;
				std::vector<int> dDeletedFilters;
				auto dAnalyzers = pReader->CreateAnalyzerOrPrefilter ( { tFilter }, dDeletedFilters, tAllBlocks );
				CHECK ( dAnalyzers.size()==1 );

				std::unique_ptr<common::BlockIterator_i> pAnalyzer ( dAnalyzers.front() );
				std::vector<uint32_t> dExpected, dActual;
				for ( uint32_t i=0; i<NUM_DOCS; ++i )
					if ( ( i%4==1 && bLeftClosed ) || ( i%4==2 && bRightClosed ) )
						dExpected.push_back(i);

				util::Span_T<uint32_t> dRows;
				while ( pAnalyzer->GetNextRowIdBlock(dRows) )
					dActual.insert ( dActual.end(), dRows.begin(), dRows.end() );

				CHECK ( dActual==dExpected );
			}
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
		&& TestUnsupported(sFile)
		&& TestSignedTableRanges(sFile);
	std::filesystem::remove(tFile);
	return bOk ? 0 : 1;
}
