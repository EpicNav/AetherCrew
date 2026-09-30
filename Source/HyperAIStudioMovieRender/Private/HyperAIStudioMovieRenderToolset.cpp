// Games by Hyper 2026.

#include "HyperAIStudioMovieRenderToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Graph/MovieGraphConfig.h"
#include "Graph/MovieGraphNode.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAsyncJobHost.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSourceControl.h"
#include "LevelSequence.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "MoviePipelineConsoleVariableSetting.h"
#include "MoviePipelineExecutor.h"
#include "MoviePipelineNewProcessExecutor.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelinePIEExecutor.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MoviePipelineQueue.h"
#include "MoviePipelineQueueSubsystem.h"
#include "MoviePipelineSetting.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMovieRender, Log, All);

FString FHyperAIStudioMovieRenderContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioMovieRender.HyperAIStudioMovieRenderToolset");
}

TArray<FString> FHyperAIStudioMovieRenderContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioMovieRenderContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::MovieRenderTools::Private
{
	using C = FHyperAIStudioMovieRenderContracts;

	/** The graph Epic's editor starts a new Movie Render Graph from. */
	const TCHAR* const TemplateGraph = TEXT("/MovieRenderPipeline/DefaultRenderGraph.DefaultRenderGraph");
	/** A local render may run this long before it is stopped; the job host allows no longer. */
	constexpr int32 RenderDeadlineMs = FHyperAIStudioAsyncJobHost::MaxDeadlineMs;

	FString KindOf(const FHyperAIMovieRenderOp& Op)
	{
		return Op.Kind.TrimStartAndEnd().ToLower();
	}

	FString Lower(const FString& Text)
	{
		return Text.TrimStartAndEnd().ToLower().Replace(TEXT(" "), TEXT("_")).Replace(TEXT("-"), TEXT("_"));
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	template <typename T>
	T* Load(const FString& Path)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/"))) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	FString PackageOf(const FString& Reference)
	{
		FString Path = Reference.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
		return Path;
	}

	bool IsNewAssetPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/Game/")) && FPackageName::IsValidLongPackageName(Path) && !FPackageName::DoesPackageExist(Path);
	}

	UMoviePipelineQueueSubsystem* Queues()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UMoviePipelineQueueSubsystem>() : nullptr;
	}

	// --- Settings -------------------------------------------------------------------------------------------------

	/** Friendly names for the config settings, and the classes behind them. */
	struct FSettingKind
	{
		const TCHAR* Name;
		const TCHAR* Class;
	};

	const TArray<FSettingKind>& SettingKinds()
	{
		static const TArray<FSettingKind> Kinds = {
			{TEXT("output"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelineOutputSetting")},
			{TEXT("anti_aliasing"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelineAntiAliasingSetting")},
			{TEXT("camera"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelineCameraSetting")},
			{TEXT("game_overrides"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelineGameOverrideSetting")},
			{TEXT("high_res"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelineHighResSetting")},
			{TEXT("color"), TEXT("/Script/MovieRenderPipelineCore.MoviePipelineColorSetting")},
			{TEXT("console_variables"), TEXT("/Script/MovieRenderPipelineSettings.MoviePipelineConsoleVariableSetting")},
			{TEXT("deferred"), TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineDeferredPassBase")},
			{TEXT("path_tracer"), TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineDeferredPass_PathTracer")},
			{TEXT("png"), TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineImageSequenceOutput_PNG")},
			{TEXT("jpg"), TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineImageSequenceOutput_JPG")},
			{TEXT("bmp"), TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineImageSequenceOutput_BMP")},
			{TEXT("exr"), TEXT("/Script/MovieRenderPipelineRenderPasses.MoviePipelineImageSequenceOutput_EXR")},
			{TEXT("mp4"), TEXT("/Script/MovieRenderPipelineMP4Encoder.MoviePipelineMP4EncoderOutput")}};
		return Kinds;
	}

	const TArray<FString>& Formats()
	{
		static const TArray<FString> All = {TEXT("png"), TEXT("jpg"), TEXT("bmp"), TEXT("exr"), TEXT("mp4")};
		return All;
	}

	/** A setting class by friendly name or class name; null when unknown or its plugin is off. */
	UClass* SettingClass(const FString& Name)
	{
		const FString Wanted = Lower(Name);
		for (const FSettingKind& Kind : SettingKinds())
		{
			UClass* Class = FindObject<UClass>(nullptr, Kind.Class);
			if (Class && (Wanted == Kind.Name || Wanted == Class->GetName().ToLower())) return Class;
		}
		return nullptr;
	}

	FString SettingKindNames()
	{
		TArray<FString> Names;
		for (const FSettingKind& Kind : SettingKinds())
		{
			if (FindObject<UClass>(nullptr, Kind.Class)) Names.Add(Kind.Name);
		}
		return FString::Join(Names, TEXT(", "));
	}

	FString FriendlyName(const UClass& Class)
	{
		for (const FSettingKind& Kind : SettingKinds())
		{
			if (FindObject<UClass>(nullptr, Kind.Class) == &Class) return Kind.Name;
		}
		return Class.GetName();
	}

	bool IsSetting(const FProperty& Property)
	{
		return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated);
	}

	FProperty* FindSetting(const UStruct& Type, const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It) && (It->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || It->GetName().Equals(TEXT("b") + Wanted, ESearchCase::IgnoreCase)))
			{
				return *It;
			}
		}
		return nullptr;
	}

	FString SettingNames(const UStruct& Type)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It) && !It->GetName().StartsWith(TEXT("bOverride_"))) Names.Add(It->GetName());
		}
		return FString::Join(Names, TEXT(", "));
	}

	FString ExportValue(const FProperty& Property, const void* Container, UObject* Owner)
	{
		FString Value;
		Property.ExportText_InContainer(0, Value, Container, Container, Owner, PPF_None);
		return Value.Len() > 200 ? Value.Left(200) + TEXT("...") : Value;
	}

	/**
	 * One property written in Unreal's text form, as the details panel would write it; a Movie Render Graph node's
	 * matching bOverride_ flag is raised so the value takes effect.
	 */
	bool SetProperty(UObject& Object, const FString& Name, const FString& Value, const bool bNotify, FString& OutError)
	{
		FProperty* Property = FindSetting(*Object.GetClass(), Name);
		if (!Property)
		{
			OutError = FString::Printf(TEXT("%s has no setting '%s'; its settings are %s."), *Object.GetClass()->GetName(), *Name, *SettingNames(*Object.GetClass()));
			return false;
		}
		const FString Before = ExportValue(*Property, &Object, &Object);
		if (bNotify) Object.PreEditChange(Property);
		const TCHAR* Rest = Property->ImportText_InContainer(*Value, &Object, &Object, PPF_None);
		if (FBoolProperty* Override = FindFProperty<FBoolProperty>(Object.GetClass(), *(TEXT("bOverride_") + Property->GetName())))
		{
			Override->SetPropertyValue_InContainer(&Object, true);
		}
		if (bNotify)
		{
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Object.PostEditChangeProperty(Event);
		}
		if (!Rest || !FString(Rest).TrimStartAndEnd().IsEmpty())
		{
			OutError = FString::Printf(TEXT("'%s' is not a value for %s (%s); it reads %s."), *Value, *Property->GetName(), *Property->GetCPPType(), *Before);
			return false;
		}
		return true;
	}

	/** The config setting a Settings name opens with (output.FileNameFormat: the output setting), made when missing. */
	UMoviePipelineSetting* SettingFor(UMoviePipelinePrimaryConfig& Config, const FString& Kind, const bool bAdd, FString& OutError)
	{
		UClass* Class = SettingClass(Kind);
		if (!Class)
		{
			OutError = FString::Printf(TEXT("'%s' is not a render setting; they are %s."), *Kind, *SettingKindNames());
			return nullptr;
		}
		UMoviePipelineSetting* Setting = bAdd ? Config.FindOrAddSettingByClass(Class) : Config.FindSettingByClass(Class);
		if (!Setting) OutError = FString::Printf(TEXT("The config has no %s setting."), *Kind);
		return Setting;
	}

	bool ApplyConfigSettings(UMoviePipelinePrimaryConfig& Config, const TArray<FHyperAIMovieRenderSetting>& Settings, const bool bNotify, FString& OutError)
	{
		for (const FHyperAIMovieRenderSetting& Setting : Settings)
		{
			FString Kind;
			FString Property;
			if (!Setting.Name.Split(TEXT("."), &Kind, &Property))
			{
				OutError = FString::Printf(TEXT("'%s' names no setting: write it as setting.Property, like output.FileNameFormat."), *Setting.Name);
				return false;
			}
			UMoviePipelineSetting* Target = SettingFor(Config, Kind, /*bAdd=*/true, OutError);
			if (!Target || !SetProperty(*Target, Property, Setting.Value, bNotify, OutError)) return false;
		}
		return true;
	}

	/** The format's output as the config's only image or video output. */
	bool SetFormat(UMoviePipelinePrimaryConfig& Config, const FString& Format, FString& OutError)
	{
		UClass* Wanted = SettingClass(Format);
		if (!Wanted)
		{
			OutError = FString::Printf(TEXT("'%s' is not a format here: png, exr, jpg, bmp or mp4 (with its plugin)."), *Format);
			return false;
		}
		for (const FString& Other : Formats())
		{
			UClass* Class = SettingClass(Other);
			UMoviePipelineSetting* Existing = Class && Class != Wanted ? Config.FindSettingByClass(Class, /*bIncludeDisabledSettings=*/true) : nullptr;
			if (Existing) Config.RemoveSetting(Existing);
		}
		Config.FindOrAddSettingByClass(Wanted);
		return true;
	}

	/** Draft renders fast; final renders with accumulated samples, warm-up frames and cinematic quality. */
	void SetPreset(UMoviePipelinePrimaryConfig& Config, const FString& Preset)
	{
		const bool bFinal = Lower(Preset) == TEXT("final");
		FString Ignored;
		UMoviePipelineSetting* AntiAliasing = SettingFor(Config, TEXT("anti_aliasing"), /*bAdd=*/true, Ignored);
		if (AntiAliasing)
		{
			SetProperty(*AntiAliasing, TEXT("SpatialSampleCount"), TEXT("1"), false, Ignored);
			SetProperty(*AntiAliasing, TEXT("TemporalSampleCount"), bFinal ? TEXT("8") : TEXT("1"), false, Ignored);
			SetProperty(*AntiAliasing, TEXT("bOverrideAntiAliasing"), bFinal ? TEXT("True") : TEXT("False"), false, Ignored);
			if (bFinal) SetProperty(*AntiAliasing, TEXT("AntiAliasingMethod"), TEXT("AAM_None"), false, Ignored);
			SetProperty(*AntiAliasing, TEXT("EngineWarmUpCount"), bFinal ? TEXT("32") : TEXT("0"), false, Ignored);
			SetProperty(*AntiAliasing, TEXT("RenderWarmUpCount"), bFinal ? TEXT("32") : TEXT("0"), false, Ignored);
		}
		if (!bFinal) return;
		SettingFor(Config, TEXT("game_overrides"), /*bAdd=*/true, Ignored);
		if (UMoviePipelineConsoleVariableSetting* Variables = Cast<UMoviePipelineConsoleVariableSetting>(SettingFor(Config, TEXT("console_variables"), /*bAdd=*/true, Ignored)))
		{
			// Film quality where games trade it for speed.
			const TPair<const TCHAR*, float> Cinematic[] = {
				{TEXT("r.MotionBlurQuality"), 4.f}, {TEXT("r.MotionBlurSeparable"), 1.f}, {TEXT("r.DepthOfFieldQuality"), 4.f},
				{TEXT("r.BloomQuality"), 5.f}, {TEXT("r.Tonemapper.Quality"), 5.f}, {TEXT("r.ViewDistanceScale"), 50.f},
				{TEXT("r.SkeletalMeshLODBias"), -2.f}, {TEXT("r.Shadow.DistanceScale"), 2.f}};
			for (const TPair<const TCHAR*, float>& Variable : Cinematic)
			{
				Variables->AddOrUpdateConsoleVariable(Variable.Key, Variable.Value);
			}
		}
	}

	/** The op's own values onto a config: preset, format, resolution, frame rate, folder, settings and console variables. */
	bool ShapeConfig(UMoviePipelinePrimaryConfig& Config, const FHyperAIMovieRenderOp& Op, const bool bNotify, FString& OutError)
	{
		FString Ignored;
		SettingFor(Config, TEXT("deferred"), /*bAdd=*/true, Ignored);
		if (!Op.Preset.IsEmpty()) SetPreset(Config, Op.Preset);
		if (!Op.Format.IsEmpty() && !SetFormat(Config, Op.Format, OutError)) return false;
		UMoviePipelineSetting* Output = SettingFor(Config, TEXT("output"), /*bAdd=*/true, OutError);
		if (!Output) return false;
		if (Op.Resolution.X > 0 && Op.Resolution.Y > 0
			&& !SetProperty(*Output, TEXT("OutputResolution"), FString::Printf(TEXT("(X=%d,Y=%d)"), Op.Resolution.X, Op.Resolution.Y), bNotify, OutError))
		{
			return false;
		}
		if (Op.FrameRate > 0.f)
		{
			SetProperty(*Output, TEXT("bUseCustomFrameRate"), TEXT("True"), bNotify, Ignored);
			if (!SetProperty(*Output, TEXT("OutputFrameRate"), FString::Printf(TEXT("(Numerator=%d,Denominator=1)"), FMath::RoundToInt32(Op.FrameRate)), bNotify, OutError)) return false;
		}
		if (!Op.Directory.IsEmpty() && !SetProperty(*Output, TEXT("OutputDirectory"), FString::Printf(TEXT("(Path=\"%s\")"), *Op.Directory.Replace(TEXT("\\"), TEXT("/"))), bNotify, OutError))
		{
			return false;
		}
		if (!ApplyConfigSettings(Config, Op.Settings, bNotify, OutError)) return false;
		if (!Op.ConsoleVariables.IsEmpty())
		{
			UMoviePipelineConsoleVariableSetting* Variables = Cast<UMoviePipelineConsoleVariableSetting>(SettingFor(Config, TEXT("console_variables"), /*bAdd=*/true, OutError));
			if (!Variables) return false;
			for (const FHyperAIMovieRenderSetting& Variable : Op.ConsoleVariables)
			{
				Variables->AddOrUpdateConsoleVariable(Variable.Name.TrimStartAndEnd(), FCString::Atof(*Variable.Value));
			}
		}
		return true;
	}

	TArray<FString> DescribeConfig(UMoviePipelinePrimaryConfig& Config)
	{
		TArray<FString> Lines;
		for (UMoviePipelineSetting* Setting : Config.GetUserSettings())
		{
			if (!Setting) continue;
			TArray<FString> Values;
			for (TFieldIterator<FProperty> It(Setting->GetClass()); It; ++It)
			{
				if (!IsSetting(**It) || It->GetOwnerClass() == UMoviePipelineSetting::StaticClass()) continue;
				const FString Value = ExportValue(**It, Setting, Setting);
				if (!Value.IsEmpty()) Values.Add(It->GetName() + TEXT("=") + Value);
			}
			if (const UMoviePipelineConsoleVariableSetting* Variables = Cast<UMoviePipelineConsoleVariableSetting>(Setting))
			{
				Values.Reset();
				for (const FMoviePipelineConsoleVariableEntry& Entry : Variables->GetConsoleVariables())
				{
					Values.Add(FString::Printf(TEXT("%s %g"), *Entry.Name, Entry.Value));
				}
			}
			Lines.Add(FriendlyName(*Setting->GetClass()) + TEXT(": ") + FString::Join(Values, TEXT(", ")).Left(1200));
		}
		return Lines;
	}

	// --- Graphs ---------------------------------------------------------------------------------------------------

	FString NodeName(const UMovieGraphNode& Node)
	{
		FString Name = Node.GetClass()->GetName();
		Name.RemoveFromStart(TEXT("MovieGraph"));
		Name.RemoveFromEnd(TEXT("Node"));
		return Name;
	}

	/** A graph node by its class (GlobalOutputSetting, DeferredRenderPass...) or its title. */
	UMovieGraphNode* FindNode(UMovieGraphConfig& Graph, const FString& Name)
	{
		const FString Wanted = Lower(Name).Replace(TEXT("_"), TEXT(""));
		for (UMovieGraphNode* Node : Graph.GetNodes())
		{
			if (!Node) continue;
			const FString Class = NodeName(*Node).ToLower();
			const FString Title = Node->GetNodeTitle().ToString().ToLower().Replace(TEXT(" "), TEXT(""));
			if (Class == Wanted || Class.StartsWith(Wanted) || Wanted.StartsWith(Class) || Title == Wanted) return Node;
		}
		return nullptr;
	}

	FString NodeNames(UMovieGraphConfig& Graph)
	{
		TArray<FString> Names;
		for (UMovieGraphNode* Node : Graph.GetNodes())
		{
			if (Node) Names.Add(NodeName(*Node));
		}
		return FString::Join(Names, TEXT(", "));
	}

	bool SetGraphProperty(UMovieGraphConfig& Graph, const FString& NodeKey, const FString& Property, const FString& Value, const bool bNotify, FString& OutError)
	{
		UMovieGraphNode* Node = FindNode(Graph, NodeKey);
		if (!Node)
		{
			OutError = FString::Printf(TEXT("The graph has no %s node; its nodes are %s."), *NodeKey, *NodeNames(Graph));
			return false;
		}
		if (bNotify) Node->Modify();
		return SetProperty(*Node, Property, Value, bNotify, OutError);
	}

	/** The op's own values onto a graph's global output settings, then its settings by node. */
	bool ShapeGraph(UMovieGraphConfig& Graph, const FHyperAIMovieRenderOp& Op, const bool bNotify, FString& OutError)
	{
		if (Op.Resolution.X > 0 && Op.Resolution.Y > 0 && !SetGraphProperty(Graph, TEXT("GlobalOutputSetting"), TEXT("OutputResolution"),
			FString::Printf(TEXT("(ProfileName=\"Custom\",Resolution=(X=%d,Y=%d))"), Op.Resolution.X, Op.Resolution.Y), bNotify, OutError))
		{
			return false;
		}
		if (Op.FrameRate > 0.f && !SetGraphProperty(Graph, TEXT("GlobalOutputSetting"), TEXT("OutputFrameRate"),
			FString::Printf(TEXT("(Numerator=%d,Denominator=1)"), FMath::RoundToInt32(Op.FrameRate)), bNotify, OutError))
		{
			return false;
		}
		if (!Op.Directory.IsEmpty() && !SetGraphProperty(Graph, TEXT("GlobalOutputSetting"), TEXT("OutputDirectory"),
			FString::Printf(TEXT("(Path=\"%s\")"), *Op.Directory.Replace(TEXT("\\"), TEXT("/"))), bNotify, OutError))
		{
			return false;
		}
		for (const FHyperAIMovieRenderSetting& Setting : Op.Settings)
		{
			FString Node;
			FString Property;
			if (!Setting.Name.Split(TEXT("."), &Node, &Property))
			{
				OutError = FString::Printf(TEXT("'%s' names no node: write it as Node.Property, like GlobalOutputSetting.OutputFrameRate."), *Setting.Name);
				return false;
			}
			if (!SetGraphProperty(Graph, Node, Property, Setting.Value, bNotify, OutError)) return false;
		}
		return true;
	}

	TArray<FString> DescribeGraph(UMovieGraphConfig& Graph)
	{
		TArray<FString> Lines;
		for (UMovieGraphNode* Node : Graph.GetNodes())
		{
			if (!Node) continue;
			TArray<FString> Values;
			for (TFieldIterator<FBoolProperty> It(Node->GetClass()); It; ++It)
			{
				FString Name = It->GetName();
				if (!Name.RemoveFromStart(TEXT("bOverride_")) || !It->GetPropertyValue_InContainer(Node)) continue;
				if (const FProperty* Value = Node->GetClass()->FindPropertyByName(FName(*Name))) Values.Add(Name + TEXT("=") + ExportValue(*Value, Node, Node));
			}
			Lines.Add(FString::Printf(TEXT("%s (%s)%s"), *NodeName(*Node), *Node->GetNodeTitle().ToString(), Values.IsEmpty() ? TEXT("") : *(TEXT(": ") + FString::Join(Values, TEXT(", ")))));
		}
		return Lines;
	}

	// --- Rendering ------------------------------------------------------------------------------------------------

	/** One render's life, shared by the executor's callbacks and the job host's polls. */
	struct FRender
	{
		TStrongObjectPtr<UMoviePipelineQueue> Queue;
		TStrongObjectPtr<UMoviePipelineExecutorBase> Executor;
		FString Directory;
		FString Sequence;
		bool bDone = false;
		bool bOk = false;
		FString Error;
	};

	int32 CountFrames(const FString& Directory)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("*.*"), /*Files=*/true, /*Directories=*/false);
		return Files.Num();
	}

	/** A render started: the queue job built, the executor told, and a job the Activity panel and hyper_operation_wait follow. */
	bool StartRender(UWorld& World, const FHyperAIMovieRenderOp& Op, const FString& OperationId, FHyperAIMovieRenderPlanReport& Report, FString& OutError)
	{
		UMoviePipelineQueueSubsystem* Subsystem = Queues();
		if (!Subsystem)
		{
			OutError = TEXT("The Movie Render Queue is not available.");
			return false;
		}
		if (Subsystem->IsRendering())
		{
			OutError = TEXT("A render is running already; wait for it, or cancel it from the Activity panel.");
			return false;
		}
		ULevelSequence* Sequence = Load<ULevelSequence>(Op.Sequence);
		if (!Sequence)
		{
			OutError = FString::Printf(TEXT("%s is not a level sequence."), *Op.Sequence);
			return false;
		}
		const bool bRemote = Lower(Op.Executor) == TEXT("remote");
		const FString Map = Op.Map.TrimStartAndEnd().IsEmpty() ? World.GetOutermost()->GetName() : PackageOf(Op.Map);
		const FString Directory = Op.Directory.TrimStartAndEnd().IsEmpty()
			? FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MovieRenders") / (Sequence->GetName() + FDateTime::Now().ToString(TEXT("_%Y%m%d_%H%M%S"))))
			: Op.Directory.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
		TSharedRef<FRender> Render = MakeShared<FRender>();
		Render->Queue.Reset(NewObject<UMoviePipelineQueue>(GetTransientPackage()));
		UMoviePipelineExecutorJob* Job = Render->Queue->AllocateNewJob(UMoviePipelineExecutorJob::StaticClass());
		Job->JobName = Sequence->GetName();
		Job->SetSequence(FSoftObjectPath(Sequence));
		Job->Map = FSoftObjectPath(Map + TEXT(".") + FPackageName::GetShortName(Map));
		FHyperAIMovieRenderOp Shape = Op;
		Shape.Directory = Directory;
		if (UMovieGraphConfig* Graph = Load<UMovieGraphConfig>(Op.Asset))
		{
			Job->SetGraphPreset(Graph);
			Report.Notes.Add(FString::Printf(TEXT("Rendering with graph %s; its own output folder applies."), *Graph->GetName()));
		}
		else
		{
			// The job renders with its own copy of the config, so the render's folder never changes the asset.
			if (UMoviePipelinePrimaryConfig* Preset = Load<UMoviePipelinePrimaryConfig>(Op.Asset))
			{
				Job->SetConfiguration(Preset);
				Shape.Preset.Reset();
			}
			else if (Shape.Preset.IsEmpty())
			{
				Shape.Preset = TEXT("draft");
			}
			if (Shape.Format.IsEmpty() && !Load<UMoviePipelinePrimaryConfig>(Op.Asset)) Shape.Format = TEXT("png");
			if (!ShapeConfig(*Job->GetConfiguration(), Shape, /*bNotify=*/false, OutError)) return false;
			Render->Directory = Directory;
		}
		Render->Sequence = Sequence->GetPathName();
		if (bRemote)
		{
			Render->Executor.Reset(NewObject<UMoviePipelineNewProcessExecutor>(GetTransientPackage()));
		}
		else
		{
			UMoviePipelinePIEExecutor* Local = NewObject<UMoviePipelinePIEExecutor>(GetTransientPackage());
			Local->SetIsRenderingOffscreen(true);
			Local->SetAllowUsingUnsavedLevels(true);
			Render->Executor.Reset(Local);
		}
		TWeakPtr<FRender> Weak = Render;
		Render->Executor->OnExecutorFinished().AddLambda([Weak](UMoviePipelineExecutorBase*, const bool bSuccess)
		{
			if (const TSharedPtr<FRender> Pinned = Weak.Pin())
			{
				Pinned->bDone = true;
				Pinned->bOk = bSuccess && Pinned->Error.IsEmpty();
			}
		});
		Render->Executor->OnExecutorErrored().AddLambda([Weak](UMoviePipelineExecutorBase*, UMoviePipeline*, const bool bFatal, const FText Reason)
		{
			if (const TSharedPtr<FRender> Pinned = Weak.Pin())
			{
				Pinned->Error = Reason.ToString();
				if (bFatal) Pinned->bOk = false;
			}
		});
		FHyperAIStudioAsyncJobRequest Request;
		Request.PackId = C::PackId;
		Request.ToolName = C::PlanToolName;
		Request.Target = Render->Sequence;
		Request.OperationId = OperationId;
		Request.DeadlineMs = RenderDeadlineMs;
		Request.PollIntervalMs = 500;
		// A remote render runs in its own process: the wait may end first, but the render goes on.
		if (!bRemote)
		{
			Request.OnCancel = [Weak]()
			{
				if (const TSharedPtr<FRender> Pinned = Weak.Pin())
				{
					if (Pinned->Executor.IsValid()) Pinned->Executor->CancelAllJobs();
				}
			};
		}
		FString JobId;
		const bool bStarted = FHyperAIStudioAsyncJobHost::Start(Request, [Render](FString& OutProgress, FString& OutDiagnostic)
		{
			const float Progress = Render->Executor.IsValid() ? Render->Executor->GetStatusProgress() : 0.f;
			OutProgress = FString::Printf(TEXT("%.0f%% of %s"), Progress * 100.f, *FPackageName::ObjectPathToObjectName(Render->Sequence));
			if (!Render->bDone) return EHyperAIStudioAsyncJobPoll::Running;
			const int32 Frames = Render->Directory.IsEmpty() ? 0 : CountFrames(Render->Directory);
			OutDiagnostic = Render->bOk
				? (Render->Directory.IsEmpty() ? FString(TEXT("Rendered.")) : FString::Printf(TEXT("Rendered %d file(s) to %s."), Frames, *Render->Directory))
				: FString::Printf(TEXT("The render failed%s."), Render->Error.IsEmpty() ? TEXT("") : *(TEXT(": ") + Render->Error));
			Render->Executor.Reset();
			Render->Queue.Reset();
			return Render->bOk ? EHyperAIStudioAsyncJobPoll::Completed : EHyperAIStudioAsyncJobPoll::Failed;
		}, JobId, OutError);
		if (!bStarted) return false;
		Subsystem->RenderQueueInstanceWithExecutorInstance(Render->Queue.Get(), Render->Executor.Get());
		Report.Output = Render->Directory;
		Report.Notes.Add(FString::Printf(TEXT("Rendering %s in %s %s%s."), *Sequence->GetName(), *FPackageName::GetShortName(Map), bRemote ? TEXT("in a new process") : TEXT("here, off screen"),
			Render->Directory.IsEmpty() ? TEXT("") : *(TEXT(", to ") + Render->Directory)));
		return true;
	}

	// --- Plan -----------------------------------------------------------------------------------------------------

	bool Check(UWorld& World, const FHyperAIMovieRenderPlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		TSet<FString> NewConfigs;
		TSet<FString> NewGraphs;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIMovieRenderOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			FString Why;
			const FString Path = PackageOf(Op.Asset);
			if (!Op.Preset.IsEmpty() && Lower(Op.Preset) != TEXT("draft") && Lower(Op.Preset) != TEXT("final")) return Fail(TEXT("Preset is draft or final."));
			if (!Op.Format.IsEmpty() && (!Formats().Contains(Lower(Op.Format)) || !SettingClass(Op.Format)))
			{
				return Fail(FString::Printf(TEXT("'%s' is not a format here: png, exr, jpg, bmp, or mp4 with its plugin."), *Op.Format));
			}
			if ((Op.Resolution.X != 0 || Op.Resolution.Y != 0) && !(Op.Resolution.X >= 16 && Op.Resolution.Y >= 16 && Op.Resolution.X <= 16384 && Op.Resolution.Y <= 16384))
			{
				return Fail(TEXT("Resolution is 16 to 16384 pixels a side."));
			}
			if (Op.FrameRate != 0.f && !(Op.FrameRate >= 1.f && Op.FrameRate <= 240.f)) return Fail(TEXT("FrameRate is 1 to 240."));
			if (!Op.Directory.IsEmpty() && FPaths::IsRelative(Op.Directory)) return Fail(TEXT("Directory is an absolute folder."));
			if (Kind == TEXT("config.create") || Kind == TEXT("config.set"))
			{
				const bool bCreate = Kind == TEXT("config.create");
				if (bCreate && (!IsNewAssetPath(Path) || NewConfigs.Contains(Path.ToLower()))) return Fail(TEXT("Name the new config as a free path under /Game, like /Game/Cinematics/MRQ_Final."));
				if (!bCreate && !Load<UMoviePipelinePrimaryConfig>(Path) && !NewConfigs.Contains(Path.ToLower())) return Fail(FString::Printf(TEXT("%s is not a Movie Render Queue config."), *Op.Asset));
				// The values, tried on a scratch config that nothing keeps.
				UMoviePipelinePrimaryConfig* Scratch = NewObject<UMoviePipelinePrimaryConfig>(GetTransientPackage(), NAME_None, RF_Transient);
				if (!ShapeConfig(*Scratch, Op, /*bNotify=*/false, Why)) return Fail(Why);
				if (bCreate) NewConfigs.Add(Path.ToLower());
			}
			else if (Kind == TEXT("graph.create") || Kind == TEXT("graph.set"))
			{
				const bool bCreate = Kind == TEXT("graph.create");
				UMovieGraphConfig* Existing = bCreate ? Load<UMovieGraphConfig>(FString(TemplateGraph)) : Load<UMovieGraphConfig>(Path);
				if (bCreate && (!IsNewAssetPath(Path) || NewGraphs.Contains(Path.ToLower()))) return Fail(TEXT("Name the new graph as a free path under /Game, like /Game/Cinematics/MRG_Final."));
				if (!Existing && !(NewGraphs.Contains(Path.ToLower()))) return Fail(bCreate ? FString(TEXT("Epic's default render graph is missing.")) : FString::Printf(TEXT("%s is not a Movie Render Graph."), *Op.Asset));
				if (!Op.Format.IsEmpty() || !Op.Preset.IsEmpty()) return Fail(TEXT("A graph's outputs and sampling are its nodes: set them with Settings (Node.Property)."));
				if (Existing)
				{
					UMovieGraphConfig* Scratch = DuplicateObject<UMovieGraphConfig>(Existing, GetTransientPackage());
					Scratch->ClearFlags(RF_Public | RF_Standalone);
					Scratch->SetFlags(RF_Transient);
					if (!ShapeGraph(*Scratch, Op, /*bNotify=*/false, Why)) return Fail(Why);
				}
				if (bCreate) NewGraphs.Add(Path.ToLower());
			}
			else if (Kind == TEXT("render"))
			{
				if (Index != Request.Operations.Num() - 1) return Fail(TEXT("A plan renders once, as its last op."));
				if (!Load<ULevelSequence>(Op.Sequence)) return Fail(FString::Printf(TEXT("%s is not a level sequence."), *Op.Sequence));
				const FString Executor = Lower(Op.Executor);
				if (!Executor.IsEmpty() && Executor != TEXT("local") && Executor != TEXT("remote")) return Fail(TEXT("Executor is local or remote."));
				const FString Map = Op.Map.TrimStartAndEnd().IsEmpty() ? World.GetOutermost()->GetName() : PackageOf(Op.Map);
				if (Executor == TEXT("remote") && (Map.StartsWith(TEXT("/Temp/")) || !FPackageName::DoesPackageExist(Map)))
				{
					return Fail(TEXT("A remote render loads the level from disk: save it first, or render locally."));
				}
				const bool bAsset = !Op.Asset.IsEmpty();
				if (bAsset && !Load<UMoviePipelinePrimaryConfig>(Path) && !Load<UMovieGraphConfig>(Path) && !NewConfigs.Contains(Path.ToLower()) && !NewGraphs.Contains(Path.ToLower()))
				{
					return Fail(FString::Printf(TEXT("%s is neither a render config nor a render graph."), *Op.Asset));
				}
				if (!bAsset)
				{
					UMoviePipelinePrimaryConfig* Scratch = NewObject<UMoviePipelinePrimaryConfig>(GetTransientPackage(), NAME_None, RF_Transient);
					if (!ShapeConfig(*Scratch, Op, /*bNotify=*/false, Why)) return Fail(Why);
				}
				if (Queues() && Queues()->IsRendering()) return Fail(TEXT("A render is running already."));
			}
			else
			{
				return Fail(TEXT("Unknown op; see hyper_movie_render_apply_plan's description."));
			}
		}
		return true;
	}

	bool SaveAsset(UObject& Asset, FString& OutError)
	{
		UPackage* Package = Asset.GetOutermost();
		if (!FHyperAIStudioSourceControl::MakeEditable({Package->GetName()}, OutError)) return false;
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, &Asset, *File, Args))
		{
			OutError = FString::Printf(TEXT("%s could not be saved."), *Package->GetName());
			return false;
		}
		return true;
	}

	/** One op; configs and graphs are saved as they change, and a render starts last. */
	bool Apply(UWorld& World, const FHyperAIMovieRenderOp& Op, const FString& OperationId, FHyperAIMovieRenderPlanReport& Report, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		const FString Path = PackageOf(Op.Asset);
		UObject* Saved = nullptr;
		if (Kind == TEXT("config.create") || Kind == TEXT("config.set"))
		{
			UMoviePipelinePrimaryConfig* Config = nullptr;
			if (Kind == TEXT("config.create"))
			{
				UPackage* Package = CreatePackage(*Path);
				Config = NewObject<UMoviePipelinePrimaryConfig>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
				if (Op.Preset.IsEmpty()) SetPreset(*Config, TEXT("draft"));
				if (Op.Format.IsEmpty()) SetFormat(*Config, TEXT("png"), OutError);
				FAssetRegistryModule::AssetCreated(Config);
			}
			else
			{
				Config = Load<UMoviePipelinePrimaryConfig>(Path);
				Config->Modify();
			}
			if (!ShapeConfig(*Config, Op, /*bNotify=*/true, OutError)) return false;
			Config->MarkPackageDirty();
			Saved = Config;
			Report.Notes.Add(FString::Printf(TEXT("Config %s: %s."), *Config->GetName(), *FString::Join(DescribeConfig(*Config), TEXT("; ")).Left(600)));
		}
		else if (Kind == TEXT("graph.create") || Kind == TEXT("graph.set"))
		{
			UMovieGraphConfig* Graph = nullptr;
			if (Kind == TEXT("graph.create"))
			{
				UPackage* Package = CreatePackage(*Path);
				Graph = DuplicateObject<UMovieGraphConfig>(Load<UMovieGraphConfig>(FString(TemplateGraph)), Package, *FPackageName::GetShortName(Path));
				Graph->SetFlags(RF_Public | RF_Standalone | RF_Transactional);
				FAssetRegistryModule::AssetCreated(Graph);
			}
			else
			{
				Graph = Load<UMovieGraphConfig>(Path);
				Graph->Modify();
			}
			if (!ShapeGraph(*Graph, Op, /*bNotify=*/true, OutError)) return false;
			Graph->MarkPackageDirty();
			Saved = Graph;
			Report.Notes.Add(FString::Printf(TEXT("Graph %s: %s."), *Graph->GetName(), *FString::Join(DescribeGraph(*Graph), TEXT("; ")).Left(600)));
		}
		else if (Kind == TEXT("render"))
		{
			return StartRender(World, Op, OperationId, Report, OutError);
		}
		if (Saved)
		{
			if (!SaveAsset(*Saved, OutError)) return false;
			Report.Saved.Add(Saved->GetPathName());
		}
		return true;
	}

	FHyperAIMovieRenderPlanReport Execute(const FHyperAIMovieRenderPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIMovieRenderPlanReport Report;
		Report.OperationId = OperationId;
		UWorld* World = EditorWorld();
		if (!World)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("No level is open in the editor.");
			return Report;
		}
		bool bRendering = false;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			FString Error;
			if (!Apply(*World, Request.Operations[Index], OperationId, Report, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Request.Operations[Index].Kind, *Error,
					Report.Applied > 0 ? TEXT(" The configs and graphs before it are saved.") : TEXT(""));
				return Report;
			}
			bRendering |= KindOf(Request.Operations[Index]) == TEXT("render");
			++Report.Applied;
		}
		Report.bOk = true;
		Report.Status = bRendering ? TEXT("rendering") : TEXT("completed");
		Report.Diagnostic = bRendering
			? FString::Printf(TEXT("%d operation(s) applied and the render started; call hyper_operation_wait with operation_id %s to wait for its frames."), Report.Applied, *OperationId)
			: FString::Printf(TEXT("%d operation(s) applied and saved."), Report.Applied);
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIMovieRenderPlanReport& Done)
	{
		// A render's own job reports when it finishes; this reports only a plan that renders nothing, or fails.
		if (Done.Status == TEXT("rendering")) return;
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Saved.IsEmpty() ? FString() : Done.Saved[0].Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAIMovieRenderInspectReport FHyperAIStudioMovieRenderContracts::Inspect(const FHyperAIMovieRenderInspectRequest& Request)
{
	using namespace HyperAIStudio::MovieRenderTools::Private;
	FHyperAIMovieRenderInspectReport Report;
	for (const FHyperAIStudioAsyncJobStatus& Job : FHyperAIStudioAsyncJobHost::GetSnapshot())
	{
		if (Job.ToolName == PlanToolName && Job.State == EHyperAIStudioAsyncJobState::Running)
		{
			Report.Renders.Add(FString::Printf(TEXT("%s: %s, %s"), *Job.OperationId, *Job.Target, *Job.Progress));
		}
	}
	if (!Request.Asset.TrimStartAndEnd().IsEmpty())
	{
		if (UMoviePipelinePrimaryConfig* Config = Load<UMoviePipelinePrimaryConfig>(Request.Asset))
		{
			Report.Settings = DescribeConfig(*Config);
		}
		else if (UMovieGraphConfig* Graph = Load<UMovieGraphConfig>(Request.Asset))
		{
			Report.Settings = DescribeGraph(*Graph);
		}
		else
		{
			Report.Diagnostic = FString::Printf(TEXT("%s is neither a render config nor a render graph."), *Request.Asset);
			return Report;
		}
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%s: %d setting(s) or node(s); %d render(s) running."), *Request.Asset, Report.Settings.Num(), Report.Renders.Num());
		return Report;
	}
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	Registry.GetAssetsByClass(UMoviePipelinePrimaryConfig::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.PackageName.ToString().StartsWith(TEXT("/Game/")) && Report.Configs.Num() < 100) Report.Configs.Add(Asset.GetObjectPathString());
	}
	Assets.Reset();
	Registry.GetAssetsByClass(UMovieGraphConfig::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.PackageName.ToString().StartsWith(TEXT("/Game/")) && Report.Graphs.Num() < 100) Report.Graphs.Add(Asset.GetObjectPathString());
	}
	Report.Configs.Sort();
	Report.Graphs.Sort();
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%d config(s), %d graph(s) under /Game; %d render(s) running."), Report.Configs.Num(), Report.Graphs.Num(), Report.Renders.Num());
	return Report;
}

FHyperAIMovieRenderPlanReport FHyperAIStudioMovieRenderContracts::ApplyPlan(const FHyperAIMovieRenderPlanRequest& Request)
{
	using namespace HyperAIStudio::MovieRenderTools::Private;
	FHyperAIMovieRenderPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("render-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	UWorld* World = EditorWorld();
	FString Error;
	if (!World)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	if (!Check(*World, Request, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked; nothing was changed."), Request.Operations.Num());
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("movie_render.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.Operations.Last().Sequence.IsEmpty() ? Request.Operations[0].Asset : Request.Operations.Last().Sequence;
		Summary.Effects.Add(FString::Printf(TEXT("Render configs, graphs and renders: %d operation(s)"), Request.Operations.Num()));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIMovieRenderOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s"), *Op.Kind, Op.Sequence.IsEmpty() ? *Op.Asset : *Op.Sequence));
			Summary.Touches.AddUnique(Op.Sequence.IsEmpty() ? Op.Asset : Op.Sequence);
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				Record(OperationId, Execute(Request, OperationId));
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		return Report;
	}
	return Execute(Request, Report.OperationId);
}

FHyperAIMovieRenderInspectReport UHyperAIStudioMovieRenderToolset::hyper_movie_render_inspect(const FHyperAIMovieRenderInspectRequest& Request)
{
	return FHyperAIStudioMovieRenderContracts::Inspect(Request);
}

FHyperAIMovieRenderPlanReport UHyperAIStudioMovieRenderToolset::hyper_movie_render_apply_plan(const FHyperAIMovieRenderPlanRequest& Request)
{
	return FHyperAIStudioMovieRenderContracts::ApplyPlan(Request);
}

void FHyperAIStudioMovieRenderRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioMovieRenderRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioMovieRenderRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioMovieRenderToolset::StaticClass(),
			FHyperAIStudioMovieRenderContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMovieRender, Warning, TEXT("Could not unregister the Movie Render toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioMovieRenderRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioMovieRenderContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioMovieRender, Verbose, TEXT("The Movie Render source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioMovieRenderToolset::StaticClass(),
		FHyperAIStudioMovieRenderContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioMovieRender, Error, TEXT("Movie Render cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}
