// Games by Hyper 2026.

#include "HyperAIStudioDataRegistryToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/RealCurve.h"
#include "DataRegistry.h"
#include "DataRegistryId.h"
#include "DataRegistrySource.h"
#include "DataRegistrySource_CurveTable.h"
#include "DataRegistryTypes.h"
#include "DataRegistrySource_DataTable.h"
#include "DataRegistrySubsystem.h"
#include "Editor.h"
#include "EditorUndoClient.h"
#include "Engine/CurveTable.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "GameplayTagsManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioDataRegistryGate.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "JsonObjectConverter.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioDataRegistry, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioDataRegistry"

FString FHyperAIStudioDataRegistryContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioDataRegistry.HyperAIStudioDataRegistryToolset");
}

TArray<FString> FHyperAIStudioDataRegistryContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName, LookupToolName};
}

bool FHyperAIStudioDataRegistryContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::DataRegistryTools::Private
{
	using C = FHyperAIStudioDataRegistryContracts;
	namespace Gate = HyperAIStudio::DataRegistry::Gate;

	/**
	 * Registries a plan made or changed, so undo and redo keep the Content Browser and the Data Registry subsystem in step:
	 * the transaction marks a made registry garbage on undo and alive on redo, and puts back the sources it had, but the
	 * registry's runtime sources and the subsystem's map of types are not part of the undo record.
	 */
	class FTouchedRegistries final : public FSelfRegisteringEditorUndoClient
	{
	public:
		void Add(UDataRegistry* Registry, const bool bMade)
		{
			Touched.RemoveAll([](const TWeakObjectPtr<UDataRegistry>& Each) { return Each.IsStale(/*bIncludingGarbage=*/false); });
			Touched.AddUnique(Registry);
			if (bMade) Listed.Add(Registry, true);
		}

		virtual void PostUndo(bool bSuccess) override { Sync(); }
		virtual void PostRedo(bool bSuccess) override { Sync(); }

	private:
		void Sync()
		{
			bool bMapChanged = false;
			for (int32 Index = Touched.Num() - 1; Index >= 0; --Index)
			{
				UDataRegistry* Registry = Touched[Index].Get(/*bEvenIfGarbage=*/true);
				if (!Registry)
				{
					Touched.RemoveAt(Index);
					continue;
				}
				const bool bAlive = IsValid(Registry);
				if (bool* Known = Listed.Find(Registry); Known && *Known != bAlive)
				{
					// Undone, nothing of it is left to save; redone, it is unsaved work again.
					if (bAlive)
					{
						FAssetRegistryModule::AssetCreated(Registry);
						Registry->MarkPackageDirty();
					}
					else
					{
						FAssetRegistryModule::AssetDeleted(Registry);
						Registry->GetPackage()->SetDirtyFlag(false);
					}
					*Known = bAlive;
					bMapChanged = true;
				}
				if (bAlive)
				{
					TArray<FString> Ignored;
					Gate::Refresh(*Registry, Ignored);
				}
			}
			UDataRegistrySubsystem* Subsystem = GEngine ? GEngine->GetEngineSubsystem<UDataRegistrySubsystem>() : nullptr;
			if (bMapChanged && Subsystem && Subsystem->AreRegistriesInitialized()) Subsystem->RefreshRegistryMap();
		}

		TArray<TWeakObjectPtr<UDataRegistry>> Touched;
		TMap<TWeakObjectPtr<UDataRegistry>, bool> Listed;
	};

	/** Made on first use and let go at module shutdown, while the editor it registered with is still there. */
	TUniquePtr<FTouchedRegistries> GTouched;

	FTouchedRegistries& TouchedRegistries()
	{
		if (!GTouched) GTouched = MakeUnique<FTouchedRegistries>();
		return *GTouched;
	}

	UDataRegistry* LoadRegistry(const FString& Reference, FString& OutReason)
	{
		const FString Path = Reference.TrimStartAndEnd();
		if (!FHyperAIStudioNames::IsObjectPath(Path, TEXT("Asset"), &OutReason)) return nullptr;
		const FString ObjectPath = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
		UDataRegistry* Registry = FindObject<UDataRegistry>(nullptr, *ObjectPath);
		if (!Registry && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
		{
			Registry = LoadObject<UDataRegistry>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!IsValid(Registry))
		{
			OutReason = FString::Printf(TEXT("%s is not a Data Registry."), *Path);
			return nullptr;
		}
		return Registry;
	}

	/** The registry games know by Type: the subsystem's when it runs, otherwise the one the asset registry lists with that type. */
	UDataRegistry* RegistryOfType(const FString& Type, FString& OutReason)
	{
		if (!FHyperAIStudioNames::IsName(Type, TEXT("RegistryType"), &OutReason)) return nullptr;
		const FName Wanted(*Type.TrimStartAndEnd());
		const UDataRegistrySubsystem* Subsystem = GEngine ? GEngine->GetEngineSubsystem<UDataRegistrySubsystem>() : nullptr;
		if (Subsystem && Subsystem->AreRegistriesInitialized())
		{
			if (UDataRegistry* Found = Subsystem->GetRegistryForType(Wanted)) return Found;
		}
		const FString Taken = Gate::TypeTakenBy(Wanted, FString());
		if (!Taken.IsEmpty()) return LoadRegistry(Taken, OutReason);
		OutReason = FString::Printf(TEXT("No Data Registry has the type %s; hyper_data_registry_inspect without Asset lists the registries."), *Type);
		return nullptr;
	}

	/** Lines describing the registry's sources as it holds them. */
	TArray<FString> SourceLines(const UDataRegistry& Registry)
	{
		TArray<FString> Out;
		const TArray<TObjectPtr<UDataRegistrySource>>& Sources = Registry.GetDataSources();
		for (int32 Index = 0; Index < Sources.Num(); ++Index)
		{
			const UDataRegistrySource* Source = Sources[Index];
			FString Line = FString::Printf(TEXT("%d %s"), Index, *Gate::SourceKindOf(Source));
			if (const UDataRegistrySource_DataTable* Table = Cast<UDataRegistrySource_DataTable>(Source)) Line += TEXT(" ") + Table->SourceTable.ToString();
			else if (const UDataRegistrySource_CurveTable* Curves = Cast<UDataRegistrySource_CurveTable>(Source)) Line += TEXT(" ") + Curves->SourceTable.ToString();
			else if (const UMetaDataRegistrySource* Meta = Cast<UMetaDataRegistrySource>(Source)) Line += TEXT(" over ") + FString::Join(Meta->SearchRules.AssetScanPaths, TEXT(", "));
			Out.Add(Line);
		}
		return Out;
	}

	/** The plan for real: the registry in one undo step, refreshed, then saved if asked. */
	FHyperAIDataRegistryPlanReport Execute(const FHyperAIDataRegistryPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIDataRegistryPlanReport Report;
		Report.OperationId = OperationId;
		Gate::FPlan Plan;
		FString Error;
		if (!Gate::Check(Request, Plan, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		UDataRegistry* Registry = nullptr;
		{
			const FScopedTransaction Transaction(LOCTEXT("DataRegistryPlan", "HyperAI: Data Registry Plan"));
			if (Plan.bMake)
			{
				// What an undone plan made here is out of the way first: this plan's step ends the redo that kept it.
				if (UDataRegistry* Undone = Gate::UndoneAt(Plan.AssetPath))
				{
					// UDataRegistry::PostRename asks the subsystem to validate it, and logs an error when the subsystem has no
					// registries (none are scanned): moving an undone registry aside is not worth that error.
					const ELogVerbosity::Type Was = LogDataRegistry.GetVerbosity();
					LogDataRegistry.SetVerbosity(ELogVerbosity::Fatal);
					Undone->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
					LogDataRegistry.SetVerbosity(Was);
				}
				UPackage* Package = CreatePackage(*Plan.AssetPath);
				Registry = NewObject<UDataRegistry>(Package, *FPackageName::GetShortName(Plan.AssetPath), RF_Public | RF_Standalone | RF_Transactional);
				// Recorded as made in this undo step: garbage when the step began, alive when it ended. Undo marks it garbage
				// again and redo brings it back.
				Registry->MarkAsGarbage();
				Registry->Modify();
				Registry->ClearGarbage();
				FAssetRegistryModule::AssetCreated(Registry);
			}
			else
			{
				Registry = Plan.Existing;
				Registry->Modify();
			}
			if (!Gate::Write(*Registry, Plan, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = Error + TEXT(" Edit > Undo takes back what the plan did.");
				return Report;
			}
			Registry->MarkPackageDirty();
		}
		TouchedRegistries().Add(Registry, Plan.bMake);
		Report.Notes = Plan.Notes;
		Report.Warnings = Plan.Warnings;
		// After the step: the registry's runtime sources and the subsystem's map are not part of the undo record.
		Gate::Refresh(*Registry, Report.Notes);
		Report.Asset = Registry->GetPathName();
		Report.RegistryType = Registry->GetRegistryType().ToString();
		Report.ItemStruct = GetPathNameSafe(Registry->GetItemStruct());
		Report.Sources = SourceLines(*Registry);
		TArray<FString> Skipped;
		const TArray<FName> Names = Gate::ItemNames(Gate::ResolveTables(*Registry, Skipped));
		Report.Warnings.Append(Skipped);
		Report.ItemCount = Names.Num();
		for (int32 Index = 0; Index < Names.Num() && Index < 20; ++Index) Report.Items.Add(Names[Index].ToString());
		Report.bRegistered = Gate::IsRegistered(*Registry);
		Report.bInScannedDirectory = Gate::IsScanned(Plan.AssetPath);
		if (Request.bSave)
		{
			if (!FHyperAIStudioSourceControl::SaveAsset(*Registry, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Registry->GetPathName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%s: type %s, items %s, %d source(s), %d item(s)%s; %s. Look items up with hyper_data_registry_lookup."), *Report.Asset, *Report.RegistryType,
			*GetNameSafe(Registry->GetItemStruct()), Report.Sources.Num(), Report.ItemCount, Report.bInScannedDirectory ? TEXT("") : TEXT(", outside the scanned folders (see Warnings)"),
			Request.bSave ? TEXT("saved") : TEXT("unsaved, and one Edit > Undo takes the whole plan back"));
		return Report;
	}

	FHyperAIDataRegistryPlanReport DryRun(const FHyperAIDataRegistryPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIDataRegistryPlanReport Report;
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
		Report.RegistryType = Plan.RegistryType.ToString();
		Report.ItemStruct = GetPathNameSafe(Plan.ItemStruct);
		for (int32 Index = 0; Index < Plan.Sources.Num(); ++Index) Report.Sources.Add(Plan.Sources[Index].Describe(Index));
		Report.Notes = Plan.Notes;
		Report.Warnings = Plan.Warnings;
		Report.bInScannedDirectory = Gate::IsScanned(Plan.AssetPath);
		Report.bRegistered = Plan.Existing && Gate::IsRegistered(*Plan.Existing);
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked: %s %s with %d source(s); nothing was changed."), Request.Operations.Num(),
			Plan.bMake ? TEXT("make") : TEXT("change"), *Plan.AssetPath, Plan.Sources.Num());
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIDataRegistryPlanReport& Done)
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

	FString SummaryOf(const FString& Kind)
	{
		if (Kind == TEXT("create")) return TEXT("Makes the registry (first, once): RegistryType, ItemStruct and an optional base gameplay Tag; Params for the settings below.");
		if (Kind == TEXT("set")) return TEXT("Changes the registry's settings with Params, these names.");
		if (Kind == TEXT("source.add")) return TEXT("Adds a source of SourceKind: Table for data_table and curve_table, Paths for the meta kinds, Params for its settings (see SourceKinds); Index places it, earlier sources winning an id.");
		if (Kind == TEXT("source.set")) return TEXT("Changes source Index: its Table, Paths or Params.");
		return TEXT("Removes source Index.");
	}

	FString SourceSummaryOf(const FString& Kind)
	{
		if (Kind == TEXT("data_table")) return TEXT("One data table whose rows are the item struct (or a struct made from it).");
		if (Kind == TEXT("curve_table")) return TEXT("One curve table, for a registry of RealCurve, RichCurve or SimpleCurve items.");
		if (Kind == TEXT("data_table_meta")) return TEXT("Every data table with matching rows under Paths, plus tables game features register at run time; AssetUsage SearchAssets by default here.");
		return TEXT("Every curve table under Paths, plus tables registered at run time; AssetUsage SearchAssets by default here.");
	}
}

FHyperAIDataRegistryInspectReport FHyperAIStudioDataRegistryContracts::Inspect(const FHyperAIDataRegistryInspectRequest& Request)
{
	using namespace HyperAIStudio::DataRegistryTools::Private;
	FHyperAIDataRegistryInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	Report.ScanDirectories = Gate::ScanDirectories();
	if (Request.Asset.TrimStartAndEnd().IsEmpty())
	{
		for (const FString& Kind : Gate::OpKinds())
		{
			FHyperAIDataRegistryKind& Entry = Report.Kinds.AddDefaulted_GetRef();
			Entry.Kind = Kind;
			Entry.Summary = SummaryOf(Kind);
			if (Kind == TEXT("create") || Kind == TEXT("set")) Entry.Settings = Gate::DescribeLeaves(*UDataRegistry::StaticClass(), nullptr, false);
		}
		for (const FString& Kind : Gate::SourceKinds())
		{
			FHyperAIDataRegistryKind& Entry = Report.SourceKinds.AddDefaulted_GetRef();
			Entry.Kind = Kind;
			Entry.Summary = SourceSummaryOf(Kind);
			Entry.Settings = Gate::DescribeLeaves(*Gate::SourceClassOf(Kind), nullptr, false);
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByClass(UDataRegistry::StaticClass()->GetClassPathName(), Assets);
		for (const FAssetData& Each : Assets)
		{
			const FString Path = Each.GetObjectPathString();
			if (!Path.StartsWith(TEXT("/Game/")) || Report.Registries.Num() >= 200) continue;
			if (UObject* Loaded = Each.FastGetAsset(false); Loaded && !IsValid(Loaded)) continue;
			FString Type, Struct;
			Each.GetTagValue(FName(TEXT("RegistryType")), Type);
			Each.GetTagValue(FName(TEXT("ItemStruct")), Struct);
			Report.Registries.Add(FString::Printf(TEXT("%s (type %s, items %s%s)"), *Path, *Type, *Struct, Gate::IsScanned(Each.PackageName.ToString()) ? TEXT("") : TEXT(", not in a scanned folder")));
		}
		Report.Registries.Sort();
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%d op kind(s) and %d source kind(s) hyper_data_registry_apply_plan takes, and %d Data Registr%s under /Game; give Asset to read one."),
			Report.Kinds.Num(), Report.SourceKinds.Num(), Report.Registries.Num(), Report.Registries.Num() == 1 ? TEXT("y") : TEXT("ies"));
		return Report;
	}
	UDataRegistry* Registry = LoadRegistry(Request.Asset, Report.Diagnostic);
	if (!Registry) return Report;
	Report.Asset = Registry->GetPathName();
	Report.RegistryType = Registry->GetRegistryType().ToString();
	Report.ItemStruct = GetPathNameSafe(Registry->GetItemStruct());
	const FGameplayTag BaseTag = Registry->GetIdFormat().BaseGameplayTag;
	Report.BaseTag = BaseTag.IsValid() ? BaseTag.ToString() : FString();
	for (const FString& Line : Gate::DescribeLeaves(*UDataRegistry::StaticClass(), Registry, true))
	{
		// Type, struct and tag have fields of their own.
		if (!Line.StartsWith(TEXT("RegistryType:")) && !Line.StartsWith(TEXT("ItemStruct:")) && !Line.StartsWith(TEXT("BaseGameplayTag:"))) Report.Settings.Add(Line);
	}
	TArray<FString> Notes;
	const TArray<Gate::FResolvedTable> Tables = Gate::ResolveTables(*Registry, Notes);
	const TArray<TObjectPtr<UDataRegistrySource>>& Sources = Registry->GetDataSources();
	for (int32 Index = 0; Index < Sources.Num(); ++Index)
	{
		UDataRegistrySource* Source = Sources[Index];
		FHyperAIDataRegistrySource& Entry = Report.Sources.AddDefaulted_GetRef();
		Entry.Index = Index;
		Entry.Kind = Gate::SourceKindOf(Source);
		if (!Source) continue;
		Entry.Settings = Gate::DescribeLeaves(*Source->GetClass(), Source, true);
		if (const UDataRegistrySource_DataTable* Table = Cast<UDataRegistrySource_DataTable>(Source)) Entry.Table = Table->SourceTable.ToString();
		else if (const UDataRegistrySource_CurveTable* Curves = Cast<UDataRegistrySource_CurveTable>(Source)) Entry.Table = Curves->SourceTable.ToString();
		else if (const UMetaDataRegistrySource* Meta = Cast<UMetaDataRegistrySource>(Source))
		{
			Entry.SearchPaths = Meta->SearchRules.AssetScanPaths;
			FString Usage = StaticEnum<EMetaDataRegistrySourceAssetUsage>()->GetNameStringByValue(static_cast<int64>(Meta->AssetUsage));
			int32 Colons = Usage.Find(TEXT("::"));
			Entry.AssetUsage = Colons == INDEX_NONE ? Usage : Usage.Mid(Colons + 2);
		}
		for (const Gate::FResolvedTable& Table : Tables)
		{
			if (Table.SourceIndex != Index) continue;
			Entry.Tables.Add(FString::Printf(TEXT("%s (%d row(s))"), *Table.Path, Table.DataTable ? Table.DataTable->GetRowMap().Num() : Table.CurveTable ? Table.CurveTable->GetRowMap().Num() : 0));
		}
		TArray<UDataRegistrySource*> Children;
		Registry->GetChildRuntimeSources(Source, Children);
		for (const UDataRegistrySource* Child : Children)
		{
			if (Child) Entry.RuntimeSources.Add(FString::Printf(TEXT("%s (%s)"), *Child->GetDebugString(), *Child->GetSourceAssetPath().ToString()));
		}
	}
	const TArray<FName> Names = Gate::ItemNames(Tables);
	Report.ItemCount = Names.Num();
	const int32 Listed = FMath::Clamp(Request.MaxItems, 0, MaxListedItems);
	for (int32 Index = 0; Index < Names.Num() && Index < Listed; ++Index) Report.Items.Add(Names[Index].ToString());
	Report.bInScannedDirectory = Gate::IsScanned(Registry->GetPackage()->GetName());
	Report.bRegistered = Gate::IsRegistered(*Registry);
	Report.bInitialized = Registry->IsInitialized();
	Report.Warnings = Notes;
	if (!Report.bInScannedDirectory) Report.Warnings.Add(Gate::ScanWarning(Registry->GetPackage()->GetName()));
	if (Sources.IsEmpty()) Report.Warnings.Add(TEXT("The registry has no sources, so it holds no items."));
	if (BaseTag.IsValid())
	{
		// Ids picked through the tag are the tag's children; a row named otherwise is reachable only by its plain name.
		const FGameplayTagContainer Children = UGameplayTagsManager::Get().RequestGameplayTagChildren(BaseTag);
		int32 Outside = 0;
		for (const FName& Name : Names)
		{
			if (!Children.HasTagExact(UGameplayTagsManager::Get().RequestGameplayTag(Name, /*ErrorIfNotFound=*/false))) ++Outside;
		}
		if (Outside > 0) Report.Warnings.Add(FString::Printf(TEXT("%d item name(s) are not tags under %s, so the id pickers in Blueprints and Details cannot choose them."), Outside, *Report.BaseTag));
	}
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%s: type %s, items %s, %d source(s), %d item(s) in the editor; %s, %s."), *Report.Asset, *Report.RegistryType, *GetNameSafe(Registry->GetItemStruct()),
		Report.Sources.Num(), Report.ItemCount, Report.bInScannedDirectory ? TEXT("in a scanned folder") : TEXT("NOT in a scanned folder"),
		Report.bRegistered ? TEXT("registered with the Data Registry subsystem") : TEXT("not registered with the Data Registry subsystem now"));
	return Report;
}

FHyperAIDataRegistryPlanReport FHyperAIStudioDataRegistryContracts::ApplyPlan(const FHyperAIDataRegistryPlanRequest& Request)
{
	using namespace HyperAIStudio::DataRegistryTools::Private;
	FHyperAIDataRegistryPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("dataregistry-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Params")})) return Report;
	if (GEditor && GEditor->PlayWorld)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("Play In Editor is running; stop it first, then make or change the Data Registry.");
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
		Summary.VariantId = TEXT("dataregistry.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Plan.AssetPath;
		Summary.Effects.Add(FString::Printf(TEXT("%s the Data Registry %s: %d operation(s), one undo step%s"), Plan.bMake ? TEXT("Make") : TEXT("Change"), *Plan.AssetPath,
			Request.Operations.Num(), Request.bSave ? TEXT(", then saved") : TEXT("")));
		for (int32 Index = 0; Index < Plan.Notes.Num() && Index < 24; ++Index) Summary.Effects.Add(TEXT("| ") + Plan.Notes[Index].Left(160));
		Summary.Touches.Add(Plan.AssetPath);
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited (an asset made at the path, a table deleted), so Execute
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

FHyperAIDataRegistryLookupReport FHyperAIStudioDataRegistryContracts::Lookup(const FHyperAIDataRegistryLookupRequest& Request)
{
	using namespace HyperAIStudio::DataRegistryTools::Private;
	FHyperAIDataRegistryLookupReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	const FString Format = Request.Format.TrimStartAndEnd().ToLower();
	if (!Format.IsEmpty() && Format != TEXT("text") && Format != TEXT("json"))
	{
		Report.Diagnostic = TEXT("Format is text (the default) or json.");
		return Report;
	}
	if (Request.Items.IsEmpty() || Request.Items.Num() > MaxLookupItems)
	{
		Report.Diagnostic = FString::Printf(TEXT("Give 1 to %d Items: item names (Sword) or ids (Items:Sword)."), MaxLookupItems);
		return Report;
	}
	UDataRegistry* Registry = nullptr;
	if (!Request.Asset.TrimStartAndEnd().IsEmpty()) Registry = LoadRegistry(Request.Asset, Report.Diagnostic);
	else if (!Request.RegistryType.TrimStartAndEnd().IsEmpty()) Registry = RegistryOfType(Request.RegistryType, Report.Diagnostic);
	else Report.Diagnostic = TEXT("Give Asset (the registry's path) or RegistryType.");
	if (!Registry) return Report;
	Report.Asset = Registry->GetPathName();
	Report.RegistryType = Registry->GetRegistryType().ToString();
	Report.ItemStruct = GetPathNameSafe(Registry->GetItemStruct());
	TArray<FString> Notes;
	const TArray<Gate::FResolvedTable> Tables = Gate::ResolveTables(*Registry, Notes);
	Report.Warnings = Notes;
	// The running registry answers first, as games ask it; its cache holds only what was acquired or precached.
	const bool bRunning = Registry->IsInitialized();
	Report.ResolvedBy = bRunning ? TEXT("registry") : TEXT("tables");
	if (!bRunning)
	{
		Report.Warnings.Add(TEXT("The registry is not initialized in this editor (the Data Registry subsystem has not loaded it), so its items were read from its sources' tables, in the order the registry asks them."));
	}
	const UScriptStruct* RealCurve = FRealCurve::StaticStruct();
	for (const FString& Asked : Request.Items)
	{
		FHyperAIDataRegistryItem& Item = Report.Items.AddDefaulted_GetRef();
		const FString Whole = Asked.TrimStartAndEnd();
		FString Type, Name = Whole;
		if (Whole.Contains(TEXT(":"))) Whole.Split(TEXT(":"), &Type, &Name);
		Type.TrimStartAndEndInline();
		Name.TrimStartAndEndInline();
		Item.Id = Report.RegistryType + TEXT(":") + Name;
		if (Name.IsEmpty())
		{
			Item.Diagnostic = FString::Printf(TEXT("'%s' names no item."), *Asked);
			Report.Missing.Add(Asked);
			continue;
		}
		if (!Type.IsEmpty() && FName(*Type) != Registry->GetRegistryType())
		{
			Item.Diagnostic = FString::Printf(TEXT("%s is an id of the registry type %s, not %s."), *Asked, *Type, *Report.RegistryType);
			Report.Missing.Add(Asked);
			continue;
		}
		const FDataRegistryId Id(Registry->GetRegistryType(), FName(*Name));
		const uint8* Memory = nullptr;
		const UScriptStruct* Struct = Registry->GetItemStruct();
		if (bRunning)
		{
			const UScriptStruct* Cached = nullptr;
			if (Registry->GetCachedItemRaw(Memory, Cached, Id) && Memory)
			{
				Struct = Cached;
				FDataRegistryLookup Found;
				FName Resolved;
				if (Registry->ResolveDataRegistryId(Found, Id))
				{
					if (UDataRegistrySource* Source = Registry->LookupSource(Resolved, Found, 0)) Item.Source = TEXT("runtime source ") + Source->GetDebugString();
				}
			}
		}
		for (const Gate::FResolvedTable& Table : Tables)
		{
			const uint8* Row = Table.DataTable ? Table.DataTable->FindRowUnchecked(Id.ItemName)
				: Table.CurveTable ? reinterpret_cast<const uint8*>(Table.CurveTable->FindCurveUnchecked(Id.ItemName)) : nullptr;
			if (!Row) continue;
			if (!Memory)
			{
				Memory = Row;
				if (Table.DataTable) Struct = Table.DataTable->GetRowStruct();
				Item.Source = Table.Describe();
			}
			else if (!Item.Source.EndsWith(Table.Path))
			{
				Item.Shadowed.Add(Table.Describe());
			}
		}
		if (!Memory || !Struct)
		{
			TArray<FName> Names = Gate::ItemNames(Tables);
			TArray<FString> Some;
			for (int32 Index = 0; Index < Names.Num() && Index < 8; ++Index) Some.Add(Names[Index].ToString());
			Item.Diagnostic = Tables.IsEmpty() ? FString::Printf(TEXT("No source of the registry reads a table, so it holds no %s."), *Name)
				: FString::Printf(TEXT("No source holds %s%s."), *Name, Some.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; names look like %s"), *FString::Join(Some, TEXT(", "))));
			Report.Missing.Add(Asked);
			continue;
		}
		Item.bFound = true;
		Item.Struct = Struct->GetName();
		if (Struct->IsChildOf(RealCurve))
		{
			const FRealCurve* Curve = reinterpret_cast<const FRealCurve*>(Memory);
			Item.bCurve = true;
			Item.Value = Curve->Eval(Request.Level);
			Item.Fields.Add(FString::Printf(TEXT("Value at %g: %g"), Request.Level, Item.Value));
			Item.Fields.Add(FString::Printf(TEXT("Keys: %d"), Curve->GetNumKeys()));
			int32 Shown = 0;
			for (auto Key = Curve->GetKeyHandleIterator(); Key && Shown < 20; ++Key, ++Shown)
			{
				const TPair<float, float> Pair = Curve->GetKeyTimeValuePair(*Key);
				Item.Fields.Add(FString::Printf(TEXT("%g: %g"), Pair.Key, Pair.Value));
			}
			continue;
		}
		if (Format == TEXT("json"))
		{
			FJsonObjectConverter::UStructToJsonObjectString(Struct, Memory, Item.Json, 0, 0, 0, nullptr, /*bPrettyPrint=*/false);
		}
		else
		{
			Item.Fields = Gate::ExportFields(*Struct, Memory);
		}
	}
	Report.bOk = true;
	Report.Diagnostic = FString::Printf(TEXT("%s: %d of %d item(s) found%s."), *Report.Asset, Request.Items.Num() - Report.Missing.Num(), Request.Items.Num(),
		Report.Missing.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; missing %s"), *FString::Join(Report.Missing, TEXT(", "))));
	return Report;
}

FHyperAIDataRegistryInspectReport UHyperAIStudioDataRegistryToolset::hyper_data_registry_inspect(const FHyperAIDataRegistryInspectRequest& Request)
{
	return FHyperAIStudioDataRegistryContracts::Inspect(Request);
}

FHyperAIDataRegistryPlanReport UHyperAIStudioDataRegistryToolset::hyper_data_registry_apply_plan(const FHyperAIDataRegistryPlanRequest& Request)
{
	return FHyperAIStudioDataRegistryContracts::ApplyPlan(Request);
}

FHyperAIDataRegistryLookupReport UHyperAIStudioDataRegistryToolset::hyper_data_registry_lookup(const FHyperAIDataRegistryLookupRequest& Request)
{
	return FHyperAIStudioDataRegistryContracts::Lookup(Request);
}

void FHyperAIStudioDataRegistryRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioDataRegistryRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioDataRegistryRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioDataRegistryToolset::StaticClass(),
			FHyperAIStudioDataRegistryContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioDataRegistry, Warning, TEXT("Could not unregister the data registry toolset: %s"), *Error);
		}
	}
	HyperAIStudio::DataRegistryTools::Private::GTouched.Reset();
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioDataRegistryRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioDataRegistryContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioDataRegistry, Verbose, TEXT("The data registry source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioDataRegistryToolset::StaticClass(),
		FHyperAIStudioDataRegistryContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioDataRegistry, Error, TEXT("Data registry cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
