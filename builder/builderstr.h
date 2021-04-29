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

#ifndef _builderstr_
#define _builderstr_

#include "builder.h"

namespace columnar
{

enum class StrPacking_e : uint32_t
{
	CONST = 0,
	CONSTLEN,
	TABLE,
	GENERIC,

	TOTAL
};

const uint64_t STR_HASH_SEED = 0xCBF29CE484222325ULL;

class Packer_i;
Packer_i * CreatePackerStr ( const Settings_t & tSettings, const std::string & sName, StringHash_fn fnHashCalc );

} // namespace columnar

#endif // _builderstr_