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

#include "util_private.h"

namespace util
{

class IntCodec_i
{
public:
	virtual			~IntCodec_i() = default;

	virtual void	Encode ( const util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) = 0;
	virtual void	EncodeDelta ( util::Span_T<uint32_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) = 0;

	virtual void	Encode ( const util::Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) = 0;
	virtual void	EncodeDelta ( util::Span_T<uint64_t> & dUncompressed, std::vector<uint32_t> & dCompressed ) = 0;

	virtual void	Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed ) = 0;
	virtual void	DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint32_t> & dDecompressed ) = 0;

	virtual void	Decode ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint64_t> & dDecompressed ) = 0;
	virtual void	DecodeDelta ( const util::Span_T<uint32_t> & dCompressed, util::SpanResizeable_T<uint64_t> & dDecompressed ) = 0;
};


void BitPack ( const std::vector<uint32_t> & dValues, std::vector<uint32_t> & dPacked, int iBits );
void BitUnpack ( const std::vector<uint32_t> & dPacked, std::vector<uint32_t> & dValues, int iBits );
void BitUnpack ( const util::Span_T<uint32_t> & dPacked, util::Span_T<uint32_t> & dValues, int iBits );

IntCodec_i * CreateIntCodec ( const std::string & sCodec32, const std::string & sCodec64 );

} // namespace util
