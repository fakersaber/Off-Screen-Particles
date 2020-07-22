// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MobileTranslucentRendering.cpp: translucent rendering implementation.
=============================================================================*/

#include "CoreMinimal.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "HitProxies.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "SceneUtils.h"
#include "RHIStaticStates.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "SceneRenderTargetParameters.h"
#include "SceneRendering.h"
#include "LightMapRendering.h"
#include "MaterialShaderType.h"
#include "MeshMaterialShaderType.h"
#include "MeshMaterialShader.h"
#include "BasePassRendering.h"
#include "DynamicPrimitiveDrawing.h"
#include "TranslucentRendering.h"
#include "MobileBasePassRendering.h"
#include "ScenePrivate.h"
#include "ScreenRendering.h"
#include "PostProcess/SceneFilterRendering.h"
#include "PipelineStateCache.h"
#include "MeshPassProcessor.inl"


/** Pixel shader used to copy scene color into another texture so that materials can read from scene color with a node. */
class FMobileCopySceneAlphaPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FMobileCopySceneAlphaPS,Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsMobilePlatform(Parameters.Platform); }

	FMobileCopySceneAlphaPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer):
		FGlobalShader(Initializer)
	{
		SceneTextureParameters.Bind(Initializer);
	}
	FMobileCopySceneAlphaPS() {}

	void SetParameters(FRHICommandList& RHICmdList, const FSceneView& View)
	{
		SceneTextureParameters.Set(RHICmdList, RHICmdList.GetBoundPixelShader(), View.FeatureLevel, ESceneTextureSetupMode::All);
	}

private:
	LAYOUT_FIELD(FSceneTextureShaderParameters, SceneTextureParameters)
};

IMPLEMENT_SHADER_TYPE(,FMobileCopySceneAlphaPS,TEXT("/Engine/Private/TranslucentLightingShaders.usf"),TEXT("CopySceneAlphaMain"),SF_Pixel);

void FMobileSceneRenderer::CopySceneAlpha(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	SCOPED_DRAW_EVENTF(RHICmdList, EventCopy, TEXT("CopySceneAlpha"));
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	RHICmdList.CopyToResolveTarget(SceneContext.GetSceneColorSurface(), SceneContext.GetSceneColorTexture(), FResolveRect(0, 0, FamilySize.X, FamilySize.Y));

	SceneContext.BeginRenderingSceneAlphaCopy(RHICmdList);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false,CF_Always>::GetRHI();
	GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();

	int X = SceneContext.GetBufferSizeXY().X;
	int Y = SceneContext.GetBufferSizeXY().Y;

	RHICmdList.SetViewport(0, 0, 0.0f, X, Y, 1.0f);

	TShaderMapRef<FScreenVS> ScreenVertexShader(View.ShaderMap);
	TShaderMapRef<FMobileCopySceneAlphaPS> PixelShader(View.ShaderMap);

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

	PixelShader->SetParameters(RHICmdList, View);

	DrawRectangle( 
		RHICmdList,
		0, 0, 
		X, Y, 
		0, 0, 
		X, Y,
		FIntPoint(X, Y),
		SceneContext.GetBufferSizeXY(),
		ScreenVertexShader,
		EDRF_UseTriangleOptimization);

	SceneContext.FinishRenderingSceneAlphaCopy(RHICmdList);
}

void FMobileSceneRenderer::RenderTranslucency(FRHICommandListImmediate& RHICmdList, const TArrayView<const FViewInfo*> PassViews, bool bRenderToSceneColor, bool bShouldRenderDownSampleTranslucency)
{
	//YJH Created 2020-7-19
	//移动硬件不支持FrameFetch时bShouldRenderDownSampleTranslucency必为false
	ETranslucencyPass::Type TranslucencyPass = ViewFamily.AllowTranslucencyAfterDOF() ? ETranslucencyPass::TPT_StandardTranslucency : ETranslucencyPass::TPT_AllTranslucency;
	bool bShouldRenderTranslucency = ShouldRenderTranslucency(TranslucencyPass);

	const float DownsamplingScale = 0.5f;
	if (bShouldRenderTranslucency)
	{
		SCOPED_DRAW_EVENT(RHICmdList, Translucency);

		//新建Pass状态，结束前面Pass
		if (bShouldRenderDownSampleTranslucency) {
			RHICmdList.EndRenderPass();
		}

		for (int32 ViewIndex = 0; ViewIndex < PassViews.Num(); ViewIndex++)
		{
			//Test The Particles that have been culling will not increase the number of TranslucentPrimCount
			//UE_LOG(LogTemp, Log, TEXT("1 : %d, 2 : %d, 3 : %d "), Views[ViewIndex].bHasTranslucentViewMeshElements, Views[ViewIndex].TranslucentPrimCount.Num(TranslucencyPass), ViewIndex);
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, EventView, Views.Num() > 1, TEXT("View%d"), ViewIndex);

			const FViewInfo& View = *PassViews[ViewIndex];
			if (!View.ShouldRenderView())
			{
				continue;
			}

			if (bShouldRenderDownSampleTranslucency) {

				FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

				//SetUp UniformBuffer for DownSampleDepthAndDrawTranslucency Pass
				FIntPoint SeparateTranslucencyBufferSize = FIntPoint(SceneContext.GetBufferSizeXY().X * DownsamplingScale, SceneContext.GetBufferSizeXY().Y * DownsamplingScale);

				// Update the parts of DownsampledTranslucencyParameters which are dependent on the buffer size and view rect
				FViewUniformShaderParameters DownsampledTranslucencyViewParameters = *View.CachedViewUniformShaderParameters;

				View.SetupViewRectUniformBufferParameters(
					DownsampledTranslucencyViewParameters,
					SeparateTranslucencyBufferSize,
					FIntRect(View.ViewRect.Min.X * DownsamplingScale, View.ViewRect.Min.Y * DownsamplingScale, View.ViewRect.Max.X * DownsamplingScale, View.ViewRect.Max.Y * DownsamplingScale),
					View.ViewMatrices,
					View.PrevViewInfo.ViewMatrices
				);

				Scene->UniformBuffers.ViewUniformBuffer.UpdateUniformBufferImmediate(DownsampledTranslucencyViewParameters);

				//
				MobileDownSampleDepth(RHICmdList, Views[ViewIndex], DownsamplingScale);
			}
			else {
				// Mobile multi-view is not side by side stereo
				const FViewInfo& TranslucentViewport = (View.bIsMobileMultiViewEnabled) ? Views[0] : View;
				RHICmdList.SetViewport(TranslucentViewport.ViewRect.Min.X, TranslucentViewport.ViewRect.Min.Y, 0.0f, TranslucentViewport.ViewRect.Max.X, TranslucentViewport.ViewRect.Max.Y, 1.0f);
			}


			if (!View.Family->UseDebugViewPS())
			{
				if (Scene->UniformBuffers.UpdateViewUniformBuffer(View))
				{
					UpdateTranslucentBasePassUniformBuffer(RHICmdList, View);
					UpdateDirectionalLightUniformBuffers(RHICmdList, View);
				}
		
				const EMeshPass::Type MeshPass = TranslucencyPassToMeshPass(TranslucencyPass);
				View.ParallelMeshDrawCommandPasses[MeshPass].DispatchDraw(nullptr, RHICmdList);
			}

			if (bShouldRenderDownSampleTranslucency) {
				//End Translucency Pass
				RHICmdList.EndRenderPass();

				//restore ViewUniformBuffer
				Scene->UniformBuffers.ViewUniformBuffer.UpdateUniformBufferImmediate(*View.CachedViewUniformShaderParameters);

				UpsampleTranslucency(RHICmdList, View, DownsamplingScale);
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Translucent material inverse opacity render code
// Used to generate inverse opacity channel for scene captures that require opacity information.
// See MobileSceneCaptureRendering for more details.

/**
* Vertex shader for mobile opacity only pass
*/
class FOpacityOnlyVS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FOpacityOnlyVS, MeshMaterial);
protected:

	FOpacityOnlyVS() {}
	FOpacityOnlyVS(const FMeshMaterialShaderType::CompiledShaderInitializerType& Initializer) :
		FMeshMaterialShader(Initializer)
	{
		PassUniformBuffer.Bind(Initializer.ParameterMap, FMobileBasePassUniformParameters::StaticStructMetadata.GetShaderVariableName());
	}

public:

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return IsTranslucentBlendMode(Parameters.MaterialParameters.BlendMode) && IsMobilePlatform(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		static auto* MobileUseHWsRGBEncodingCVAR = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.UseHWsRGBEncoding"));
		const bool bMobileUseHWsRGBEncoding = (MobileUseHWsRGBEncodingCVAR && MobileUseHWsRGBEncodingCVAR->GetValueOnAnyThread() == 1);

		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("OUTPUT_GAMMA_SPACE"), IsMobileHDR() == false && !bMobileUseHWsRGBEncoding);
		OutEnvironment.SetDefine(TEXT("OUTPUT_MOBILE_HDR"), IsMobileHDR() == true);
	}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FOpacityOnlyVS, TEXT("/Engine/Private/MobileOpacityShaders.usf"), TEXT("MainVS"), SF_Vertex);

/**
* Pixel shader for mobile opacity only pass, writes opacity to alpha channel.
*/
class FOpacityOnlyPS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FOpacityOnlyPS, MeshMaterial);
public:

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return IsTranslucentBlendMode(Parameters.MaterialParameters.BlendMode) && IsMobilePlatform(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMeshMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SCENE_TEXTURES_DISABLED"), 1u);
	}

	FOpacityOnlyPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FMeshMaterialShader(Initializer)
	{
		PassUniformBuffer.Bind(Initializer.ParameterMap, FMobileBasePassUniformParameters::StaticStructMetadata.GetShaderVariableName());
	}

	FOpacityOnlyPS() {}
};

IMPLEMENT_MATERIAL_SHADER_TYPE(, FOpacityOnlyPS, TEXT("/Engine/Private/MobileOpacityShaders.usf"), TEXT("MainPS"), SF_Pixel);

bool FMobileSceneRenderer::RenderInverseOpacity(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	// Function MUST be self-contained wrt RenderPasses
	check(RHICmdList.IsOutsideRenderPass());

	bool bDirty = false;
	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	SceneContext.AllocSceneColor(RHICmdList);

	const bool bMobileMSAA = SceneContext.GetSceneColorSurface()->GetNumSamples() > 1;
	
	FRHITexture* SceneColorResolve = bMobileMSAA ? SceneContext.GetSceneColorTexture() : nullptr;
	ERenderTargetActions ColorTargetAction = bMobileMSAA ? ERenderTargetActions::Clear_Resolve : ERenderTargetActions::Clear_Store;
	FRHIRenderPassInfo RPInfo(
		SceneContext.GetSceneColorSurface(), 
		ColorTargetAction,
		SceneColorResolve,
		SceneContext.GetSceneDepthSurface(),
		EDepthStencilTargetActions::ClearDepthStencil_DontStoreDepthStencil,
		nullptr,
		FExclusiveDepthStencil::DepthRead_StencilRead
	);

	// make sure targets are writable
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, SceneContext.GetSceneColorSurface());
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, SceneContext.GetSceneDepthSurface());
	if (SceneColorResolve)
	{
		RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, SceneColorResolve);
	}

	if (Scene->UniformBuffers.UpdateViewUniformBuffer(View))
	{
		UpdateTranslucentBasePassUniformBuffer(RHICmdList, View);
		UpdateDirectionalLightUniformBuffers(RHICmdList, View);
	}
	
	RHICmdList.BeginRenderPass(RPInfo, TEXT("RenderInverseOpacity"));

	if (ShouldRenderTranslucency(ETranslucencyPass::TPT_AllTranslucency))
	{		
		// Mobile multi-view is not side by side stereo
		const FViewInfo& TranslucentViewport = (View.bIsMobileMultiViewEnabled) ? Views[0] : View;
		RHICmdList.SetViewport(TranslucentViewport.ViewRect.Min.X, TranslucentViewport.ViewRect.Min.Y, 0.0f, TranslucentViewport.ViewRect.Max.X, TranslucentViewport.ViewRect.Max.Y, 1.0f);

		View.ParallelMeshDrawCommandPasses[EMeshPass::MobileInverseOpacity].DispatchDraw(nullptr, RHICmdList);
				
		bDirty |= View.ParallelMeshDrawCommandPasses[EMeshPass::MobileInverseOpacity].HasAnyDraw();
	}
	
	RHICmdList.EndRenderPass();
	
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, SceneContext.GetSceneColorTexture());
	
	return bDirty;
}

class FMobileInverseOpacityMeshProcessor : public FMeshPassProcessor
{
public:
	const FMeshPassProcessorRenderState PassDrawRenderState;

public:
	FMobileInverseOpacityMeshProcessor(const FScene* InScene, ERHIFeatureLevel::Type InFeatureLevel, const FSceneView* InViewIfDynamicMeshCommand, const FMeshPassProcessorRenderState& InDrawRenderState, FMeshPassDrawListContext* InDrawListContext)
		: FMeshPassProcessor(InScene, InFeatureLevel, InViewIfDynamicMeshCommand, InDrawListContext)
		, PassDrawRenderState(InDrawRenderState)
	{
		// expect dynamic path
		check(InViewIfDynamicMeshCommand);
	}

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final
	{
		if (MeshBatch.bUseForMaterial)
		{
			// Determine the mesh's material and blend mode.
			const FMaterialRenderProxy* FallbackMaterialRenderProxyPtr = nullptr;
			const FMaterial& Material = MeshBatch.MaterialRenderProxy->GetMaterialWithFallback(FeatureLevel, FallbackMaterialRenderProxyPtr);
			const FMaterialRenderProxy& MaterialRenderProxy = FallbackMaterialRenderProxyPtr ? *FallbackMaterialRenderProxyPtr : *MeshBatch.MaterialRenderProxy;
			const EBlendMode BlendMode = Material.GetBlendMode();
			const bool bIsTranslucent = IsTranslucentBlendMode(BlendMode);

			if (bIsTranslucent)
			{
				Process(MeshBatch, BatchElementMask, Material, MaterialRenderProxy, PrimitiveSceneProxy, StaticMeshId);
			}
		}
	}

private:
	void Process(const FMeshBatch& MeshBatch, uint64 BatchElementMask, const FMaterial& Material, const FMaterialRenderProxy& MaterialRenderProxy, const FPrimitiveSceneProxy* PrimitiveSceneProxy, int32 StaticMeshId)
	{
		const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;

		TMeshProcessorShaders<
			FOpacityOnlyVS,
			FBaseHS,
			FBaseDS,
			FOpacityOnlyPS> InverseOpacityShaders;
		
		InverseOpacityShaders.VertexShader = Material.GetShader<FOpacityOnlyVS>(VertexFactory->GetType());
		InverseOpacityShaders.PixelShader = Material.GetShader<FOpacityOnlyPS>(VertexFactory->GetType());

		FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);
		MobileBasePass::SetTranslucentRenderState(DrawRenderState, Material);

		const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
		ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, Material, OverrideSettings);
		ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, Material, OverrideSettings);
	
		FMeshMaterialShaderElementData ShaderElementData;
		ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

		FMeshDrawCommandSortKey SortKey = CalculateTranslucentMeshStaticSortKey(PrimitiveSceneProxy, MeshBatch.MeshIdInPrimitive);
		
		BuildMeshDrawCommands(
			MeshBatch,
			BatchElementMask,
			PrimitiveSceneProxy,
			MaterialRenderProxy,
			Material,
			PassDrawRenderState,
			InverseOpacityShaders,
			MeshFillMode,
			MeshCullMode,
			SortKey,
			EMeshPassFeatures::Default,
			ShaderElementData);
	}
};

// This pass is registered only when we render to scene capture, see UpdateSceneCaptureContentMobile_RenderThread()
FMeshPassProcessor* CreateMobileInverseOpacityPassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState PassDrawRenderState(Scene->UniformBuffers.ViewUniformBuffer, Scene->UniformBuffers.MobileTranslucentBasePassUniformBuffer);
	PassDrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	PassDrawRenderState.SetBlendState(TStaticBlendState<CW_ALPHA, BO_Add, BF_DestColor, BF_Zero, BO_Add, BF_Zero, BF_InverseSourceAlpha>::GetRHI());

	return new(FMemStack::Get()) FMobileInverseOpacityMeshProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, PassDrawRenderState, InDrawListContext);
}



/** A simple pixel shader used on Mobile to read scene depth from scene color alpha and write it to a downsized depth buffer. */
class FMobileDownsampleSceneDepthPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FMobileDownsampleSceneDepthPS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	FMobileDownsampleSceneDepthPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FGlobalShader(Initializer)
	{
		//SLSceneColorTexture.Bind(Initializer.ParameterMap, TEXT("SLSceneColorTexture"));
		SLSceneDepthTexture.Bind(Initializer.ParameterMap, TEXT("SLSceneDepthTexture"));
	}
	FMobileDownsampleSceneDepthPS() {}

	void SetParameters(FRHICommandList& RHICmdList, const FViewInfo& View)
	{
		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

		//注意MSAA
		//FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, RHICmdList.GetBoundPixelShader(), View.ViewUniformBuffer);
		//SetTextureParameter(RHICmdList, RHICmdList.GetBoundPixelShader(), SLSceneDepthTexture, SceneContext.GetSceneColorSurface());


		SetTextureParameter(RHICmdList, RHICmdList.GetBoundPixelShader(), SLSceneDepthTexture, SceneContext.GetSceneDepthSurface());
	}

	//LAYOUT_FIELD(FShaderResourceParameter, SLSceneColorTexture);
	LAYOUT_FIELD(FShaderResourceParameter, SLSceneDepthTexture);
};

IMPLEMENT_SHADER_TYPE(, FMobileDownsampleSceneDepthPS, TEXT("/Engine/Private/MobileDownSampleDepthPixelShader.usf"), TEXT("Main"), SF_Pixel);


void FMobileSceneRenderer::MobileDownSampleDepth(FRHICommandListImmediate& RHICmdList, const FViewInfo& View, float DownsamplingScale) {

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	FIntPoint MobileSeparateTranslucencyBufferSize = FIntPoint(SceneContext.GetBufferSizeXY().X * DownsamplingScale, SceneContext.GetBufferSizeXY().Y * DownsamplingScale);

	FRHIRenderPassInfo RPInfo(
		SceneContext.GetSeparateTranslucency(RHICmdList, MobileSeparateTranslucencyBufferSize)->GetRenderTargetItem().TargetableTexture,
		ERenderTargetActions::Clear_Store, 
		nullptr, //暂时不管MSAA
		SceneContext.GetDownsampledTranslucencyDepth(RHICmdList, MobileSeparateTranslucencyBufferSize)->GetRenderTargetItem().TargetableTexture,
		EDepthStencilTargetActions::LoadDepthStencil_StoreDepthStencil,  //Depth与Stencil必须Load，并且一旦Load了Depth那么Stencil肯定也被加载了
		nullptr,
		FExclusiveDepthStencil::DepthWrite_StencilWrite //
	);

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, SceneContext.GetSceneColorSurface());

	RHICmdList.BeginRenderPass(RPInfo, TEXT("DownsampleDepthAndSeparatePass"));
	{
		SCOPED_DRAW_EVENT(RHICmdList, DownsampleDepth);

		// Set shaders and texture
		TShaderMapRef<FScreenVS> ScreenVertexShader(View.ShaderMap);
		TShaderMapRef<FMobileDownsampleSceneDepthPS> PixelShader(View.ShaderMap);

		extern TGlobalResource<FFilterVertexDeclaration> GFilterVertexDeclaration;

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

		GraphicsPSOInit.BlendState = TStaticBlendState<CW_NONE>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_Always>::GetRHI(); //直接强制写深度了

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		PixelShader->SetParameters(RHICmdList, View);

		const uint32 DownsampledViewSizeX = FMath::TruncToInt(View.ViewRect.Width() * DownsamplingScale);
		const uint32 DownsampledViewSizeY = FMath::TruncToInt(View.ViewRect.Height() * DownsamplingScale);

		RHICmdList.SetViewport(0, 0, 0.0f, DownsampledViewSizeX, DownsampledViewSizeY, 1.0f);

		//因为没有用到UV，无所谓UV值
		DrawRectangle(
			RHICmdList,
			0, 0,
			DownsampledViewSizeX, DownsampledViewSizeY,
			0, 0,
			View.ViewRect.Width(), View.ViewRect.Height(),
			FIntPoint(DownsampledViewSizeX, DownsampledViewSizeY),
			View.ViewRect.Size(), 
			ScreenVertexShader,
			EDRF_UseTriangleOptimization);
	}

	//对于RT，在此保证可写
	RHICmdList.TransitionResource(EResourceTransitionAccess::EWritable, RPInfo.ColorRenderTargets[0].RenderTarget);
}



class FMobileTranslucencyUpsamplingPS : public FGlobalShader
{
	//DECLARE_INLINE_TYPE_LAYOUT(FMobileTranslucencyUpsamplingPS, NonVirtual);
	DECLARE_SHADER_TYPE(FMobileTranslucencyUpsamplingPS, Global);
public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	/** Default constructor. */
	FMobileTranslucencyUpsamplingPS()
	{

	}

	LAYOUT_FIELD(FShaderResourceParameter, LowResDepthTexture);
	LAYOUT_FIELD(FShaderResourceParameter, LowResColorTexture);
	//LAYOUT_FIELD(FShaderResourceParameter, FullResDepthTexture); //直接FrameFetch
	LAYOUT_FIELD(FShaderResourceParameter, BilinearClampedSampler);
	LAYOUT_FIELD(FShaderResourceParameter, PointClampedSampler);
	LAYOUT_FIELD(FShaderResourceParameter, BilinearLowDepthClampedSampler);

public:

	/** Initialization constructor. */
	FMobileTranslucencyUpsamplingPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
		LowResDepthTexture.Bind(Initializer.ParameterMap, TEXT("LowResDepthTexture"));
		LowResColorTexture.Bind(Initializer.ParameterMap, TEXT("LowResColorTexture"));
		//FullResDepthTexture.Bind(Initializer.ParameterMap, TEXT("FullResDepthTexture"));
		BilinearClampedSampler.Bind(Initializer.ParameterMap, TEXT("BilinearClampedSampler"));
		PointClampedSampler.Bind(Initializer.ParameterMap, TEXT("PointClampedSampler"));
		BilinearLowDepthClampedSampler.Bind(Initializer.ParameterMap, TEXT("BilinearLowDepthClampedSampler"));
	}

	void SetParameters(FRHICommandList& RHICmdList, const FViewInfo& View)
	{
		FRHIPixelShader* ShaderRHI = RHICmdList.GetBoundPixelShader();

		FGlobalShader::SetParameters<FViewUniformShaderParameters>(RHICmdList, ShaderRHI, View.ViewUniformBuffer);

		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);
		TRefCountPtr<IPooledRenderTarget>& DownsampledTranslucency = SceneContext.SeparateTranslucencyRT;

		//一定使用的Resolve后的LowResColorTexture
		SetTextureParameter(RHICmdList, ShaderRHI, LowResColorTexture, DownsampledTranslucency->GetRenderTargetItem().ShaderResourceTexture);
		SetTextureParameter(RHICmdList, ShaderRHI, LowResDepthTexture, SceneContext.GetDownsampledTranslucencyDepthSurface());
		//SetTextureParameter(RHICmdList, ShaderRHI, FullResDepthTexture, SceneContext.GetSceneDepthSurface());


		SetSamplerParameter(RHICmdList, ShaderRHI, BilinearClampedSampler, TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		SetSamplerParameter(RHICmdList, ShaderRHI, PointClampedSampler, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
		SetSamplerParameter(RHICmdList, ShaderRHI, BilinearLowDepthClampedSampler, TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI());
	}

};


IMPLEMENT_SHADER_TYPE(, FMobileTranslucencyUpsamplingPS, TEXT("/Engine/Private/MobileTranslucencyUpsampling.usf"), TEXT("MobileNearestDepthNeighborUpsamplingPS"), SF_Pixel);

void FMobileSceneRenderer::UpsampleTranslucency(FRHICommandList& RHICmdList, const FViewInfo& View, float DownsamplingScale)
{
	SCOPED_DRAW_EVENTF(RHICmdList, EventUpsampleCopy, TEXT("Upsample translucency"));

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);


	FRHIRenderPassInfo RPInfo(SceneContext.GetSceneColorSurface(), ERenderTargetActions::Load_Store);

	//FRHIRenderPassInfo RPInfo(
	//	SceneContext.GetSceneColorSurface(),
	//	ERenderTargetActions::Load_Store,
	//	nullptr, //暂时不管MSAA
	//	nullptr,
	//	EDepthStencilTargetActions::DontLoad_StoreDepthStencil, //深度与Stencil都不需要
	//	nullptr,
	//	FExclusiveDepthStencil::DepthRead_StencilWrite
	//);

	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, SceneContext.GetDownsampledTranslucencyDepthSurface());
	RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, SceneContext.SeparateTranslucencyRT->GetRenderTargetItem().TargetableTexture);

	//RT Transition
	//TransitionRenderPassTargets(RHICmdList, RPInfo);

	RHICmdList.BeginRenderPass(RPInfo, TEXT("UpsampleTranslucency"));

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

	GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha>::GetRHI();

	TShaderMapRef<FScreenVS> ScreenVertexShader(View.ShaderMap);
	TShaderMapRef<FMobileTranslucencyUpsamplingPS> PixelShader(View.ShaderMap);

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

	PixelShader->SetParameters(RHICmdList, View);

	TRefCountPtr<IPooledRenderTarget>& DownsampledTranslucency = SceneContext.SeparateTranslucencyRT;
	int32 TextureWidth = DownsampledTranslucency->GetDesc().Extent.X;
	int32 TextureHeight = DownsampledTranslucency->GetDesc().Extent.Y;


	RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0.0f, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1.0f);

	//即使TextureSize发生变化，但按照比例贴图刚好写入部分
	DrawRectangle(
		RHICmdList,
		0, 0,
		View.ViewRect.Width(), View.ViewRect.Height(),
		0, 0,
		View.ViewRect.Width() * DownsamplingScale, View.ViewRect.Height() * DownsamplingScale,
		View.ViewRect.Size(),
		FIntPoint(TextureWidth, TextureHeight),
		ScreenVertexShader,
		EDRF_UseTriangleOptimization);

}