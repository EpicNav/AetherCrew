// Games by Hyper 2026.

#include "HyperAIStudioMotionWarpingToolset.h"

#include "AnimNotifyState_MotionWarping.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Editor/Transactor.h"
#include "EditorUndoClient.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioMotionWarpingGate.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "MotionWarpingComponent.h"
#include "RootMotionModifier.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioMotionWarping, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioMotionWarping"

FString FHyperAIStudioMotionWarpingContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioMotionWarping.HyperAIStudioMotionWarpingToolset");
}

TArray<FString> FHyperAIStudioMotionWarpingContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioMotionWarpingContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::MotionWarpingTools::Private
{
	using C = FHyperAIStudioMotionWarpingContracts;
	namespace Gate = HyperAIStudio::MotionWarping::Gate;

	/**
	 * Character Blueprints a plan changed, so undo and redo keep their generated class in step: the transaction puts the
	 * construction script back, but the class keeps the compiled component until the Blueprint compiles again.
	 */
	class FEditedBlueprints final : public FSelfRegisteringEditorUndoClient
	{
	public:
		void Add(UBlueprint* Blueprint)
		{
			Edited.RemoveAll([](const TWeakObjectPtr<UBlueprint>& Each) { return !Each.IsValid(); });
			Edited.AddUnique(Blueprint);
		}

		virtual void PostUndo(bool bSuccess) override
		{
			if (bSuccess && GEditor && GEditor->Trans) Sync(GEditor->Trans->GetTransaction(GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()));
		}

		virtual void PostRedo(bool bSuccess) override
		{
			if (bSuccess && GEditor && GEditor->Trans) Sync(GEditor->Trans->GetTransaction(GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() - 1));
		}

	private:
		void Sync(const FTransaction* Transaction)
		{
			if (!Transaction) return;
			TArray<UObject*> Objects;
			Transaction->GetTransactionObjects(Objects);
			for (const TWeakObjectPtr<UBlueprint>& Each : Edited)
			{
				UBlueprint* Blueprint = Each.Get();
				if (!Blueprint) continue;
				const UPackage* Package = Blueprint->GetOutermost();
				if (Objects.ContainsByPredicate([Package](const UObject* Object) { return Object && Object->GetOutermost() == Package; }))
				{
					FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
				}
			}
		}

		TArray<TWeakObjectPtr<UBlueprint>> Edited;
	};

	/** Made on first use and let go at module shutdown, while the editor it registered with is still there. */
	TUniquePtr<FEditedBlueprints> GEditedBlueprints;

	FEditedBlueprints& EditedBlueprints()
	{
		if (!GEditedBlueprints) GEditedBlueprints = MakeUnique<FEditedBlueprints>();
		return *GEditedBlueprints;
	}

	FString Seconds(const float Value)
	{
		return FString::Printf(TEXT("%.3f s"), Value);
	}

	/** The notify track by name, made when missing; empty picks the first, making "1" when there is none. */
	int32 EnsureTrack(UAnimSequenceBase& Asset, const FString& Name)
	{
		if (Name.IsEmpty() && !Asset.AnimNotifyTracks.IsEmpty())
		{
			return 0;
		}
		const FName TrackName(Name.IsEmpty() ? TEXT("1") : *Name);
		const int32 Existing = Asset.AnimNotifyTracks.IndexOfByPredicate([TrackName](const FAnimNotifyTrack& Track) { return Track.TrackName == TrackName; });
		if (Existing != INDEX_NONE)
		{
			return Existing;
		}
		FAnimNotifyTrack Track;
		Track.TrackName = TrackName;
		Track.TrackColor = FLinearColor::White;
		return Asset.AnimNotifyTracks.Add(Track);
	}

	/** Places the window's start and end the way the notify panel does: linked to the segment, trigger offsets set. */
	void PlaceWindow(UAnimSequenceBase& Asset, FAnimNotifyEvent& Event, const float Start, const float End)
	{
		Event.Link(&Asset, Start);
		Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Asset.CalculateOffsetForNotify(Start));
		Event.SetDuration(End - Start);
		Event.EndTriggerTimeOffset = GetTriggerTimeOffsetForType(Asset.CalculateOffsetForNotify(End));
		Event.EndLink.Link(&Asset, Event.EndLink.GetTime());
	}

	FString WindowName(const Gate::FStep& Step)
	{
		const UAnimNotifyState_MotionWarping* State = Step.Window;
		const URootMotionModifier_Warp* Warp = State ? Cast<URootMotionModifier_Warp>(State->RootMotionModifier) : nullptr;
		return Warp && !Warp->WarpTargetName.IsNone() ? FString::Printf(TEXT("the window to %s"), *Warp->WarpTargetName.ToString()) : FString(TEXT("the window"));
	}

	/** What a step does, in the words of the report; bDone false for a dry run. */
	FString StepNote(const Gate::FStep& Step, const bool bDone)
	{
		const FString Target = [&Step]()
		{
			for (const Gate::FSetting& Each : Step.Settings)
			{
				if (Each.Property && Each.Property->GetFName() == TEXT("WarpTargetName")) return Each.Import;
			}
			return FString();
		}();
		TArray<FString> Set;
		for (const Gate::FSetting& Each : Step.Settings)
		{
			if (Each.Property && Each.Property->GetFName() != TEXT("WarpTargetName")) Set.Add(Each.Property->GetName() + TEXT("=") + Each.Import);
		}
		const FString Settings = Set.IsEmpty() ? FString() : FString::Printf(TEXT(", %s"), *FString::Join(Set, TEXT(", ")));
		const FString Verb = bDone ? TEXT("") : TEXT("would ");
		if (Step.Kind == TEXT("window.add"))
		{
			return FString::Printf(TEXT("%s: %sadd a %s window%s from %s to %s%s."), *Step.Describe(), *Verb, *Gate::ModifierWord(Step.ModifierClass),
				Target.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" to %s"), *Target), *Seconds(Step.Start), *Seconds(Step.End), *Settings);
		}
		if (Step.Kind == TEXT("window.remove"))
		{
			return FString::Printf(TEXT("%s: %sremove %s."), *Step.Describe(), *Verb, *WindowName(Step));
		}
		if (Step.Kind == TEXT("window.set"))
		{
			TArray<FString> Changes;
			if (Step.bTimes) Changes.Add(FString::Printf(TEXT("%s to %s"), *Seconds(Step.Start), *Seconds(Step.End)));
			if (Step.ModifierClass) Changes.Add(FString::Printf(TEXT("modifier %s"), *Gate::ModifierWord(Step.ModifierClass)));
			if (!Target.IsEmpty()) Changes.Add(TEXT("WarpTargetName=") + Target);
			Changes.Append(Set);
			return FString::Printf(TEXT("%s: %sset %s: %s."), *Step.Describe(), *Verb, *WindowName(Step), *FString::Join(Changes, TEXT(", ")));
		}
		if (Step.Kind == TEXT("component.add"))
		{
			return FString::Printf(TEXT("%s: %sadd the Motion Warping component %s%s, then compile the Blueprint."), *Step.Describe(), *Verb, *Step.ComponentName, *Settings);
		}
		return FString::Printf(TEXT("%s: %sset the Motion Warping component's %s, then compile the Blueprint."), *Step.Describe(), *Verb, *FString::Join(Set, TEXT(", ")));
	}

	/** The windows or component as they are now, with warnings, into the report. */
	void Describe(const Gate::FPlan& Plan, FHyperAIMotionWarpingPlanReport& Report)
	{
		Report.Asset = Plan.AssetPath;
		Report.AssetKind = Gate::AssetKindName(Plan.Kind);
		for (const Gate::FStep& Step : Plan.Steps)
		{
			const Gate::FModifierInfo* Modifier = Step.ModifierClass ? Gate::FindModifier(Gate::ModifierWord(Step.ModifierClass)) : nullptr;
			if (Modifier && Modifier->bExperimental)
			{
				Report.Warnings.AddUnique(FString::Printf(TEXT("%s is experimental in Unreal 5.8; skew_warp is the tried one."), Modifier->Word));
			}
		}
		if (const UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Plan.Asset))
		{
			Gate::FAnimView View = Gate::ViewAnim(*Anim);
			Report.LengthSeconds = View.Length;
			Report.Windows = MoveTemp(View.Windows);
			Report.WarpTargets = MoveTemp(View.WarpTargets);
			Report.Warnings.Append(View.Warnings);
		}
		else if (const UBlueprint* Blueprint = Cast<UBlueprint>(Plan.Asset))
		{
			Gate::FComponentFound Found;
			Report.bHasComponent = Gate::FindComponent(*Blueprint, Found);
			if (Report.bHasComponent)
			{
				Report.ComponentName = Found.Name;
				Report.ComponentSettings = Gate::SettingsOf(*Found.Template, /*bOwnOnly=*/true);
			}
			else
			{
				Report.Warnings.Add(FString::Printf(TEXT("%s has no Motion Warping component, so no montage it plays warps; add one with component.add."), *Blueprint->GetName()));
			}
		}
	}

	FHyperAIMotionWarpingPlanReport Fail(FHyperAIMotionWarpingPlanReport& Report, const FString& Error)
	{
		Report.bOk = false;
		Report.Status = TEXT("operation_failed");
		Report.Diagnostic = Error + TEXT(" Edit > Undo takes back what the plan changed.");
		return Report;
	}

	/** The plan for real, in one undo step, then compiled (a Blueprint) and saved if asked. */
	FHyperAIMotionWarpingPlanReport Execute(const FHyperAIMotionWarpingPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIMotionWarpingPlanReport Report;
		Report.OperationId = OperationId;
		Gate::FPlan Plan;
		FString Error;
		if (!Gate::Check(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.Asset = Plan.AssetPath;
		UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Plan.Asset);
		UBlueprint* Blueprint = Cast<UBlueprint>(Plan.Asset);
		{
			const FScopedTransaction Transaction(LOCTEXT("MotionWarpingPlan", "HyperAI: Motion Warping Plan"));
			if (Anim)
			{
				Anim->Modify();
				for (const Gate::FStep& Step : Plan.Steps)
				{
					if (Step.Kind == TEXT("window.add"))
					{
						const int32 Track = EnsureTrack(*Anim, Step.Op.Track.TrimStartAndEnd());
						// Made without undo tracking until its settings are in, so undo and redo never meet it half made; the
						// notify list the asset's Modify() recorded is what undo puts back.
						UAnimNotifyState_MotionWarping* State = NewObject<UAnimNotifyState_MotionWarping>(Anim, UAnimNotifyState_MotionWarping::StaticClass(), NAME_None, RF_NoFlags);
						URootMotionModifier* Modifier = State->RootMotionModifier;
						if (!Modifier || Modifier->GetClass() != Step.ModifierClass)
						{
							Modifier = NewObject<URootMotionModifier>(State, Step.ModifierClass, NAME_None, RF_NoFlags);
							State->RootMotionModifier = Modifier;
						}
						if (!Gate::WriteSettings(*Modifier, Step.Settings, Error)) return Fail(Report, Step.Describe() + TEXT(": ") + Error);
						Modifier->SetFlags(RF_Transactional);
						State->SetFlags(RF_Transactional);
						FAnimNotifyEvent& Event = Anim->Notifies.AddDefaulted_GetRef();
#if WITH_EDITORONLY_DATA
						Event.Guid = FGuid::NewGuid();
#endif
						Event.TrackIndex = Track;
						Event.NotifyStateClass = State;
						Event.NotifyName = FName(*State->GetNotifyName());
						PlaceWindow(*Anim, Event, Step.Start, Step.End);
					}
					else
					{
						const int32 EventIndex = Gate::FindEvent(*Anim, Step.Window);
						if (EventIndex == INDEX_NONE) return Fail(Report, Step.Describe() + TEXT(": the window is no longer on the asset."));
						if (Step.Kind == TEXT("window.remove"))
						{
							Anim->Notifies.RemoveAt(EventIndex);
						}
						else
						{
							UAnimNotifyState_MotionWarping* State = Step.Window;
							State->SetFlags(RF_Transactional);
							State->Modify();
							if (Step.ModifierClass)
							{
								URootMotionModifier* Old = State->RootMotionModifier;
								URootMotionModifier* New = NewObject<URootMotionModifier>(State, Step.ModifierClass, NAME_None, RF_NoFlags);
								// What the old modifier and the new one share (the warp target, translation and rotation switches) carries over.
								if (Old)
								{
									for (FProperty* Property : Gate::Settable(*Step.ModifierClass, /*bOwnOnly=*/false))
									{
										const FProperty* Same = FindFProperty<FProperty>(Old->GetClass(), Property->GetFName());
										if (Same && Same->SameType(Property)) Property->CopyCompleteValue_InContainer(New, Old);
									}
								}
								if (!Gate::WriteSettings(*New, Step.Settings, Error)) return Fail(Report, Step.Describe() + TEXT(": ") + Error);
								New->SetFlags(RF_Transactional);
								State->RootMotionModifier = New;
							}
							else if (!Step.Settings.IsEmpty())
							{
								URootMotionModifier* Modifier = State->RootMotionModifier;
								Modifier->SetFlags(RF_Transactional);
								Modifier->Modify();
								if (!Gate::WriteSettings(*Modifier, Step.Settings, Error)) return Fail(Report, Step.Describe() + TEXT(": ") + Error);
							}
							if (Step.bTimes) PlaceWindow(*Anim, Anim->Notifies[EventIndex], Step.Start, Step.End);
						}
					}
					Anim->RefreshCacheData();
					Report.Notes.Add(StepNote(Step, /*bDone=*/true));
				}
				Anim->PostEditChange();
				Anim->MarkPackageDirty();
			}
			else if (Blueprint)
			{
				USimpleConstructionScript* Script = Blueprint->SimpleConstructionScript;
				Blueprint->Modify();
				Script->SaveToTransactionBuffer();
				UActorComponent* Added = nullptr;
				for (const Gate::FStep& Step : Plan.Steps)
				{
					if (Step.Kind == TEXT("component.add"))
					{
						// As the Components panel adds one: the node and its template recorded in this undo step.
						USCS_Node* Node = Script->CreateNode(UMotionWarpingComponent::StaticClass(), FName(*Step.ComponentName));
						Node->Modify();
						if (!Gate::WriteSettings(*Node->ComponentTemplate, Step.Settings, Error)) return Fail(Report, Step.Describe() + TEXT(": ") + Error);
						Script->AddNode(Node);
						FBlueprintEditorUtils::ValidateBlueprintChildVariables(Blueprint, Node->GetVariableName());
						Added = Node->ComponentTemplate;
						Gate::FStep Named = Step;
						Named.ComponentName = Node->GetVariableName().ToString();
						Report.Notes.Add(StepNote(Named, /*bDone=*/true));
						continue;
					}
					Gate::FComponentFound Found;
					UActorComponent* Template = Added;
					if (!Template && Gate::FindComponent(*Blueprint, Found) && Found.OwnNode) Template = Found.OwnNode->ComponentTemplate;
					if (!Template) return Fail(Report, Step.Describe() + TEXT(": the Motion Warping component is no longer on the Blueprint."));
					Template->SetFlags(RF_Transactional);
					Template->Modify();
					if (!Gate::WriteSettings(*Template, Step.Settings, Error)) return Fail(Report, Step.Describe() + TEXT(": ") + Error);
					Report.Notes.Add(StepNote(Step, /*bDone=*/true));
				}
				Blueprint->MarkPackageDirty();
			}
		}
		if (Blueprint)
		{
			// The generated class takes the component when the Blueprint compiles; the compile is not an undo step, and undo
			// or redo of this plan compiles it again.
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			EditedBlueprints().Add(Blueprint);
		}
		Describe(Plan, Report);
		for (const FString& Name : Plan.TargetsBefore)
		{
			if (!Report.WarpTargets.Contains(Name))
			{
				Report.Warnings.Add(FString::Printf(TEXT("%s is used by no window now; game code that sets the warp target %s warps nothing."), *Name, *Name));
			}
		}
		if (Request.bSave)
		{
			if (!FHyperAIStudioSourceControl::SaveAsset(*Plan.Asset, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Plan.Asset->GetOutermost()->GetName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = Anim
			? FString::Printf(TEXT("%s: %d operation(s) applied, %d warp window(s) now (%s); %s."), *Report.Asset, Plan.Steps.Num(), Report.Windows.Num(),
				Report.WarpTargets.IsEmpty() ? TEXT("no warp targets") : *FString::Join(Report.WarpTargets, TEXT(", ")),
				Request.bSave ? TEXT("saved") : TEXT("unsaved, and one Edit > Undo takes the whole plan back"))
			: FString::Printf(TEXT("%s: %d operation(s) applied, Motion Warping component %s, compiled; %s."), *Report.Asset, Plan.Steps.Num(),
				Report.bHasComponent ? *Report.ComponentName : TEXT("missing"), Request.bSave ? TEXT("saved") : TEXT("unsaved, and one Edit > Undo takes the whole plan back"));
		return Report;
	}

	FHyperAIMotionWarpingPlanReport DryRun(const FHyperAIMotionWarpingPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIMotionWarpingPlanReport Report;
		Report.OperationId = OperationId;
		Gate::FPlan Plan;
		FString Error;
		if (!Gate::Check(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		for (const Gate::FStep& Step : Plan.Steps) Report.Notes.Add(StepNote(Step, /*bDone=*/false));
		Describe(Plan, Report);
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked on %s; nothing was changed. Windows and Warnings show the asset as it is now."), Plan.Steps.Num(), *Plan.AssetPath);
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIMotionWarpingPlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Asset;
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAIMotionWarpingInspectReport FHyperAIStudioMotionWarpingContracts::Inspect(const FHyperAIMotionWarpingInspectRequest& Request)
{
	using namespace HyperAIStudio::MotionWarpingTools::Private;
	FHyperAIMotionWarpingInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	const FString Path = Request.Asset.TrimStartAndEnd();
	if (Path.IsEmpty())
	{
		for (const Gate::FKindInfo& Info : Gate::Kinds())
		{
			FHyperAIMotionWarpingKind& Kind = Report.Kinds.AddDefaulted_GetRef();
			Kind.Kind = Info.Kind;
			Kind.Summary = Info.Summary;
			if (Kind.Kind.StartsWith(TEXT("component."))) Kind.Settings = Gate::DescribeSettings(*UMotionWarpingComponent::StaticClass(), /*bOwnOnly=*/true);
		}
		for (const Gate::FModifierInfo& Info : Gate::Modifiers())
		{
			FHyperAIMotionWarpingKind& Modifier = Report.Modifiers.AddDefaulted_GetRef();
			Modifier.Kind = Info.Word;
			Modifier.Summary = Info.Summary;
			if (Info.Class) Modifier.Settings = Gate::DescribeSettings(*Info.Class, /*bOwnOnly=*/false);
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UAnimMontage::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses=*/true);
		// The notify names an animation lists in the asset registry, so montages are found without loading them.
		const FString Needle = USkeleton::AnimNotifyTagDelimiter + UAnimNotifyState_MotionWarping::StaticClass()->GetName() + USkeleton::AnimNotifyTagDelimiter;
		for (const FAssetData& Each : Assets)
		{
			const FString AssetPath = Each.GetObjectPathString();
			FString Notifies;
			if (!AssetPath.StartsWith(TEXT("/Game/")) || !Each.GetTagValue(USkeleton::AnimNotifyTag, Notifies) || !Notifies.Contains(Needle)) continue;
			if (Report.Assets.Num() < MaxListedAssets) Report.Assets.Add(AssetPath);
		}
		Report.Assets.Sort();
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%d op kind(s) hyper_motion_warping_apply_plan takes, %d window modifier(s), and %d montage(s) under /Game with warp windows; give Asset to read one."),
			Report.Kinds.Num(), Report.Modifiers.Num(), Report.Assets.Num());
		return Report;
	}
	FString Error;
	UObject* Asset = Gate::LoadAsset(Path, Error);
	if (!Asset)
	{
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Asset = Asset->GetPathName();
	if (const UAnimSequenceBase* Anim = Cast<UAnimSequenceBase>(Asset); Anim && (Anim->IsA<UAnimMontage>() || Anim->IsA<UAnimSequence>()))
	{
		Gate::FAnimView View = Gate::ViewAnim(*Anim);
		Report.AssetKind = Gate::AssetKindName(Anim->IsA<UAnimMontage>() ? Gate::EAssetKind::Montage : Gate::EAssetKind::Sequence);
		Report.LengthSeconds = View.Length;
		Report.bRootMotion = View.bRootMotion;
		Report.Windows = MoveTemp(View.Windows);
		Report.WarpTargets = MoveTemp(View.WarpTargets);
		Report.Warnings = MoveTemp(View.Warnings);
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%s: %.3f s, %d warp window(s)%s, root motion %s."), *Report.Asset, Report.LengthSeconds, Report.Windows.Num(),
			Report.WarpTargets.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" to %s"), *FString::Join(Report.WarpTargets, TEXT(", "))), Report.bRootMotion ? TEXT("on") : TEXT("off"));
		return Report;
	}
	const UBlueprint* Blueprint = Cast<UBlueprint>(Asset);
	if (!Blueprint || !Gate::IsCharacterBlueprint(*Blueprint))
	{
		Report.Diagnostic = FString::Printf(TEXT("%s is a %s, not a montage, animation sequence or character Blueprint."), *Report.Asset,
			Blueprint ? TEXT("Blueprint whose parent is not a Character") : *Asset->GetClass()->GetName());
		return Report;
	}
	Report.AssetKind = Gate::AssetKindName(Gate::EAssetKind::Character);
	Gate::FComponentFound Found;
	Report.bHasComponent = Gate::FindComponent(*Blueprint, Found);
	if (Report.bHasComponent)
	{
		Report.ComponentName = Found.Name;
		Report.ComponentSource = Found.Source;
		Report.ComponentSettings = Gate::SettingsOf(*Found.Template, /*bOwnOnly=*/true);
	}
	else
	{
		Report.Warnings.Add(FString::Printf(TEXT("%s has no Motion Warping component, so no montage it plays warps; add one with component.add."), *Blueprint->GetName()));
	}
	Report.bOk = true;
	Report.Diagnostic = Report.bHasComponent
		? FString::Printf(TEXT("%s: Motion Warping component %s, from %s."), *Report.Asset, *Report.ComponentName, *Report.ComponentSource)
		: FString::Printf(TEXT("%s: no Motion Warping component."), *Report.Asset);
	return Report;
}

FHyperAIMotionWarpingPlanReport FHyperAIStudioMotionWarpingContracts::ApplyPlan(const FHyperAIMotionWarpingPlanRequest& Request)
{
	using namespace HyperAIStudio::MotionWarpingTools::Private;
	FHyperAIMotionWarpingPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("motionwarping-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Params")})) return Report;
	if (GEditor && GEditor->PlayWorld)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play In Editor is running; stop it first, then change motion warping.");
		return Report;
	}
	Gate::FPlan Plan;
	FString Error;
	if (!Gate::Check(Request, Plan, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bDryRun) return DryRun(Request, Report.OperationId);
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("motionwarping.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Plan.AssetPath;
		Summary.Effects.Add(FString::Printf(TEXT("Change the motion warping of %s: %d operation(s), one undo step%s%s"), *Plan.AssetPath, Request.Operations.Num(),
			Plan.Kind == Gate::EAssetKind::Character ? TEXT(", then compile the Blueprint") : TEXT(""), Request.bSave ? TEXT(", then saved") : TEXT("")));
		for (int32 Index = 0; Index < Plan.Steps.Num() && Index < 24; ++Index)
		{
			Summary.Effects.Add(TEXT("| ") + StepNote(Plan.Steps[Index], /*bDone=*/false).Left(200));
		}
		Summary.Touches.Add(Plan.AssetPath);
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited (a window removed, a component added), so Execute
				// checks the plan again before anything changes.
				Record(OperationId, Execute(Request, OperationId));
			}, Error))
		{
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Asset = Plan.AssetPath;
		Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
		FHyperAIStudioApprovalGate::DescribeHeld(OperationId, Report.Status, Report.Diagnostic);
		return Report;
	}
	Report = Execute(Request, Report.OperationId);
	Record(Report.OperationId, Report);
	return Report;
}

FHyperAIMotionWarpingInspectReport UHyperAIStudioMotionWarpingToolset::hyper_motion_warping_inspect(const FHyperAIMotionWarpingInspectRequest& Request)
{
	return FHyperAIStudioMotionWarpingContracts::Inspect(Request);
}

FHyperAIMotionWarpingPlanReport UHyperAIStudioMotionWarpingToolset::hyper_motion_warping_apply_plan(const FHyperAIMotionWarpingPlanRequest& Request)
{
	return FHyperAIStudioMotionWarpingContracts::ApplyPlan(Request);
}

void FHyperAIStudioMotionWarpingRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioMotionWarpingRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioMotionWarpingRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioMotionWarpingToolset::StaticClass(),
			FHyperAIStudioMotionWarpingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioMotionWarping, Warning, TEXT("Could not unregister the motion warping toolset: %s"), *Error);
		}
	}
	HyperAIStudio::MotionWarpingTools::Private::GEditedBlueprints.Reset();
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioMotionWarpingRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioMotionWarpingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioMotionWarping, Verbose, TEXT("The motion warping source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioMotionWarpingToolset::StaticClass(),
		FHyperAIStudioMotionWarpingContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioMotionWarping, Error, TEXT("Motion warping cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
