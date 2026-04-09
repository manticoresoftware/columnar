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
#include "quantizer.h"
#include <functional>

namespace knn
{

struct QuantizationSettings_t;

enum class DistFuncId_e
{
	NONE,
	IP_FLOAT32,
	IP_BINARY_GENERIC,
	IP_BINARY_SIMD16,
	IP_BINARY_SIMD16_RESIDUALS,
	L2_FLOAT32,
	L2_BINARY_GENERIC,
	L2_BINARY_SIMD16,
	L2_BINARY_SIMD16_RESIDUALS
};

class Space_i : public hnswlib::SpaceInterface<float>
{
public:
	virtual void SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer ) {}
	virtual DistFuncId_e GetDistFuncId() const { return DistFuncId_e::NONE; }
};

class Space_c : public Space_i
{
	using Dist_fn = hnswlib::DISTFUNC<float>;

public:
			Space_c ( size_t uDim ) : m_uDim ( uDim ) {}

	Dist_fn	get_dist_func()	override		{ return m_fnDist; }
	void *	get_dist_func_param() override	{ return &m_uDim; }
	DistFuncId_e GetDistFuncId() const override { return m_eDistFuncId; }

protected:
	Dist_fn	m_fnDist = nullptr;
	size_t	m_uDim = 0;
	DistFuncId_e m_eDistFuncId = DistFuncId_e::NONE;
};

///////////////////////////////////////////////////////////////////////////////

class L2Space32BitFloat_c : public Space_c
{
public:
			L2Space32BitFloat_c ( size_t uDim );

	size_t	get_data_size() override		{ return m_uDim*sizeof(float); }
};


struct DistFuncParamL2_t
{
	size_t	m_uDim;
	float	m_fA;
};


class L2Space8BitFloat_c : public Space_c
{
 public:
			L2Space8BitFloat_c ( size_t uDim );

	void *	get_dist_func_param() override	{ return &m_tDistFuncParam; }
	size_t	get_data_size() override		{ return m_uDim; }

	void	SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer ) override;

protected:
	DistFuncParamL2_t m_tDistFuncParam;

	virtual float CalcAlpha ( const QuantizationSettings_t & tSettings ) const	{ return ( tSettings.m_fMax-tSettings.m_fMin ) / 255.0; }
};

class L2Space1BitFloat_c : public L2Space8BitFloat_c
{
 public:
			L2Space1BitFloat_c ( size_t uDim );

	size_t	get_data_size() override		{ return (m_uDim+7)>>3; }

protected:
	float	CalcAlpha ( const QuantizationSettings_t & tSettings ) const override { return tSettings.m_fMax-tSettings.m_fMin; }
};

///////////////////////////////////////////////////////////////////////////////

class IPSpace32BitFloat_c : public Space_c
{
public:
			IPSpace32BitFloat_c ( size_t uDim );

	size_t	get_data_size() override		{ return m_uDim*sizeof(float); }
};


struct DistFuncParamIP_t
{
	size_t	m_uDim;
	float	m_fK;
	float	m_fB;

	FORCE_INLINE float CalcIP ( int iDotProduct ) const;
};

class IPSpace8BitFloat_c : public Space_c
{
 public:
			IPSpace8BitFloat_c ( size_t uDim );

	void *	get_dist_func_param() override	{ return &m_tDistFuncParam; }
	size_t	get_data_size() override		{ return m_uDim + sizeof(float); }

	void	SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer ) override;

private:
	DistFuncParamIP_t m_tDistFuncParam;
};


class IPSpace1BitFloat_c : public IPSpace8BitFloat_c
{
 public:
			IPSpace1BitFloat_c ( size_t uDim );

	size_t	get_data_size() override		{ return (m_uDim+7)>>3; }
};


struct DistFuncParamBinary_t
{
	size_t		m_uDim = 0;
	std::function<const uint8_t *(uint32_t)> m_fnFetcher;
	float		m_fCentroidDotCentroid = 0.0f;
	float		m_fSqrtDim = 0.0f;
	float		m_fInvSqrtDim = 0.0f;
	float		m_fDoubleInvSqrtDim = 0.0f;
	float		m_fMaxError = 0.0f;

	DistFuncParamBinary_t ( size_t uDim )
	{
		m_uDim = uDim;
		m_fSqrtDim = sqrt(uDim);
		m_fInvSqrtDim = 1.0f / m_fSqrtDim;
		m_fDoubleInvSqrtDim = 2.0f * m_fInvSqrtDim;

		int iDimPadded = CalcPadding ( m_uDim, 64 );
		m_fMaxError = (float) ( 1.9f / sqrt ( float(iDimPadded) - 1.0f ) );
	}

	static int CalcPadding ( int iValue, int iPad )
	{
		return ( ( iValue + iPad - 1 ) / iPad ) * iPad;
	}
};

float	IPFloatDistance ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
void	IPFloatDistanceBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB );
float	IPBinaryFloatDistanceGeneric ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
float	IPBinaryFloatDistanceSIMD16 ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
float	IPBinaryFloatDistanceSIMD16Residuals ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
void	IPBinaryFloatDistanceSIMD16Batch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB );
void	IPBinaryFloatDistanceSIMD16ResidualsBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB );
float	L2FloatDistance ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
void	L2FloatDistanceBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB );
float	L2BinaryFloatDistanceGeneric ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
float	L2BinaryFloatDistanceSIMD16 ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
float	L2BinaryFloatDistanceSIMD16Residuals ( const void * pVect1, const void * pVect2, size_t uRowID1, size_t uRowID2, const void * pParam );
void	L2BinaryFloatDistanceSIMD16Batch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB );
void	L2BinaryFloatDistanceSIMD16ResidualsBatch2 ( const void * pVect1, const void * pVect2A, const void * pVect2B, size_t uRowID1, size_t uRowID2A, size_t uRowID2B, const void * pParam, float & fDistA, float & fDistB );


class IPSpaceBinaryFloat_c : public Space_c
{
 public:
			IPSpaceBinaryFloat_c ( size_t uDim, bool bBuild );

	void *	get_dist_func_param() override	{ return &m_tDistFuncParam; }
	size_t	get_data_size() override		{ return ( (m_uDim+7)>>3 ) + sizeof(float)*4; }

	void	SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer ) override;

private:
	DistFuncParamBinary_t m_tDistFuncParam;
};

class L2SpaceBinaryFloat_c : public Space_c
{
 public:
			L2SpaceBinaryFloat_c ( size_t uDim, bool bBuild );

	void *	get_dist_func_param() override	{ return &m_tDistFuncParam; }
	size_t	get_data_size() override		{ return ( (m_uDim+7)>>3 ) + sizeof(float)*3; }

	void	SetQuantizationSettings ( ScalarQuantizer_i & tQuantizer ) override;

private:
	DistFuncParamBinary_t m_tDistFuncParam;
};



} // namespace knn
