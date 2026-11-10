// Games by Hyper 2026.

#include "HyperAIStudioAudioAuthoringToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioAudioAnalysis.h"
#include "HyperAIStudioAudioWrappers.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioMetaSoundGraph.h"
#include "HyperAIStudioMetaSoundRecipes.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSoundCueGraph.h"
#include "HyperAIStudioSoundMix.h"
#include "HyperAIStudioSourceControl.h"
#include "MetasoundSource.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundConcurrency.h"
#include "Sound/SoundCue.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundNode.h"
#include "Sound/SoundSubmix.h"
#include "Sound/SoundWave.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioAudioAuthoring"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAudioAuthoring, Log, All);

FString FHyperAIStudioAudioAuthoringContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioAudioAuthoring.HyperAIStudioAudioAuthoringToolset");
}

TArray<FString> FHyperAIStudioAudioAuthoringContracts::GetToolNames()
{
	return {CueToolName, MixToolName, InspectToolName, RecipeToolName, NodeSearchToolName, NodeDescribeToolName, GraphPlanToolName, AnalyzeToolName, PreviewToolName};
}

bool FHyperAIStudioAudioAuthoringContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::AudioAuthoring::Private
{
	using C = FHyperAIStudioAudioAuthoringContracts;

	FString NewOperationId(const FString& Requested, const TCHAR* Prefix)
	{
		const FString Trimmed = Requested.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? FString::Printf(TEXT("%s-%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Trimmed;
	}

	void Record(const TCHAR* Tool, const FString& OperationId, const FString& Target, const bool bOk, const FString& Diagnostic)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = Tool;
		Entry.Target = Target.Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	/** Edits wait while Play In Editor runs: assets changed now would not reach the running game. */
	bool RefuseWhilePlaying(FString& OutStatus, FString& OutDiagnostic)
	{
		if (GEditor && GEditor->PlayWorld)
		{
			OutStatus = TEXT("play_in_editor_running");
			OutDiagnostic = TEXT("Stop Play In Editor first: assets changed now would not reach the running game.");
			return true;
		}
		return false;
	}

	/** An asset to set on the cue: empty leaves it, none clears it; OutAsset is null for both. False when it does not load. */
	template <typename T>
	bool ResolveSetting(const FString& Path, const TCHAR* Field, T*& OutAsset, bool& bOutSet, FString& OutError)
	{
		OutAsset = nullptr;
		const FString Trimmed = Path.TrimStartAndEnd();
		bOutSet = !Trimmed.IsEmpty();
		if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		FString Reason;
		OutAsset = Cast<T>(CueGraph::LoadAsset(Trimmed, *T::StaticClass(), &Reason));
		if (!OutAsset)
		{
			OutError = Reason.IsEmpty() ? FString::Printf(TEXT("%s %s is not a %s in the project."), Field, *Trimmed, *T::StaticClass()->GetName()) : Reason;
			return false;
		}
		return true;
	}

	/** SoundWaves by path; false with the first that is not one. */
	bool LoadWaves(const TArray<FString>& Paths, const TCHAR* Field, TArray<USoundWave*>& OutWaves, FString& OutError)
	{
		OutWaves.Reset();
		for (const FString& Path : Paths)
		{
			FString Reason;
			USoundWave* Wave = Cast<USoundWave>(CueGraph::LoadAsset(Path.TrimStartAndEnd(), *USoundWave::StaticClass(), &Reason));
			if (!Wave || Wave->IsA<UMetaSoundSource>())
			{
				OutError = Reason.IsEmpty() ? FString::Printf(TEXT("%s: %s is not a SoundWave in the project."), Field, *Path) : FString::Printf(TEXT("%s: %s"), Field, *Reason);
				return false;
			}
			OutWaves.Add(Wave);
		}
		return true;
	}

	/** The Sound Cue plan, checked against the project as it is now. */
	struct FCuePlan
	{
		FString Package;
		TWeakObjectPtr<USoundCue> Existing;
		CueGraph::FCheckedGraph Graph;
		bool bGraph = false;
		USoundClass* SoundClass = nullptr;
		bool bSetClass = false;
		USoundAttenuation* Attenuation = nullptr;
		bool bSetAttenuation = false;
		TArray<USoundConcurrency*> Concurrency;
		bool bSetConcurrency = false;
	};

	bool CheckCue(const FHyperAISoundCuePlanRequest& Request, FCuePlan& Plan, FHyperAISoundCuePlanReport& Report)
	{
		auto Refuse = [&Report](const TCHAR* Status, const FString& Why)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Why;
			return false;
		};
		Plan.Package = CueGraph::PackageOf(Request.Cue);
		FString Reason;
		if (!FHyperAIStudioNames::IsPackagePath(Plan.Package, TEXT("Cue"), &Reason))
		{
			return Refuse(TEXT("invalid_request"), Reason.IsEmpty() ? FString(TEXT("Cue is a content path, such as /Game/Audio/SC_Footsteps.")) : Reason);
		}
		Report.Cue = Plan.Package + TEXT(".") + FPackageName::GetShortName(Plan.Package);
		if (!FHyperAIStudioNames::IsFreePackage(Plan.Package))
		{
			UObject* Found = CueGraph::LoadAsset(Plan.Package, *UObject::StaticClass());
			Plan.Existing = Cast<USoundCue>(Found);
			if (!Plan.Existing.IsValid())
			{
				return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("%s is %s, not a Sound Cue."), *Plan.Package, Found ? *Found->GetClass()->GetName() : TEXT("an asset that does not load")));
			}
		}
		Report.bCreates = !Plan.Existing.IsValid();
		Plan.bGraph = !Request.Nodes.IsEmpty();
		if (Report.bCreates && !Plan.bGraph)
		{
			return Refuse(TEXT("invalid_request"), TEXT("A new Sound Cue needs its graph in Nodes."));
		}
		if (!Report.bCreates && Plan.bGraph && !Request.bReplaceGraph)
		{
			return Refuse(TEXT("graph_exists"), FString::Printf(TEXT("%s already has a graph. Set bReplaceGraph to rebuild it from Nodes (hyper_sound_asset_inspect gives its current graph as nodes), or leave Nodes empty to change only its settings."), *Plan.Package));
		}
		FString Error;
		if (Plan.bGraph && !CueGraph::Check(Request.Nodes, Request.Root, Plan.Graph, Error))
		{
			return Refuse(TEXT("invalid_request"), Error);
		}
		if (!ResolveSetting(Request.SoundClass, TEXT("SoundClass"), Plan.SoundClass, Plan.bSetClass, Error)
			|| !ResolveSetting(Request.Attenuation, TEXT("Attenuation"), Plan.Attenuation, Plan.bSetAttenuation, Error))
		{
			return Refuse(TEXT("invalid_request"), Error);
		}
		Plan.bSetConcurrency = !Request.Concurrency.IsEmpty();
		const bool bClearConcurrency = Request.Concurrency.Num() == 1 && Request.Concurrency[0].TrimStartAndEnd().Equals(TEXT("none"), ESearchCase::IgnoreCase);
		if (Plan.bSetConcurrency && !bClearConcurrency)
		{
			if (Request.Concurrency.Num() > 8)
			{
				return Refuse(TEXT("invalid_request"), TEXT("At most 8 Sound Concurrency assets."));
			}
			for (const FString& Path : Request.Concurrency)
			{
				USoundConcurrency* Concurrency = nullptr;
				bool bSet = false;
				if (!ResolveSetting(Path, TEXT("Concurrency"), Concurrency, bSet, Error) || !Concurrency)
				{
					return Refuse(TEXT("invalid_request"), Error.IsEmpty() ? TEXT("Concurrency lists Sound Concurrency assets, or just none.") : Error);
				}
				Plan.Concurrency.AddUnique(Concurrency);
			}
		}
		if (Request.VolumeMultiplier < 0.f || Request.VolumeMultiplier > 4.f || Request.PitchMultiplier < 0.f || Request.PitchMultiplier > 4.f)
		{
			return Refuse(TEXT("invalid_request"), TEXT("VolumeMultiplier and PitchMultiplier are up to 4; 0 leaves them."));
		}
		Report.Nodes = Plan.bGraph ? CueGraph::Describe(Plan.Graph) : TArray<FHyperAISoundCueNodeInfo>();
		Report.Settings.Reset();
		if (Plan.bSetClass) Report.Settings.Add(TEXT("Sound Class: ") + (Plan.SoundClass ? Plan.SoundClass->GetPathName() : FString(TEXT("none"))));
		if (Plan.bSetAttenuation) Report.Settings.Add(TEXT("Attenuation: ") + (Plan.Attenuation ? Plan.Attenuation->GetPathName() : FString(TEXT("none"))));
		if (Plan.bSetConcurrency)
		{
			Report.Settings.Add(TEXT("Concurrency: ") + (Plan.Concurrency.IsEmpty() ? FString(TEXT("none"))
				: FString::JoinBy(Plan.Concurrency, TEXT(", "), [](const USoundConcurrency* Concurrency) { return Concurrency->GetPathName(); })));
		}
		if (Request.VolumeMultiplier > 0.f) Report.Settings.Add(FString::Printf(TEXT("Volume Multiplier: %g"), Request.VolumeMultiplier));
		if (Request.PitchMultiplier > 0.f) Report.Settings.Add(FString::Printf(TEXT("Pitch Multiplier: %g"), Request.PitchMultiplier));
		if (!Plan.bGraph && Report.Settings.IsEmpty())
		{
			return Refuse(TEXT("invalid_request"), TEXT("The plan changes nothing: give Nodes, or a setting (SoundClass, Attenuation, Concurrency, VolumeMultiplier, PitchMultiplier)."));
		}
		return true;
	}

	FHyperAISoundCuePlanReport ExecuteCue(const FHyperAISoundCuePlanRequest& Request, const FString& OperationId)
	{
		FHyperAISoundCuePlanReport Report;
		Report.OperationId = OperationId;
		// Checked again: the cue, its waves or the mix may have changed while the plan waited for approval.
		FCuePlan Plan;
		if (!CheckCue(Request, Plan, Report))
		{
			Report.Diagnostic = TEXT("The plan no longer passes its check; nothing was changed: ") + Report.Diagnostic;
			return Report;
		}
		USoundCue* Cue = nullptr;
		{
			const FScopedTransaction Transaction(LOCTEXT("SoundCuePlan", "HyperAI: Sound Cue Plan"));
			if (Plan.Existing.IsValid())
			{
				Cue = Plan.Existing.Get();
				Cue->Modify();
			}
			else
			{
				UPackage* Outer = CreatePackage(*Plan.Package);
				// A new asset takes its settings without PreEditChange, which would let undo blank them.
				Cue = NewObject<USoundCue>(Outer, FName(*FPackageName::GetShortName(Plan.Package)), RF_Public | RF_Standalone | RF_Transactional);
				FAssetRegistryModule::AssetCreated(Cue);
			}
			FString Error;
			if (Plan.bGraph && !CueGraph::Build(*Cue, Plan.Graph, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = Error + TEXT(" Edit > Undo takes back what was changed.");
				return Report;
			}
			if (Plan.bSetClass) Cue->SoundClassObject = Plan.SoundClass;
			if (Plan.bSetAttenuation) Cue->AttenuationSettings = Plan.Attenuation;
			if (Plan.bSetConcurrency)
			{
				Cue->ConcurrencySet.Reset();
				for (USoundConcurrency* Concurrency : Plan.Concurrency)
				{
					Cue->ConcurrencySet.Add(Concurrency);
				}
			}
			if (Request.VolumeMultiplier > 0.f) Cue->VolumeMultiplier = Request.VolumeMultiplier;
			if (Request.PitchMultiplier > 0.f) Cue->PitchMultiplier = Request.PitchMultiplier;
			Cue->PostEditChange();
			Cue->MarkPackageDirty();
		}
		Report.Nodes = Plan.bGraph ? CueGraph::Describe(Plan.Graph) : TArray<FHyperAISoundCueNodeInfo>();
		Report.Issues = CueGraph::Validate(*Cue);
		Report.DurationSeconds = CueGraph::DurationOf(*Cue);
		Report.Settings.Reset();
		Report.Settings.Add(TEXT("Sound Class: ") + (Cue->SoundClassObject ? Cue->SoundClassObject->GetPathName() : FString(TEXT("none"))));
		Report.Settings.Add(TEXT("Attenuation: ") + (Cue->AttenuationSettings ? Cue->AttenuationSettings->GetPathName() : FString(TEXT("none"))));
		Report.Settings.Add(FString::Printf(TEXT("Concurrency: %d asset(s)"), Cue->ConcurrencySet.Num()));
		Report.Settings.Add(FString::Printf(TEXT("Volume Multiplier: %g, Pitch Multiplier: %g"), Cue->VolumeMultiplier, Cue->PitchMultiplier));
		const int32 Errors = Report.Issues.FilterByPredicate([](const FHyperAISoundIssue& Issue) { return Issue.Severity == TEXT("error"); }).Num();
		if (Errors > 0)
		{
			Report.Status = TEXT("validation_failed");
			Report.Diagnostic = FString::Printf(TEXT("The cue was built but its graph has %d error(s) (see Issues); it was not saved. Edit > Undo takes it back."), Errors);
			return Report;
		}
		if (Request.bSave)
		{
			FString Error;
			if (!FHyperAIStudioSourceControl::SaveAsset(*Cue, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Cue->GetPathName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%s %s in one undo step%s: %d node(s), %s."), *Cue->GetPathName(), Report.bCreates ? TEXT("made") : TEXT("changed"),
			Request.bSave ? TEXT(" and saved") : TEXT(", unsaved"), Cue->AllNodes.Num(),
			Report.DurationSeconds < 0.f ? TEXT("loops forever") : *FString::Printf(TEXT("%.2f s long"), Report.DurationSeconds));
		return Report;
	}

	FHyperAISoundMixPlanReport ExecuteMix(const FHyperAISoundMixPlanRequest& Request, const FString& OperationId)
	{
		FHyperAISoundMixPlanReport Report;
		Report.OperationId = OperationId;
		TArray<Mix::FCheckedAsset> Checked;
		if (!Mix::Check(Request, Checked, Report))
		{
			Report.Diagnostic = TEXT("The plan no longer passes its check; nothing was changed: ") + Report.Diagnostic;
			return Report;
		}
		TArray<UObject*> Touched;
		{
			const FScopedTransaction Transaction(LOCTEXT("SoundMixPlan", "HyperAI: Sound Mix Plan"));
			if (!Mix::Apply(Checked, Touched, Report))
			{
				return Report;
			}
		}
		if (Request.bSave)
		{
			for (UObject* Asset : Touched)
			{
				FString Error;
				if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
				{
					Report.Status = TEXT("save_failed");
					Report.Diagnostic = Error;
					return Report;
				}
				Report.Saved.Add(Asset->GetPathName());
			}
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d asset(s) changed in one undo step, %d made%s."), Touched.Num(), Report.Created.Num(),
			Request.bSave ? TEXT(", all saved") : TEXT("; unsaved"));
		return Report;
	}

	FHyperAIMetaSoundRecipeReport ExecuteRecipe(const FHyperAIMetaSoundRecipeRequest& Request, const FString& Package, const FString& OperationId)
	{
		FHyperAIMetaSoundRecipeReport Report;
		Report.OperationId = OperationId;
		Report.Recipes = MetaSounds::Recipes(Request.Recipe.TrimStartAndEnd());
		if (!FHyperAIStudioNames::IsFreePackage(Package))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = FString::Printf(TEXT("%s appeared while the plan waited; nothing was made over it."), *Package);
			return Report;
		}
		FString Error;
		TArray<USoundWave*> Waves;
		TArray<USoundWave*> Tails;
		if (!LoadWaves(Request.Waves, TEXT("Waves"), Waves, Error) || !LoadWaves(Request.Tails, TEXT("Tails"), Tails, Error))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("A wave went missing while the plan waited; nothing was made: ") + Error;
			return Report;
		}
		{
			const FScopedTransaction Transaction(LOCTEXT("MetaSoundRecipe", "HyperAI: MetaSound Recipe"));
			Report.Path = MetaSounds::BuildRecipe(Request.Recipe.TrimStartAndEnd(), Package, Request.Inputs, Waves, Tails, Report.NodeCount, Error);
		}
		if (Report.Path.IsEmpty())
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Saved.Add(Report.Path);
		Report.Diagnostic = FString::Printf(TEXT("Recipe %s built as %s (%d nodes) and saved. Its inputs are MetaSound parameters: set them from a Blueprint ")
			TEXT("(Set Float Parameter on the Audio Component) or in the Details panel. Measure it with hyper_audio_analyze, then let the user tune it by ear."), *Request.Recipe, *Report.Path, Report.NodeCount);
		return Report;
	}

	FHyperAIMetaSoundGraphPlanReport ExecuteGraph(const FHyperAIMetaSoundGraphPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIMetaSoundGraphPlanReport Report;
		Report.OperationId = OperationId;
		// Checked again: the registry, the MetaSound or a wave it names may have changed while the plan waited.
		Graph::FCheckedPlan Plan;
		if (!Graph::Check(Request, Plan, Report))
		{
			Report.Diagnostic = TEXT("The plan no longer passes its check; nothing was changed: ") + Report.Diagnostic;
			return Report;
		}
		if (Report.bCreates && !FHyperAIStudioNames::IsFreePackage(Plan.Package))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = FString::Printf(TEXT("%s appeared while the plan waited; nothing was made over it."), *Plan.Package);
			return Report;
		}
		bool bBuilt = false;
		{
			const FScopedTransaction Transaction(LOCTEXT("MetaSoundGraphPlan", "HyperAI: MetaSound Graph Plan"));
			bBuilt = Graph::Build(Plan, Request.bSave, Report);
		}
		if (!bBuilt)
		{
			Report.Diagnostic += TEXT(" Edit > Undo takes back what was changed.");
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%s %s in one undo step%s: %d node(s), %d wire(s)%s. Hear it with hyper_audio_analyze."), *Report.Path,
			Report.bCreates ? TEXT("made") : TEXT("extended"), Request.bSave ? TEXT(" and saved") : TEXT(", unsaved"), Report.Nodes.Num(), Report.ConnectionCount,
			Report.Warnings.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; %d warning(s)"), Report.Warnings.Num()));
		return Report;
	}

	/** Every audio asset under Folder, of the kinds inspect reads. */
	TArray<FString> AssetsIn(const FString& Folder, const bool bRecursive, const int32 Max, bool& bOutTruncated)
	{
		FARFilter Filter;
		Filter.PackagePaths.Add(FName(*Folder));
		Filter.bRecursivePaths = bRecursive;
		Filter.bRecursiveClasses = true;
		for (const UClass* Class : {USoundCue::StaticClass(), USoundClass::StaticClass(), USoundMix::StaticClass(), USoundAttenuation::StaticClass(),
			USoundConcurrency::StaticClass(), USoundSubmix::StaticClass(), USoundWave::StaticClass(), UMetaSoundSource::StaticClass()})
		{
			Filter.ClassPaths.Add(Class->GetClassPathName());
		}
		TArray<FAssetData> Found;
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssets(Filter, Found);
		Found.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
		TArray<FString> Out;
		for (const FAssetData& Asset : Found)
		{
			if (Out.Num() >= Max)
			{
				bOutTruncated = true;
				break;
			}
			Out.Add(Asset.GetObjectPathString());
		}
		return Out;
	}

	FHyperAISoundAssetInfo DescribeAsset(UObject& Asset, const bool bAll)
	{
		FHyperAISoundAssetInfo Info;
		Info.Path = Asset.GetPathName();
		if (USoundCue* Cue = Cast<USoundCue>(&Asset))
		{
			Info.Kind = TEXT("sound_cue");
			Info.Settings = Mix::ReadSettings(*Cue, bAll);
			CueGraph::Read(*Cue, Info.CueNodes, Info.CueRoot);
			Info.Issues = CueGraph::Validate(*Cue);
			Info.DurationSeconds = CueGraph::DurationOf(*Cue);
			return Info;
		}
		// Before SoundWave: a MetaSound Source is a procedural SoundWave underneath.
		if (UMetaSoundSource* Source = Cast<UMetaSoundSource>(&Asset))
		{
			Info.Kind = TEXT("metasound_source");
			Info.Settings = Mix::ReadSettings(*Source, bAll);
			Info.Details = MetaSounds::DescribeInputs(*Source);
			Info.Details.Append(Graph::DescribeGraph(*Source));
			Info.DurationSeconds = Source->GetDuration() >= INDEFINITELY_LOOPING_DURATION ? -1.f : Source->GetDuration();
			return Info;
		}
		if (USoundWave* Wave = Cast<USoundWave>(&Asset))
		{
			Info.Kind = TEXT("sound_wave");
			Info.Settings = Mix::ReadSettings(*Wave, bAll);
			FHyperAIAudioSetting& Channels = Info.Settings.AddDefaulted_GetRef();
			Channels.Name = TEXT("NumChannels");
			Channels.Value = FString::FromInt(Wave->NumChannels);
			FHyperAIAudioSetting& Rate = Info.Settings.AddDefaulted_GetRef();
			Rate.Name = TEXT("SampleRate");
			Rate.Value = FString::SanitizeFloat(Wave->GetSampleRateForCurrentPlatform());
			Info.DurationSeconds = Wave->GetDuration() >= INDEFINITELY_LOOPING_DURATION ? -1.f : Wave->GetDuration();
			return Info;
		}
		if (!Mix::KindOf(Asset).IsEmpty())
		{
			Mix::Describe(Asset, bAll, Info);
			return Info;
		}
		Info.Kind = TEXT("unsupported");
		FHyperAISoundIssue& Issue = Info.Issues.AddDefaulted_GetRef();
		Issue.Severity = TEXT("warning");
		Issue.Code = TEXT("unsupported");
		Issue.Message = FString::Printf(TEXT("%s is a %s, which this tool does not read."), *Asset.GetName(), *Asset.GetClass()->GetName());
		return Info;
	}

	/** Plans wait for the user's approval when edits need it; otherwise they run now. */
	template <typename TReport, typename TRun>
	TReport Submit(TReport Report, const TCHAR* Tool, const TCHAR* Variant, const FString& Target, const TArray<FString>& Effects, const TArray<FString>& Touches, TRun Run)
	{
		if (!FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
		{
			TReport Done = Run();
			Record(Tool, Done.OperationId, Target, Done.bOk, Done.Diagnostic);
			return Done;
		}
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = C::PackId;
		Summary.ToolName = Tool;
		Summary.VariantId = Variant;
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Target;
		Summary.Effects = Effects;
		Summary.Touches = Touches;
		const FString OperationId = Report.OperationId;
		FString Error;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Run, Tool, Target]()
			{
				const TReport Done = Run();
				Record(Tool, Done.OperationId, Target, Done.bOk, Done.Diagnostic);
			}, Error))
		{
			Report.bOk = false;
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		FHyperAIStudioApprovalGate::DescribeHeld(OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}
}

FHyperAISoundCuePlanReport FHyperAIStudioAudioAuthoringContracts::CuePlan(const FHyperAISoundCuePlanRequest& Request)
{
	using namespace HyperAIStudio::AudioAuthoring;
	using namespace HyperAIStudio::AudioAuthoring::Private;
	FHyperAISoundCuePlanReport Report;
	Report.OperationId = NewOperationId(Request.OperationId, TEXT("soundcue"));
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread() || RefuseWhilePlaying(Report.Status, Report.Diagnostic))
	{
		Report.Status = Report.Status.IsEmpty() ? TEXT("invalid_request") : Report.Status;
		Report.Diagnostic = Report.Diagnostic.IsEmpty() ? TEXT("hyper_sound_cue_plan runs on the game thread.") : Report.Diagnostic;
		return Report;
	}
	FCuePlan Plan;
	if (!CheckCue(Request, Plan, Report))
	{
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would %s %s%s; nothing was changed. Nodes shows the graph and where each node goes."),
			Report.bCreates ? TEXT("make") : TEXT("change"), *Plan.Package,
			Plan.bGraph ? *FString::Printf(TEXT(" with %d node(s)"), Plan.Graph.Nodes.Num()) : TEXT(""));
		return Report;
	}
	TArray<FString> Effects = {FString::Printf(TEXT("Sound Cue %s: %s, one undo step%s"), *Plan.Package,
		Report.bCreates ? TEXT("made") : Plan.bGraph ? TEXT("graph rebuilt") : TEXT("settings changed"), Request.bSave ? TEXT(", then saved") : TEXT(""))};
	for (int32 Index = 0; Index < Report.Nodes.Num() && Index < 24; ++Index)
	{
		const FHyperAISoundCueNodeInfo& Node = Report.Nodes[Index];
		Effects.Add(FString::Printf(TEXT("| %s %s%s"), *Node.Kind, *Node.Id, Node.Detail.IsEmpty() ? TEXT("") : *(TEXT(": ") + Node.Detail)));
	}
	for (const FString& Setting : Report.Settings)
	{
		Effects.Add(TEXT("| ") + Setting);
	}
	const FString OperationId = Report.OperationId;
	return Submit(Report, CueToolName, TEXT("sound_cue.plan.v1"), Plan.Package, Effects, {Plan.Package},
		[Request, OperationId]() { return ExecuteCue(Request, OperationId); });
}

FHyperAISoundMixPlanReport FHyperAIStudioAudioAuthoringContracts::MixPlan(const FHyperAISoundMixPlanRequest& Request)
{
	using namespace HyperAIStudio::AudioAuthoring;
	using namespace HyperAIStudio::AudioAuthoring::Private;
	FHyperAISoundMixPlanReport Report;
	Report.OperationId = NewOperationId(Request.OperationId, TEXT("soundmix"));
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread() || RefuseWhilePlaying(Report.Status, Report.Diagnostic))
	{
		Report.Status = Report.Status.IsEmpty() ? TEXT("invalid_request") : Report.Status;
		Report.Diagnostic = Report.Diagnostic.IsEmpty() ? TEXT("hyper_sound_mix_plan runs on the game thread.") : Report.Diagnostic;
		return Report;
	}
	TArray<Mix::FCheckedAsset> Checked;
	if (!Mix::Check(Request, Checked, Report))
	{
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d asset(s) checked; nothing was changed. Changes says what each would get."), Checked.Num());
		return Report;
	}
	TArray<FString> Effects = {FString::Printf(TEXT("Mix: %d asset(s), one undo step%s"), Checked.Num(), Request.bSave ? TEXT(", then saved") : TEXT(""))};
	TArray<FString> Touches;
	for (const Mix::FCheckedAsset& Asset : Checked)
	{
		Touches.Add(Asset.Package);
	}
	for (int32 Index = 0; Index < Report.Changes.Num() && Index < 40; ++Index)
	{
		Effects.Add(TEXT("| ") + Report.Changes[Index]);
	}
	const FString OperationId = Report.OperationId;
	return Submit(Report, MixToolName, TEXT("sound_mix.plan.v1"), Touches[0], Effects, Touches,
		[Request, OperationId]() { return ExecuteMix(Request, OperationId); });
}

FHyperAISoundAssetInspectReport FHyperAIStudioAudioAuthoringContracts::Inspect(const FHyperAISoundAssetInspectRequest& Request)
{
	using namespace HyperAIStudio::AudioAuthoring;
	using namespace HyperAIStudio::AudioAuthoring::Private;
	FHyperAISoundAssetInspectReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_sound_asset_inspect runs on the game thread.");
		return Report;
	}
	const int32 Max = FMath::Clamp(Request.MaxAssets <= 0 ? 50 : Request.MaxAssets, 1, 200);
	TArray<FString> Paths = Request.Paths;
	const FString Folder = Request.Folder.TrimStartAndEnd();
	if (!Folder.IsEmpty())
	{
		FString Reason;
		if (!FHyperAIStudioNames::IsFolderPath(Folder, TEXT("Folder"), &Reason))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Reason;
			return Report;
		}
		Paths.Append(AssetsIn(Folder, Request.bRecursive, Max, Report.bTruncated));
	}
	if (Paths.IsEmpty())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Give Paths, or a Folder to read every audio asset in.");
		return Report;
	}
	TArray<FString> Missing;
	for (const FString& Path : Paths)
	{
		if (Report.Assets.Num() >= Max)
		{
			Report.bTruncated = true;
			break;
		}
		FString Reason;
		UObject* Asset = CueGraph::LoadAsset(Path, *UObject::StaticClass(), &Reason);
		if (!Asset)
		{
			Missing.Add(Reason.IsEmpty() ? Path : Reason);
			continue;
		}
		Report.Assets.Add(DescribeAsset(*Asset, Request.bAllSettings));
	}
	Report.bOk = !Report.Assets.IsEmpty() || Missing.IsEmpty();
	Report.Status = Report.bOk ? TEXT("ok") : TEXT("not_found");
	Report.Diagnostic = FString::Printf(TEXT("%d asset(s) read%s%s."), Report.Assets.Num(),
		Missing.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; not found: %s"), *FString::Join(Missing, TEXT(", "))),
		Report.bTruncated ? *FString::Printf(TEXT("; stopped at %d, raise MaxAssets (up to 200) or narrow the folder"), Max) : TEXT(""));
	return Report;
}

FHyperAIMetaSoundRecipeReport FHyperAIStudioAudioAuthoringContracts::Recipe(const FHyperAIMetaSoundRecipeRequest& Request)
{
	using namespace HyperAIStudio::AudioAuthoring;
	using namespace HyperAIStudio::AudioAuthoring::Private;
	FHyperAIMetaSoundRecipeReport Report;
	Report.OperationId = NewOperationId(Request.OperationId, TEXT("metasound"));
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	const FString RecipeId = Request.Recipe.TrimStartAndEnd().ToLower();
	if (RecipeId.IsEmpty())
	{
		Report.bOk = true;
		Report.Status = TEXT("listed");
		Report.Recipes = MetaSounds::Recipes();
		Report.Diagnostic = FString::Printf(TEXT("%d experimental recipes; pick one with Recipe and Path."), Report.Recipes.Num());
		return Report;
	}
	Report.Recipes = MetaSounds::Recipes(RecipeId);
	FString Error;
	TArray<USoundWave*> Waves;
	TArray<USoundWave*> Tails;
	const FString Kind = Request.Kind.TrimStartAndEnd().ToLower();
	if (!MetaSounds::CheckInputs(RecipeId, Request.Inputs, Error) || !LoadWaves(Request.Waves, TEXT("Waves"), Waves, Error)
		|| !LoadWaves(Request.Tails, TEXT("Tails"), Tails, Error) || !MetaSounds::CheckWaves(RecipeId, Kind, Waves, Tails, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Kind == TEXT("sound_cue"))
	{
		// The cue version is a Sound Cue plan: the same check, approval and undo step as hyper_sound_cue_plan.
		FHyperAISoundCuePlanRequest Cue;
		Cue.Cue = Request.Path;
		Cue.bDryRun = Request.bDryRun;
		Cue.OperationId = Report.OperationId;
		if (!MetaSounds::CueRecipe(RecipeId, Waves, Tails, Request.Inputs, Cue.Nodes, Cue.Root, Cue.VolumeMultiplier, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		Cue.VolumeMultiplier = FMath::Clamp(Cue.VolumeMultiplier, 0.01f, 4.f);
		if (!FHyperAIStudioNames::IsFreePackage(CueGraph::PackageOf(Request.Path)))
		{
			Report.Status = TEXT("asset_exists");
			Report.Diagnostic = FString::Printf(TEXT("%s is taken; a recipe makes a new Sound Cue, so give a path of its own."), *Request.Path);
			return Report;
		}
		const FHyperAISoundCuePlanReport Planned = CuePlan(Cue);
		Report.bOk = Planned.bOk;
		Report.Status = Planned.Status;
		Report.Diagnostic = Planned.Diagnostic;
		Report.OperationId = Planned.OperationId;
		Report.Path = Planned.Cue;
		Report.NodeCount = Planned.Nodes.Num();
		Report.Saved = Planned.Saved;
		return Report;
	}
	if (!IsInGameThread() || RefuseWhilePlaying(Report.Status, Report.Diagnostic))
	{
		Report.Status = Report.Status.IsEmpty() ? TEXT("invalid_request") : Report.Status;
		Report.Diagnostic = Report.Diagnostic.IsEmpty() ? TEXT("hyper_metasound_recipe runs on the game thread.") : Report.Diagnostic;
		return Report;
	}
	const FString Package = CueGraph::PackageOf(Request.Path);
	FString Reason;
	if (!FHyperAIStudioNames::IsPackagePath(Package, TEXT("Path"), &Reason))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Reason.IsEmpty() ? FString(TEXT("Path is a new asset's content path, such as /Game/Audio/MS_Wind.")) : Reason;
		return Report;
	}
	if (!FHyperAIStudioNames::IsFreePackage(Package))
	{
		Report.Status = TEXT("asset_exists");
		Report.Diagnostic = FString::Printf(TEXT("%s is taken; a recipe makes a new MetaSound Source, so give a path of its own."), *Package);
		return Report;
	}
	Report.Path = Package + TEXT(".") + FPackageName::GetShortName(Package);
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would build the experimental recipe %s as %s; nothing was changed. Recipes shows its inputs."), *RecipeId, *Package);
		return Report;
	}
	TArray<FString> Effects = {FString::Printf(TEXT("MetaSound Source %s from the experimental recipe %s, then saved"), *Package, *RecipeId)};
	for (const FHyperAIAudioSetting& Input : Request.Inputs)
	{
		Effects.Add(FString::Printf(TEXT("| %s = %s"), *Input.Name, *Input.Value));
	}
	const FString OperationId = Report.OperationId;
	return Submit(Report, RecipeToolName, TEXT("metasound.recipe.v1"), Package, Effects, {Package},
		[Request, Package, OperationId]() { return ExecuteRecipe(Request, Package, OperationId); });
}

FHyperAIMetaSoundNodeSearchReport FHyperAIStudioAudioAuthoringContracts::NodeSearch(const FHyperAIMetaSoundNodeSearchRequest& Request)
{
	FHyperAIMetaSoundNodeSearchReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_metasound_node_search runs on the game thread.");
		return Report;
	}
	return HyperAIStudio::AudioAuthoring::Graph::Search(Request);
}

FHyperAIMetaSoundNodeDescribeReport FHyperAIStudioAudioAuthoringContracts::NodeDescribe(const FHyperAIMetaSoundNodeDescribeRequest& Request)
{
	FHyperAIMetaSoundNodeDescribeReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_metasound_node_describe runs on the game thread.");
		return Report;
	}
	return HyperAIStudio::AudioAuthoring::Graph::DescribeClasses(Request);
}

FHyperAIMetaSoundGraphPlanReport FHyperAIStudioAudioAuthoringContracts::GraphPlan(const FHyperAIMetaSoundGraphPlanRequest& Request)
{
	using namespace HyperAIStudio::AudioAuthoring;
	using namespace HyperAIStudio::AudioAuthoring::Private;
	FHyperAIMetaSoundGraphPlanReport Report;
	Report.OperationId = NewOperationId(Request.OperationId, TEXT("metasoundgraph"));
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread() || RefuseWhilePlaying(Report.Status, Report.Diagnostic))
	{
		Report.Status = Report.Status.IsEmpty() ? TEXT("invalid_request") : Report.Status;
		Report.Diagnostic = Report.Diagnostic.IsEmpty() ? TEXT("hyper_metasound_graph_plan runs on the game thread.") : Report.Diagnostic;
		return Report;
	}
	Graph::FCheckedPlan Plan;
	if (!Graph::Check(Request, Plan, Report))
	{
		return Report;
	}
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would %s %s with %d node(s), %d wire(s), %d input(s) and %d output(s)%s; nothing was changed. Nodes shows where each goes."),
			Report.bCreates ? TEXT("make") : TEXT("extend"), *Plan.Package, Plan.Nodes.Num(), Plan.Connections.Num(), Plan.Inputs.Num(), Plan.Outputs.Num(),
			Report.Warnings.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(". %d warning(s): see Warnings"), Report.Warnings.Num()));
		return Report;
	}
	TArray<FString> Effects = {FString::Printf(TEXT("MetaSound Source %s: %s, one undo step%s"), *Plan.Package,
		Report.bCreates ? TEXT("made") : TEXT("nodes and wires added"), Request.bSave ? TEXT(", then saved") : TEXT(""))};
	for (int32 Index = 0; Index < Report.Nodes.Num() && Index < 24; ++Index)
	{
		Effects.Add(FString::Printf(TEXT("| node %s %s"), *Report.Nodes[Index].Id, *Report.Nodes[Index].Class));
	}
	for (const TPair<FName, FName>& Input : Plan.Inputs)
	{
		Effects.Add(FString::Printf(TEXT("| input %s (%s)"), *Input.Key.ToString(), *Input.Value.ToString()));
	}
	Effects.Add(FString::Printf(TEXT("| %d wire(s)"), Plan.Connections.Num()));
	const FString OperationId = Report.OperationId;
	return Submit(Report, GraphPlanToolName, TEXT("metasound.graph_plan.v1"), Plan.Package, Effects, {Plan.Package},
		[Request, OperationId]() { return ExecuteGraph(Request, OperationId); });
}

FHyperAIAudioAnalyzeReport FHyperAIStudioAudioAuthoringContracts::Analyze(const FHyperAIAudioAnalyzeRequest& Request)
{
	FHyperAIAudioAnalyzeReport Report;
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_audio_analyze runs on the game thread.");
		return Report;
	}
	return HyperAIStudio::AudioAuthoring::Analysis::Analyze(Request);
}

FString FHyperAIStudioAudioWrappers::MakeMetaSoundPlayer(const FString& Package, const TArray<USoundWave*>& Waves, const FString& Mode,
	const FVector2D PitchSemitones, const FVector2D Volume, FString& OutError)
{
	if (!FHyperAIStudioNames::IsPackagePath(Package, TEXT("Package"), &OutError) || !FHyperAIStudioNames::IsFreePackage(Package))
	{
		OutError = OutError.IsEmpty() ? FString::Printf(TEXT("%s is taken."), *Package) : OutError;
		return FString();
	}
	return HyperAIStudio::AudioAuthoring::MetaSounds::BuildPlayer(Package, Waves, Mode, PitchSemitones, Volume, OutError);
}

FString FHyperAIStudioAudioWrappers::MakeRandomCue(const FString& Package, const TArray<USoundWave*>& Waves, const FVector2D PitchSemitones, const FVector2D Volume,
	const bool bLoop, FString& OutError)
{
	using namespace HyperAIStudio::AudioAuthoring;
	if (!FHyperAIStudioNames::IsPackagePath(Package, TEXT("Package"), &OutError) || !FHyperAIStudioNames::IsFreePackage(Package))
	{
		OutError = OutError.IsEmpty() ? FString::Printf(TEXT("%s is taken."), *Package) : OutError;
		return FString();
	}
	if (Waves.IsEmpty() || Waves.Contains(nullptr) || Waves.Num() > USoundNode::MAX_ALLOWED_CHILD_NODES)
	{
		OutError = FString::Printf(TEXT("A random cue plays 1 to %d SoundWaves."), USoundNode::MAX_ALLOWED_CHILD_NODES);
		return FString();
	}
	// Modulator, then (when it loops) Looping, then Random over a Wave Player per wave; a single wave needs no Random.
	TArray<FHyperAISoundCueNode> Nodes;
	FHyperAISoundCueNode& Modulator = Nodes.AddDefaulted_GetRef();
	Modulator.Id = TEXT("variation");
	Modulator.Kind = TEXT("modulator");
	Modulator.PitchMin = FMath::Pow(2.f, static_cast<float>(FMath::Min(PitchSemitones.X, PitchSemitones.Y)) / 12.f);
	Modulator.PitchMax = FMath::Pow(2.f, static_cast<float>(FMath::Max(PitchSemitones.X, PitchSemitones.Y)) / 12.f);
	Modulator.VolumeMin = static_cast<float>(FMath::Min(Volume.X, Volume.Y));
	Modulator.VolumeMax = static_cast<float>(FMath::Max(Volume.X, Volume.Y));
	FString Below = Waves.Num() > 1 ? TEXT("pick") : TEXT("wave_1");
	if (bLoop)
	{
		FHyperAISoundCueNode& Looping = Nodes.AddDefaulted_GetRef();
		Looping.Id = TEXT("loop");
		Looping.Kind = TEXT("looping");
		Looping.Children = {Below};
		Below = Looping.Id;
	}
	Nodes[0].Children = {Below};
	if (Waves.Num() > 1)
	{
		FHyperAISoundCueNode& Random = Nodes.AddDefaulted_GetRef();
		Random.Id = TEXT("pick");
		Random.Kind = TEXT("random");
		Random.bNoRepeat = true;
		for (int32 Index = 0; Index < Waves.Num(); ++Index)
		{
			Random.Children.Add(FString::Printf(TEXT("wave_%d"), Index + 1));
		}
	}
	for (int32 Index = 0; Index < Waves.Num(); ++Index)
	{
		FHyperAISoundCueNode& Player = Nodes.AddDefaulted_GetRef();
		Player.Id = FString::Printf(TEXT("wave_%d"), Index + 1);
		Player.Kind = TEXT("wave_player");
		Player.Wave = Waves[Index]->GetPathName();
	}
	CueGraph::FCheckedGraph Graph;
	if (!CueGraph::Check(Nodes, TEXT("variation"), Graph, OutError))
	{
		return FString();
	}
	UPackage* Outer = CreatePackage(*Package);
	USoundCue* Cue = NewObject<USoundCue>(Outer, FName(*FPackageName::GetShortName(Package)), RF_Public | RF_Standalone | RF_Transactional);
	FAssetRegistryModule::AssetCreated(Cue);
	if (!CueGraph::Build(*Cue, Graph, OutError))
	{
		return FString();
	}
	Cue->PostEditChange();
	Cue->MarkPackageDirty();
	return FHyperAIStudioSourceControl::SaveAsset(*Cue, OutError) ? Cue->GetPathName() : FString();
}

FHyperAISoundCuePlanReport UHyperAIStudioAudioAuthoringToolset::hyper_sound_cue_plan(const FHyperAISoundCuePlanRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::CuePlan(Request);
}

FHyperAISoundMixPlanReport UHyperAIStudioAudioAuthoringToolset::hyper_sound_mix_plan(const FHyperAISoundMixPlanRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::MixPlan(Request);
}

FHyperAISoundAssetInspectReport UHyperAIStudioAudioAuthoringToolset::hyper_sound_asset_inspect(const FHyperAISoundAssetInspectRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::Inspect(Request);
}

FHyperAIMetaSoundRecipeReport UHyperAIStudioAudioAuthoringToolset::hyper_metasound_recipe(const FHyperAIMetaSoundRecipeRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::Recipe(Request);
}

FHyperAIMetaSoundNodeSearchReport UHyperAIStudioAudioAuthoringToolset::hyper_metasound_node_search(const FHyperAIMetaSoundNodeSearchRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::NodeSearch(Request);
}

FHyperAIMetaSoundNodeDescribeReport UHyperAIStudioAudioAuthoringToolset::hyper_metasound_node_describe(const FHyperAIMetaSoundNodeDescribeRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::NodeDescribe(Request);
}

FHyperAIMetaSoundGraphPlanReport UHyperAIStudioAudioAuthoringToolset::hyper_metasound_graph_plan(const FHyperAIMetaSoundGraphPlanRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::GraphPlan(Request);
}

FHyperAIAudioAnalyzeReport UHyperAIStudioAudioAuthoringToolset::hyper_audio_analyze(const FHyperAIAudioAnalyzeRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::Analyze(Request);
}

FHyperAIAudioPreviewReport UHyperAIStudioAudioAuthoringToolset::hyper_audio_preview(const FHyperAIAudioPreviewRequest& Request)
{
	return FHyperAIStudioAudioAuthoringContracts::Preview(Request);
}

void FHyperAIStudioAudioAuthoringRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioAudioAuthoringRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioAudioAuthoringRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioAudioAuthoringToolset::StaticClass(),
			FHyperAIStudioAudioAuthoringContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioAudioAuthoring, Warning, TEXT("Could not unregister the audio authoring toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioAudioAuthoringRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioAudioAuthoringContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioAudioAuthoring, Verbose, TEXT("The audio authoring source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioAudioAuthoringToolset::StaticClass(),
		FHyperAIStudioAudioAuthoringContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioAudioAuthoring, Error, TEXT("Audio authoring cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
