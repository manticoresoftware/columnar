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

// Regression test for the post-build HNSW connectivity repair pass.
//
// HNSW construction can leave a node with no incoming level-0 edge. The row stays in the index and
// is still returned by a scan, but no knn() query can reach it. The repair pass in graphrepair.cpp
// relinks such nodes before the index is written.
//

#include "knn.h"
#include "graphrepair.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int g_iFailures = 0;

#define CHECK( _expr, ... ) \
	do { if (!(_expr)) { printf ( "FAILED %s:%d: %s\n         ", __FILE__, __LINE__, #_expr ); printf ( __VA_ARGS__ ); printf ( "\n" ); g_iFailures++; } } while(0)


// Deterministic fixture. Tight clusters plus a few far outliers, because outliers are what the
// construction heuristic gives 1-2 out-links to, and those are the nodes that end up stranded
class LCG_c
{
public:
	explicit LCG_c ( uint64_t uSeed ) : m_uState ( uSeed ) {}

	uint64_t Next()
	{
		m_uState = m_uState*6364136223846793005ULL + 1442695040888963407ULL;
		return m_uState;
	}

	double Unit()	{ return double ( Next()>>11 ) * ( 1.0/9007199254740992.0 ); }

	double Gauss()
	{
		double fRes = 0.0;
		for ( int i=0; i<12; i++ )
			fRes += Unit();

		return fRes-6.0;
	}

private:
	uint64_t m_uState;
};


static std::vector<float> GenerateFixture ( int iRows, int iDims, int iClusters, int iOutlierPct, uint64_t uSeed )
{
	LCG_c tRng(uSeed);

	std::vector<std::vector<double>> dCentroids ( iClusters, std::vector<double>(iDims) );
	for ( auto & dCentroid : dCentroids )
		for ( auto & fVal : dCentroid )
			fVal = tRng.Gauss();

	std::vector<float> dData ( (size_t)iRows*iDims );
	std::vector<double> dPoint(iDims);
	for ( int i=0; i<iRows; i++ )
	{
		bool bOutlier = ( i%100 ) < iOutlierPct;
		const auto & dCentroid = dCentroids[i%iClusters];
		for ( int j=0; j<iDims; j++ )
			dPoint[j] = bOutlier ? tRng.Gauss()*3.0 : dCentroid[j] + tRng.Gauss()*0.35;

		double fNorm = 0.0;
		for ( double fVal : dPoint )
			fNorm += fVal*fVal;

		fNorm = std::sqrt(fNorm);
		for ( int j=0; j<iDims; j++ )
			dData [ (size_t)i*iDims + j ] = float ( dPoint[j]/fNorm );
	}

	return dData;
}


struct BuildResult_t
{
	knn::RepairStats_t	m_tStats;
	std::string			m_sFilename;
	bool				m_bOk = false;
};


static BuildResult_t BuildIndex ( const std::vector<float> & dData, int iRows, int iDims, knn::HNSWSimilarity_e eSimilarity, knn::Quantization_e eQuantization, const std::string & sTag, uint32_t uFullThreshold, int iHNSWM = 16, int iEFConstruction = 200, int iNumElements = -1 )
{
	BuildResult_t tRes;
	tRes.m_sFilename = "graphrepair_test_" + sTag + ".spknn";
	if ( iNumElements<0 )
		iNumElements = iRows;

	knn::AttrWithSettings_t tAttr;
	tAttr.m_sName					= "vec";
	tAttr.m_bKNN					= true;
	tAttr.m_iDims					= iDims;
	tAttr.m_eHNSWSimilarity			= eSimilarity;
	tAttr.m_eQuantization			= eQuantization;
	tAttr.m_iHNSWM					= iHNSWM;
	tAttr.m_iHNSWEFConstruction		= iEFConstruction;

	knn::Schema_t dSchema { tAttr };
	std::unique_ptr<knn::Builder_i> pBuilder { CreateKNNBuilder ( dSchema, iNumElements, tRes.m_sFilename+".tmp" ) };
	if ( !pBuilder )
	{
		printf ( "FAILED %s: CreateKNNBuilder returned null\n", sTag.c_str() );
		g_iFailures++;
		return tRes;
	}

	for ( int i=0; i<iRows; i++ )
		pBuilder->Train ( 0, i, { (float*)dData.data() + (size_t)i*iDims, (size_t)iDims } );

	std::string sError;
	if ( !pBuilder->FinalizeTraining(sError) )
	{
		printf ( "FAILED %s: FinalizeTraining: %s\n", sTag.c_str(), sError.c_str() );
		g_iFailures++;
		return tRes;
	}

	knn::BuildContext_t tBuildCtx;
	for ( int i=0; i<iRows; i++ )
		if ( !pBuilder->SetAttr ( 0, i, { (float*)dData.data() + (size_t)i*iDims, (size_t)iDims }, tBuildCtx ) )
		{
			printf ( "FAILED %s: SetAttr row %d: %s\n", sTag.c_str(), i, tBuildCtx.m_sError.c_str() );
			g_iFailures++;
			return tRes;
		}

	knn::Test_SetRepairFullThreshold(uFullThreshold);
	knn::Test_ResetRepairStats();
	bool bSaved = pBuilder->Save ( tRes.m_sFilename, 1024*1024, sError );
	knn::Test_SetRepairFullThreshold(0);

	tRes.m_tStats = knn::Test_GetRepairStats();
	if ( !bSaved )
	{
		printf ( "FAILED %s: Save: %s\n", sTag.c_str(), sError.c_str() );
		g_iFailures++;
		return tRes;
	}

	tRes.m_bOk = true;
	return tRes;
}


// Every row must be findable by searching with its own vector
static int CountUnfindable ( const std::string & sFilename, const std::vector<float> & dData, int iRows, int iDims, int iK )
{
	std::unique_ptr<knn::KNN_i> pKNN { CreateKNN() };
	std::string sError;
	if ( !pKNN->Load ( sFilename, sError ) )
	{
		printf ( "FAILED: KNN load '%s': %s\n", sFilename.c_str(), sError.c_str() );
		g_iFailures++;
		return -1;
	}

	int iUnfindable = 0;
	for ( int i=0; i<iRows; i++ )
	{
		util::Span_T<float> dPoint { (float*)dData.data() + (size_t)i*iDims, (size_t)iDims };
		std::unique_ptr<knn::Iterator_i> pIt { pKNN->CreateIterator ( "vec", dPoint, iK, 128, nullptr, knn::HNSWTerminationPolicy_e::NONE, false, sError ) };
		if ( !pIt )
		{
			printf ( "FAILED: CreateIterator row %d: %s\n", i, sError.c_str() );
			g_iFailures++;
			return -1;
		}

		bool bFound = false;
		for ( const auto & tHit : pIt->GetData() )
			if ( tHit.m_tRowID==(uint32_t)i )
			{
				bFound = true;
				break;
			}

		if ( !bFound )
			iUnfindable++;
	}

	return iUnfindable;
}


static bool BuildMultiAttr ( const std::vector<float> & dData, int iRows, int iDims, int iAttrs, const char * szTag, knn::RepairStats_t & tLast )
{
	knn::Schema_t dSchema;
	for ( int i=0; i<iAttrs; i++ )
	{
		knn::AttrWithSettings_t tAttr;
		tAttr.m_sName				= "vec" + std::to_string(i);
		tAttr.m_bKNN				= true;
		tAttr.m_iDims				= iDims;
		tAttr.m_eHNSWSimilarity		= knn::HNSWSimilarity_e::COSINE;
		tAttr.m_iHNSWM				= 16;
		tAttr.m_iHNSWEFConstruction	= 200;
		dSchema.push_back(tAttr);
	}

	std::string sFilename = std::string("graphrepair_test_") + szTag + ".spknn";
	std::unique_ptr<knn::Builder_i> pBuilder { CreateKNNBuilder ( dSchema, iRows, sFilename+".tmp" ) };
	if ( !pBuilder )
		return false;

	for ( int iAttr=0; iAttr<iAttrs; iAttr++ )
		for ( int i=0; i<iRows; i++ )
			pBuilder->Train ( iAttr, i, { (float*)dData.data() + (size_t)i*iDims, (size_t)iDims } );

	std::string sError;
	if ( !pBuilder->FinalizeTraining(sError) )
		return false;

	knn::BuildContext_t tBuildCtx;
	for ( int iAttr=0; iAttr<iAttrs; iAttr++ )
		for ( int i=0; i<iRows; i++ )
			if ( !pBuilder->SetAttr ( iAttr, i, { (float*)dData.data() + (size_t)i*iDims, (size_t)iDims }, tBuildCtx ) )
				return false;

	knn::Test_ResetRepairStats();
	bool bOk = pBuilder->Save ( sFilename, 1024*1024, sError );
	tLast = knn::Test_GetRepairStats();	// accumulated over every attribute, with m_uRuns
	return bOk;
}


static bool BuildMultiVector ( const std::vector<float> & dData, int iRows, int iDims, int iVecsPerRow, knn::RepairStats_t & tLast )
{
	knn::AttrWithSettings_t tAttr;
	tAttr.m_sName				= "vec";
	tAttr.m_bKNN				= true;
	tAttr.m_iDims				= iDims;
	tAttr.m_eHNSWSimilarity		= knn::HNSWSimilarity_e::COSINE;
	tAttr.m_iHNSWM				= 16;
	tAttr.m_iHNSWEFConstruction	= 200;
	tAttr.m_bMulti				= true;

	const int iDocs = iRows/iVecsPerRow;
	knn::Schema_t dSchema { tAttr };
	std::string sFilename = "graphrepair_test_multivec.spknn";
	std::unique_ptr<knn::Builder_i> pBuilder { CreateKNNBuilder ( dSchema, iDocs, sFilename+".tmp" ) };
	if ( !pBuilder )
		return false;

	const size_t uRowFloats = (size_t)iDims*iVecsPerRow;
	for ( int i=0; i<iDocs; i++ )
		pBuilder->Train ( 0, i, { (float*)dData.data() + (size_t)i*uRowFloats, uRowFloats } );

	std::string sError;
	if ( !pBuilder->FinalizeTraining(sError) )
		return false;

	knn::BuildContext_t tBuildCtx;
	for ( int i=0; i<iDocs; i++ )
		if ( !pBuilder->SetAttr ( 0, i, { (float*)dData.data() + (size_t)i*uRowFloats, uRowFloats }, tBuildCtx ) )
			return false;

	knn::Test_ResetRepairStats();
	bool bOk = pBuilder->Save ( sFilename, 1024*1024, sError );
	tLast = knn::Test_GetRepairStats();
	return bOk;
}


/// Fixture search. Fixture needs regenerating if hardcoded settings no longer produce orphans
static int RunSweep()
{
	struct Mode_t { knn::HNSWSimilarity_e m_eSim; const char * m_szSim; knn::Quantization_e m_eQuant; const char * m_szQuant; };
	const Mode_t dModes[] =
	{
		{ knn::HNSWSimilarity_e::COSINE, "cosine", knn::Quantization_e::NONE, "none" },
		{ knn::HNSWSimilarity_e::COSINE, "cosine", knn::Quantization_e::BIT8, "8bit" },
		{ knn::HNSWSimilarity_e::COSINE, "cosine", knn::Quantization_e::BIT1, "1bit" },
		{ knn::HNSWSimilarity_e::L2,     "l2",     knn::Quantization_e::BIT1, "1bit" },
	};

	const int dM[]        = { 4, 8, 16 };
	const int dOutliers[] = { 3, 10 };

	for ( const auto & tMode : dModes )
		for ( int iHNSWM : dM )
			for ( int iOutlierPct : dOutliers )
				for ( uint64_t uSeed=1; uSeed<=2; uSeed++ )
				{
					const int iRows = 5000, iDims = 64;
					std::vector<float> dData = GenerateFixture ( iRows, iDims, 8, iOutlierPct, uSeed );
					BuildResult_t tRes = BuildIndex ( dData, iRows, iDims, tMode.m_eSim, tMode.m_eQuant, "sweep", 0, iHNSWM );
					printf ( "%-6s %-4s M=%-3d outlier%%=%-3d seed=%llu -> orphans=%llu appends=%llu widenings=%llu unreachable_after=%llu\n",
						tMode.m_szSim, tMode.m_szQuant, iHNSWM, iOutlierPct, (unsigned long long)uSeed,
						(unsigned long long)tRes.m_tStats.m_uOrphansFound, (unsigned long long)tRes.m_tStats.m_uAppends,
						(unsigned long long)tRes.m_tStats.m_u2HopWidenings,
						(unsigned long long)tRes.m_tStats.m_uUnreachableAfter );
				}

	return 0;
}


// Reports how many nodes a build would strand for vectors loaded from a text file ("<rows> <dims>" then row-major floats)
static int RunOrphanCount ( const char * szFile, int iHNSWM )
{
	FILE * pFile = fopen ( szFile, "rt" );
	if ( !pFile )
	{
		printf ( "cannot open '%s'\n", szFile );
		return 1;
	}

	int iRows = 0, iDims = 0;
	if ( fscanf ( pFile, "%d %d", &iRows, &iDims )!=2 || iRows<=0 || iDims<=0 )
	{
		printf ( "bad header in '%s'\n", szFile );
		fclose(pFile);
		return 1;
	}

	std::vector<float> dData ( (size_t)iRows*iDims );
	for ( auto & fVal : dData )
		if ( fscanf ( pFile, "%f", &fVal )!=1 )
		{
			printf ( "truncated data in '%s'\n", szFile );
			fclose(pFile);
			return 1;
		}

	fclose(pFile);

	BuildResult_t tRes = BuildIndex ( dData, iRows, iDims, knn::HNSWSimilarity_e::COSINE, knn::Quantization_e::NONE, "orphancount", 0, iHNSWM );
	printf ( "rows=%d dims=%d M=%d -> orphans=%llu appends=%llu unreachable_after=%llu\n", iRows, iDims, iHNSWM,
		(unsigned long long)tRes.m_tStats.m_uOrphansFound, (unsigned long long)tRes.m_tStats.m_uAppends,
		(unsigned long long)tRes.m_tStats.m_uUnreachableAfter );

	return 0;
}


int main ( int iArgs, char ** ppArgs )
{
	if ( iArgs>1 && std::string(ppArgs[1])=="--sweep" )
		return RunSweep();

	if ( iArgs>2 && std::string(ppArgs[1])=="--orphan-count" )
		return RunOrphanCount ( ppArgs[2], iArgs>3 ? atoi(ppArgs[3]) : 16 );

	const int iRows = 5000, iDims = 64;
	std::vector<float> dData = GenerateFixture ( iRows, iDims, 8, 3, 1 );

	// 1. The fixture must actually contain orphans, otherwise everything test don't test anything
	printf ( "--- baseline: cosine / no quantization ---\n" );
	BuildResult_t tBase = BuildIndex ( dData, iRows, iDims, knn::HNSWSimilarity_e::COSINE, knn::Quantization_e::NONE, "cosine_none", 0 );
	printf ( "nodes=%llu orphans=%llu appends=%llu widenings=%llu evictions=%llu distcalls=%llu unreachable_after=%llu\n",
		(unsigned long long)tBase.m_tStats.m_uNodes, (unsigned long long)tBase.m_tStats.m_uOrphansFound,
		(unsigned long long)tBase.m_tStats.m_uAppends, (unsigned long long)tBase.m_tStats.m_u2HopWidenings,
		(unsigned long long)tBase.m_tStats.m_uEvictions, (unsigned long long)tBase.m_tStats.m_uDistanceCalls,
		(unsigned long long)tBase.m_tStats.m_uUnreachableAfter );

	CHECK ( tBase.m_tStats.m_uNodes==(uint64_t)iRows, "expected %d nodes", iRows );
	CHECK ( tBase.m_tStats.m_uOrphansFound>0, "fixture produced no orphans, regenerate it" );
	CHECK ( tBase.m_tStats.m_uAppends>0, "no node was relinked" );
	CHECK ( tBase.m_tStats.m_uDistanceCalls>0, "repair evaluated no distances" );
	CHECK ( tBase.m_tStats.m_uUnreachableAfter==0, "%llu nodes still unreachable", (unsigned long long)tBase.m_tStats.m_uUnreachableAfter );

	if ( tBase.m_bOk )
	{
		int iUnfindable = CountUnfindable ( tBase.m_sFilename, dData, iRows, iDims, 5 );
		printf ( "rows not findable by their own vector: %d\n", iUnfindable );
		CHECK ( iUnfindable==0, "%d rows cannot be found by knn()", iUnfindable );
	}

	// 2. Quantization matrix. M is part of the fixture - 1-bit needs M<=8 here to orphan at all
	printf ( "\n--- similarity x quantization matrix ---\n" );
	struct Case_t
	{
		knn::HNSWSimilarity_e	m_eSim;
		const char *			m_szSim;
		knn::Quantization_e		m_eQuant;
		const char *			m_szQuant;
		int						m_iM;
		bool					m_bMustOrphan;
	};

	const Case_t dCases[] =
	{
		{ knn::HNSWSimilarity_e::L2,     "l2",     knn::Quantization_e::NONE, "none", 16, true  },
		{ knn::HNSWSimilarity_e::IP,     "ip",     knn::Quantization_e::NONE, "none", 16, true  },
		{ knn::HNSWSimilarity_e::COSINE, "cosine", knn::Quantization_e::BIT8, "8bit", 16, true  },
		{ knn::HNSWSimilarity_e::L2,     "l2",     knn::Quantization_e::BIT8, "8bit",  8, true  },
		{ knn::HNSWSimilarity_e::COSINE, "cosine", knn::Quantization_e::BIT1, "1bit",  8, true  },
		{ knn::HNSWSimilarity_e::L2,     "l2",     knn::Quantization_e::BIT1, "1bit",  8, true  },
		{ knn::HNSWSimilarity_e::COSINE, "cosine", knn::Quantization_e::BIT1, "1bit",  4, true  },
		{ knn::HNSWSimilarity_e::IP,     "ip",     knn::Quantization_e::BIT8, "8bit", 16, true  },
		{ knn::HNSWSimilarity_e::IP,     "ip",     knn::Quantization_e::BIT1, "1bit",  8, true  },
		{ knn::HNSWSimilarity_e::COSINE, "cosine", knn::Quantization_e::BIT1, "1bit", 16, false },
	};

	for ( const auto & tCase : dCases )
	{
		std::string sTag = std::string(tCase.m_szSim) + "_" + tCase.m_szQuant + "_m" + std::to_string(tCase.m_iM);
		BuildResult_t tRes = BuildIndex ( dData, iRows, iDims, tCase.m_eSim, tCase.m_eQuant, sTag, 0, tCase.m_iM );
		printf ( "%-20s orphans=%-5llu appends=%-5llu evictions=%-3llu distcalls=%-6llu unreachable_after=%llu\n", sTag.c_str(),
			(unsigned long long)tRes.m_tStats.m_uOrphansFound, (unsigned long long)tRes.m_tStats.m_uAppends,
			(unsigned long long)tRes.m_tStats.m_uEvictions, (unsigned long long)tRes.m_tStats.m_uDistanceCalls,
			(unsigned long long)tRes.m_tStats.m_uUnreachableAfter );

		CHECK ( tRes.m_bOk, "%s: build failed", sTag.c_str() );
		CHECK ( tRes.m_tStats.m_uUnreachableAfter==0, "%s: %llu nodes still unreachable", sTag.c_str(), (unsigned long long)tRes.m_tStats.m_uUnreachableAfter );

		if ( tCase.m_bMustOrphan )
		{
			CHECK ( tRes.m_tStats.m_uOrphansFound>0, "%s: fixture no longer strands anything, re-pin it with --sweep", sTag.c_str() );
			CHECK ( tRes.m_tStats.m_uAppends>0, "%s: nothing was relinked", sTag.c_str() );
			CHECK ( tRes.m_tStats.m_uDistanceCalls>0, "%s: repair evaluated no distances, so this case never exercises the build kernel", sTag.c_str() );
		}

		// Only meaningful without quantization. A quantized search compares quantized codes, so a self-query with the original float vector can legitimately miss the row
		if ( tRes.m_bOk && tCase.m_eQuant==knn::Quantization_e::NONE )
		{
			int iUnfindable = CountUnfindable ( tRes.m_sFilename, dData, iRows, iDims, 20 );
			CHECK ( iUnfindable==0, "%s: %d rows cannot be found by knn()", sTag.c_str(), iUnfindable );
		}
	}

	// 3. Force the eviction branch. Real data never fills enough lists to reach it, so lower the
	// "list is full" threshold. Eviction must relink without stranding anything.
	printf ( "\n--- forced eviction (full threshold = 4) ---\n" );
	BuildResult_t tEvict = BuildIndex ( dData, iRows, iDims, knn::HNSWSimilarity_e::COSINE, knn::Quantization_e::NONE, "evict", 4 );
	printf ( "orphans=%llu appends=%llu evictions=%llu distcalls=%llu unreachable_after=%llu\n",
		(unsigned long long)tEvict.m_tStats.m_uOrphansFound, (unsigned long long)tEvict.m_tStats.m_uAppends,
		(unsigned long long)tEvict.m_tStats.m_uEvictions, (unsigned long long)tEvict.m_tStats.m_uDistanceCalls,
		(unsigned long long)tEvict.m_tStats.m_uUnreachableAfter );

	CHECK ( tEvict.m_tStats.m_uEvictions>0, "eviction branch never ran, so it is untested" );
	CHECK ( tEvict.m_tStats.m_uDistanceCalls>0, "eviction ranked no victims by distance" );
	CHECK ( tEvict.m_tStats.m_uUnreachableAfter==0, "eviction stranded %llu nodes", (unsigned long long)tEvict.m_tStats.m_uUnreachableAfter );

	if ( tEvict.m_bOk )
	{
		int iUnfindable = CountUnfindable ( tEvict.m_sFilename, dData, iRows, iDims, 5 );
		printf ( "rows not findable by their own vector: %d\n", iUnfindable );
		CHECK ( iUnfindable==0, "%d rows cannot be found by knn() after eviction", iUnfindable );
	}

	// 4. Degenerate sizes must not trip the entry-point guards. Zero elements matters most: the graph
	// is empty and enterpoint_node_ is (tableint)-1, so the pass has to bail before touching it.
	printf ( "\n--- degenerate sizes ---\n" );
	for ( int iSmall : { 0, 1, 2, 3 } )
	{
		std::vector<float> dSmall ( dData.begin(), dData.begin() + (size_t)iSmall*iDims );
		BuildResult_t tRes = BuildIndex ( dSmall, iSmall, iDims, knn::HNSWSimilarity_e::COSINE, knn::Quantization_e::NONE, "small" + std::to_string(iSmall), 0 );
		printf ( "rows=%-2d -> ok=%d nodes=%llu unreachable_after=%llu\n", iSmall, (int)tRes.m_bOk,
			(unsigned long long)tRes.m_tStats.m_uNodes, (unsigned long long)tRes.m_tStats.m_uUnreachableAfter );
		CHECK ( tRes.m_bOk, "%d-row build failed", iSmall );
		CHECK ( tRes.m_tStats.m_uNodes==(uint64_t)iSmall, "%d-row build reported %llu nodes", iSmall, (unsigned long long)tRes.m_tStats.m_uNodes );
		CHECK ( tRes.m_tStats.m_uUnreachableAfter==0, "%d-row index has unreachable nodes", iSmall );
	}

	// 5. cur_element_count < max_elements_. Rows whose vector attribute is empty never reach addPoint, so the graph is smaller than the capacity it was built with.
	// Iterating to max_elements_ would walk uninitialised level-0 memory.
	printf ( "\n--- cur_element_count < max_elements_ ---\n" );
	{
		const int iPartial = iRows/2;
		std::vector<float> dPartial ( dData.begin(), dData.begin() + (size_t)iPartial*iDims );
		BuildResult_t tRes = BuildIndex ( dPartial, iPartial, iDims, knn::HNSWSimilarity_e::COSINE, knn::Quantization_e::NONE, "partial", 0, 16, 200, iRows );
		printf ( "capacity=%d filled=%d -> nodes=%llu orphans=%llu unreachable_after=%llu\n", iRows, iPartial,
			(unsigned long long)tRes.m_tStats.m_uNodes, (unsigned long long)tRes.m_tStats.m_uOrphansFound,
			(unsigned long long)tRes.m_tStats.m_uUnreachableAfter );
		CHECK ( tRes.m_bOk, "partially filled build failed" );
		CHECK ( tRes.m_tStats.m_uNodes==(uint64_t)iPartial, "pass saw %llu nodes, expected the filled count %d", (unsigned long long)tRes.m_tStats.m_uNodes, iPartial );
		CHECK ( tRes.m_tStats.m_uUnreachableAfter==0, "partially filled index has unreachable nodes" );
	}

	// 6. Several KNN attributes on one index: each owns its own graph and its own Save call.
	printf ( "\n--- multiple KNN attributes ---\n" );
	{
		const int iAttrs = 3;
		knn::RepairStats_t tAcc;
		bool bOk = BuildMultiAttr ( dData, iRows, iDims, iAttrs, "multiattr", tAcc );
		printf ( "%d attributes -> ok=%d runs=%llu nodes(sum)=%llu orphans(sum)=%llu unreachable_after(sum)=%llu\n",
			iAttrs, (int)bOk, (unsigned long long)tAcc.m_uRuns, (unsigned long long)tAcc.m_uNodes,
			(unsigned long long)tAcc.m_uOrphansFound, (unsigned long long)tAcc.m_uUnreachableAfter );
		CHECK ( bOk, "multi-attribute build failed" );
		// the stats accumulate, so this catches an attribute being skipped entirely - reading only the
		// last run would pass even if the earlier graphs were never repaired
		CHECK ( tAcc.m_uRuns==(uint64_t)iAttrs, "repair ran %llu times for %d attributes", (unsigned long long)tAcc.m_uRuns, iAttrs );
		CHECK ( tAcc.m_uNodes==(uint64_t)iRows*iAttrs, "multi-attribute: pass saw %llu nodes across all attributes, expected %d", (unsigned long long)tAcc.m_uNodes, iRows*iAttrs );
		CHECK ( tAcc.m_uOrphansFound>0, "multi-attribute fixture stranded nothing, so it proves nothing" );
		CHECK ( tAcc.m_uUnreachableAfter==0, "multi-attribute index has unreachable nodes" );
	}

	// 7. Multi-vector attribute: labels are vector ids, and the graph is allocated in FinalizeTraining rather than the constructor.
	printf ( "\n--- multi-vector attribute ---\n" );
	{
		knn::RepairStats_t tAcc;
		bool bOk = BuildMultiVector ( dData, iRows, iDims, 2, tAcc );
		printf ( "2 vectors/row -> ok=%d runs=%llu nodes=%llu orphans=%llu unreachable_after=%llu\n", (int)bOk,
			(unsigned long long)tAcc.m_uRuns, (unsigned long long)tAcc.m_uNodes,
			(unsigned long long)tAcc.m_uOrphansFound, (unsigned long long)tAcc.m_uUnreachableAfter );
		CHECK ( bOk, "multi-vector build failed" );
		CHECK ( tAcc.m_uRuns==1, "repair ran %llu times for one attribute", (unsigned long long)tAcc.m_uRuns );
		CHECK ( tAcc.m_uNodes==(uint64_t)iRows, "multi-vector: pass saw %llu nodes, expected %d vectors", (unsigned long long)tAcc.m_uNodes, iRows );
		CHECK ( tAcc.m_uUnreachableAfter==0, "multi-vector index has unreachable nodes" );
	}

	printf ( "\n%s (%d failure(s))\n", g_iFailures ? "FAILED" : "PASSED", g_iFailures );
	return g_iFailures ? 1 : 0;
}
