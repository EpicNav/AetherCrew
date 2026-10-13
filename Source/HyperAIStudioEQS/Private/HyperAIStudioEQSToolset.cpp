// Games by Hyper 2026.

#include "HyperAIStudioEQSToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorUndoClient.h"
#include "Engine/Engine.h"
#include "Engine/Selection.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryManager.h"
#include "EnvironmentQuery/Items/EnvQueryItemType_ActorBase.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioEQSGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioEQS, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioEQS"

FString FHyperAIStudioEQSContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioEQS.HyperAIStudioEQSToolset");
}

TArray<FString> FHyperAIStudioEQSContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName, RunToolName};
}

bool FHyperAIStudioEQSContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::EQSTools::Private
{
	using C = FHyperAIStudioEQSContracts;
	namespace Gate = HyperAIStudio::EQS::Gate;

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	/** The query asset's caches in every world dropped, so the next run reads the query as it is now. */
	void RefreshRuns(UEnvQuery* Query)
	{
#if USE_EQS_DEBUGGER
		UEnvQueryManager::NotifyAssetUpdate(Query);
#endif
	}

	/**
	 * Queries a plan made, so undo and redo keep the Content Browser in step (the transaction marks a made asset garbage
	 * on undo and alive on redo), and the query manager's cached copies are dropped after either.
	 */
	class FMadeAssets final : public FSelfRegisteringEditorUndoClient
	{
	public:
		void Add(UObject* Asset)
		{
			Made.RemoveAll([](const TWeakObjectPtr<UObject>& Each) { return Each.IsStale(/*bIncludingGarbage=*/false); });
			Made.Add(Asset);
			Listed.Add(Asset, true);
		}

		virtual void PostUndo(bool bSuccess) override { Sync(); }
		virtual void PostRedo(bool bSuccess) override { Sync(); }

	private:
		void Sync()
		{
			for (int32 Index = Made.Num() - 1; Index >= 0; --Index)
			{
				UObject* Asset = Made[Index].Get(/*bEvenIfGarbage=*/true);
				if (!Asset)
				{
					Made.RemoveAt(Index);
					continue;
				}
				const bool bAlive = IsValid(Asset);
				bool* Known = Listed.Find(Asset);
				if (Known && *Known == bAlive) continue;
				// Undone, nothing of it is left to save; redone, it is unsaved work again.
				if (bAlive)
				{
					FAssetRegistryModule::AssetCreated(Asset);
					Asset->MarkPackageDirty();
				}
				else
				{
					FAssetRegistryModule::AssetDeleted(Asset);
					Asset->GetPackage()->SetDirtyFlag(false);
				}
				Listed.Add(Asset, bAlive);
			}
			RefreshRuns(nullptr);
		}

		TArray<TWeakObjectPtr<UObject>> Made;
		TMap<TWeakObjectPtr<UObject>, bool> Listed;
	};

	/** Made on first use and let go at module shutdown, while the editor it registered with is still there. */
	TUniquePtr<FMadeAssets> GMadeAssets;

	FMadeAssets& MadeAssets()
	{
		if (!GMadeAssets) GMadeAssets = MakeUnique<FMadeAssets>();
		return *GMadeAssets;
	}

	FString PackageOf(const FString& Reference)
	{
		FString Path = Reference.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
		return Path;
	}

	UObject* FindAt(const FString& PackagePath)
	{
		UPackage* Package = FindPackage(nullptr, *PackagePath);
		return Package ? StaticFindObjectFast(UObject::StaticClass(), Package, FName(*FPackageName::GetShortName(PackagePath)), EFindObjectFlags::None, RF_NoFlags,
			EInternalObjectFlags::None) : nullptr;
	}

	/** The query an undone plan made at PackagePath, kept as garbage for redo; null when there is none. */
	UEnvQuery* UndoneAt(const FString& PackagePath)
	{
		if (FPackageName::DoesPackageExist(PackagePath)) return nullptr;
		UEnvQuery* Query = Cast<UEnvQuery>(FindAt(PackagePath));
		return Query && !IsValid(Query) ? Query : nullptr;
	}

	UEnvQuery* LoadQuery(const FString& Reference, FString& OutError)
	{
		const FString Path = PackageOf(Reference);
		if (!FHyperAIStudioNames::IsObjectPath(Path, TEXT("Asset"), &OutError)) return nullptr;
		const FString ObjectPath = Path + TEXT(".") + FPackageName::GetShortName(Path);
		UEnvQuery* Query = FindObject<UEnvQuery>(nullptr, *ObjectPath);
		if (!Query && FPackageName::DoesPackageExist(Path))
		{
			Query = LoadObject<UEnvQuery>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Query || !IsValid(Query))
		{
			OutError = FString::Printf(TEXT("%s is not an Environment Query."), *Path);
			return nullptr;
		}
		return Query;
	}

	bool IsQueryOp(const FString& Kind)
	{
		return Kind.StartsWith(TEXT("option.")) || Kind.StartsWith(TEXT("test."));
	}

	/** The plan, checked: its target, and the ops run on a throwaway copy of the query. */
	struct FPlan
	{
		FString AssetPath;
		bool bCreate = false;
		TObjectPtr<UEnvQuery> Existing = nullptr;
		bool bChangesQuery = false;
		TArray<FString> Notes;
		TArray<FHyperAIEQSOptionView> Options;
		TArray<FString> QueryParams;
		TArray<FString> Warnings;
		int32 TreeOps = 0;
	};

	bool Check(const FHyperAIEQSPlanRequest& Request, FPlan& OutPlan, FString& OutError)
	{
		OutPlan = FPlan();
		const FString Path = PackageOf(Request.Asset);
		if (Path.IsEmpty())
		{
			OutError = TEXT("Give Asset, the Environment Query to make or change, as /Game/Folder/EQS_Name.");
			return false;
		}
		if (!Path.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("%s is not project content: a plan makes and changes Environment Queries under /Game only, never Engine or plugin content."), *Path);
			return false;
		}
		if (!FHyperAIStudioNames::IsPackagePath(Path, TEXT("Asset"), &OutError)) return false;
		OutPlan.AssetPath = Path;
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		static const TArray<FString> KnownKinds = {TEXT("create"), TEXT("option.add"), TEXT("option.set"), TEXT("option.remove"), TEXT("test.add"), TEXT("test.set"),
			TEXT("test.remove"), TEXT("bt.add_composite"), TEXT("bt.run_eqs_task"), TEXT("bt.run_eqs_service")};
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FString Kind = Request.Operations[Index].Kind.TrimStartAndEnd().ToLower();
			if (!KnownKinds.Contains(Kind))
			{
				OutError = FString::Printf(TEXT("Operation %d: %s is not a kind; the kinds are %s."), Index, Request.Operations[Index].Kind.IsEmpty() ? TEXT("(empty Kind)") : *Request.Operations[Index].Kind,
					*FString::Join(KnownKinds, TEXT(", ")));
				return false;
			}
			if (Kind == TEXT("create") && Index > 0)
			{
				OutError = FString::Printf(TEXT("Operation %d (create): create comes first, once: the ops after it work on what it made. Nothing was changed."), Index);
				return false;
			}
			OutPlan.bChangesQuery |= IsQueryOp(Kind);
			OutPlan.TreeOps += Kind.StartsWith(TEXT("bt.")) ? 1 : 0;
		}
		OutPlan.bCreate = Request.Operations[0].Kind.TrimStartAndEnd().Equals(TEXT("create"), ESearchCase::IgnoreCase);
		UEnvQuery* Working = nullptr;
		if (OutPlan.bCreate)
		{
			if (!FHyperAIStudioNames::IsFreePackage(Path) && !UndoneAt(Path))
			{
				OutError = FString::Printf(TEXT("%s exists already; leave out create to change it, or name a new Asset."), *Path);
				return false;
			}
			Working = NewObject<UEnvQuery>(GetTransientPackage());
		}
		else
		{
			FString Why;
			OutPlan.Existing = LoadQuery(Path, Why);
			if (!OutPlan.Existing)
			{
				OutError = FString::Printf(TEXT("%s; start the plan with create to make it."), *Why.LeftChop(Why.EndsWith(TEXT(".")) ? 1 : 0));
				return false;
			}
			Working = DuplicateObject<UEnvQuery>(OutPlan.Existing, GetTransientPackage());
		}
		const bool bQueryOk = Gate::ApplyQueryOps(*Working, Request.Operations, OutPlan.Notes, OutError);
		if (bQueryOk)
		{
			OutPlan.Options = Gate::Describe(*Working);
			OutPlan.QueryParams = Gate::QueryParams(*Working);
			OutPlan.Warnings = Gate::Warn(*Working);
		}
		Working->MarkAsGarbage();
		if (!bQueryOk) return false;
		return Gate::CheckTreeOps(Request.Operations, OutError);
	}

	void Fill(FHyperAIEQSPlanReport& Report, const FPlan& Plan)
	{
		Report.Asset = Plan.AssetPath;
		Report.Options = Plan.Options;
		Report.QueryParams = Plan.QueryParams;
		Report.Warnings = Plan.Warnings;
		Report.Notes = Plan.Notes;
	}

	/** The plan for real: the query and the trees in one undo step, then saved if asked. */
	FHyperAIEQSPlanReport Execute(const FHyperAIEQSPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIEQSPlanReport Report;
		Report.OperationId = OperationId;
		FPlan Plan;
		FString Error;
		if (!Check(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		Fill(Report, Plan);
		Report.Notes.Reset();
		if (Plan.Existing && Plan.bChangesQuery && GEditor)
		{
			// The EQS editor holds the query's graph; it closes first, and builds the graph again from the query when reopened.
			GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->CloseAllEditorsForAsset(Plan.Existing);
		}
		UEnvQuery* Query = nullptr;
		TArray<UBehaviorTree*> Trees;
		{
			const FScopedTransaction Transaction(LOCTEXT("EQSPlan", "HyperAI: EQS Plan"));
			if (Plan.bCreate)
			{
				// What an undone plan made here is out of the way first: this plan's step ends the redo that kept it.
				if (UEnvQuery* Undone = UndoneAt(Plan.AssetPath))
				{
					Undone->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
				}
				UPackage* Package = CreatePackage(*Plan.AssetPath);
				Query = NewObject<UEnvQuery>(Package, *FPackageName::GetShortName(Plan.AssetPath), RF_Public | RF_Standalone | RF_Transactional);
				// Recorded as made in this undo step: garbage when the step began, alive when it ended. Undo marks it garbage
				// again and redo brings it back.
				Query->MarkAsGarbage();
				Query->Modify();
				Query->ClearGarbage();
				FAssetRegistryModule::AssetCreated(Query);
				MadeAssets().Add(Query);
				Report.Notes.Add(FString::Printf(TEXT("Made %s."), *Query->GetPathName()));
			}
			else
			{
				Query = Plan.Existing;
				Query->Modify();
			}
#if WITH_EDITORONLY_DATA
			if (Plan.bChangesQuery && Query->EdGraph)
			{
				// The graph is the editor's view of the options; a stale one would write the old options back on its next
				// change. Without one the EQS editor builds it from the options when it opens.
				Query->EdGraph = nullptr;
			}
#endif
			if (!Gate::ApplyQueryOps(*Query, Request.Operations, Report.Notes, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = Error + TEXT(" The query is partly changed and unsaved; Edit > Undo takes the plan back.");
				return Report;
			}
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				const FHyperAIEQSOp& Op = Request.Operations[Index];
				if (!Op.Kind.TrimStartAndEnd().StartsWith(TEXT("bt."), ESearchCase::IgnoreCase)) continue;
				UBehaviorTree* Tree = Gate::LoadTree(Op.BehaviorTree, Error);
				FString Note;
				if (!Tree || !Gate::ApplyTreeOp(*Tree, Op, *Query, Note, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s Nothing is saved; Edit > Undo takes the plan back."), Index, *Op.Kind, *Error);
					return Report;
				}
				Trees.AddUnique(Tree);
				Report.TreeNodes.Add(Note);
				Report.Notes.Add(TEXT("Added ") + Note + TEXT("."));
			}
			Query->MarkPackageDirty();
		}
		RefreshRuns(Query);
		Report.Asset = Query->GetPathName();
		Report.Options = Gate::Describe(*Query);
		Report.QueryParams = Gate::QueryParams(*Query);
		Report.Warnings = Gate::Warn(*Query);
		if (Request.bSave)
		{
			if (!FHyperAIStudioSourceControl::SaveAsset(*Query, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Query->GetPathName());
			for (UBehaviorTree* Tree : Trees)
			{
				if (!FHyperAIStudioSourceControl::SaveAsset(*Tree, Error))
				{
					Report.Status = TEXT("save_failed");
					Report.Diagnostic = FString::Printf(TEXT("The query is saved but %s is not: %s"), *Tree->GetPathName(), *Error);
					return Report;
				}
				Report.Saved.Add(Tree->GetPathName());
			}
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%s: %d option(s)%s%s; %s. Run it with hyper_eqs_run from a querier."), *Report.Asset, Report.Options.Num(),
			Report.TreeNodes.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", %d Behavior Tree node(s) added"), Report.TreeNodes.Num()),
			Report.QueryParams.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", reading %s"), *FString::Join(Report.QueryParams, TEXT(", "))),
			Request.bSave ? TEXT("saved") : TEXT("unsaved, and one Edit > Undo takes the whole plan back"));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIEQSPlanReport& Done)
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

	AActor* FindActor(UWorld& World, const FString& Label)
	{
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) return *It;
		}
		return nullptr;
	}
}

FHyperAIEQSInspectReport FHyperAIStudioEQSContracts::Inspect(const FHyperAIEQSInspectRequest& Request)
{
	using namespace HyperAIStudio::EQSTools::Private;
	FHyperAIEQSInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	FString Error;
	if (!Request.BehaviorTree.TrimStartAndEnd().IsEmpty())
	{
		FString Path = Request.BehaviorTree.TrimStartAndEnd();
		if (!FHyperAIStudioNames::IsObjectPath(Path, TEXT("BehaviorTree"), &Error))
		{
			Report.Diagnostic = Error;
			return Report;
		}
		if (!Path.Contains(TEXT("."))) Path += TEXT(".") + FPackageName::GetShortName(Path);
		UBehaviorTree* Tree = FindObject<UBehaviorTree>(nullptr, *Path);
		if (!Tree && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path)))
		{
			Tree = LoadObject<UBehaviorTree>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!Tree)
		{
			Report.Diagnostic = FString::Printf(TEXT("%s is not a Behavior Tree."), *Request.BehaviorTree);
			return Report;
		}
		Report.BehaviorTree = Tree->GetPathName();
		Gate::DescribeTree(*Tree, Report.Composites, Report.BlackboardKeys, Report.TreeNodes);
	}
	if (!Request.Asset.TrimStartAndEnd().IsEmpty())
	{
		UEnvQuery* Query = LoadQuery(Request.Asset, Error);
		if (!Query)
		{
			Report.Diagnostic = Error;
			return Report;
		}
		Report.Asset = Query->GetPathName();
		Report.Options = Gate::Describe(*Query);
		Report.QueryParams = Gate::QueryParams(*Query);
		Report.Warnings = Gate::Warn(*Query);
	}
	if (Report.Asset.IsEmpty() && Report.BehaviorTree.IsEmpty())
	{
		const auto AddKinds = [&Report](const TCHAR* Role, const TArray<Gate::FKindInfo>& Kinds)
		{
			for (const Gate::FKindInfo& Info : Kinds)
			{
				FHyperAIEQSKind& Kind = Report.Kinds.AddDefaulted_GetRef();
				Kind.Role = Role;
				Kind.Kind = Info.Kind;
				Kind.Class = Info.Class->GetPathName();
				Kind.Summary = Info.Summary;
				if (FCString::Strcmp(Role, TEXT("context")) != 0) Kind.Settings = Gate::DescribeKindSettings(Info.Class);
			}
		};
		AddKinds(TEXT("generator"), Gate::Generators());
		AddKinds(TEXT("test"), Gate::Tests());
		AddKinds(TEXT("context"), Gate::Contexts());
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UEnvQuery::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Each : Assets)
		{
			const FString Path = Each.GetObjectPathString();
			if (Path.StartsWith(TEXT("/Game/")) && Report.Assets.Num() < 200) Report.Assets.Add(Path);
		}
		Report.Assets.Sort();
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%d generator, test and context kind(s) hyper_eqs_apply_plan takes, and %d Environment Quer%s under /Game; give Asset or BehaviorTree to read one."),
			Report.Kinds.Num(), Report.Assets.Num(), Report.Assets.Num() == 1 ? TEXT("y") : TEXT("ies"));
		return Report;
	}
	Report.bOk = true;
	TArray<FString> Parts;
	if (!Report.Asset.IsEmpty())
	{
		int32 Tests = 0;
		for (const FHyperAIEQSOptionView& Option : Report.Options) Tests += Option.Tests.Num();
		Parts.Add(FString::Printf(TEXT("%s: %d option(s), %d test(s)%s"), *Report.Asset, Report.Options.Num(), Tests,
			Report.QueryParams.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", params %s"), *FString::Join(Report.QueryParams, TEXT(", ")))));
	}
	if (!Report.BehaviorTree.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("%s: %d composite(s), %d Blackboard key(s), %d EQS node(s)"), *Report.BehaviorTree, Report.Composites.Num(), Report.BlackboardKeys.Num(),
			Report.TreeNodes.Num()));
	}
	Report.Diagnostic = FString::Join(Parts, TEXT("; ")) + TEXT(".");
	return Report;
}

FHyperAIEQSPlanReport FHyperAIStudioEQSContracts::ApplyPlan(const FHyperAIEQSPlanRequest& Request)
{
	using namespace HyperAIStudio::EQSTools::Private;
	FHyperAIEQSPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("eqs-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Params"), TEXT("Children")})) return Report;
	if (GEditor && GEditor->PlayWorld)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play In Editor is running; stop it first, then make or change the query. hyper_eqs_run runs a query during play.");
		return Report;
	}
	FPlan Plan;
	FString Error;
	if (!Check(Request, Plan, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bDryRun)
	{
		Fill(Report, Plan);
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked on a copy: the query would have %d option(s)%s; nothing was changed."), Request.Operations.Num(), Plan.Options.Num(),
			Plan.TreeOps > 0 ? *FString::Printf(TEXT(" and %d Behavior Tree node(s) would be added"), Plan.TreeOps) : TEXT(""));
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("eqs.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Plan.AssetPath;
		Summary.Effects.Add(FString::Printf(TEXT("%s the Environment Query %s: %d operation(s), one undo step%s"), Plan.bCreate ? TEXT("Make") : TEXT("Change"), *Plan.AssetPath,
			Request.Operations.Num(), Request.bSave ? TEXT(", then saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIEQSOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s %s"), *Op.Kind, *Op.Type, *Op.Params.Left(120)));
		}
		Summary.Touches.Add(Plan.AssetPath);
		for (const FHyperAIEQSOp& Op : Request.Operations)
		{
			if (!Op.BehaviorTree.IsEmpty()) Summary.Touches.AddUnique(Op.BehaviorTree);
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited, so Execute checks the plan again before anything changes.
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

FHyperAIEQSRunReport FHyperAIStudioEQSContracts::Run(const FHyperAIEQSRunRequest& Request)
{
	using namespace HyperAIStudio::EQSTools::Private;
	FHyperAIEQSRunReport Report;
	Report.Status = TEXT("invalid_request");
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Params")})) return Report;
	FString Error;
	UEnvQuery* Query = LoadQuery(Request.Asset, Error);
	if (!Query)
	{
		Report.Diagnostic = Error;
		return Report;
	}
	Report.Asset = Query->GetPathName();
	const FString Mode = Request.RunMode.TrimStartAndEnd().ToLower();
	if (!Mode.IsEmpty() && Mode != TEXT("single") && Mode != TEXT("all") && Mode != TEXT("random_best_5") && Mode != TEXT("random_best_25"))
	{
		Report.Diagnostic = TEXT("RunMode is single, all, random_best_5 or random_best_25.");
		return Report;
	}
	UWorld* World = GEditor && GEditor->PlayWorld ? GEditor->PlayWorld.Get() : EditorWorld();
	if (!World)
	{
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	Report.World = World->IsPlayInEditor() ? TEXT("play") : TEXT("editor");
	FString Label = Request.Querier.TrimStartAndEnd();
	if (Label.IsEmpty() && GEditor)
	{
		if (const AActor* Selected = GEditor->GetSelectedActors()->GetTop<AActor>()) Label = Selected->GetActorLabel();
	}
	if (Label.IsEmpty())
	{
		Report.Diagnostic = TEXT("Give Querier, the label of the actor that asks (the AI's pawn), or select it in the level.");
		return Report;
	}
	AActor* Querier = FindActor(*World, Label);
	if (!Querier)
	{
		Report.Diagnostic = FString::Printf(TEXT("No actor in the %s world is labelled %s."), *Report.World, *Label);
		return Report;
	}
	Report.Querier = Querier->GetActorLabel();
	UEnvQueryManager* Manager = UEnvQueryManager::GetCurrent(World);
	if (!Manager)
	{
		Report.Diagnostic = TEXT("This world has no AI system to run queries (World Settings > Enable AI System is off).");
		return Report;
	}
	FEnvQueryRequest Run(Query, Querier);
	TArray<TPair<FString, FString>> Params;
	if (!Gate::SplitParams(Request.Params, Params, Error))
	{
		Report.Diagnostic = Error;
		return Report;
	}
	TArray<FAIDynamicParam> Wanted;
	Query->CollectQueryParams(*Query, Wanted);
	for (const TPair<FString, FString>& Param : Params)
	{
		const FAIDynamicParam* Known = Wanted.FindByPredicate([&Param](const FAIDynamicParam& Each) { return Each.ParamName.ToString().Equals(Param.Key, ESearchCase::IgnoreCase); });
		if (!Known)
		{
			TArray<FString> Names;
			for (const FAIDynamicParam& Each : Wanted) Names.Add(Each.ParamName.ToString());
			Report.Diagnostic = FString::Printf(TEXT("The query reads no param %s; it reads %s."), *Param.Key, Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", ")));
			return Report;
		}
		if (Known->ParamType == EAIParamType::Bool)
		{
			const FString Word = Param.Value.ToLower();
			Run.SetBoolParam(Known->ParamName, Word == TEXT("true") || Word == TEXT("1") || Word == TEXT("yes"));
		}
		else if (!Param.Value.IsNumeric())
		{
			Report.Diagnostic = FString::Printf(TEXT("Param %s takes a number."), *Param.Key);
			return Report;
		}
		else if (Known->ParamType == EAIParamType::Int)
		{
			Run.SetIntParam(Known->ParamName, FCString::Atoi(*Param.Value));
		}
		else
		{
			Run.SetFloatParam(Known->ParamName, FCString::Atof(*Param.Value));
		}
	}
	TArray<FString> NotGiven;
	for (const FAIDynamicParam& Each : Wanted)
	{
		if (!Params.ContainsByPredicate([&Each](const TPair<FString, FString>& Param) { return Each.ParamName.ToString().Equals(Param.Key, ESearchCase::IgnoreCase); }))
		{
			NotGiven.AddUnique(Each.ParamName.ToString());
		}
	}
	if (!NotGiven.IsEmpty())
	{
		// EQS reads a param nobody set as 0, which quietly changes what passes; a run states every one.
		Report.Status = TEXT("missing_param");
		Report.Diagnostic = FString::Printf(TEXT("The query reads %s; give each in Params (Name=Value), as the Behavior Tree node's Query Config would."), *FString::Join(NotGiven, TEXT(", ")));
		return Report;
	}
	RefreshRuns(Query);
	const double Start = FPlatformTime::Seconds();
	// Every item that passes, best first: the single and random modes pick from these.
	const TSharedPtr<FEnvQueryResult> Result = Manager->RunInstantQuery(Run, EEnvQueryRunMode::AllMatching);
	Report.Milliseconds = FMath::RoundToInt((FPlatformTime::Seconds() - Start) * 1000.0);
	Report.Warnings = Gate::Warn(*Query);
	if (!Result.IsValid())
	{
		Report.Status = TEXT("failed");
		Report.Diagnostic = TEXT("The query could not start (it has no options with a generator).");
		return Report;
	}
	if (Result->GetRawStatus() == EEnvQueryStatus::MissingParam)
	{
		Report.Status = TEXT("missing_param");
		Report.Diagnostic = FString::Printf(TEXT("The query needs its params: %s."), *FString::Join(Gate::QueryParams(*Query), TEXT(", ")));
		return Report;
	}
	Report.Option = Result->IsSuccessful() ? Result->OptionIndex : -1;
	Report.ItemType = Result->ItemType ? (Result->ItemType->IsChildOf(UEnvQueryItemType_ActorBase::StaticClass()) ? TEXT("actor") : TEXT("point")) : TEXT("none");
	Report.Passed = Result->IsSuccessful() ? Result->Items.Num() : 0;
	const int32 Listed = FMath::Clamp(Request.MaxItems, 1, MaxRunItems);
	const bool bActors = Result->ItemType && Result->ItemType->IsChildOf(UEnvQueryItemType_ActorBase::StaticClass());
	for (int32 Index = 0; Index < Report.Passed && Report.Items.Num() < Listed; ++Index)
	{
		FHyperAIEQSItem& Item = Report.Items.AddDefaulted_GetRef();
		Item.Rank = Index + 1;
		Item.Score = Result->GetItemScore(Index);
		Item.Location = Gate::VectorText(Result->GetItemAsLocation(Index));
		if (bActors)
		{
			if (const AActor* Actor = Result->GetItemAsActor(Index)) Item.Actor = Actor->GetActorLabel();
		}
	}
	Report.bOk = true;
	if (!Result->IsSuccessful() || Report.Passed == 0)
	{
		Report.Status = TEXT("failed");
		Report.Diagnostic = FString::Printf(TEXT("No item passed in %d ms from %s in the %s world: every option's items were filtered out (or none were made). Read Warnings, then loosen a filter."),
			Report.Milliseconds, *Report.Querier, *Report.World);
		return Report;
	}
	Report.Status = TEXT("success");
	const FHyperAIEQSItem& Best = Report.Items[0];
	FString Pick;
	if (Mode == TEXT("random_best_5") || Mode == TEXT("random_best_25"))
	{
		const float Floor = Best.Score * (Mode == TEXT("random_best_5") ? 0.95f : 0.75f);
		int32 Pool = 0;
		for (int32 Index = 0; Index < Report.Passed; ++Index) Pool += Result->GetItemScore(Index) >= Floor ? 1 : 0;
		Pick = FString::Printf(TEXT(" %s picks at random among the %d item(s) scoring %.3f or more."), *Mode, Pool, Floor);
	}
	else if (Mode == TEXT("all"))
	{
		Pick = TEXT(" all returns every item listed.");
	}
	Report.Diagnostic = FString::Printf(TEXT("Option %d won from %s in the %s world: %d item(s) passed in %d ms; the best is %s%s, score %.3f.%s"), Report.Option, *Report.Querier, *Report.World,
		Report.Passed, Report.Milliseconds, *Best.Location, Best.Actor.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Best.Actor), Best.Score, *Pick);
	return Report;
}

FHyperAIEQSInspectReport UHyperAIStudioEQSToolset::hyper_eqs_inspect(const FHyperAIEQSInspectRequest& Request)
{
	return FHyperAIStudioEQSContracts::Inspect(Request);
}

FHyperAIEQSPlanReport UHyperAIStudioEQSToolset::hyper_eqs_apply_plan(const FHyperAIEQSPlanRequest& Request)
{
	return FHyperAIStudioEQSContracts::ApplyPlan(Request);
}

FHyperAIEQSRunReport UHyperAIStudioEQSToolset::hyper_eqs_run(const FHyperAIEQSRunRequest& Request)
{
	return FHyperAIStudioEQSContracts::Run(Request);
}

void FHyperAIStudioEQSRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioEQSRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioEQSRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioEQSToolset::StaticClass(), FHyperAIStudioEQSContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioEQS, Warning, TEXT("Could not unregister the EQS toolset: %s"), *Error);
		}
	}
	HyperAIStudio::EQSTools::Private::GMadeAssets.Reset();
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioEQSRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioEQSContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioEQS, Verbose, TEXT("The EQS source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioEQSToolset::StaticClass(), FHyperAIStudioEQSContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioEQS, Error, TEXT("EQS cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
