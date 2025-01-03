// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	PrimitiveSceneInfo.cpp: Primitive scene info implementation.
=============================================================================*/

#include "PrimitiveSceneInfo.h"
#include "PrimitiveSceneProxy.h"
#include "Components/PrimitiveComponent.h"
#include "SceneManagement.h"
#include "SceneCore.h"
#include "VelocityRendering.h"
#include "ScenePrivate.h"
#include "RayTracingGeometry.h"
#include "Components/ComponentInterfaces.h"

#include "RendererModule.h"
#include "HAL/LowLevelMemTracker.h"
#include "RayTracing/RayTracingMaterialHitShaders.h"
#include "RayTracing/RayTracingInstanceMask.h"
#include "VT/RuntimeVirtualTextureSceneProxy.h"
#include "VT/VirtualTextureSystem.h"
#include "GPUScene.h"
#include "Async/ParallelFor.h"
#include "ProfilingDebugging/ExternalProfiler.h"
#include "Nanite/Nanite.h"
#include "Nanite/NaniteRayTracing.h"
#include "Nanite/NaniteShading.h"
#include "Rendering/NaniteResources.h"
#include "NaniteSceneProxy.h"
#include "Lumen/LumenSceneData.h"
#include "Lumen/LumenSceneCardCapture.h"
#include "RayTracingDefinitions.h"
#include "RenderCore.h"
#include "Materials/MaterialRenderProxy.h"
#include "StaticMeshBatch.h"
#include "PrimitiveSceneDesc.h"
#include "BasePassRendering.h" // TODO: Remove with later refactor (moving Nanite shading into its own files)
#include "InstanceDataSceneProxy.h"


extern int32 GGPUSceneInstanceClearList;

static int32 GMeshDrawCommandsCacheMultithreaded = 1;
static FAutoConsoleVariableRef CVarDrawCommandsCacheMultithreaded(
	TEXT("r.MeshDrawCommands.CacheMultithreaded"),
	GMeshDrawCommandsCacheMultithreaded,
	TEXT("Enable multithreading of draw command caching for static meshes. 0=disabled, 1=enabled (default)"),
	ECVF_RenderThreadSafe);

static int32 GMeshDrawCommandsBatchSize = 12;
static FAutoConsoleVariableRef CVarDrawCommandsCacheMultithreadedBatchSize(
	TEXT("r.MeshDrawCommands.BatchSize"),
	GMeshDrawCommandsBatchSize,
	TEXT("Batch size of cache mesh draw commands when multithreading of draw command caching is enabled"),
	ECVF_RenderThreadSafe);

static int32 GNaniteDrawCommandCacheMultithreaded = 1;
static FAutoConsoleVariableRef CVarNaniteDrawCommandCacheMultithreaded(
	TEXT("r.Nanite.MeshDrawCommands.CacheMultithreaded"),
	GNaniteDrawCommandCacheMultithreaded,
	TEXT("Enable multithreading of draw command caching for Nanite materials. 0=disabled, 1=enabled (default)"),
	ECVF_RenderThreadSafe);

static int32 GRayTracingPrimitiveCacheMultithreaded = 1;
static FAutoConsoleVariableRef CVarRayTracingPrimitiveCacheMultithreaded(
	TEXT("r.RayTracing.MeshDrawCommands.CacheMultithreaded"),
	GRayTracingPrimitiveCacheMultithreaded,
	TEXT("Enable multithreading of raytracing primitive mesh command caching. 0=disabled, 1=enabled (default)"),
	ECVF_RenderThreadSafe);

/** An implementation of FStaticPrimitiveDrawInterface that stores the drawn elements for the rendering thread to use. */
class FBatchingSPDI : public FStaticPrimitiveDrawInterface
{
public:

	// Constructor.
	FBatchingSPDI(FPrimitiveSceneInfo* InPrimitiveSceneInfo):
		PrimitiveSceneInfo(InPrimitiveSceneInfo)
	{}

	// FStaticPrimitiveDrawInterface.
	virtual void SetHitProxy(HHitProxy* HitProxy) final override
	{
		CurrentHitProxy = HitProxy;

		if(HitProxy)
		{
			// Only use static scene primitive hit proxies in the editor.
			if(GIsEditor)
			{
				// Keep a reference to the hit proxy from the FPrimitiveSceneInfo, to ensure it isn't deleted while the static mesh still
				// uses its id.
				PrimitiveSceneInfo->HitProxies.Add(HitProxy);
			}
		}
	}

	virtual void ReserveMemoryForMeshes(int32 MeshNum)
	{
		PrimitiveSceneInfo->StaticMeshRelevances.Reserve(PrimitiveSceneInfo->StaticMeshRelevances.Num() + MeshNum);
		PrimitiveSceneInfo->StaticMeshes.Reserve(PrimitiveSceneInfo->StaticMeshes.Num() + MeshNum);
	}

	virtual void DrawMesh(const FMeshBatch& Mesh, float ScreenSize) final override
	{
		if (Mesh.HasAnyDrawCalls())
		{
			checkSlow(IsInParallelRenderingThread());

			FPrimitiveSceneProxy* PrimitiveSceneProxy = PrimitiveSceneInfo->Proxy;
			const ERHIFeatureLevel::Type FeatureLevel = PrimitiveSceneInfo->Scene->GetFeatureLevel();

			if (!Mesh.Validate(PrimitiveSceneProxy, FeatureLevel))
			{
				return;
			}

			FStaticMeshBatch* StaticMesh = new(PrimitiveSceneInfo->StaticMeshes) FStaticMeshBatch(
				PrimitiveSceneInfo,
				Mesh,
				CurrentHitProxy ? CurrentHitProxy->Id : FHitProxyId()
			);

			StaticMesh->PreparePrimitiveUniformBuffer(PrimitiveSceneProxy, FeatureLevel);
			// Volumetric self shadow mesh commands need to be generated every frame, as they depend on single frame uniform buffers with self shadow data.
			const bool bSupportsCachingMeshDrawCommands = SupportsCachingMeshDrawCommands(*StaticMesh, FeatureLevel) && !PrimitiveSceneProxy->CastsVolumetricTranslucentShadow();

			const FMaterial& Material = Mesh.MaterialRenderProxy->GetIncompleteMaterialWithFallback(FeatureLevel);
			bool bUseSkyMaterial = Material.IsSky();
			bool bUseSingleLayerWaterMaterial = Material.GetShadingModels().HasShadingModel(MSM_SingleLayerWater);
			bool bUseAnisotropy = Material.GetShadingModels().HasAnyShadingModel({MSM_DefaultLit, MSM_ClearCoat}) && Material.MaterialUsesAnisotropy_RenderThread();
			bool bSupportsNaniteRendering = SupportsNaniteRendering(StaticMesh->VertexFactory, PrimitiveSceneProxy, Mesh.MaterialRenderProxy, FeatureLevel);
			bool bSupportsGPUScene = StaticMesh->VertexFactory->SupportsGPUScene(FeatureLevel);
			bool bUseForWaterInfoTextureDepth = Mesh.bUseForWaterInfoTextureDepth;
			bool bUseForLumenSceneCapture = Mesh.bUseForLumenSurfaceCacheCapture;

			FStaticMeshBatchRelevance* StaticMeshRelevance = new(PrimitiveSceneInfo->StaticMeshRelevances) FStaticMeshBatchRelevance(
				*StaticMesh, 
				ScreenSize, 
				bSupportsCachingMeshDrawCommands,
				bUseSkyMaterial,
				bUseSingleLayerWaterMaterial,
				bUseAnisotropy,
				bSupportsNaniteRendering,
				bSupportsGPUScene,
				bUseForWaterInfoTextureDepth,
				bUseForLumenSceneCapture,
				FeatureLevel
				);
		}
	}

private:
	FPrimitiveSceneInfo* PrimitiveSceneInfo;
	TRefCountPtr<HHitProxy> CurrentHitProxy;
};

FPrimitiveSceneInfo::FPrimitiveSceneInfoEvent FPrimitiveSceneInfo::OnGPUSceneInstancesAllocated;
FPrimitiveSceneInfo::FPrimitiveSceneInfoEvent FPrimitiveSceneInfo::OnGPUSceneInstancesFreed;

FPrimitiveFlagsCompact::FPrimitiveFlagsCompact(const FPrimitiveSceneProxy* Proxy)
	: bCastDynamicShadow(Proxy->CastsDynamicShadow())
	, bStaticLighting(Proxy->HasStaticLighting())
	, bCastStaticShadow(Proxy->CastsStaticShadow())
	, bIsNaniteMesh(Proxy->IsNaniteMesh())
	, bIsAlwaysVisible(Proxy->IsAlwaysVisible())
	, bSupportsGPUScene(Proxy->SupportsGPUScene())
{}

FPrimitiveSceneInfoCompact::FPrimitiveSceneInfoCompact(FPrimitiveSceneInfo* InPrimitiveSceneInfo) :
	PrimitiveFlagsCompact(InPrimitiveSceneInfo->Proxy)
{
	PrimitiveSceneInfo = InPrimitiveSceneInfo;
	Proxy = PrimitiveSceneInfo->Proxy;
	Bounds = FCompactBoxSphereBounds(PrimitiveSceneInfo->Proxy->GetBounds());
	MinDrawDistance = PrimitiveSceneInfo->Proxy->GetMinDrawDistance();
	MaxDrawDistance = PrimitiveSceneInfo->Proxy->GetMaxDrawDistance();

	VisibilityId = PrimitiveSceneInfo->Proxy->GetVisibilityId();
}

struct FPrimitiveSceneInfoAdapter
{
	void CreateHitProxies()
	{
		if (PrimitiveComponentInterface)
		{
			// Support for legacy path for proxy creation, if not handled it'll internally invoke the IPrimitiveComponentInterface path
			if (UPrimitiveComponent* PrimitiveComponent =  PrimitiveComponentInterface->GetUObject<UPrimitiveComponent>())
			{		
				DefaultHitProxy = SceneProxy->CreateHitProxies(PrimitiveComponent, HitProxies);			
			}
			else 
			{
				// For all other implementers
				DefaultHitProxy = SceneProxy->CreateHitProxies(PrimitiveComponentInterface, HitProxies);			
			}
		}
	}

	FPrimitiveSceneInfoAdapter(UPrimitiveComponent* InComponent)		
	{		
		SceneProxy = InComponent->SceneProxy;
		SceneData = &InComponent->SceneData;
		ComponentId = SceneData->PrimitiveSceneId;			
		check(InComponent->GetSceneData().RegistrationSerialNumber != -1);
		RegistrationSerialNumber = InComponent->GetSceneData().RegistrationSerialNumber;
		Component = InComponent;
		PrimitiveComponentInterface = InComponent->GetPrimitiveComponentInterface();
		PrimitiveDesc = nullptr;		
		
		// This validates the UPrimitiveComponent has properly initialized its OwnerLastRenderTimePtr
		check(InComponent->SceneData.OwnerLastRenderTimePtr == FActorLastRenderTime::GetPtr(InComponent->GetOwner()));		
		Mobility = InComponent->Mobility;

		const UPrimitiveComponent* SearchParentComponent = InComponent->GetLightingAttachmentRoot();

		if (SearchParentComponent && SearchParentComponent != InComponent)
		{
			LightingAttachmentComponentId = SearchParentComponent->GetPrimitiveSceneId();
		}

		// set LOD parent info if exists
		UPrimitiveComponent* LODParent = InComponent->GetLODParentPrimitive();
		if (LODParent)
		{
			LODParentComponentId = LODParent->GetPrimitiveSceneId();
		}		

		if (GIsEditor)
		{
			CreateHitProxies();
		}
		
	}
	
	FPrimitiveSceneInfoAdapter(FPrimitiveSceneDesc* InPrimitiveSceneDesc)
	{		
		check(InPrimitiveSceneDesc);		

		Component = nullptr;
		PrimitiveComponentInterface = InPrimitiveSceneDesc->GetPrimitiveComponentInterface();
		SceneData = &InPrimitiveSceneDesc->GetSceneData();
		PrimitiveDesc = InPrimitiveSceneDesc;
		SceneProxy = InPrimitiveSceneDesc->GetSceneProxy();
		check(SceneProxy);
		ComponentId = InPrimitiveSceneDesc->GetPrimitiveSceneId();
		RegistrationSerialNumber = InPrimitiveSceneDesc->GetRegistrationSerialNumber();
		LODParentComponentId = InPrimitiveSceneDesc->GetLODParentId();
		LightingAttachmentComponentId = InPrimitiveSceneDesc->GetLightingAttachmentId();			 		
		Mobility = InPrimitiveSceneDesc->GetMobility();				
		
		if (GIsEditor && PrimitiveComponentInterface)
		{
			CreateHitProxies();
		}

	}
	
	FPrimitiveSceneProxy* SceneProxy;
	FPrimitiveComponentId ComponentId;
	int32 RegistrationSerialNumber;
	FPrimitiveComponentId LODParentComponentId;
	FPrimitiveComponentId LightingAttachmentComponentId;	
	EComponentMobility::Type Mobility;

	// mutable so that hit proxies can be moved to final destination
	mutable TArray<TRefCountPtr<HHitProxy> > HitProxies;
	HHitProxy* DefaultHitProxy = nullptr;

	FPrimitiveSceneInfoData* SceneData;
	UPrimitiveComponent* Component;
	IPrimitiveComponent* PrimitiveComponentInterface;
	FPrimitiveSceneDesc* PrimitiveDesc;
};

FPrimitiveSceneInfo::FPrimitiveSceneInfo(const FPrimitiveSceneInfoAdapter& InAdapter, FScene* InScene):
	Proxy(InAdapter.SceneProxy),
	PrimitiveComponentId(InAdapter.ComponentId),
	RegistrationSerialNumber(InAdapter.RegistrationSerialNumber),
	IndirectLightingCacheAllocation(NULL),
	CachedPlanarReflectionProxy(NULL),
	CachedReflectionCaptureProxy(NULL),
	DefaultDynamicHitProxy(NULL),
	LastRenderTime(-FLT_MAX),
	LightList(NULL),
	Scene(InScene),
	NumMobileDynamicLocalLights(0),
	GpuLodInstanceRadius(0),
	PackedIndex(INDEX_NONE),
	PersistentIndex(FPersistentPrimitiveIndex{ INDEX_NONE }),
	PrimitiveComponentInterfaceForDebuggingOnly(InAdapter.PrimitiveComponentInterface),
	SceneData(InAdapter.SceneData),	
	bNeedsUniformBufferUpdate(false),
	bIndirectLightingCacheBufferDirty(false),
	bRegisteredVirtualTextureProducerCallback(false),
	bRegisteredWithVelocityData(false),
	bCacheShadowAsStatic((InAdapter.Mobility != EComponentMobility::Movable && InAdapter.SceneProxy->GetShadowCacheInvalidationBehavior() != EShadowCacheInvalidationBehavior::Always) || InAdapter.SceneProxy->GetShadowCacheInvalidationBehavior() == EShadowCacheInvalidationBehavior::Static),
	bNaniteRasterBinsRenderCustomDepth(false),
	bPendingAddToScene(false),
	bPendingAddStaticMeshes(false),
	bPendingFlushVirtualTexture(false),
	bNeedsCachedReflectionCaptureUpdate(true),
	bShouldRenderInMainPass(InAdapter.SceneProxy->ShouldRenderInMainPass()),
	bVisibleInRealTimeSkyCapture(InAdapter.SceneProxy->IsVisibleInRealTimeSkyCaptures()),
#if RHI_RAYTRACING
	bDrawInGame(Proxy->IsDrawnInGame()),
	bRayTracingFarField(Proxy->IsRayTracingFarField()),
	bIsVisibleInSceneCaptures(!InAdapter.SceneProxy->IsHiddenInSceneCapture()),
	bIsVisibleInSceneCapturesOnly(InAdapter.SceneProxy->IsVisibleInSceneCaptureOnly()),
	bIsRayTracingRelevant(InAdapter.SceneProxy->IsRayTracingRelevant()),
	bIsRayTracingStaticRelevant(InAdapter.SceneProxy->IsRayTracingStaticRelevant()),
	bIsVisibleInRayTracing(InAdapter.SceneProxy->IsVisibleInRayTracing()),
	bCachedRaytracingDataDirty(true),
	bCachedRayTracingInstanceMaskAndFlagsDirty(true),
	bCachedRayTracingInstanceAnySegmentsDecal(false),
	bCachedRayTracingInstanceAllSegmentsDecal(false),
	CoarseMeshStreamingHandle(InAdapter.SceneProxy->GetCoarseMeshStreamingHandle()),
#endif
	// We want the unsynchronized access here, as the responsibility passes to the primitive scene info.
	InstanceSceneDataBuffersInternal(InAdapter.SceneProxy->GetInstanceSceneDataBuffers(FPrimitiveSceneProxy::EInstanceBufferAccessFlags::UnsynchronizedAndUnsafe)),
	InstanceDataUpdateTaskInfo(InAdapter.SceneProxy->GetInstanceDataUpdateTaskInfo()),
	LevelUpdateNotificationIndex(INDEX_NONE),
	InstanceSceneDataOffset(INDEX_NONE),
	NumInstanceSceneDataEntries(0),
	InstancePayloadDataOffset(INDEX_NONE),
	InstancePayloadDataStride(0),
	LightmapDataOffset(INDEX_NONE),
	NumLightmapDataEntries(0)
{
	check(PrimitiveComponentId.IsValid());
	check(Proxy);
	check(SceneData);

	LightingAttachmentRoot  = InAdapter.LightingAttachmentComponentId;	

	// Only create hit proxies in the Editor as that's where they are used.
	if (GIsEditor)
	{
		// Create a dynamic hit proxy for the primitive. 
		DefaultDynamicHitProxy = InAdapter.DefaultHitProxy;		
		HitProxies = MoveTemp(InAdapter.HitProxies);

		if( DefaultDynamicHitProxy )
		{
			check(HitProxies.Contains(DefaultDynamicHitProxy));
			DefaultDynamicHitProxyId = DefaultDynamicHitProxy->Id;
		}
	}
	
	LODParentComponentId = InAdapter.LODParentComponentId;

	FMemory::Memzero(CachedReflectionCaptureProxies);

#if RHI_RAYTRACING
	// Cache static ray tracing geometries in SceneInfo to avoid having to access SceneProxy later
	StaticRayTracingGeometries = InAdapter.SceneProxy->GetStaticRayTracingGeometries();
	CachedRayTracingGeometry = nullptr;
#endif

	if (FInstanceCullingContext::IsGPUCullingEnabled())
	{
		GpuLodInstanceRadius = InAdapter.SceneProxy->GetGpuLodInstanceRadius();
	}
}

FPrimitiveSceneInfo::FPrimitiveSceneInfo(UPrimitiveComponent* InPrimitive,FScene* InScene)
	: FPrimitiveSceneInfo(FPrimitiveSceneInfoAdapter(InPrimitive), InScene)
{
}	

FPrimitiveSceneInfo::FPrimitiveSceneInfo(FPrimitiveSceneDesc* InPrimitiveSceneDesc,FScene* InScene)
	: FPrimitiveSceneInfo(FPrimitiveSceneInfoAdapter(InPrimitiveSceneDesc), InScene)
{
}

FPrimitiveSceneInfo::~FPrimitiveSceneInfo()
{
	check(!OctreeId.IsValidId());
	for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
	{
		check(StaticMeshCommandInfos.Num() == 0);
	}
}

#if RHI_RAYTRACING
bool FPrimitiveSceneInfo::IsCachedRayTracingGeometryValid() const
{
	if (CachedRayTracingGeometry)
	{
		// TODO: Doesn't take Nanite Ray Tracing into account
		check(CachedRayTracingGeometry->RayTracingGeometryRHI == CachedRayTracingInstance.GeometryRHI);
		check(!CachedRayTracingGeometry->GetRequiresBuild() && !CachedRayTracingGeometry->HasPendingBuildRequest());

		return CachedRayTracingGeometry->IsValid();
	}

	return false;
}

FRHIRayTracingGeometry* FPrimitiveSceneInfo::GetStaticRayTracingGeometryInstance(int LodLevel) const
{
	if (StaticRayTracingGeometries.Num() > LodLevel)
	{
		// TODO: Select different LOD, when build is still pending for this LOD?
		if (StaticRayTracingGeometries[LodLevel]->HasPendingBuildRequest())
		{
			StaticRayTracingGeometries[LodLevel]->BoostBuildPriority();
			return nullptr;
		}
		else if (StaticRayTracingGeometries[LodLevel]->IsValid())
		{
			return StaticRayTracingGeometries[LodLevel]->RayTracingGeometryRHI;
		}
		else
		{
			return nullptr;
		}
	}
	else
	{
		return nullptr;
	}
}
#endif


void FPrimitiveSceneInfo::CacheMeshDrawCommands(FScene* Scene, TArrayView<FPrimitiveSceneInfo*> SceneInfos)
{
	SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheMeshDrawCommands, FColor::Emerald);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheMeshDrawCommands);

	QUICK_SCOPE_CYCLE_COUNTER(STAT_CacheMeshDrawCommands);

	// This reduce stuttering in editor by improving balancing of all the 
	// shadermap processing. Keep it as it is for runtime as the requirements are different.
	const int BATCH_SIZE = WITH_EDITOR ? 1 : GMeshDrawCommandsBatchSize;
	const int NumBatches = (SceneInfos.Num() + BATCH_SIZE - 1) / BATCH_SIZE;

	auto DoWorkLambda = [Scene, SceneInfos, BATCH_SIZE](FCachedPassMeshDrawListContext& DrawListContext, int32 Index)
	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheMeshDrawCommand, FColor::Green);

		struct FMeshInfoAndIndex
		{
			int32 InfoIndex;
			int32 MeshIndex;
		};

		TArray<FMeshInfoAndIndex, SceneRenderingAllocator> MeshBatches;
		MeshBatches.Reserve(3 * BATCH_SIZE);

		int LocalNum = FMath::Min((Index * BATCH_SIZE) + BATCH_SIZE, SceneInfos.Num());
		for (int LocalIndex = (Index * BATCH_SIZE); LocalIndex < LocalNum; LocalIndex++)
		{
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[LocalIndex];
			check(SceneInfo->StaticMeshCommandInfos.Num() == 0);
			SceneInfo->StaticMeshCommandInfos.AddDefaulted(EMeshPass::Num * SceneInfo->StaticMeshes.Num());
			FPrimitiveSceneProxy* SceneProxy = SceneInfo->Proxy;

			// Volumetric self shadow mesh commands need to be generated every frame, as they depend on single frame uniform buffers with self shadow data.
			if (!SceneProxy->CastsVolumetricTranslucentShadow())
			{
				for (int32 MeshIndex = 0; MeshIndex < SceneInfo->StaticMeshes.Num(); MeshIndex++)
				{
					FStaticMeshBatch& Mesh = SceneInfo->StaticMeshes[MeshIndex];
					if (SupportsCachingMeshDrawCommands(Mesh))
					{
						MeshBatches.Add(FMeshInfoAndIndex{ LocalIndex, MeshIndex });
					}
				}
			}
		}

		for (int32 PassIndex = 0; PassIndex < EMeshPass::Num; PassIndex++)
		{
			const EShadingPath ShadingPath = GetFeatureLevelShadingPath(Scene->GetFeatureLevel());
			EMeshPass::Type PassType = (EMeshPass::Type)PassIndex;

			if ((FPassProcessorManager::GetPassFlags(ShadingPath, PassType) & EMeshPassFlags::CachedMeshCommands) != EMeshPassFlags::None)
			{
				FCachedPassMeshDrawListContext::FMeshPassScope MeshPassScope(DrawListContext, PassType);

				FMeshPassProcessor* PassMeshProcessor = FPassProcessorManager::CreateMeshPassProcessor(ShadingPath, PassType, Scene->GetFeatureLevel(), Scene, nullptr, &DrawListContext);

				if (PassMeshProcessor != nullptr)
				{
					for (const FMeshInfoAndIndex& MeshAndInfo : MeshBatches)
					{
						FPrimitiveSceneInfo* SceneInfo = SceneInfos[MeshAndInfo.InfoIndex];
						FStaticMeshBatch& Mesh = SceneInfo->StaticMeshes[MeshAndInfo.MeshIndex];

						FStaticMeshBatchRelevance& MeshRelevance = SceneInfo->StaticMeshRelevances[MeshAndInfo.MeshIndex];

						check(!MeshRelevance.CommandInfosMask.Get(PassType));

						uint64 BatchElementMask = ~0ull;
						// NOTE: AddMeshBatch calls FCachedPassMeshDrawListContext::FinalizeCommand
						PassMeshProcessor->AddMeshBatch(Mesh, BatchElementMask, SceneInfo->Proxy);

						FCachedMeshDrawCommandInfo CommandInfo = DrawListContext.GetCommandInfoAndReset();
						if (CommandInfo.CommandIndex != -1 || CommandInfo.StateBucketId != -1)
						{
							static_assert(sizeof(MeshRelevance.CommandInfosMask) * 8 >= EMeshPass::Num, "CommandInfosMask is too small to contain all mesh passes.");
							MeshRelevance.CommandInfosMask.Set(PassType);
							MeshRelevance.CommandInfosBase++;

							int CommandInfoIndex = MeshAndInfo.MeshIndex * EMeshPass::Num + PassType;
							FCachedMeshDrawCommandInfo& CurrentCommandInfo = SceneInfo->StaticMeshCommandInfos[CommandInfoIndex];
							checkf(CurrentCommandInfo.MeshPass == EMeshPass::Num,
								TEXT("SceneInfo->StaticMeshCommandInfos[%d] is not expected to be initialized yet. MeshPass is %d, but expected EMeshPass::Num (%d)."),
								CommandInfoIndex, (int32)EMeshPass::Num, CurrentCommandInfo.MeshPass);
							CurrentCommandInfo = CommandInfo;
						}
					}

					delete PassMeshProcessor;
				}
			}
		}

		for (int LocalIndex = (Index * BATCH_SIZE); LocalIndex < LocalNum; LocalIndex++)
		{
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[LocalIndex];
			int PrefixSum = 0;
			for (int32 MeshIndex = 0; MeshIndex < SceneInfo->StaticMeshes.Num(); MeshIndex++)
			{
				FStaticMeshBatchRelevance& MeshRelevance = SceneInfo->StaticMeshRelevances[MeshIndex];
				if (MeshRelevance.CommandInfosBase > 0)
				{
					EMeshPass::Type PassType = EMeshPass::DepthPass;
					int NewPrefixSum = PrefixSum;
					for (;;)
					{
						PassType = MeshRelevance.CommandInfosMask.SkipEmpty(PassType);
						if (PassType == EMeshPass::Num)
						{
							break;
						}

						int CommandInfoIndex = MeshIndex * EMeshPass::Num + PassType;
						checkSlow(CommandInfoIndex >= NewPrefixSum);
						SceneInfo->StaticMeshCommandInfos[NewPrefixSum] = SceneInfo->StaticMeshCommandInfos[CommandInfoIndex];
						NewPrefixSum++;
						PassType = EMeshPass::Type(PassType + 1);
					}

#if DO_GUARD_SLOW
					int NumBits = MeshRelevance.CommandInfosMask.GetNum();
					check(PrefixSum + NumBits == NewPrefixSum);
					int LastPass = -1;
					for (int32 TestIndex = PrefixSum; TestIndex < NewPrefixSum; TestIndex++)
					{
						int MeshPass = SceneInfo->StaticMeshCommandInfos[TestIndex].MeshPass;
						check(MeshPass > LastPass);
						LastPass = MeshPass;
					}
#endif
					MeshRelevance.CommandInfosBase = PrefixSum;
					PrefixSum = NewPrefixSum;
				}
			}

			SceneInfo->StaticMeshCommandInfos.SetNum(PrefixSum, EAllowShrinking::No);
			SceneInfo->StaticMeshCommandInfos.Shrink();
		}
	};

	bool bAnyLooseParameterBuffers = false;
	if (GMeshDrawCommandsCacheMultithreaded && FApp::ShouldUseThreadingForPerformance())
	{
		TArray<FCachedPassMeshDrawListContextDeferred, SceneRenderingAllocator> DrawListContexts;
		DrawListContexts.Reserve(NumBatches);
		for(int32 ContextIndex = 0; ContextIndex < NumBatches; ++ContextIndex)
		{
			DrawListContexts.Emplace(*Scene);
		}

		ParallelForTemplate(
			NumBatches, 
			[&DrawListContexts, &DoWorkLambda](int32 Index)
			{
				FOptionalTaskTagScope Scope(ETaskTag::EParallelRenderingThread);
				DoWorkLambda(DrawListContexts[Index], Index);
			},
			EParallelForFlags::Unbalanced
		);

		if (NumBatches > 0)
		{
			SCOPED_NAMED_EVENT(DeferredFinalizeMeshDrawCommands, FColor::Emerald);

			for (int32 Index = 0; Index < NumBatches; ++Index)
			{
				FCachedPassMeshDrawListContextDeferred& DrawListContext = DrawListContexts[Index];
				const int32 Start = Index * BATCH_SIZE;
				const int32 End = FMath::Min((Index * BATCH_SIZE) + BATCH_SIZE, SceneInfos.Num());
				DrawListContext.DeferredFinalizeMeshDrawCommands(SceneInfos, Start, End);
				bAnyLooseParameterBuffers |= DrawListContext.HasAnyLooseParameterBuffers();
			}
		}
	}
	else
	{
		FCachedPassMeshDrawListContextImmediate DrawListContext(*Scene);
		for (int Idx = 0; Idx < NumBatches; Idx++)
		{
			DoWorkLambda(DrawListContext, Idx);
		}
		bAnyLooseParameterBuffers = DrawListContext.HasAnyLooseParameterBuffers();
	}

#if DO_GUARD_SLOW
	{
		static int32 LogCount = 0;
		if (bAnyLooseParameterBuffers && (LogCount++ % 1000) == 0)
		{
			UE_LOG(LogRenderer, Warning, TEXT("One or more Cached Mesh Draw commands use loose parameters. This causes overhead and will break dynamic instancing, potentially reducing performance further. Use Uniform Buffers instead."));
		}
	}
#endif

	if (!FParallelMeshDrawCommandPass::IsOnDemandShaderCreationEnabled())
	{
		FGraphicsMinimalPipelineStateId::InitializePersistentIds();
	}
}

void FPrimitiveSceneInfo::RemoveCachedMeshDrawCommands()
{
	checkSlow(IsInRenderingThread());

	for (int32 CommandIndex = 0; CommandIndex < StaticMeshCommandInfos.Num(); ++CommandIndex)
	{
		const FCachedMeshDrawCommandInfo& CachedCommand = StaticMeshCommandInfos[CommandIndex];

		if (CachedCommand.StateBucketId != INDEX_NONE)
		{
			EMeshPass::Type PassIndex = CachedCommand.MeshPass;
			FGraphicsMinimalPipelineStateId CachedPipelineId;

			{
				auto& ElementKVP = Scene->CachedMeshDrawCommandStateBuckets[PassIndex].GetByElementId(CachedCommand.StateBucketId);
				CachedPipelineId = ElementKVP.Key.CachedPipelineId;

				FMeshDrawCommandCount& StateBucketCount = ElementKVP.Value;
				check(StateBucketCount.Num > 0);
				StateBucketCount.Num--;
				if (StateBucketCount.Num == 0)
				{
					Scene->CachedMeshDrawCommandStateBuckets[PassIndex].RemoveByElementId(CachedCommand.StateBucketId);
				}
			}

			FGraphicsMinimalPipelineStateId::RemovePersistentId(CachedPipelineId);
		}
		else if (CachedCommand.CommandIndex >= 0)
		{
			FCachedPassMeshDrawList& PassDrawList = Scene->CachedDrawLists[CachedCommand.MeshPass];
			FGraphicsMinimalPipelineStateId CachedPipelineId = PassDrawList.MeshDrawCommands[CachedCommand.CommandIndex].CachedPipelineId;

			PassDrawList.MeshDrawCommands.RemoveAt(CachedCommand.CommandIndex);
			FGraphicsMinimalPipelineStateId::RemovePersistentId(CachedPipelineId);

			// Track the lowest index that might be free for faster AddAtLowestFreeIndex
			PassDrawList.LowestFreeIndexSearchStart = FMath::Min(PassDrawList.LowestFreeIndexSearchStart, CachedCommand.CommandIndex);
		}

	}

	for (int32 MeshIndex = 0; MeshIndex < StaticMeshRelevances.Num(); ++MeshIndex)
	{
		FStaticMeshBatchRelevance& MeshRelevance = StaticMeshRelevances[MeshIndex];

		MeshRelevance.CommandInfosMask.Reset();
	}

	StaticMeshCommandInfos.Empty();
}

static void BuildNaniteMaterialBins(FScene* Scene, FPrimitiveSceneInfo* PrimitiveSceneInfo, FNaniteDrawListContext& DrawListContext);

void FPrimitiveSceneInfo::CacheNaniteMaterialBins(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheNaniteMaterialBins, FColor::Emerald);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheNaniteMaterialBins);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_CacheNaniteMaterialBins);

	const bool bNaniteEnabled = DoesPlatformSupportNanite(GMaxRHIShaderPlatform);
	if (bNaniteEnabled)
	{
		TArray<FNaniteDrawListContext, TInlineAllocator<1>> DrawListContexts;

		if (GNaniteDrawCommandCacheMultithreaded && FApp::ShouldUseThreadingForPerformance())
		{
			ParallelForWithTaskContext(
				DrawListContexts,
				SceneInfos.Num(),
				[Scene, &SceneInfos](FNaniteDrawListContext& Context, int32 Index)
				{
					FOptionalTaskTagScope Scope(ETaskTag::EParallelRenderingThread);
					BuildNaniteMaterialBins(Scene, SceneInfos[Index], Context);
				}
			);
		}
		else
		{
			FNaniteDrawListContext& DrawListContext = DrawListContexts.AddDefaulted_GetRef();
			for (FPrimitiveSceneInfo* PrimitiveSceneInfo : SceneInfos)
			{
				BuildNaniteMaterialBins(Scene, PrimitiveSceneInfo, DrawListContext);
			}
		}

		FPrimitiveViewRelevance& CombinedPrimitiveRelevance = Scene->NaniteShadingPipelines[ENaniteMeshPass::BasePass].CombinedRelevance;
		CombinedPrimitiveRelevance = FPrimitiveViewRelevance();

		if (DrawListContexts.Num() > 0)
		{
			SCOPED_NAMED_EVENT(NaniteDrawListApply, FColor::Emerald);

			FMaterialRelevance CombinedMaterialRelevance;

			CombinedPrimitiveRelevance.bDrawRelevance = true;
			CombinedPrimitiveRelevance.bStaticRelevance = true;
			CombinedPrimitiveRelevance.bRenderInMainPass = true;
			CombinedPrimitiveRelevance.bShadowRelevance = true;

			// Nanite::GetSupportsCustomDepthRendering() && ShouldRenderCustomDepth();
			CombinedPrimitiveRelevance.bRenderCustomDepth = false; // TODO: Unsupported in fast path

			// GetLightingChannelMask() != GetDefaultLightingChannelMask();
			CombinedPrimitiveRelevance.bUsesLightingChannels = false; // TODO: Unsupported in fast path

			for (FNaniteDrawListContext& Context : DrawListContexts)
			{
				Context.Apply(*Scene);

				// Update combined material relevance
				CombinedMaterialRelevance |= Context.CombinedRelevance;
			}

			// Apply combined material relevance to combined primitive view relevance
			CombinedMaterialRelevance.SetPrimitiveViewRelevance(CombinedPrimitiveRelevance);
		}

		if (UseNaniteComputeMaterials())
		{
			Scene->NaniteShadingPipelines[ENaniteMeshPass::BasePass].bBuildCommands = true;
			Scene->NaniteShadingPipelines[ENaniteMeshPass::LumenCardCapture].bBuildCommands = true;
		}
	}
}

void BuildNaniteMaterialBins(FScene* Scene, FPrimitiveSceneInfo* PrimitiveSceneInfo, FNaniteDrawListContext& DrawListContext)
{
	const bool bUseComputeMaterials = UseNaniteComputeMaterials();

	FPrimitiveSceneProxy* Proxy = PrimitiveSceneInfo->Proxy;
	if (Proxy->IsNaniteMesh())
	{
		Nanite::FSceneProxyBase* NaniteProxy = static_cast<Nanite::FSceneProxyBase*>(Proxy);
		{
			FNaniteDrawListContext::FPrimitiveSceneInfoScope PrimInfoScope(DrawListContext, *PrimitiveSceneInfo);
	
			auto PassBody = [Scene, PrimitiveSceneInfo, NaniteProxy, &DrawListContext, bUseComputeMaterials](ENaniteMeshPass::Type MeshPass, FMeshPassProcessor* const NaniteMeshProcessor)
			{
				FNaniteDrawListContext::FMeshPassScope MeshPassScope(DrawListContext, MeshPass);

				int32 StaticMeshesCount = PrimitiveSceneInfo->StaticMeshes.Num();
				for (int32 MeshIndex = 0; MeshIndex < StaticMeshesCount; ++MeshIndex)
				{
					FStaticMeshBatchRelevance& MeshRelevance = PrimitiveSceneInfo->StaticMeshRelevances[MeshIndex];
					FStaticMeshBatch& Mesh = PrimitiveSceneInfo->StaticMeshes[MeshIndex];

					if (MeshRelevance.bSupportsNaniteRendering && Mesh.bUseForMaterial)
					{
						uint64 BatchElementMask = ~0ull;
						NaniteMeshProcessor->AddMeshBatch(Mesh, BatchElementMask, NaniteProxy);
					}
				}

				TArray<Nanite::FSceneProxyBase::FMaterialSection>& NaniteMaterialSections = NaniteProxy->GetMaterialSections();
				if (NaniteMaterialSections.Num() > 0)
				{
					FNaniteDrawListContext::FDeferredPipelines& PipelinesCommand = DrawListContext.DeferredPipelines[MeshPass].Emplace_GetRef();
					PipelinesCommand.PrimitiveSceneInfo = PrimitiveSceneInfo;

					DrawListContext.CombinedRelevance |= NaniteProxy->GetCombinedMaterialRelevance();

					for (int32 MaterialSectionIndex = 0; MaterialSectionIndex < NaniteMaterialSections.Num(); ++MaterialSectionIndex)
					{
						Nanite::FSceneProxyBase::FMaterialSection& MaterialSection = NaniteMaterialSections[MaterialSectionIndex];
						check(MaterialSection.RasterMaterialProxy != nullptr);
						check(MaterialSection.ShadingMaterialProxy != nullptr);

						FNaniteRasterPipeline& RasterPipeline = PipelinesCommand.RasterPipelines.Emplace_GetRef();
						RasterPipeline.RasterMaterial = MaterialSection.RasterMaterialProxy;
						RasterPipeline.bIsTwoSided = !!MaterialSection.MaterialRelevance.bTwoSided;
						RasterPipeline.bSplineMesh = NaniteProxy->IsSplineMesh();
						RasterPipeline.bPerPixelEval = MaterialSection.MaterialRelevance.bMasked ||
													   MaterialSection.MaterialRelevance.bUsesPixelDepthOffset;

						RasterPipeline.DisplacementScaling = MaterialSection.DisplacementScaling;

						float WPODisableDistance;
						RasterPipeline.bWPODisableDistance =
							MaterialSection.MaterialRelevance.bUsesWorldPositionOffset &&
							!MaterialSection.bAlwaysEvaluateWPO &&
							NaniteProxy->GetInstanceWorldPositionOffsetDisableDistance(WPODisableDistance);

						if (bUseComputeMaterials)
						{
							FNaniteShadingPipeline& ShadingPipeline = PipelinesCommand.ShadingPipelines.Emplace_GetRef();

							// TODO: Refactor this out
							bool bLoaded = false;
							if (MeshPass == ENaniteMeshPass::BasePass)
							{
								bLoaded = LoadBasePassPipeline(*Scene, NaniteProxy, MaterialSection, ShadingPipeline);
							}
							else if (MeshPass == ENaniteMeshPass::LumenCardCapture)
							{
								bLoaded = LoadLumenCardPipeline(*Scene, NaniteProxy, MaterialSection, ShadingPipeline);
							}

							check(bLoaded);
						}
					}
				}
			};

			// ENaniteMeshPass::BasePass
			{
				FMeshPassProcessor* NaniteMeshProcessor = CreateNaniteMeshProcessor(Scene->GetFeatureLevel(), Scene, nullptr, &DrawListContext);
				PassBody(ENaniteMeshPass::BasePass, NaniteMeshProcessor);
				delete NaniteMeshProcessor;
			}

			// ENaniteMeshPass::LumenCardCapture
			if (LumenScene::HasPrimitiveNaniteMeshBatches(Proxy) && DoesPlatformSupportLumenGI(GetFeatureLevelShaderPlatform(Scene->GetFeatureLevel())))
			{
				FMeshPassProcessor* NaniteMeshProcessor = CreateLumenCardNaniteMeshProcessor(Scene->GetFeatureLevel(), Scene, nullptr, &DrawListContext);
				PassBody(ENaniteMeshPass::LumenCardCapture, NaniteMeshProcessor);
				delete NaniteMeshProcessor;
			}

			static_assert(ENaniteMeshPass::Num == 2, "Change BuildNaniteMaterialBins() to account for more Nanite mesh passes");
		}
	}
}

void FPrimitiveSceneInfo::RemoveCachedNaniteMaterialBins()
{
	checkSlow(IsInRenderingThread());

	if (!Proxy->IsNaniteMesh())
	{
		return;
	}

	QUICK_SCOPE_CYCLE_COUNTER(STAT_RemoveCachedNaniteMaterialBins);

	for (int32 NaniteMeshPassIndex = 0; NaniteMeshPassIndex < ENaniteMeshPass::Num; ++NaniteMeshPassIndex)
	{
		FNaniteMaterialCommands& ShadingCommands = Scene->NaniteMaterials[NaniteMeshPassIndex];
		FNaniteRasterPipelines& RasterPipelines = Scene->NaniteRasterPipelines[NaniteMeshPassIndex];
		FNaniteShadingPipelines& ShadingPipelines = Scene->NaniteShadingPipelines[NaniteMeshPassIndex];
		FNaniteVisibility& Visibility = Scene->NaniteVisibility[NaniteMeshPassIndex];

		TArray<FNaniteCommandInfo>& NanitePassCommandInfo = NaniteCommandInfos[NaniteMeshPassIndex];
		for (int32 CommandIndex = 0; CommandIndex < NanitePassCommandInfo.Num(); ++CommandIndex)
		{
			const FNaniteCommandInfo& CommandInfo = NanitePassCommandInfo[CommandIndex];
			ShadingCommands.Unregister(CommandInfo);
		}

		TArray<FNaniteRasterBin>& NanitePassRasterBins = NaniteRasterBins[NaniteMeshPassIndex];
		for (int32 RasterBinIndex = 0; RasterBinIndex < NanitePassRasterBins.Num(); ++RasterBinIndex)
		{
			const FNaniteRasterBin& RasterBin = NanitePassRasterBins[RasterBinIndex];
			if (NaniteMeshPassIndex == ENaniteMeshPass::BasePass && bNaniteRasterBinsRenderCustomDepth)
			{
				// need to unregister these bins for custom pass first
				RasterPipelines.UnregisterBinForCustomPass(RasterBin.BinIndex);
			}
			RasterPipelines.Unregister(RasterBin);
		}

		TArray<FNaniteShadingBin>& NanitePassShadingBins = NaniteShadingBins[NaniteMeshPassIndex];
		for (int32 ShadingBinIndex = 0; ShadingBinIndex < NanitePassShadingBins.Num(); ++ShadingBinIndex)
		{
			const FNaniteShadingBin& ShadingBin = NanitePassShadingBins[ShadingBinIndex];
			ShadingPipelines.Unregister(ShadingBin);
		}

		// Need to rebuild the shading commands list
		ShadingPipelines.bBuildCommands = true;

		Visibility.RemoveReferences(this);

		NanitePassRasterBins.Reset();
		NanitePassShadingBins.Reset();
		NanitePassCommandInfo.Reset();
		NaniteMaterialSlots[NaniteMeshPassIndex].Reset();
	}

	bNaniteRasterBinsRenderCustomDepth = false;
}

#if RHI_RAYTRACING
void FScene::RefreshRayTracingMeshCommandCache()
{
	// Get rid of all existing cached commands
	CachedRayTracingMeshCommands.Empty(CachedRayTracingMeshCommands.Num());

	// Re-cache all current primitives
	FPrimitiveSceneInfo::CacheRayTracingPrimitives(this, Primitives);
}

void FScene::RefreshRayTracingInstances()
{
	// Re-cache all current primitives
	FPrimitiveSceneInfo::UpdateCachedRayTracingInstances(this, Primitives);
}

void FScene::UpdateRayTracedLights()
{
	// Whether a light can use ray traced shadows depends on CVars that may be changed at runtime.
	// It is not enough to check the light shadow mode when light is added to the scene. This must be done during rendering.

	bHasRayTracedLights = false;

	if (!IsRayTracingEnabled())
	{
		return;
	}

	// We currently don't need a full list of RT lights, only whether there are any RT lights at all.
	for (const FLightSceneInfoCompact& LightSceneInfoCompact : Lights)
	{
		if (ShouldRenderRayTracingShadowsForLight(LightSceneInfoCompact))
		{
			bHasRayTracedLights = true;
			break;
		}
	}
}

void FPrimitiveSceneInfo::UpdateCachedRayTracingInstances(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (IsRayTracingEnabled())
	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_UpdateCachedRayTracingInstances, FColor::Turquoise);
		checkf(GRHISupportsMultithreadedShaderCreation, TEXT("Raytracing code needs the ability to create shaders from task threads."));

		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			const int32 PrimitiveIndex = SceneInfo->GetIndex();

#if DO_CHECK
			{
				Experimental::FHashElementId SceneRayTracingGroupId;
				const int32 RayTracingGroupId = SceneInfo->Proxy->GetRayTracingGroupId();
				if (RayTracingGroupId != -1)
				{
					SceneRayTracingGroupId = Scene->PrimitiveRayTracingGroups.FindId(RayTracingGroupId);
				}

				check(Scene->PrimitiveRayTracingGroupIds[PrimitiveIndex] == SceneRayTracingGroupId);
				check(SceneInfo->CoarseMeshStreamingHandle == SceneInfo->Proxy->GetCoarseMeshStreamingHandle());
			}
#endif

			FRayTracingInstance CachedRayTracingInstance;
			ERayTracingPrimitiveFlags& Flags = Scene->PrimitiveRayTracingFlags[PrimitiveIndex];

			// Write flags
			Flags = SceneInfo->Proxy->GetCachedRayTracingInstance(CachedRayTracingInstance);
			UpdateCachedRayTracingInstance(SceneInfo, CachedRayTracingInstance, Flags);
		}
	}
}

struct DeferredMeshLODCommandIndex
{
	FPrimitiveSceneInfo* SceneInfo;
	int8 MeshLODIndex;
	int32 CommandIndex;
};

template<class T>
class FCacheRayTracingPrimitivesContext
{
public:
	FCacheRayTracingPrimitivesContext(FScene* Scene)
		: CommandContext(Commands)
		, RayTracingMeshProcessor(&CommandContext, Scene, nullptr, Scene->CachedRayTracingMeshCommandsMode)
	{ }

	FTempRayTracingMeshCommandStorage Commands;
	FCachedRayTracingMeshCommandContext<T> CommandContext;
	FRayTracingMeshProcessor RayTracingMeshProcessor;
	TArray<DeferredMeshLODCommandIndex> DeferredMeshLODCommandIndices;
};

template<bool bDeferLODCommandIndices, class T>
void CacheRayTracingMeshBatch(
	const FMeshBatch& MeshBatch,
	FPrimitiveSceneInfo* SceneInfo,
	T& Commands,
	FCachedRayTracingMeshCommandContext<T>& CommandContext,
	FRayTracingMeshProcessor& RayTracingMeshProcessor,
	TArray<DeferredMeshLODCommandIndex>* DeferredMeshLODCommandIndices,
	bool bMustEmitCommand)
{
	// Why do we pass a full mask here when the dynamic case only uses a mask of 1?
	// Also note that the code below assumes only a single command was generated per batch (see SupportsCachingMeshDrawCommands(...))
	const uint64 BatchElementMask = ~0ull;
	RayTracingMeshProcessor.AddMeshBatch(MeshBatch, BatchElementMask, SceneInfo->Proxy);

	check(!bMustEmitCommand || CommandContext.CommandIndex >= 0);

	if (bMustEmitCommand || CommandContext.CommandIndex >= 0)
	{
		uint64& Hash = SceneInfo->CachedRayTracingMeshCommandsHashPerLOD[MeshBatch.LODIndex];

		// We want the hash to change if either the shader or the binding contents change. This is used by the autoinstance feature.
		const FRHIShader* Shader = Commands[CommandContext.CommandIndex].MaterialShader;

		// TODO: It would be better to use 64 bits for both of these to reduce the chance of hash collisions
		//       but GetDynamicInstancingHash is currently a public function, so changing the return type would be an API change
		uint32 ShaderHash = Shader != nullptr ? GetTypeHash(Shader->GetHash()) : 0;
		uint32 ShaderBindingsHash = Commands[CommandContext.CommandIndex].ShaderBindings.GetDynamicInstancingHash();

		Hash <<= 1; // TODO: It would probably be better to use some kind of proper 64 bit mix here?
		Hash ^= (uint64(ShaderBindingsHash) << 32) | uint64(ShaderHash);

		if (bDeferLODCommandIndices)
		{
			DeferredMeshLODCommandIndices->Add({ SceneInfo, MeshBatch.LODIndex, CommandContext.CommandIndex });
		}
		else
		{
			SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[MeshBatch.LODIndex].Add(CommandContext.CommandIndex);
		}

		CommandContext.CommandIndex = -1;
	}
}

template<bool bDeferLODCommandIndices, class T>
void CacheRayTracingPrimitive(
	FScene* Scene, 
	FPrimitiveSceneInfo* SceneInfo,
	T& Commands,
	FCachedRayTracingMeshCommandContext<T>& CommandContext,
	FRayTracingMeshProcessor& RayTracingMeshProcessor,
	TArray<DeferredMeshLODCommandIndex>* DeferredMeshLODCommandIndices,
	FRayTracingInstance& OutCachedRayTracingInstance, 
	ERayTracingPrimitiveFlags& OutFlags)
{
#if DO_CHECK
	{
		Experimental::FHashElementId SceneRayTracingGroupId;
		const int32 RayTracingGroupId = SceneInfo->Proxy->GetRayTracingGroupId();
		if (RayTracingGroupId != -1)
		{
			SceneRayTracingGroupId = Scene->PrimitiveRayTracingGroups.FindId(RayTracingGroupId);
		}

		check(Scene->PrimitiveRayTracingGroupIds[SceneInfo->GetIndex()] == SceneRayTracingGroupId);
		check(SceneInfo->CoarseMeshStreamingHandle == SceneInfo->Proxy->GetCoarseMeshStreamingHandle());
	}
#endif

	// Write flags
	OutFlags = SceneInfo->Proxy->GetCachedRayTracingInstance(OutCachedRayTracingInstance);

	// the following flags cause ray tracing mesh command caching to be disabled
	static const ERayTracingPrimitiveFlags DisableCacheMeshCommandsFlags = ERayTracingPrimitiveFlags::Dynamic
		| ERayTracingPrimitiveFlags::Exclude
		| ERayTracingPrimitiveFlags::Skip
		| ERayTracingPrimitiveFlags::UnsupportedProxyType;

	if (!EnumHasAnyFlags(OutFlags, DisableCacheMeshCommandsFlags))
	{
		// Cache ray tracing mesh commands in FPrimitiveSceneInfo

		int32 MaxLOD = -1;

		if (OutCachedRayTracingInstance.Materials.Num() > 0)
		{
			// TODO: LOD w/ screen size support. Probably needs another array parallel to OutRayTracingInstances
			// We assume it is exactly 1 LOD now (true for Nanite proxies)
			MaxLOD = 0;
		}
		else
		{
			for (const FStaticMeshBatch& Mesh : SceneInfo->StaticMeshes)
			{
				MaxLOD = MaxLOD < Mesh.LODIndex ? Mesh.LODIndex : MaxLOD;
			}
		}

		SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.Empty(MaxLOD + 1);
		SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD.AddDefaulted(MaxLOD + 1); // should be initialzied to -1?

		SceneInfo->CachedRayTracingMeshCommandsHashPerLOD.Empty(MaxLOD + 1);
		SceneInfo->CachedRayTracingMeshCommandsHashPerLOD.AddZeroed(MaxLOD + 1);

		if (OutCachedRayTracingInstance.Materials.Num() > 0)
		{
			// The material section must emit a command. Otherwise, it should have been excluded earlier
			const bool bMustEmitCommand = true;

			for (const FMeshBatch& Mesh : OutCachedRayTracingInstance.Materials)
			{
				CacheRayTracingMeshBatch<bDeferLODCommandIndices>(Mesh, SceneInfo, Commands, CommandContext, RayTracingMeshProcessor, DeferredMeshLODCommandIndices, bMustEmitCommand);
			}
		}
		else
		{
			const bool bMustEmitCommand = false;

			for (const FStaticMeshBatch& Mesh : SceneInfo->StaticMeshes)
			{
				CacheRayTracingMeshBatch<bDeferLODCommandIndices>(Mesh, SceneInfo, Commands, CommandContext, RayTracingMeshProcessor, DeferredMeshLODCommandIndices, bMustEmitCommand);
			}
		}
	}
}

void FPrimitiveSceneInfo::CacheRayTracingPrimitives(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (IsRayTracingEnabled(Scene->GetShaderPlatform()))
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheRayTracingPrimitives)
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheRayTracingPrimitives, FColor::Emerald);

		checkf(GRHISupportsMultithreadedShaderCreation, TEXT("Raytracing code needs the ability to create shaders from task threads."));
		
		FCachedRayTracingMeshCommandStorage& CachedRayTracingMeshCommands = Scene->CachedRayTracingMeshCommands;

		if (GRayTracingPrimitiveCacheMultithreaded && FApp::ShouldUseThreadingForPerformance())
		{
			TArray<FCacheRayTracingPrimitivesContext<FTempRayTracingMeshCommandStorage>> Contexts;
			ParallelForWithTaskContext(
				Contexts,
				SceneInfos.Num(),
				[Scene](int32 ContextIndex, int32 NumContexts) { return Scene; },
				[Scene, &SceneInfos](FCacheRayTracingPrimitivesContext<FTempRayTracingMeshCommandStorage>& Context, int32 Index)
				{
					FOptionalTaskTagScope Scope(ETaskTag::EParallelRenderingThread);

					FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index];
					FRayTracingInstance CachedInstance;
					ERayTracingPrimitiveFlags& Flags = Scene->PrimitiveRayTracingFlags[SceneInfo->GetIndex()];
					CacheRayTracingPrimitive<true>(Scene, SceneInfo, Context.Commands, Context.CommandContext, Context.RayTracingMeshProcessor, &Context.DeferredMeshLODCommandIndices, CachedInstance, Flags);
					UpdateCachedRayTracingInstance(SceneInfo, CachedInstance, Flags);
					SceneInfo->bCachedRaytracingDataDirty = false;
				}
			);

			if (Contexts.Num() > 0)
			{
				CSV_SCOPED_TIMING_STAT_EXCLUSIVE(FPrimitiveSceneInfo_CacheRayTracingPrimitives_Merge)
				SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_CacheRayTracingPrimitives_Merge, FColor::Emerald);

				// copy commands generated by multiple threads to the sparse array in FScene
				// and set each mesh LOD command index
				for (const auto& Context : Contexts)
				{
					for (const DeferredMeshLODCommandIndex& Entry : Context.DeferredMeshLODCommandIndices)
					{
						int32 CommandIndex = CachedRayTracingMeshCommands.Add(Context.Commands[Entry.CommandIndex]);
						Entry.SceneInfo->CachedRayTracingMeshCommandIndicesPerLOD[Entry.MeshLODIndex].Add(CommandIndex);
					}
				}
			}
		}
		else
		{
			FCachedRayTracingMeshCommandContext CommandContext(CachedRayTracingMeshCommands);
			FRayTracingMeshProcessor RayTracingMeshProcessor(&CommandContext, Scene, nullptr, Scene->CachedRayTracingMeshCommandsMode);

			for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
			{
				FRayTracingInstance CachedRayTracingInstance;
				ERayTracingPrimitiveFlags& Flags = Scene->PrimitiveRayTracingFlags[SceneInfo->GetIndex()];
				CacheRayTracingPrimitive<false>(Scene, SceneInfo, CachedRayTracingMeshCommands, CommandContext, RayTracingMeshProcessor, nullptr, CachedRayTracingInstance, Flags);
				UpdateCachedRayTracingInstance(SceneInfo, CachedRayTracingInstance, Flags);
				SceneInfo->bCachedRaytracingDataDirty = false;
			}
		}
	}
}

void FPrimitiveSceneInfo::UpdateCachedRayTracingInstance(FPrimitiveSceneInfo* SceneInfo, const FRayTracingInstance& CachedRayTracingInstance, const ERayTracingPrimitiveFlags Flags)
{
	if (EnumHasAnyFlags(Flags, ERayTracingPrimitiveFlags::CacheInstances))
	{
		checkf(CachedRayTracingInstance.InstanceTransforms.IsEmpty() && CachedRayTracingInstance.InstanceTransformsView.IsEmpty(),
			TEXT("Primitives with ERayTracingPrimitiveFlags::CacheInstances get instances transforms from GPUScene"));

		FPrimitiveSceneProxy* SceneProxy = SceneInfo->Proxy;

		// TODO: allocate from FRayTracingScene & do better low-level caching
		SceneInfo->CachedRayTracingInstance.NumTransforms = CachedRayTracingInstance.NumTransforms;
		SceneInfo->CachedRayTracingInstance.BaseInstanceSceneDataOffset = SceneInfo->GetInstanceSceneDataOffset();

		SceneInfo->CachedRayTracingGeometry = CachedRayTracingInstance.Geometry;

		if (Nanite::GetRayTracingMode() != Nanite::ERayTracingMode::Fallback && SceneProxy->IsNaniteMesh())
		{
			SceneInfo->CachedRayTracingInstance.GeometryRHI = Nanite::GRayTracingManager.GetRayTracingGeometry(SceneInfo);

			// nanite ray tracing geometry might not be ready yet
			// if not ready, this pointer will be patched as soon as it is
		}
		else
		{
			checkf(CachedRayTracingInstance.Geometry, TEXT("Cached ray tracing instances must have valid geometries.")); // unless using nanite ray tracing

			SceneInfo->CachedRayTracingInstance.GeometryRHI = CachedRayTracingInstance.Geometry->RayTracingGeometryRHI;
		}

		// At this point (in AddToScene()) PrimitiveIndex has been set
		check(SceneInfo->GetPersistentIndex().IsValid());
		SceneInfo->CachedRayTracingInstance.DefaultUserData = (uint32)SceneInfo->GetPersistentIndex().Index;

		SceneInfo->CachedRayTracingInstance.bApplyLocalBoundsTransform = CachedRayTracingInstance.bApplyLocalBoundsTransform;

		SceneInfo->CachedRayTracingInstance.Flags = ERayTracingInstanceFlags::None;

		FRayTracingMaskAndFlags InstanceMaskAndFlags;

		// TODO: Check CachedRayTracingInstance.bInstanceMaskAndFlagsDirty?

		if (CachedRayTracingInstance.GetMaterials().IsEmpty())
		{
		 	// If the material list is empty, explicitly set the mask to 0 so it will not be added in the raytracing scene
			InstanceMaskAndFlags.Mask = 0;
		}
		else
		{
			InstanceMaskAndFlags = BuildRayTracingInstanceMaskAndFlags(CachedRayTracingInstance, *SceneProxy, nullptr);
		}

		SceneInfo->CachedRayTracingInstance.Mask = InstanceMaskAndFlags.Mask; // When no cached command is found, InstanceMask == 0 and the instance is effectively filtered out

		SceneInfo->CachedRayTracingInstance.Flags = ERayTracingInstanceFlags::None;

		if (InstanceMaskAndFlags.bForceOpaque)
		{
			SceneInfo->CachedRayTracingInstance.Flags |= ERayTracingInstanceFlags::ForceOpaque;
		}

		if (InstanceMaskAndFlags.bDoubleSided)
		{
			SceneInfo->CachedRayTracingInstance.Flags |= ERayTracingInstanceFlags::TriangleCullDisable;
		}

		SceneInfo->bCachedRayTracingInstanceAnySegmentsDecal = InstanceMaskAndFlags.bAnySegmentsDecal;
		SceneInfo->bCachedRayTracingInstanceAllSegmentsDecal = InstanceMaskAndFlags.bAllSegmentsDecal;

		SceneInfo->bCachedRayTracingInstanceMaskAndFlagsDirty = false;
	}
	else
	{
		SceneInfo->CachedRayTracingInstance.Mask = 0xFF;
		SceneInfo->CachedRayTracingInstance.Flags = ERayTracingInstanceFlags::None;
		SceneInfo->bCachedRayTracingInstanceAnySegmentsDecal = false;
		SceneInfo->bCachedRayTracingInstanceAllSegmentsDecal = false;

		SceneInfo->bCachedRayTracingInstanceMaskAndFlagsDirty = true;
	}
}

void FPrimitiveSceneInfo::RemoveCachedRayTracingPrimitives()
{
	if (IsRayTracingEnabled())
	{
		for (auto& CachedRayTracingMeshCommandIndices : CachedRayTracingMeshCommandIndicesPerLOD)
		{
			for (auto CommandIndex : CachedRayTracingMeshCommandIndices)
			{
				if (CommandIndex >= 0)
				{
					Scene->CachedRayTracingMeshCommands.RemoveAt(CommandIndex);
				}
			}
		}

		CachedRayTracingMeshCommandIndicesPerLOD.Empty();

		CachedRayTracingMeshCommandsHashPerLOD.Empty();
	}
}
#endif

void FPrimitiveSceneInfo::AddStaticMeshes(FRHICommandListBase& RHICmdList, FScene* Scene, TArrayView<FPrimitiveSceneInfo*> SceneInfos, bool bCacheMeshDrawCommands)
{
	LLM_SCOPE(ELLMTag::StaticMesh);

	{
		ParallelForTemplate(SceneInfos.Num(), [Scene, &SceneInfos](int32 Index)
		{
			FOptionalTaskTagScope Scope(ETaskTag::EParallelRenderingThread);
			SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddStaticMeshes_DrawStaticElements, FColor::Magenta);
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index];
			// Cache the primitive's static mesh elements.
			FBatchingSPDI BatchingSPDI(SceneInfo);
			BatchingSPDI.SetHitProxy(SceneInfo->DefaultDynamicHitProxy);
			SceneInfo->Proxy->DrawStaticElements(&BatchingSPDI);
			SceneInfo->StaticMeshes.Shrink();
			SceneInfo->StaticMeshRelevances.Shrink();
			SceneInfo->bPendingAddStaticMeshes = false;

			check(SceneInfo->StaticMeshRelevances.Num() == SceneInfo->StaticMeshes.Num());
		});
	}

	{
		const ERHIFeatureLevel::Type FeatureLevel = Scene->GetFeatureLevel();

		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddStaticMeshes_UpdateSceneArrays, FColor::Blue);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			// Allocate OIT index buffer where needed
			const bool bAllocateSortedTriangles = OIT::IsSortedTrianglesEnabled(GMaxRHIShaderPlatform) && SceneInfo->Proxy->SupportsSortedTriangles();

			for (int32 MeshIndex = 0; MeshIndex < SceneInfo->StaticMeshes.Num(); MeshIndex++)
			{
				FStaticMeshBatchRelevance& MeshRelevance = SceneInfo->StaticMeshRelevances[MeshIndex];
				FStaticMeshBatch& Mesh = SceneInfo->StaticMeshes[MeshIndex];

				// Add the static mesh to the scene's static mesh list.
				FSparseArrayAllocationInfo SceneArrayAllocation = Scene->StaticMeshes.AddUninitialized();
				Scene->StaticMeshes[SceneArrayAllocation.Index] = &Mesh;
				Mesh.Id = SceneArrayAllocation.Index;
				MeshRelevance.Id = SceneArrayAllocation.Index;

				if (bAllocateSortedTriangles && OIT::IsCompatible(Mesh, FeatureLevel))
				{
					Scene->OITSceneData.Allocate(RHICmdList, EPrimitiveType(Mesh.Type), Mesh.Elements[0], Mesh.Elements[0].DynamicIndexBuffer);
				}
			}
		}
	}

	if (bCacheMeshDrawCommands)
	{
		CacheMeshDrawCommands(Scene, SceneInfos);
		CacheNaniteMaterialBins(Scene, SceneInfos);
	#if RHI_RAYTRACING
		CacheRayTracingPrimitives(Scene, SceneInfos);
	#endif
	}
}

static void OnVirtualTextureDestroyed(const FVirtualTextureProducerHandle& InHandle, void* Baton)
{
	FPrimitiveSceneInfo* PrimitiveSceneInfo = static_cast<FPrimitiveSceneInfo*>(Baton);

	// Update the main uniform buffer
	PrimitiveSceneInfo->UpdateStaticLightingBuffer();

	// Also need to update lightmap data inside GPUScene, if that's enabled
	PrimitiveSceneInfo->Scene->GPUScene.AddPrimitiveToUpdate(PrimitiveSceneInfo->GetPersistentIndex(), EPrimitiveDirtyState::ChangedStaticLighting);
}

static void GetRuntimeVirtualTextureLODRange(TArray<class FStaticMeshBatchRelevance> const& MeshRelevances, int8& OutMinLOD, int8& OutMaxLOD)
{
	OutMinLOD = MAX_int8;
	OutMaxLOD = 0;

	for (int32 MeshIndex = 0; MeshIndex < MeshRelevances.Num(); ++MeshIndex)
	{
		const FStaticMeshBatchRelevance& MeshRelevance = MeshRelevances[MeshIndex];
		if (MeshRelevance.bRenderToVirtualTexture)
		{
			OutMinLOD = FMath::Min(OutMinLOD, MeshRelevance.GetLODIndex());
			OutMaxLOD = FMath::Max(OutMaxLOD, MeshRelevance.GetLODIndex());
		}
	}

	check(OutMinLOD <= OutMaxLOD);
}

int32 FPrimitiveSceneInfo::UpdateStaticLightingBuffer()
{
	checkSlow(IsInRenderingThread());

	if (bRegisteredVirtualTextureProducerCallback)
	{
		// Remove any previous VT callbacks
		FVirtualTextureSystem::Get().RemoveAllProducerDestroyedCallbacks(this);
		bRegisteredVirtualTextureProducerCallback = false;
	}

	FPrimitiveSceneProxy::FLCIArray LCIs;
	Proxy->GetLCIs(LCIs);
	for (int32 i = 0; i < LCIs.Num(); ++i)
	{
		FLightCacheInterface* LCI = LCIs[i];

		if (LCI)
		{
			LCI->CreatePrecomputedLightingUniformBuffer_RenderingThread(Scene->GetFeatureLevel());

			// If lightmap is using virtual texture, need to set a callback to update our uniform buffers if VT is destroyed,
			// since we cache VT parameters inside these uniform buffers
			FVirtualTextureProducerHandle VTProducerHandle;
			if (LCI->GetVirtualTextureLightmapProducer(Scene->GetFeatureLevel(), VTProducerHandle))
			{
				FVirtualTextureSystem::Get().AddProducerDestroyedCallback(VTProducerHandle, &OnVirtualTextureDestroyed, this);
				bRegisteredVirtualTextureProducerCallback = true;
			}
		}
	}

	return LCIs.Num();
}

void FPrimitiveSceneInfo::AllocateGPUSceneInstances(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (Scene->GPUScene.IsEnabled())
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateGPUSceneTime);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			check
			(
				SceneInfo->InstanceSceneDataOffset == INDEX_NONE &&
				SceneInfo->NumInstanceSceneDataEntries == 0 &&
				SceneInfo->InstancePayloadDataOffset == INDEX_NONE &&
				SceneInfo->InstancePayloadDataStride == 0
			);

			// Note: this will return 1 instance for primitives without the instance data buffer.
			FInstanceDataBufferHeader InstanceDataHeader = SceneInfo->GetInstanceDataHeader();
			SceneInfo->NumInstanceSceneDataEntries = InstanceDataHeader.NumInstances;
				if (SceneInfo->NumInstanceSceneDataEntries > 0)
				{
					SceneInfo->InstanceSceneDataOffset = Scene->GPUScene.AllocateInstanceSceneDataSlots(SceneInfo->NumInstanceSceneDataEntries);
					SceneInfo->InstancePayloadDataStride = InstanceDataHeader.PayloadDataStride;
					if (SceneInfo->InstancePayloadDataStride > 0)
					{
						const uint32 TotalFloat4Count = SceneInfo->NumInstanceSceneDataEntries * SceneInfo->InstancePayloadDataStride;
						SceneInfo->InstancePayloadDataOffset = Scene->GPUScene.AllocateInstancePayloadDataSlots(TotalFloat4Count);
					}
				}
				
			// Force a primitive update in the GPU scene, 
			// NOTE: does not set Added as this is handled elsewhere.
			Scene->GPUScene.AddPrimitiveToUpdate(SceneInfo->GetPersistentIndex(), EPrimitiveDirtyState::ChangedAll);

			// Force a primitive update in the Lumen scene(s)
			for (FLumenSceneDataIterator LumenSceneData = Scene->GetLumenSceneDataIterator(); LumenSceneData; ++LumenSceneData)
			{
				LumenSceneData->UpdatePrimitiveInstanceOffset(SceneInfo->PackedIndex);
			}
		}

		OnGPUSceneInstancesAllocated.Broadcast();
	}
}

void FPrimitiveSceneInfo::ReallocateGPUSceneInstances(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	SCOPED_NAMED_EVENT(ReallocateGPUSceneInstances, FColor::Emerald);

	// Free each scene info.
	for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
	{
		SceneInfo->FreeGPUSceneInstances();
	}

	// Allocate them all.
	AllocateGPUSceneInstances(Scene, SceneInfos);
}

void FPrimitiveSceneInfo::FreeGPUSceneInstances()
{
	if (!Scene->GPUScene.IsEnabled())
	{
		return;
	}

	// Release all instance data slots associated with this primitive.
	if (InstanceSceneDataOffset != INDEX_NONE)
	{
		SCOPE_CYCLE_COUNTER(STAT_UpdateGPUSceneTime);

		check(Proxy->SupportsInstanceDataBuffer() || NumInstanceSceneDataEntries == 1);

		// Release all instance payload data slots associated with this primitive.
		if (InstancePayloadDataOffset != INDEX_NONE)
		{
			check(InstancePayloadDataStride > 0);

			const uint32 TotalFloat4Count = NumInstanceSceneDataEntries * InstancePayloadDataStride;
			Scene->GPUScene.FreeInstancePayloadDataSlots(InstancePayloadDataOffset, TotalFloat4Count);
			InstancePayloadDataOffset = INDEX_NONE;
			InstancePayloadDataStride = 0;
		}

		Scene->GPUScene.FreeInstanceSceneDataSlots(InstanceSceneDataOffset, NumInstanceSceneDataEntries);
		InstanceSceneDataOffset = INDEX_NONE;
		NumInstanceSceneDataEntries = 0;

		OnGPUSceneInstancesFreed.Broadcast();
	}
}

void FPrimitiveSceneInfo::AddToScene(FScene* Scene, TArrayView<FPrimitiveSceneInfo*> SceneInfos)
{
	check(IsInRenderingThread());
	SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene, FColor::Turquoise);

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_IndirectLightingCacheUniformBuffer, FColor::Turquoise);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
			// Create an indirect lighting cache uniform buffer if we attaching a primitive that may require it, as it may be stored inside a cached mesh command.
			if (IsIndirectLightingCacheAllowed(Scene->GetFeatureLevel())
				&& Proxy->WillEverBeLit()
				&& ((Proxy->HasStaticLighting() && Proxy->NeedsUnbuiltPreviewLighting()) || (Proxy->IsMovable() && Proxy->GetIndirectLightingCacheQuality() != ILCQ_Off) || Proxy->GetLightmapType() == ELightmapType::ForceVolumetric))
			{
				if (!SceneInfo->IndirectLightingCacheUniformBuffer)
				{
					FIndirectLightingCacheUniformParameters Parameters;

					GetIndirectLightingCacheParameters(
						Scene->GetFeatureLevel(),
						Parameters,
						nullptr,
						nullptr,
						FVector(0.0f, 0.0f, 0.0f),
						0,
						nullptr);

					SceneInfo->IndirectLightingCacheUniformBuffer = TUniformBufferRef<FIndirectLightingCacheUniformParameters>::CreateUniformBufferImmediate(Parameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
				}
			}

			SceneInfo->bPendingAddToScene = false;
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_IndirectLightingCacheAllocation, FColor::Orange);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
			// If we are attaching a primitive that should be statically lit but has unbuilt lighting,
			// Allocate space in the indirect lighting cache so that it can be used for previewing indirect lighting
			if (Proxy->HasStaticLighting()
				&& Proxy->NeedsUnbuiltPreviewLighting()
				&& IsIndirectLightingCacheAllowed(Scene->GetFeatureLevel()))
			{
				FIndirectLightingCacheAllocation* PrimitiveAllocation = Scene->IndirectLightingCache.FindPrimitiveAllocation(SceneInfo->PrimitiveComponentId);

				if (PrimitiveAllocation)
				{
					SceneInfo->IndirectLightingCacheAllocation = PrimitiveAllocation;
					PrimitiveAllocation->SetDirty();
				}
				else
				{
					PrimitiveAllocation = Scene->IndirectLightingCache.AllocatePrimitive(SceneInfo, true);
					PrimitiveAllocation->SetDirty();
					SceneInfo->IndirectLightingCacheAllocation = PrimitiveAllocation;
				}
			}
			SceneInfo->MarkIndirectLightingCacheBufferDirty();
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_LightmapDataOffset, FColor::Green);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			const bool bAllowStaticLighting = IsStaticLightingAllowed();
			if (bAllowStaticLighting)
			{
				SceneInfo->NumLightmapDataEntries = SceneInfo->UpdateStaticLightingBuffer();
				if (SceneInfo->NumLightmapDataEntries > 0 && UseGPUScene(GMaxRHIShaderPlatform, Scene->GetFeatureLevel()))
				{
					SceneInfo->LightmapDataOffset = Scene->GPUScene.LightmapDataAllocator.Allocate(SceneInfo->NumLightmapDataEntries);
				}
			}
		}
	}


	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_ReflectionCaptures, FColor::Yellow);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			// Cache the nearest reflection proxy if needed
			if (SceneInfo->NeedsReflectionCaptureUpdate())
			{
				SceneInfo->CacheReflectionCaptures();
			}
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_AddToPrimitiveOctree, FColor::Red);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			// create potential storage for our compact info
			FPrimitiveSceneInfoCompact CompactPrimitiveSceneInfo(SceneInfo);

			// Add the primitive to the octree.
			check(!SceneInfo->OctreeId.IsValidId());
			Scene->PrimitiveOctree.AddElement(CompactPrimitiveSceneInfo);
			check(SceneInfo->OctreeId.IsValidId());
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_UpdateBounds, FColor::Cyan);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
			int32 PackedIndex = SceneInfo->PackedIndex;

			if (Proxy->CastsDynamicIndirectShadow())
			{
				Scene->DynamicIndirectCasterPrimitives.Add(SceneInfo);
			}

			Scene->PrimitiveSceneProxies[PackedIndex] = Proxy;
			Scene->PrimitiveTransforms[PackedIndex] = Proxy->GetLocalToWorld();

			// Set bounds.
			FPrimitiveBounds& PrimitiveBounds = Scene->PrimitiveBounds[PackedIndex];
			FBoxSphereBounds BoxSphereBounds = Proxy->GetBounds();
			PrimitiveBounds.BoxSphereBounds = BoxSphereBounds;
			PrimitiveBounds.MinDrawDistance = Proxy->GetMinDrawDistance();
			PrimitiveBounds.MaxDrawDistance = Proxy->GetMaxDrawDistance();
			PrimitiveBounds.MaxCullDistance = PrimitiveBounds.MaxDrawDistance;

			Scene->PrimitiveFlagsCompact[PackedIndex] = FPrimitiveFlagsCompact(Proxy);

			// Store precomputed visibility ID.
			int32 VisibilityBitIndex = Proxy->GetVisibilityId();
			FPrimitiveVisibilityId& VisibilityId = Scene->PrimitiveVisibilityIds[PackedIndex];
			VisibilityId.ByteIndex = VisibilityBitIndex / 8;
			VisibilityId.BitMask = (1 << (VisibilityBitIndex & 0x7));

			// Store occlusion flags.
			uint8 OcclusionFlags = EOcclusionFlags::None;
			if (Proxy->CanBeOccluded())
			{
				OcclusionFlags |= EOcclusionFlags::CanBeOccluded;
			}
			if (Proxy->HasSubprimitiveOcclusionQueries())
			{
				OcclusionFlags |= EOcclusionFlags::HasSubprimitiveQueries;
			}
			if (Proxy->AllowApproximateOcclusion()
				// Allow approximate occlusion if attached, even if the parent does not have bLightAttachmentsAsGroup enabled
				|| SceneInfo->LightingAttachmentRoot.IsValid())
			{
				OcclusionFlags |= EOcclusionFlags::AllowApproximateOcclusion;
			}
			if (VisibilityBitIndex >= 0)
			{
				OcclusionFlags |= EOcclusionFlags::HasPrecomputedVisibility;
			}
			Scene->PrimitiveOcclusionFlags[PackedIndex] = OcclusionFlags;

			// Store occlusion bounds.
			FBoxSphereBounds OcclusionBounds = BoxSphereBounds;
			if (Proxy->HasCustomOcclusionBounds())
			{
				OcclusionBounds = Proxy->GetCustomOcclusionBounds();
			}
			OcclusionBounds.BoxExtent.X = OcclusionBounds.BoxExtent.X + OCCLUSION_SLOP;
			OcclusionBounds.BoxExtent.Y = OcclusionBounds.BoxExtent.Y + OCCLUSION_SLOP;
			OcclusionBounds.BoxExtent.Z = OcclusionBounds.BoxExtent.Z + OCCLUSION_SLOP;
			OcclusionBounds.SphereRadius = OcclusionBounds.SphereRadius + OCCLUSION_SLOP;
			Scene->PrimitiveOcclusionBounds[PackedIndex] = OcclusionBounds;

			// Store the component.
			Scene->PrimitiveComponentIds[PackedIndex] = SceneInfo->PrimitiveComponentId;

#if RHI_RAYTRACING
			// Set group id
			const int32 RayTracingGroupId = SceneInfo->Proxy->GetRayTracingGroupId();
			if (RayTracingGroupId != -1)
			{
				Scene->PrimitiveRayTracingGroupIds[PackedIndex] = Scene->PrimitiveRayTracingGroups.FindId(RayTracingGroupId);
			}
#endif

			INC_MEMORY_STAT_BY(STAT_PrimitiveInfoMemory, sizeof(*SceneInfo) + SceneInfo->StaticMeshes.GetAllocatedSize() + SceneInfo->StaticMeshRelevances.GetAllocatedSize() + Proxy->GetMemoryFootprint());
		}
	}

	{
		SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_LevelNotifyPrimitives, FColor::Blue);
		for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
		{
			if (SceneInfo->Proxy->ShouldNotifyOnWorldAddRemove())
			{
				TArray<FPrimitiveSceneInfo*>& LevelNotifyPrimitives = Scene->PrimitivesNeedingLevelUpdateNotification.FindOrAdd(SceneInfo->Proxy->GetLevelName());
				SceneInfo->LevelUpdateNotificationIndex = LevelNotifyPrimitives.Num();
				LevelNotifyPrimitives.Add(SceneInfo);
			}
		}
	}
}

void FPrimitiveSceneInfo::UpdateVirtualTextures(FScene* Scene, TArrayView<FPrimitiveSceneInfo*> SceneInfos)
{
	SCOPED_NAMED_EVENT(FPrimitiveSceneInfo_AddToScene_UpdateVirtualTexture, FColor::Emerald);
	for (FPrimitiveSceneInfo* SceneInfo : SceneInfos)
	{
		FPrimitiveSceneProxy* Proxy = SceneInfo->Proxy;
		// Store the runtime virtual texture flags.
		SceneInfo->UpdateRuntimeVirtualTextureFlags();
		Scene->PrimitiveVirtualTextureFlags[SceneInfo->PackedIndex] = SceneInfo->RuntimeVirtualTextureFlags;

		// Store the runtime virtual texture Lod info.
		if (SceneInfo->RuntimeVirtualTextureFlags.bRenderToVirtualTexture)
		{
			int8 MinLod, MaxLod;
			GetRuntimeVirtualTextureLODRange(SceneInfo->StaticMeshRelevances, MinLod, MaxLod);

			FPrimitiveVirtualTextureLodInfo& LodInfo = Scene->PrimitiveVirtualTextureLod[SceneInfo->PackedIndex];
			LodInfo.MinLod = FMath::Clamp((int32)MinLod, 0, 15);
			LodInfo.MaxLod = FMath::Clamp((int32)MaxLod, 0, 15);
			LodInfo.LodBias = FMath::Clamp(Proxy->GetVirtualTextureLodBias() + FPrimitiveVirtualTextureLodInfo::LodBiasOffset, 0, 15);
			LodInfo.CullMethod = Proxy->GetVirtualTextureMinCoverage() == 0 ? 0 : 1;
			LodInfo.CullValue = LodInfo.CullMethod == 0 ? Proxy->GetVirtualTextureCullMips() : Proxy->GetVirtualTextureMinCoverage();
		}
	}
}

void FPrimitiveSceneInfo::RemoveStaticMeshes()
{
	// Deallocate potential OIT dynamic index buffer
	if (OIT::IsSortedTrianglesEnabled(GMaxRHIShaderPlatform))
	{
		for (int32 MeshIndex = 0; MeshIndex < StaticMeshes.Num(); MeshIndex++)
		{
			FStaticMeshBatch& Mesh = StaticMeshes[MeshIndex];
			if (Mesh.Elements.Num() > 0)
			{
				Scene->OITSceneData.Deallocate(Mesh.Elements[0]);
			}
		}
	}

	// Remove static meshes from the scene.
	StaticMeshes.Empty();
	StaticMeshRelevances.Empty();
	RemoveCachedMeshDrawCommands();
	RemoveCachedNaniteMaterialBins();
#if RHI_RAYTRACING
	RemoveCachedRayTracingPrimitives();
#endif
}

void FPrimitiveSceneInfo::RemoveFromScene(bool bUpdateStaticDrawLists)
{
	check(IsInRenderingThread());

	// implicit linked list. The destruction will update this "head" pointer to the next item in the list.
	while (LightList)
	{
		FLightPrimitiveInteraction::Destroy(LightList);
	}

	// Remove the primitive from the octree.
	check(OctreeId.IsValidId());
	check(Scene->PrimitiveOctree.GetElementById(OctreeId).PrimitiveSceneInfo == this);
	Scene->PrimitiveOctree.RemoveElement(OctreeId);
	OctreeId = FOctreeElementId2();

	if (LightmapDataOffset != INDEX_NONE && UseGPUScene(GMaxRHIShaderPlatform, Scene->GetFeatureLevel()))
	{
		Scene->GPUScene.LightmapDataAllocator.Free(LightmapDataOffset, NumLightmapDataEntries);
	}

	if (Proxy->CastsDynamicIndirectShadow())
	{
		Scene->DynamicIndirectCasterPrimitives.RemoveSingleSwap(this);
	}

	IndirectLightingCacheAllocation = NULL;

	if (Proxy->IsOftenMoving())
	{
		MarkIndirectLightingCacheBufferDirty();
	}

	DEC_MEMORY_STAT_BY(STAT_PrimitiveInfoMemory, sizeof(*this) + StaticMeshes.GetAllocatedSize() + StaticMeshRelevances.GetAllocatedSize() + Proxy->GetMemoryFootprint());

	if (bUpdateStaticDrawLists)
	{
		if (IsIndexValid()) // PackedIndex
		{
			Scene->PrimitivesNeedingStaticMeshUpdate[PackedIndex] = false;
		}

		// IndirectLightingCacheUniformBuffer may be cached inside cached mesh draw commands, so we 
		// can't delete it unless we also update cached mesh command.
		IndirectLightingCacheUniformBuffer.SafeRelease();

		RemoveStaticMeshes();
	}

	if (bRegisteredVirtualTextureProducerCallback)
	{
		FVirtualTextureSystem::Get().RemoveAllProducerDestroyedCallbacks(this);
		bRegisteredVirtualTextureProducerCallback = false;
	}

	if (Proxy->ShouldNotifyOnWorldAddRemove())
	{
		TArray<FPrimitiveSceneInfo*>* LevelNotifyPrimitives = Scene->PrimitivesNeedingLevelUpdateNotification.Find(Proxy->GetLevelName());
		if (LevelNotifyPrimitives != nullptr)
		{
			checkSlow(LevelUpdateNotificationIndex != INDEX_NONE);
			LevelNotifyPrimitives->RemoveAtSwap(LevelUpdateNotificationIndex, 1, EAllowShrinking::No);
			if (LevelNotifyPrimitives->Num() == 0)
			{
				Scene->PrimitivesNeedingLevelUpdateNotification.Remove(Proxy->GetLevelName());
			}
			else if (LevelUpdateNotificationIndex < LevelNotifyPrimitives->Num())
			{
				// Update swapped element's LevelUpdateNotificationIndex
				((*LevelNotifyPrimitives)[LevelUpdateNotificationIndex])->LevelUpdateNotificationIndex = LevelUpdateNotificationIndex;
			}
		}
	}
}

void FPrimitiveSceneInfo::UpdateRuntimeVirtualTextureFlags()
{
	RuntimeVirtualTextureFlags.bRenderToVirtualTexture = false;
	RuntimeVirtualTextureFlags.RuntimeVirtualTextureMask = 0;

	if (Proxy->WritesVirtualTexture())
	{
		if (Proxy->IsNaniteMesh())
		{
			UE_LOG(LogRenderer, Warning, TEXT("Rendering a nanite mesh to a runtime virtual texture isn't yet supported. Please disable this option on primitive component : %s"), *Proxy->GetOwnerName().ToString());
		}
		else if (StaticMeshes.Num() > 0)
		{
			RuntimeVirtualTextureFlags.bRenderToVirtualTexture = true;

			// Performance assumption: The arrays of runtime virtual textures are small (less that 5?) so that O(n^2) scan isn't expensive
			for (TSparseArray<FRuntimeVirtualTextureSceneProxy*>::TConstIterator It(Scene->RuntimeVirtualTextures); It; ++It)
			{
				int32 SceneIndex = It.GetIndex();
				if (SceneIndex < FPrimitiveVirtualTextureFlags::RuntimeVirtualTexture_BitCount)
				{
					URuntimeVirtualTexture* SceneVirtualTexture = (*It)->VirtualTexture;
					if (Proxy->WritesVirtualTexture(SceneVirtualTexture))
					{
						RuntimeVirtualTextureFlags.RuntimeVirtualTextureMask |= 1 << SceneIndex;
					}
				}
			}
		}
	}
}

void FPrimitiveSceneInfo::UpdateStaticMeshes(FScene* Scene, TArrayView<FPrimitiveSceneInfo*> SceneInfos, EUpdateStaticMeshFlags UpdateFlags, bool bReAddToDrawLists)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FPrimitiveSceneInfo_UpdateStaticMeshes);
	TRACE_CPUPROFILER_EVENT_SCOPE(FPrimitiveSceneInfo_UpdateStaticMeshes);

	const bool bUpdateRayTracingCommands = EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RayTracingCommands) || !IsRayTracingEnabled();
	const bool bUpdateAllCommands = EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RasterCommands) && bUpdateRayTracingCommands;

	const bool bNeedsStaticMeshUpdate = !(bReAddToDrawLists && bUpdateAllCommands);

	for (int32 Index = 0; Index < SceneInfos.Num(); Index++)
	{
		FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index];
		Scene->PrimitivesNeedingStaticMeshUpdate[SceneInfo->PackedIndex] = bNeedsStaticMeshUpdate;

		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RasterCommands))
		{
			SceneInfo->RemoveCachedMeshDrawCommands();
			SceneInfo->RemoveCachedNaniteMaterialBins();
		}

	#if RHI_RAYTRACING
		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RayTracingCommands))
		{
			SceneInfo->RemoveCachedRayTracingPrimitives();
		}
	#endif

		if (SceneInfo->Proxy && SceneInfo->Proxy->IsNaniteMesh())
		{
			// Make sure material table indirections are kept in sync with GPU Scene and cached Nanite MDCs
			SceneInfo->RequestGPUSceneUpdate(EPrimitiveDirtyState::ChangedOther);
		}
	}

	if (bReAddToDrawLists)
	{
		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RasterCommands))
		{
			CacheMeshDrawCommands(Scene, SceneInfos);
			CacheNaniteMaterialBins(Scene, SceneInfos);
		}

	#if RHI_RAYTRACING
		if (EnumHasAnyFlags(UpdateFlags, EUpdateStaticMeshFlags::RayTracingCommands))
		{
			CacheRayTracingPrimitives(Scene, SceneInfos);
		}
	#endif
	}
}

#if RHI_RAYTRACING
void FPrimitiveSceneInfo::UpdateCachedRaytracingData(FScene* Scene, const TArrayView<FPrimitiveSceneInfo*>& SceneInfos)
{
	if (SceneInfos.Num() > 0)
	{
		for (int32 Index = 0; Index < SceneInfos.Num(); Index++)
		{
			FPrimitiveSceneInfo* SceneInfo = SceneInfos[Index]; 
			// should have been marked dirty by calling UpdateCachedRayTracingState on the scene before
			// scene info is being updated here
			check(SceneInfo->bCachedRaytracingDataDirty);
			SceneInfo->RemoveCachedRayTracingPrimitives();
		}

		CacheRayTracingPrimitives(Scene, SceneInfos);
	}
}
#endif //RHI_RAYTRACING

void FPrimitiveSceneInfo::RequestStaticMeshUpdate()
{
	// Set a flag which causes InitViews to update the static meshes the next time the primitive is visible.
	if (IsIndexValid()) // PackedIndex
	{
		Scene->PrimitivesNeedingStaticMeshUpdate[PackedIndex] = true;
	}
}

bool FPrimitiveSceneInfo::RequestUniformBufferUpdate()
{
	if (IsIndexValid()) // PackedIndex
	{
		Scene->PrimitivesNeedingUniformBufferUpdate[PackedIndex] = true;
		return true;
	}
	return false;
}

const FInstanceSceneDataBuffers *FPrimitiveSceneInfo::GetInstanceSceneDataBuffers() const
{ 
	if (!HasInstanceDataBuffers())
	{
		return nullptr;
	}

	if (InstanceDataUpdateTaskInfo)
	{
		InstanceDataUpdateTaskInfo->WaitForUpdateCompletion();
	}
	return InstanceSceneDataBuffersInternal; 
}

FInstanceDataBufferHeader FPrimitiveSceneInfo::GetInstanceDataHeader() const
{
	if (!HasInstanceDataBuffers())
	{
		return FInstanceDataBufferHeader::SinglePrimitiveHeader;
	}

	if (InstanceDataUpdateTaskInfo)
	{
		return InstanceDataUpdateTaskInfo->GetHeader();
	}

	return InstanceSceneDataBuffersInternal->GetHeader();
}

void FPrimitiveSceneInfo::FlushRuntimeVirtualTexture()
{
	if (RuntimeVirtualTextureFlags.bRenderToVirtualTexture)
	{
		uint32 RuntimeVirtualTextureIndex = 0;
		uint32 Mask = RuntimeVirtualTextureFlags.RuntimeVirtualTextureMask;
		while (Mask != 0)
		{
			if (Mask & 1)
			{
				Scene->RuntimeVirtualTextures[RuntimeVirtualTextureIndex]->Dirty(Proxy->GetBounds());
			}
			Mask >>= 1;
			RuntimeVirtualTextureIndex++;
		}
	}
}

void FPrimitiveSceneInfo::LinkLODParentComponent()
{
	if (LODParentComponentId.IsValid())
	{
		Scene->SceneLODHierarchy.AddChildNode(LODParentComponentId, this);
	}
}

void FPrimitiveSceneInfo::UnlinkLODParentComponent()
{
	if(LODParentComponentId.IsValid())
	{
		Scene->SceneLODHierarchy.RemoveChildNode(LODParentComponentId, this);
	}
}

void FPrimitiveSceneInfo::LinkAttachmentGroup()
{
	// Add the primitive to its attachment group.
	if (LightingAttachmentRoot.IsValid())
	{
		FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(LightingAttachmentRoot);

		if (!AttachmentGroup)
		{
			// If this is the first primitive attached that uses this attachment parent, create a new attachment group.
			AttachmentGroup = &Scene->AttachmentGroups.Add(LightingAttachmentRoot, FAttachmentGroupSceneInfo());
		}

		AttachmentGroup->Primitives.Add(this);
	}
	else if (Proxy->LightAttachmentsAsGroup())
	{
		FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (!AttachmentGroup)
		{
			// Create an empty attachment group 
			AttachmentGroup = &Scene->AttachmentGroups.Add(PrimitiveComponentId, FAttachmentGroupSceneInfo());
		}

		AttachmentGroup->ParentSceneInfo = this;
	}
}

void FPrimitiveSceneInfo::UnlinkAttachmentGroup()
{
	// Remove the primitive from its attachment group.
	if (LightingAttachmentRoot.IsValid())
	{
		FAttachmentGroupSceneInfo& AttachmentGroup = Scene->AttachmentGroups.FindChecked(LightingAttachmentRoot);
		AttachmentGroup.Primitives.RemoveSwap(this);

		if (AttachmentGroup.Primitives.Num() == 0 && AttachmentGroup.ParentSceneInfo == nullptr)
		{
			// If this was the last primitive attached that uses this attachment group and the root has left the building, free the group.
			Scene->AttachmentGroups.Remove(LightingAttachmentRoot);
		}
	}
	else if (Proxy->LightAttachmentsAsGroup())
	{
		FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);
		
		if (AttachmentGroup)
		{
			AttachmentGroup->ParentSceneInfo = NULL;
			if (AttachmentGroup->Primitives.Num() == 0)
			{
				// If this was the owner and the group is empty, remove it (otherwise the above will remove when the last attached goes).
				Scene->AttachmentGroups.Remove(PrimitiveComponentId);
			}
		}
	}
}

bool FPrimitiveSceneInfo::RequestGPUSceneUpdate(EPrimitiveDirtyState PrimitiveDirtyState)
{
	if (Scene && IsIndexValid())
	{
		Scene->GPUScene.AddPrimitiveToUpdate(GetPersistentIndex(), PrimitiveDirtyState);
		return true;
	}

	return false;
}

void FPrimitiveSceneInfo::RefreshNaniteRasterBins()
{
	const bool bShouldRenderCustomDepth = Proxy->ShouldRenderCustomDepth();
	if (bShouldRenderCustomDepth == bNaniteRasterBinsRenderCustomDepth)
	{
		// nothing to do
		return;
	}

	TArray<FNaniteRasterBin>& NanitePassRasterBins = NaniteRasterBins[ENaniteMeshPass::BasePass];
	FNaniteRasterPipelines& RasterPipelines = Scene->NaniteRasterPipelines[ENaniteMeshPass::BasePass];
	for (const FNaniteRasterBin& RasterBin : NanitePassRasterBins)
	{
		if (bShouldRenderCustomDepth)
		{
			RasterPipelines.RegisterBinForCustomPass(RasterBin.BinIndex);
		}
		else
		{
			RasterPipelines.UnregisterBinForCustomPass(RasterBin.BinIndex);
		}
	}

	bNaniteRasterBinsRenderCustomDepth = bShouldRenderCustomDepth;
}

void FPrimitiveSceneInfo::GatherLightingAttachmentGroupPrimitives(TArray<FPrimitiveSceneInfo*, SceneRenderingAllocator>& OutChildSceneInfos)
{
#if ENABLE_NAN_DIAGNOSTIC
	// local function that returns full name of object
	auto GetObjectName = [](const UObject* InPrimitive)->FString
	{
		return (InPrimitive) ? InPrimitive->GetFullName() : FString(TEXT("Unknown Object"));
	};

	// verify that the current object has a valid bbox before adding it
	const float& BoundsRadius = this->Proxy->GetBounds().SphereRadius;
	if (ensureMsgf(!FMath::IsNaN(BoundsRadius) && FMath::IsFinite(BoundsRadius),
		TEXT("%s had an ill-formed bbox and was skipped during shadow setup, contact DavidH."), *GetObjectName(this->PrimitiveComponentInterfaceForDebuggingOnly->GetUObject())))
	{
		OutChildSceneInfos.Add(this);
	}
	else
	{
		// return, leaving the TArray empty
		return;
	}

#else 
	// add self at the head of this queue
	OutChildSceneInfos.Add(this);
#endif

	if (!LightingAttachmentRoot.IsValid() && Proxy->LightAttachmentsAsGroup())
	{
		const FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (AttachmentGroup)
		{
			
			for (int32 ChildIndex = 0, ChildIndexMax = AttachmentGroup->Primitives.Num(); ChildIndex < ChildIndexMax; ChildIndex++)
			{
				FPrimitiveSceneInfo* ShadowChild = AttachmentGroup->Primitives[ChildIndex];
#if ENABLE_NAN_DIAGNOSTIC
				// Only enqueue objects with valid bounds using the normality of the SphereRaduis as criteria.

				const float& ShadowChildBoundsRadius = ShadowChild->Proxy->GetBounds().SphereRadius;

				if (ensureMsgf(!FMath::IsNaN(ShadowChildBoundsRadius) && FMath::IsFinite(ShadowChildBoundsRadius),
					TEXT("%s had an ill-formed bbox and was skipped during shadow setup, contact DavidH."), *GetObjectName(ShadowChild->PrimitiveComponentInterfaceForDebuggingOnly->GetUObject())))
				{
					checkSlow(!OutChildSceneInfos.Contains(ShadowChild))
				    OutChildSceneInfos.Add(ShadowChild);
				}
#else
				// enqueue all objects.
				checkSlow(!OutChildSceneInfos.Contains(ShadowChild))
			    OutChildSceneInfos.Add(ShadowChild);
#endif
			}
		}
	}
}

void FPrimitiveSceneInfo::GatherLightingAttachmentGroupPrimitives(TArray<const FPrimitiveSceneInfo*, SceneRenderingAllocator>& OutChildSceneInfos) const
{
	OutChildSceneInfos.Add(this);

	if (!LightingAttachmentRoot.IsValid() && Proxy->LightAttachmentsAsGroup())
	{
		const FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (AttachmentGroup)
		{
			for (int32 ChildIndex = 0, ChildIndexMax = AttachmentGroup->Primitives.Num(); ChildIndex < ChildIndexMax; ChildIndex++)
			{
				const FPrimitiveSceneInfo* ShadowChild = AttachmentGroup->Primitives[ChildIndex];

				checkSlow(!OutChildSceneInfos.Contains(ShadowChild))
			    OutChildSceneInfos.Add(ShadowChild);
			}
		}
	}
}

FBoxSphereBounds FPrimitiveSceneInfo::GetAttachmentGroupBounds() const
{
	FBoxSphereBounds Bounds = Proxy->GetBounds();

	if (!LightingAttachmentRoot.IsValid() && Proxy->LightAttachmentsAsGroup())
	{
		const FAttachmentGroupSceneInfo* AttachmentGroup = Scene->AttachmentGroups.Find(PrimitiveComponentId);

		if (AttachmentGroup)
		{
			for (int32 ChildIndex = 0; ChildIndex < AttachmentGroup->Primitives.Num(); ChildIndex++)
			{
				FPrimitiveSceneInfo* AttachmentChild = AttachmentGroup->Primitives[ChildIndex];
				Bounds = Bounds + AttachmentChild->Proxy->GetBounds();
			}
		}
	}

	return Bounds;
}

uint32 FPrimitiveSceneInfo::GetMemoryFootprint()
{
	return( sizeof( *this ) + HitProxies.GetAllocatedSize() + StaticMeshes.GetAllocatedSize() + StaticMeshRelevances.GetAllocatedSize() );
}

void FPrimitiveSceneInfo::ApplyWorldOffset(FRHICommandListBase& RHICmdList, FVector InOffset)
{
	Proxy->ApplyWorldOffset(RHICmdList, InOffset);
}

void FPrimitiveSceneInfo::UpdateIndirectLightingCacheBuffer(
	FRHICommandListBase& RHICmdList,
	const FIndirectLightingCache* LightingCache,
	const FIndirectLightingCacheAllocation* LightingAllocation,
	FVector VolumetricLightmapLookupPosition,
	uint32 SceneFrameNumber,
	FVolumetricLightmapSceneData* VolumetricLightmapSceneData)
{
	FIndirectLightingCacheUniformParameters Parameters;

	GetIndirectLightingCacheParameters(
		Scene->GetFeatureLevel(),
		Parameters,
		LightingCache,
		LightingAllocation,
		VolumetricLightmapLookupPosition,
		SceneFrameNumber,
		VolumetricLightmapSceneData);

	if (IndirectLightingCacheUniformBuffer)
	{
		IndirectLightingCacheUniformBuffer.UpdateUniformBufferImmediate(RHICmdList, Parameters);
	}
}

void FPrimitiveSceneInfo::UpdateIndirectLightingCacheBuffer(FRHICommandListBase& RHICmdList)
{
	if (bIndirectLightingCacheBufferDirty)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_UpdateIndirectLightingCacheBuffer);

		if (Scene->GetFeatureLevel() < ERHIFeatureLevel::SM5
			&& Scene->VolumetricLightmapSceneData.HasData()
			&& (Proxy->IsMovable() || Proxy->NeedsUnbuiltPreviewLighting() || Proxy->GetLightmapType() == ELightmapType::ForceVolumetric)
			&& Proxy->WillEverBeLit())
		{
			UpdateIndirectLightingCacheBuffer(
				RHICmdList,
				nullptr, 
				nullptr,
				Proxy->GetBounds().Origin,
				Scene->GetFrameNumber(),
				&Scene->VolumetricLightmapSceneData);
		}
		// The update is invalid if the lighting cache allocation was not in a functional state.
		else if (IndirectLightingCacheAllocation && (Scene->IndirectLightingCache.IsInitialized() && IndirectLightingCacheAllocation->bHasEverUpdatedSingleSample))
		{
			UpdateIndirectLightingCacheBuffer(
				RHICmdList,
				&Scene->IndirectLightingCache,
				IndirectLightingCacheAllocation,
				FVector(0, 0, 0),
				0,
				nullptr);
		}
		else
		{
			// Fallback to the global empty buffer parameters
			UpdateIndirectLightingCacheBuffer(RHICmdList, nullptr, nullptr, FVector(0.0f, 0.0f, 0.0f), 0, nullptr);
		}

		bIndirectLightingCacheBufferDirty = false;
	}
}

void FPrimitiveSceneInfo::GetStaticMeshesLODRange(int8& OutMinLOD, int8& OutMaxLOD) const
{
	OutMinLOD = MAX_int8;
	OutMaxLOD = 0;

	for (int32 MeshIndex = 0; MeshIndex < StaticMeshRelevances.Num(); ++MeshIndex)
	{
		const FStaticMeshBatchRelevance& MeshRelevance = StaticMeshRelevances[MeshIndex];
		OutMinLOD = FMath::Min(OutMinLOD, MeshRelevance.GetLODIndex());
		OutMaxLOD = FMath::Max(OutMaxLOD, MeshRelevance.GetLODIndex());
	}
}

const FMeshBatch* FPrimitiveSceneInfo::GetMeshBatch(int8 InLODIndex) const
{
	if (StaticMeshes.IsValidIndex(InLODIndex))
	{
		return &StaticMeshes[InLODIndex];
	}

	return nullptr;
}

bool FPrimitiveSceneInfo::NeedsReflectionCaptureUpdate() const
{
	return bNeedsCachedReflectionCaptureUpdate && 
		// For mobile, the per-object reflection is used for everything
		(Scene->GetShadingPath() == EShadingPath::Mobile || IsForwardShadingEnabled(Scene->GetShaderPlatform()));
}

void FPrimitiveSceneInfo::CacheReflectionCaptures()
{
	// do not use Scene->PrimitiveBounds here, as it may be not initialized yet
	FBoxSphereBounds BoxSphereBounds = Proxy->GetBounds(); 
	
	CachedReflectionCaptureProxy = Scene->FindClosestReflectionCapture(BoxSphereBounds.Origin);
	CachedPlanarReflectionProxy = Scene->FindClosestPlanarReflection(BoxSphereBounds);
	if (Scene->GetShadingPath() == EShadingPath::Mobile)
	{
		// mobile HQ reflections
		Scene->FindClosestReflectionCaptures(BoxSphereBounds.Origin, CachedReflectionCaptureProxies);
	}
	
	bNeedsCachedReflectionCaptureUpdate = false;
}

void FPrimitiveSceneInfo::RemoveCachedReflectionCaptures()
{
	CachedReflectionCaptureProxy = nullptr;
	CachedPlanarReflectionProxy = nullptr;
	FMemory::Memzero(CachedReflectionCaptureProxies);
	bNeedsCachedReflectionCaptureUpdate = true;
}

void FPrimitiveSceneInfo::UpdateComponentLastRenderTime(float CurrentWorldTime, bool bUpdateLastRenderTimeOnScreen) const
{
	SceneData->SetLastRenderTime(CurrentWorldTime, bUpdateLastRenderTimeOnScreen);
}

void FPrimitiveOctreeSemantics::SetOctreeNodeIndex(const FPrimitiveSceneInfoCompact& Element, FOctreeElementId2 Id)
{
	// When a Primitive is removed from the renderer, it's index will be invalidated.  Only update if the primitive still
	// has a valid index.
	if (Element.PrimitiveSceneInfo->IsIndexValid())
	{
		Element.PrimitiveSceneInfo->Scene->PrimitiveOctreeIndex[Element.PrimitiveSceneInfo->GetIndex()] = Id.GetNodeIndex();
	}
}

FString FPrimitiveSceneInfo::GetFullnameForDebuggingOnly() const
{
	if (PrimitiveComponentInterfaceForDebuggingOnly)
	{
		return PrimitiveComponentInterfaceForDebuggingOnly->GetUObject()->GetFullGroupName(false);
	}

	return FString(TEXT("Unknown Object"));
}

FString FPrimitiveSceneInfo::GetOwnerActorNameOrLabelForDebuggingOnly() const
{
	if (PrimitiveComponentInterfaceForDebuggingOnly)
	{
		return PrimitiveComponentInterfaceForDebuggingOnly->GetOwnerName();
	}
	
	return FString(TEXT("Unknown Object"));
}

const UPrimitiveComponent* FPrimitiveSceneInfo::GetComponentForDebugOnly() const 
{ 
	return Cast<UPrimitiveComponent>(PrimitiveComponentInterfaceForDebuggingOnly->GetUObject()); 
}

const IPrimitiveComponent* FPrimitiveSceneInfo::GetComponentInterfaceForDebugOnly() const 
{
	return PrimitiveComponentInterfaceForDebuggingOnly; 
}