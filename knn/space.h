// Copyright (c) 2025, Manticore Software LTD (https://manticoresearch.com)
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

#include "hnswlib.h"

namespace knn
{

class L2Space_c : public hnswlib::SpaceInterface<int>
{
	using Dist_fn = hnswlib::DISTFUNC<int>;

public:
			L2Space_c ( size_t uDim );

	Dist_fn	get_dist_func()	override		{ return m_fnDist; }
	void *	get_dist_func_param() override	{ return &m_uDim; }

protected:
	Dist_fn	m_fnDist = nullptr;
	size_t	m_uDim = 0;
};


class L2Space1Bit_c : public L2Space_c
{
 public:
			L2Space1Bit_c ( size_t uDim );

	size_t	get_data_size() override		{ return (m_uDim+7)>>3; }
};

class L2Space4Bit_c : public L2Space_c
{
 public:
			L2Space4Bit_c ( size_t uDim );

	size_t	get_data_size() override		{ return (m_uDim+1)>>1; }
};

class L2Space8Bit_c : public L2Space_c
{
 public:
			L2Space8Bit_c ( size_t uDim );
		
	size_t	get_data_size() override		{ return m_uDim; }
};


} // namespace knn
