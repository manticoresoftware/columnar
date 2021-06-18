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

#include "builder.h"

namespace columnar
{

class Iterator_i;
class Analyzer_i;
class AttributeHeader_i;
struct Filter_t;

Iterator_i * CreateIteratorUint32 ( const AttributeHeader_i & tHeader, FileReader_c * pReader );
Iterator_i * CreateIteratorUint64 ( const AttributeHeader_i & tHeader, FileReader_c * pReader );
Analyzer_i * CreateAnalyzerInt ( const AttributeHeader_i & tHeader, FileReader_c * pReader, const Filter_t & tSettings );

} // namespace columnar
