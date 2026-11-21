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

#include "builder.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>

#define CHECK(x) do { if (!(x)) { std::fprintf ( stderr, "FAILED line %d: %s\n", __LINE__, #x ); return false; } } while (false)

static constexpr uint32_t BLOCK_ROWS = 65536;
static constexpr uint32_t SUBBLOCK_ROWS = 1024;


static std::string VariableString ( uint32_t uRow )
{
	return std::to_string(uRow) + std::string ( 1+uRow%5, 'x' );
}


static bool WriteRows ( const std::string & sFile )
{
	using common::AttrType_e;
	common::Schema_t tSchema = { { "variable", AttrType_e::STRING }, { "fixed", AttrType_e::STRING }, { "vector", AttrType_e::FLOATVEC } };
	std::string sError;
	std::unique_ptr<columnar::Builder_i> pBuilder ( CreateColumnarBuilder ( tSchema, sFile, 65536, sError ) );
	CHECK(pBuilder);

	for ( uint32_t i=0; i<BLOCK_ROWS+SUBBLOCK_ROWS; ++i )
	{
		auto sVariable = VariableString(i);
		auto sFixed = std::to_string(100000+i);
		int64_t dVector[] = { 0x3f800000+i, 0x40000000+i };

		pBuilder->SetAttr ( 0, (const uint8_t *)sVariable.data(), int(sVariable.size()) );
		pBuilder->SetAttr ( 1, (const uint8_t *)sFixed.data(), int(sFixed.size()) );
		pBuilder->SetAttr ( 2, dVector, 2 );
	}

	CHECK ( pBuilder->Done(sError) );
	return true;
}


static bool CheckRows ( const std::string & sFile, bool bMmap )
{
	std::string sError;
	std::unique_ptr<columnar::Columnar_i> pReader ( CreateColumnarStorageReader ( sFile, BLOCK_ROWS+SUBBLOCK_ROWS, bMmap, sError ) );
	CHECK(pReader);

	columnar::IteratorHints_t tHints;
	std::unique_ptr<columnar::Iterator_i> pVariable ( pReader->CreateIterator ( "variable", tHints, nullptr, sError ) );
	std::unique_ptr<columnar::Iterator_i> pFixed ( pReader->CreateIterator ( "fixed", tHints, nullptr, sError ) );
	tHints.m_bBuffered = false;
	std::unique_ptr<columnar::Iterator_i> pVector ( pReader->CreateIterator ( "vector", tHints, nullptr, sError ) );
	CHECK ( pVariable && pFixed && pVector );

	// Revisit subblock zero in a different block, then jump between subblocks of the first block.
	for ( uint32_t i : { BLOCK_ROWS+1, BLOCK_ROWS+2, 1U, 2U, 2*SUBBLOCK_ROWS+1, SUBBLOCK_ROWS+1, 1U, BLOCK_ROWS+1 } )
	{
		auto sVariable = VariableString(i);
		const uint8_t * pData = nullptr;
		CHECK ( pVariable->GetLength(i)==int(sVariable.size()) );
		CHECK ( pVariable->Get ( i, pData )==int(sVariable.size()) );
		CHECK ( !std::memcmp ( pData, sVariable.data(), sVariable.size() ) );
		CHECK ( pFixed->GetLength(i)==6 );

		uint32_t dExpected[] = { 0x3f800000+i, 0x40000000+i };
		CHECK ( pVector->Get ( i, pData )==sizeof(dExpected) );
		CHECK ( !std::memcmp ( pData, dExpected, sizeof(dExpected) ) );
	}

	return true;
}


int main()
{
	auto tFile = std::filesystem::temp_directory_path() / ( "columnar-read-order-" + std::to_string ( std::chrono::steady_clock::now().time_since_epoch().count() ) + ".spc" );
	auto sFile = tFile.string();
	bool bOk = WriteRows(sFile) && CheckRows ( sFile, false ) && CheckRows ( sFile, true );
	std::filesystem::remove(tFile);
	return bOk ? 0 : 1;
}
