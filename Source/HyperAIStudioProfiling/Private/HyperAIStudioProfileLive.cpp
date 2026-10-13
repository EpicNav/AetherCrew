// Games by Hyper 2026.

#include "HyperAIStudioProfileLive.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HyperAIStudioProfileAnalysis.h"
#include "HyperAIStudioProfilingToolset.h"
#include "NiagaraCommon.h"
#include "NiagaraComponent.h"
#include "NiagaraEffectType.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraSystem.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraSystemInstanceController.h"
#include "Particles/FXBudget.h"
#include "Particles/ParticlePerfStatsManager.h"
#include "RenderTimer.h"
#include "RenderUtils.h"
#include "Rendering/NaniteResourcesHelper.h"
#include "Rendering/NaniteStreamingManager.h"
#include "RHIStats.h"
#include "UObject/UObjectIterator.h"

/** The particle perf stats listener fx.ParticlePerfStats uses, read per system. */
class FHyperAIFxCostListener final : public FParticlePerfStatsListener_GatherAll
{
public:
	FHyperAIFxCostListener() : FParticlePerfStatsListener_GatherAll(/*World=*/false, /*System=*/true, /*Component=*/false) {}

	TArray<FHyperAIPerfFxCost> Read()
	{
		TArray<FHyperAIPerfFxCost> Out;
#if WITH_PER_SYSTEM_PARTICLE_PERF_STATS
		// The render thread adds its share in its own commands; let those land first.
		FlushRenderingCommands();
		FScopeLock Lock(&AccumulatedStatsGuard);
		for (const TPair<TWeakObjectPtr<const UFXSystemAsset>, TUniquePtr<FAccumulatedParticlePerfStats>>& Pair : AccumulatedSystemStats)
		{
			const UFXSystemAsset* System = Pair.Key.Get();
			FAccumulatedParticlePerfStats* Stats = Pair.Value.Get();
			if (!System || !Stats) continue;
			const FAccumulatedParticlePerfStats_GT& Game = Stats->GetGameThreadStats();
			const FAccumulatedParticlePerfStats_RT& Render = Stats->GetRenderThreadStats();
			const FAccumulatedParticlePerfStats_GPU& Gpu = Stats->GetGPUStats();
			if (Game.NumFrames == 0) continue;
			FHyperAIPerfFxCost& Cost = Out.AddDefaulted_GetRef();
			Cost.System = System->GetPathName();
			Cost.Instances = static_cast<double>(Game.AccumulatedStats.NumInstances) / Game.NumFrames;
			Cost.GameThreadUs = Game.GetPerFrameAvg();
			Cost.RenderThreadUs = Render.NumFrames > 0 ? Render.GetPerFrameAvg() : 0.0;
			Cost.GpuUs = static_cast<double>(Gpu.GetPerFrameAvgMicroseconds());
			Cost.MemoryKB = static_cast<double>(Game.AccumulatedStats.MemoryBytes) / 1024.0 / Game.NumFrames;
		}
#endif
		return Out;
	}
};

namespace HyperAIStudio::Profiling::Live
{
	namespace
	{
		int32 CVarInt(const TCHAR* Name, const int32 Fallback = -1)
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			return Variable ? Variable->GetInt() : Fallback;
		}

		float CVarFloat(const TCHAR* Name, const float Fallback = -1.f)
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			return Variable ? Variable->GetFloat() : Fallback;
		}

		double Average(const TArray<double>* Values)
		{
			if (!Values || Values->IsEmpty()) return -1.0;
			double Sum = 0.0;
			for (const double Value : *Values) Sum += Value;
			return Sum / Values->Num();
		}

		FString ActorLabel(const UActorComponent& Component)
		{
#if WITH_EDITOR
			if (const AActor* Owner = Component.GetOwner()) return Owner->GetActorLabel();
#endif
			return Component.GetOwner() ? Component.GetOwner()->GetName() : FString(TEXT("no actor"));
		}
	}

	void ReadNanite(UWorld& World, const int32 TopN, FHyperAIPerfNaniteLive& Out)
	{
		Out.bEnabled = UseNanite(GMaxRHIShaderPlatform);
		// Read on the game thread without a lock: these are plain counters the render thread updates, so a value can be a
		// frame old.
		const Nanite::FStreamingManager& Streaming = Nanite::GStreamingManager;
		Out.bHasResources = Streaming.HasResourceEntries();
		Out.StreamingPoolPages = static_cast<int32>(Streaming.GetMaxStreamingPages());
		Out.StreamingPoolMB = static_cast<double>(Streaming.GetMaxStreamingPages()) * NANITE_STREAMING_PAGE_GPU_SIZE / (1024.0 * 1024.0);
		Out.RootPagesLimit = static_cast<int32>(Streaming.GetMaxRootPages());
		Out.QualityScale = Streaming.GetQualityScaleFactor();
		if (Out.StreamingPoolPages == 0)
		{
			Out.StreamingPoolMB = CVarInt(TEXT("r.Nanite.Streaming.StreamingPoolSize"), 0);
			Out.Notes.Add(TEXT("The streaming pool is not allocated yet (nothing drawn with Nanite); its size here is r.Nanite.Streaming.StreamingPoolSize."));
		}

		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!IsValid(Component) || Component->GetWorld() != &World || !Component->IsRegistered() || !Component->ShouldRender()) continue;
			const UStaticMesh* Mesh = Component->GetStaticMesh();
			// A mesh still building would make every read below wait for it.
			if (!Mesh || Mesh->IsCompiling()) continue;
			const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component);
			const int64 Instances = Instanced ? Instanced->GetInstanceCount() : 1;
			if (!Mesh->HasValidNaniteData())
			{
				++Out.NonNanitePrimitives;
				continue;
			}
			if (Nanite::FNaniteResourcesHelper::ShouldCreateNaniteProxy(*Component, nullptr))
			{
				++Out.NanitePrimitives;
				Out.NaniteInstances += Instances;
				continue;
			}
			++Out.FallbackPrimitives;
			if (Out.Fallbacks.Num() < TopN)
			{
				const TCHAR* Why = Component->IsDisallowNanite() || Component->IsForceDisableNanite() ? TEXT("Nanite is disallowed on the component")
					: !Out.bEnabled ? TEXT("Nanite is off for this platform or project")
					: TEXT("a material Nanite cannot draw (translucent, or masked where masking is not allowed)");
				Out.Fallbacks.Add(FString::Printf(TEXT("%s on %s: %s"), *Mesh->GetName(), *ActorLabel(*Component), Why));
			}
		}
		if (Out.FallbackPrimitives > Out.Fallbacks.Num())
		{
			Out.Notes.Add(FString::Printf(TEXT("%d more fallback component(s) not listed."), Out.FallbackPrimitives - Out.Fallbacks.Num()));
		}
		Out.Notes.Add(TEXT("Visible clusters and instances are a GPU readback private to the renderer in UE 5.8 (shown on screen by r.Nanite.ShowStats 1); they are not read here. Per-page eviction counts are not exposed either."));
	}

	void ApplyNaniteSample(const FCsvCapture& Csv, FHyperAIPerfNaniteLive& Out)
	{
		Out.RootAllocationMB = Average(Csv.Column(TEXT("NaniteStreaming/RootAllocationMB")));
		Out.VisibleStreamingDataMB = Average(Csv.Column(TEXT("NaniteStreamingDetail/VisibleStreamingDataSizeMB")));
		Out.IORequestMBps = Average(Csv.Column(TEXT("NaniteStreamingDetail/IORequestSizeMBps")));
		if (const TArray<double>* Requested = Csv.Column(TEXT("NaniteStreamingDetail/NewStreamingDataSizeMB")))
		{
			double Sum = 0.0;
			for (const double Value : *Requested) Sum += Value;
			Out.RequestedMB = Sum;
			Out.RequestedPages = FMath::RoundToInt64(Sum * 1024.0 * 1024.0 / NANITE_STREAMING_PAGE_GPU_SIZE);
		}
		if (Out.VisibleStreamingDataMB > 0.0 && Out.StreamingPoolMB > 0.0 && Out.VisibleStreamingDataMB > Out.StreamingPoolMB)
		{
			Out.Notes.Add(TEXT("The visible set's streaming data is larger than the pool: pages are evicted and requested again every few frames. Raise r.Nanite.Streaming.StreamingPoolSize."));
		}
		if (Out.QualityScale < 0.999)
		{
			Out.Notes.Add(FString::Printf(TEXT("The streamer is at %.0f%% quality because the visible set does not fit the pool."), Out.QualityScale * 100.0));
		}
	}

	void ReadNiagara(UWorld& World, const int32 TopN, FHyperAIPerfNiagaraLive& Out)
	{
		TMap<const UNiagaraSystem*, FHyperAIPerfNiagaraSystem> Systems;
		TMap<const UNiagaraEffectType*, int32> SystemsPerType;
		for (TObjectIterator<UNiagaraComponent> It; It; ++It)
		{
			UNiagaraComponent* Component = *It;
			if (!IsValid(Component) || Component->GetWorld() != &World || !Component->IsRegistered()) continue;
			const UNiagaraSystem* Asset = Component->GetAsset();
			if (!Asset) continue;
			const FNiagaraSystemInstanceControllerConstPtr Controller = Component->GetSystemInstanceController();
			const FNiagaraSystemInstance* Instance = Controller.IsValid() ? Controller->GetSystemInstance_Unsafe() : nullptr;
			if (!Component->IsActive() || !Instance || Instance->IsComplete())
			{
				++Out.InactiveComponents;
				continue;
			}
			++Out.ActiveComponents;
			FHyperAIPerfNiagaraSystem& System = Systems.FindOrAdd(Asset);
			if (System.System.IsEmpty())
			{
				System.System = Asset->GetPathName();
				const UNiagaraEffectType* Type = Asset->GetEffectType();
				System.EffectType = Type ? Type->GetName() : FString(TEXT("none"));
				if (Type) SystemsPerType.FindOrAdd(Type);
			}
			++System.Instances;
			for (const FNiagaraEmitterInstanceRef& Emitter : Instance->GetEmitters())
			{
				if (Emitter->IsComplete()) continue;
				++System.ActiveEmitters;
				++Out.ActiveEmitters;
				const int64 Particles = FMath::Max(0, Emitter->GetNumParticles());
				System.Particles += Particles;
				if (Emitter->GetSimTarget() == ENiagaraSimTarget::GPUComputeSim)
				{
					System.GpuParticles += Particles;
					Out.GpuParticles += Particles;
				}
				else
				{
					System.CpuParticles += Particles;
					Out.CpuParticles += Particles;
				}
				Out.Particles += Particles;
			}
		}
		Systems.GenerateValueArray(Out.Systems);
		Out.Systems.Sort([](const FHyperAIPerfNiagaraSystem& A, const FHyperAIPerfNiagaraSystem& B)
		{
			return A.Particles != B.Particles ? A.Particles > B.Particles : A.Instances > B.Instances;
		});
		if (Out.Systems.Num() > TopN) Out.Systems.SetNum(TopN);

		// Effect types of the systems playing, against their caps at the current quality level.
		for (const TPair<const UNiagaraEffectType*, int32>& Pair : SystemsPerType)
		{
			const UNiagaraEffectType* Type = Pair.Key;
			const FNiagaraSystemScalabilitySettings& Settings = Type->GetActiveSystemScalabilitySettings();
			FHyperAIPerfEffectType& Row = Out.EffectTypes.AddDefaulted_GetRef();
			Row.EffectType = Type->GetName();
			Row.Instances = Type->NumInstances;
			Row.MaxInstances = Settings.bCullMaxInstanceCount ? Settings.MaxInstances : 0;
			Row.MaxSystemInstances = Settings.bCullPerSystemMaxInstanceCount ? Settings.MaxSystemInstances : 0;
			Row.MaxDistance = Settings.bCullByDistance ? Settings.MaxDistance : 0.0;
			Row.CullReaction = StaticEnum<ENiagaraCullReaction>()->GetNameStringByValue(static_cast<int64>(Type->CullReaction));
			Row.bAtCap = Row.MaxInstances > 0 && Row.Instances >= Row.MaxInstances;
			if (Row.bAtCap)
			{
				Out.OverBudget.Add(FString::Printf(TEXT("Effect type %s is at its cap: %d of %d instances, so new spawns of its systems are culled (%s)."),
					*Row.EffectType, Row.Instances, Row.MaxInstances, *Row.CullReaction));
			}
		}
		for (const FHyperAIPerfNiagaraSystem& System : Out.Systems)
		{
			const FHyperAIPerfEffectType* Type = Out.EffectTypes.FindByPredicate([&System](const FHyperAIPerfEffectType& Row) { return Row.EffectType == System.EffectType; });
			if (Type && Type->MaxSystemInstances > 0 && System.Instances >= Type->MaxSystemInstances)
			{
				Out.OverBudget.Add(FString::Printf(TEXT("%s plays %d instances, at its effect type's per-system cap of %d."),
					*System.System, System.Instances, Type->MaxSystemInstances));
			}
		}
#if WITH_GLOBAL_RUNTIME_FX_BUDGET
		if (FFXBudget::Enabled())
		{
			const FFXTimeData Time = FFXBudget::GetTime();
			const FFXTimeData Budget = FFXBudget::GetBudget();
			const FFXTimeData Usage = FFXBudget::GetUsage();
			Out.FxBudget = FString::Printf(TEXT("GT %.2f of %.2f ms, concurrent %.2f of %.2f ms, RT %.2f of %.2f ms"),
				Time.GT, Budget.GT, Time.GTConcurrent, Budget.GTConcurrent, Time.RT, Budget.RT);
			if (Usage.GT > 1.f || Usage.GTConcurrent > 1.f || Usage.RT > 1.f)
			{
				Out.OverBudget.Add(TEXT("The global FX budget is exceeded: ") + Out.FxBudget + TEXT("."));
			}
		}
		else
		{
			Out.FxBudget = TEXT("off (fx.Budget.Enabled 0)");
		}
#endif
		Out.Notes.Add(TEXT("GPU particle counts come from the GPU's last count readback, a frame or more old."));
	}

	void ApplyFxCosts(const TArray<FHyperAIPerfFxCost>& Costs, const int32 TopN, FHyperAIPerfNiagaraLive& Out)
	{
		for (const FHyperAIPerfFxCost& Cost : Costs)
		{
			FHyperAIPerfNiagaraSystem* System = Out.Systems.FindByPredicate([&Cost](const FHyperAIPerfNiagaraSystem& Row) { return Row.System == Cost.System; });
			if (!System)
			{
				// It played during the sample but has finished since.
				System = &Out.Systems.AddDefaulted_GetRef();
				System->System = Cost.System;
				System->EffectType = TEXT("finished before the readout");
			}
			System->GameThreadUs = Cost.GameThreadUs;
			System->RenderThreadUs = Cost.RenderThreadUs;
			System->GpuUs = Cost.GpuUs;
			System->MemoryKB = Cost.MemoryKB;
		}
		Out.Systems.Sort([](const FHyperAIPerfNiagaraSystem& A, const FHyperAIPerfNiagaraSystem& B)
		{
			return A.GameThreadUs != B.GameThreadUs ? A.GameThreadUs > B.GameThreadUs : A.Particles > B.Particles;
		});
		if (Out.Systems.Num() > TopN) Out.Systems.SetNum(TopN);
	}

	TArray<FString> ReadRenderer()
	{
		TArray<FString> Lines;
		auto Method = [](const int32 Value, const TCHAR* const* Names, const int32 Count)
		{
			return Value >= 0 && Value < Count ? FString(Names[Value]) : FString::Printf(TEXT("%d"), Value);
		};
		static const TCHAR* GiNames[] = {TEXT("none"), TEXT("Lumen"), TEXT("screen space"), TEXT("plugin")};
		static const TCHAR* ReflectionNames[] = {TEXT("none"), TEXT("Lumen"), TEXT("screen space")};
		Lines.Add(FString::Printf(TEXT("Global illumination: %s (r.DynamicGlobalIlluminationMethod %d); reflections: %s (r.ReflectionMethod %d); post process volumes can override both per view."),
			*Method(CVarInt(TEXT("r.DynamicGlobalIlluminationMethod")), GiNames, UE_ARRAY_COUNT(GiNames)), CVarInt(TEXT("r.DynamicGlobalIlluminationMethod")),
			*Method(CVarInt(TEXT("r.ReflectionMethod")), ReflectionNames, UE_ARRAY_COUNT(ReflectionNames)), CVarInt(TEXT("r.ReflectionMethod"))));
		Lines.Add(FString::Printf(TEXT("Lumen hardware ray tracing: %d (r.Lumen.HardwareRayTracing). Virtual shadow maps: %d (r.Shadow.Virtual.Enable), page pool %d (r.Shadow.Virtual.MaxPhysicalPages). MegaLights: %d (r.MegaLights.EnableForProject)."),
			CVarInt(TEXT("r.Lumen.HardwareRayTracing")), CVarInt(TEXT("r.Shadow.Virtual.Enable")), CVarInt(TEXT("r.Shadow.Virtual.MaxPhysicalPages")),
			CVarInt(TEXT("r.MegaLights.EnableForProject"))));
		Lines.Add(FString::Printf(TEXT("Nanite: %s. Screen percentage %.0f, anti-aliasing method %d, r.VSync %d, t.MaxFPS %.0f."),
			UseNanite(GMaxRHIShaderPlatform) ? TEXT("on") : TEXT("off"), CVarFloat(TEXT("r.ScreenPercentage")), CVarInt(TEXT("r.AntiAliasingMethod")),
			CVarInt(TEXT("r.VSync")), CVarFloat(TEXT("t.MaxFPS"))));
		const double GpuMs = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles());
		Lines.Add(FString::Printf(TEXT("Last frame: game thread %.1f ms, render thread %.1f ms, %s; %d draw calls, %d primitives."),
			FPlatformTime::ToMilliseconds(GGameThreadTime), FPlatformTime::ToMilliseconds(GRenderThreadTime),
			GUsingNullRHI ? TEXT("GPU time unavailable (null RHI)") : GpuMs > 0.0 ? *FString::Printf(TEXT("GPU %.1f ms"), GpuMs) : TEXT("GPU time unavailable"),
			GNumDrawCallsRHI[0], GNumPrimitivesDrawnRHI[0]));
		return Lines;
	}

	void ApplyRendererSample(const FCsvCapture& Csv, TArray<FString>& Out)
	{
		if (const TArray<double>* Free = Csv.Column(TEXT("VSM/FreePages")))
		{
			const double Least = Free->IsEmpty() ? 0.0 : FMath::Min(*Free);
			Out.Add(FString::Printf(TEXT("Virtual shadow maps: at least %.0f free physical pages over the sample%s."), Least,
				Least < 0.0 ? TEXT(" (negative: the page pool overflowed)") : TEXT("")));
		}
		if (Csv.Column(TEXT("RenderTargetPoolSize")))
		{
			Out.Add(FString::Printf(TEXT("Render target pool: %.0f MB, %.0f MB in use."), Average(Csv.Column(TEXT("RenderTargetPoolSize"))),
				FMath::Max(0.0, Average(Csv.Column(TEXT("RenderTargetPoolUsed"))))));
		}
	}

	FFxCostSampler::FFxCostSampler()
	{
		Listener = MakeShared<FHyperAIFxCostListener, ESPMode::ThreadSafe>();
		bGathering = FParticlePerfStats::GetStatsEnabled();
		if (bGathering)
		{
			// No reset: another listener (fx.ParticlePerfStats.RunTest, the FX budget) keeps its own figures.
			FParticlePerfStatsManager::AddListener(Listener, /*bReset=*/false);
		}
	}

	FFxCostSampler::~FFxCostSampler()
	{
		if (bGathering && Listener.IsValid())
		{
			FParticlePerfStatsManager::RemoveListener(Listener);
		}
	}

	TArray<FHyperAIPerfFxCost> FFxCostSampler::Read(const int32 TopN)
	{
		TArray<FHyperAIPerfFxCost> Costs = bGathering ? Listener->Read() : TArray<FHyperAIPerfFxCost>();
		Costs.Sort([](const FHyperAIPerfFxCost& A, const FHyperAIPerfFxCost& B) { return A.GameThreadUs > B.GameThreadUs; });
		if (Costs.Num() > TopN) Costs.SetNum(TopN);
		return Costs;
	}
}
