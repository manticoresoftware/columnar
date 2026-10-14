// Copyright (c) 2026, Manticore Software LTD (https://manticoresearch.com)
// Licensed under the Apache License, Version 2.0.

#include "knn.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

static int Fail ( const char * szMessage, const std::string & sError = {} )
{
	std::fprintf ( stderr, "FAILED: %s%s%s\n", szMessage, sError.empty() ? "" : ": ", sError.c_str() );
	return 1;
}

int main()
{
	const std::string sFile = "knn_multivector_slot_test.spknn";
	const std::string sTmp = sFile + ".tmp";
	std::remove ( sFile.c_str() );
	std::remove ( sTmp.c_str() );

	knn::AttrWithSettings_t tAttr;
	tAttr.m_sName = "vec";
	tAttr.m_bKNN = true;
	tAttr.m_iDims = 1;
	tAttr.m_eHNSWSimilarity = knn::HNSWSimilarity_e::L2;
	tAttr.m_eQuantization = knn::Quantization_e::NONE;
	tAttr.m_iHNSWM = 2;
	tAttr.m_iHNSWEFConstruction = 10;
	tAttr.m_bMulti = true;

	std::unique_ptr<knn::Builder_i> pBuilder { CreateKNNBuilder ( { tAttr }, 2, sTmp ) };
	if ( !pBuilder )
		return Fail ( "CreateKNNBuilder returned null" );

	// Row 0 has an exact tie between slots 0 and 1. Lowest global vector id/slot must win.
	std::vector<float> dRow0 { 1.0f, 1.0f };
	// Row 1's second vector is the winner, proving that the returned slot is not
	// just the default value used for scalar rows.
	std::vector<float> dRow1 { 3.0f, 0.5f };
	pBuilder->Train ( 0, 0, { dRow0.data(), dRow0.size() } );
	pBuilder->Train ( 0, 1, { dRow1.data(), dRow1.size() } );

	std::string sError;
	if ( !pBuilder->FinalizeTraining(sError) )
		return Fail ( "FinalizeTraining", sError );

	knn::BuildContext_t tBuildCtx;
	if ( !pBuilder->SetAttr ( 0, 0, { dRow0.data(), dRow0.size() }, tBuildCtx ) )
		return Fail ( "SetAttr row 0", tBuildCtx.m_sError );
	if ( !pBuilder->SetAttr ( 0, 1, { dRow1.data(), dRow1.size() }, tBuildCtx ) )
		return Fail ( "SetAttr row 1", tBuildCtx.m_sError );
	if ( !pBuilder->Save ( sFile, 1024, sError ) )
		return Fail ( "Save", sError );
	pBuilder.reset();

	std::unique_ptr<knn::KNN_i> pKNN { CreateKNN() };
	if ( !pKNN->Load ( sFile, sError ) )
		return Fail ( "Load", sError );

	float fQuery = 0.0f;
	std::unique_ptr<knn::Iterator_i> pIt { pKNN->CreateIterator ( "vec", { &fQuery, 1 }, 2, 10, nullptr, knn::HNSWTerminationPolicy_e::NONE, false, sError ) };
	if ( !pIt )
		return Fail ( "CreateIterator", sError );

	const auto dHits = pIt->GetData();
	if ( dHits.size()!=2 )
		return Fail ( "grouped search did not return one hit per row" );
	const knn::DocDist_t * pRow0 = nullptr;
	const knn::DocDist_t * pRow1 = nullptr;
	for ( const auto & tHit : dHits )
	{
		if ( tHit.m_tRowID==0 )
			pRow0 = &tHit;
		else if ( tHit.m_tRowID==1 )
			pRow1 = &tHit;
	}
	if ( !pRow0 || pRow0->m_uVectorSlot!=0 )
		return Fail ( "row 0 did not retain deterministic winning slot 0" );
	if ( !pRow1 || pRow1->m_uVectorSlot!=1 )
		return Fail ( "row 1 did not retain winning slot 1" );

	pIt.reset();
	pKNN.reset();
	std::remove ( sFile.c_str() );
	std::remove ( sTmp.c_str() );
	return 0;
}
