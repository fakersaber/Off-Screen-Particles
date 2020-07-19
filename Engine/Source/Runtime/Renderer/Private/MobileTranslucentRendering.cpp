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
		
	if (bShouldRenderTranslucency)
	{
		SCOPED_DRAW_EVENT(RHICmdList, Translucency);

		//新建Pass状态，结束前面Pass
		if (bShouldRenderDownSampleTranslucency) {
			RHICmdList.EndRenderPass();
		}

		for (int32 ViewIndex = 0; ViewIndex < PassViews.Num(); ViewIndex++)
		{
			SCOPED_CONDITIONAL_DRAW_EVENTF(RHICmdList, EventView, Views.Num() > 1, TEXT("View%d"), ViewIndex);

			const FViewInfo& View = *PassViews[ViewIndex];
			if (!View.ShouldRenderView())
			{
				continue;
			}

			if (bShouldRenderDownSampleTranslucency) {

				const float DownsamplingScale = 0.5f;

				FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

				//DownSampleDepth Pass
				RHICmdList.TransitionResource(EResourceTransitionAccess::EReadable, SceneContext.GetSceneDepthSurface());
				MobileDownSampleDepth(RHICmdList, Views[ViewIndex], DownsamplingScale);

				//RenderTranslucency Pass
				//#TODO 可以压到一个Pass与前面DownSampleDepth使用同样的RT，RTINFO只影响SetRenderTarget
				FIntPoint SeparateTranslucencyBufferSize = FIntPoint(SceneContext.GetBufferSizeXY().X * DownsamplingScale, SceneContext.GetBufferSizeXY().Y * DownsamplingScale);

				//#TODO UpSample使用的SeparateTranslucencyRT要Resolve，在BeginPass带上SRV
				SceneContext.BeginRenderingMobileSeparateTranslucency(RHICmdList, View, SeparateTranslucencyBufferSize, DownsamplingScale);

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
				//RHICmdList.EndRenderPass();

				//restore ViewUniformBuffer
				Scene->UniformBuffers.ViewUniformBuffer.UpdateUniformBufferImmediate(*View.CachedViewUniformShaderParameters);

				//UpSample Pass与后续使用同样的RT，所以也可以压到一个Pass
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
		SLSceneDepthTexture.Bind(Initializer.ParameterMap, TEXT("SLSceneDepthTexture"));

	}
	FMobileDownsampleSceneDepthPS() {}

	void SetParameters(FRHICommandList& RHICmdList)
	{
		FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

		//注意MSAA
		SetTextureParameter(RHICmdList, RHICmdList.GetBoundPixelShader(), SLSceneDepthTexture, SceneContext.GetSceneDepthSurface());
	}

	LAYOUT_FIELD(FShaderResourceParameter, SLSceneDepthTexture);
};

IMPLEMENT_SHADER_TYPE(, FMobileDownsampleSceneDepthPS, TEXT("/Engine/Private/MobileDownSampleDepthPixelShader.usf"), TEXT("Main"), SF_Pixel);


void FMobileSceneRenderer::MobileDownSampleDepth(FRHICommandListImmediate& RHICmdList, const FViewInfo& View, float DownsamplingScale) {

	FSceneRenderTargets& SceneContext = FSceneRenderTargets::Get(RHICmdList);

	//先设置为固定的四分之一分辨率 创建DownsampledTranslucencyDepthRT
	int32 ScaledX = SceneContext.GetBufferSizeXY().X * DownsamplingScale;
	int32 ScaledY = SceneContext.GetBufferSizeXY().Y * DownsamplingScale;
	SceneContext.GetDownsampledTranslucencyDepth(RHICmdList, FIntPoint(FMath::Max(ScaledX, 1), FMath::Max(ScaledY, 1)));
	const FTexture2DRHIRef& DownSampleDepthRT = SceneContext.GetDownsampledTranslucencyDepthSurface();

	//Begin DownSample
	FRHIRenderPassInfo RPInfo;
	RPInfo.DepthStencilRenderTarget.Action = EDepthStencilTargetActions::LoadDepthStencil_StoreDepthStencil;
	RPInfo.DepthStencilRenderTarget.DepthStencilTarget = DownSampleDepthRT;
	RPInfo.DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthWrite_StencilWrite;
	RHICmdList.BeginRenderPass(RPInfo, TEXT("DownsampleDepth"));
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
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_Always>::GetRHI();

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit);

		PixelShader->SetParameters(RHICmdList);

		const uint32 DownsampledSizeX = FMath::TruncToInt(View.ViewRect.Width() * DownsamplingScale);
		const uint32 DownsampledSizeY = FMath::TruncToInt(View.ViewRect.Height() * DownsamplingScale);

		RHICmdList.SetViewport(0, 0, 0.0f, DownsampledSizeX, DownsampledSizeY, 1.0f);

		DrawRectangle(
			RHICmdList,
			0, 0,
			DownsampledSizeX, DownsampledSizeY,
			0, 0,
			View.ViewRect.Width(), View.ViewRect.Height(),
			FIntPoint(DownsampledSizeX, DownsampledSizeY),
			SceneContext.GetBufferSizeXY(),
			ScreenVertexShader,
			EDRF_UseTriangleOptimization);
	}
	RHICmdList.EndRenderPass();
}