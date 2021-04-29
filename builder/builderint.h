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

#ifndef _builderint_
#define _builderint_

#include "util.h"

namespace columnar
{

enum class IntDeltaPacking_e : uint8_t
{
	DELTA_ASC,
	DELTA_DESC
};


enum class IntPacking_e : uint32_t
{
	CONST,
	TABLE,
	DELTA,
	GENERIC,

	TOTAL
};


class Packer_i;
struct Settings_t;

Packer_i * CreatePackerUint32 ( const Settings_t & tSettings, const std::string & sName );
Packer_i * CreatePackerUint64 ( const Settings_t & tSettings, const std::string & sName );
Packer_i * CreatePackerFloat ( const Settings_t & tSettings, const std::string & sName );

} // namespace columnar

#endif // _builderint_