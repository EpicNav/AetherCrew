// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/RichCurve.h"
#include "DataRegistry.h"
#include "DataRegistrySource.h"
#include "DataRegistrySource_CurveTable.h"
#include "DataRegistrySource_DataTable.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/CurveTable.h"
#include "Engine/DataTable.h"
#include "HyperAIStudioDataRegistryGate.h"
#include "HyperAIStudioDataRegistryToolset.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::DataRegistryTests
{
	using C = FHyperAIStudioDataRegistryContracts;
	namespace Gate = HyperAIStudio::DataRegistry::Gate;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	/** An engine row struct any editor has: Tag (a name) and DevComment (a string). */
	const TCHAR* const TagRow = TEXT("/Script/GameplayTags.GameplayTagTableRow");
	/** Another, unrelated to it: for the refusal of a table whose rows do not match. */
	const TCHAR* const MirrorRow = TEXT("/Script/Engine.MirrorTableRow");

	FString Unique()
	{
		return FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	}

	/** A folder of its own per test run, so a meta source over it finds only that test's tables. */
	FString TestFolder()
	{
		return FString::Printf(TEXT("/Game/HyperAITests/DataRegistry/Run_%s"), *Unique());
	}

	FString TestAsset(const FString& Folder, const TCHAR* Name)
	{
		return FString::Printf(TEXT("%s/%s_%s"), *Folder, Name, *Unique());
	}

	FHyperAIDataRegistryOp Op(const TCHAR* Kind, const TCHAR* Params = TEXT(""))
	{
		FHyperAIDataRegistryOp Out;
		Out.Kind = Kind;
		Out.Params = Params;
		return Out;
	}

	FHyperAIDataRegistryOp Create(const FString& Type, const FString& Struct, const TCHAR* Tag = TEXT(""))
	{
		FHyperAIDataRegistryOp Out = Op(TEXT("create"));
		Out.RegistryType = Type;
		Out.ItemStruct = Struct;
		Out.Tag = Tag;
		return Out;
	}

	FHyperAIDataRegistryOp AddTable(const TCHAR* Kind, const FString& Table, const int32 Index = -1, const TCHAR* Params = TEXT(""))
	{
		FHyperAIDataRegistryOp Out = Op(TEXT("source.add"), Params);
		Out.SourceKind = Kind;
		Out.Table = Table;
		Out.Index = Index;
		return Out;
	}

	FHyperAIDataRegistryOp AddMeta(const FString& Folder)
	{
		FHyperAIDataRegistryOp Out = Op(TEXT("source.add"));
		Out.SourceKind = TEXT("data_table_meta");
		Out.Paths = {Folder};
		return Out;
	}

	FHyperAIDataRegistryOp AtIndex(const TCHAR* Kind, const int32 Index, const TCHAR* Params = TEXT(""))
	{
		FHyperAIDataRegistryOp Out = Op(Kind, Params);
		Out.Index = Index;
		return Out;
	}

	/** Runs a plan with approval off, as an agent with one-shot edits allowed would. */
	FHyperAIDataRegistryPlanReport Run(const FString& Asset, TArray<FHyperAIDataRegistryOp> Ops, const bool bDryRun)
	{
		FHyperAIDataRegistryPlanRequest Request;
		Request.Asset = Asset;
		Request.Operations = MoveTemp(Ops);
		Request.bDryRun = bDryRun;
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		FHyperAIDataRegistryPlanReport Report = C::ApplyPlan(Request);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Report;
	}

	bool Refused(const FHyperAIDataRegistryPlanReport& Report, const TCHAR* Says)
	{
		return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Says);
	}

	UDataRegistry* Find(const FString& Path)
	{
		return FindObject<UDataRegistry>(nullptr, *(Path + TEXT(".") + FPackageName::GetShortName(Path)));
	}

	/** An unsaved data table in memory, listed in the asset registry as a new asset is. Rows: name to DevComment. */
	UDataTable* MakeTable(const FString& Path, const TCHAR* StructPath, const TArray<TPair<FString, FString>>& Rows)
	{
		UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, StructPath);
		if (!Struct) return nullptr;
		UPackage* Package = CreatePackage(*Path);
		UDataTable* Table = NewObject<UDataTable>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
		Table->RowStruct = Struct;
		for (const TPair<FString, FString>& Row : Rows)
		{
			FStructOnScope Memory(Struct);
			if (FNameProperty* Tag = CastField<FNameProperty>(Struct->FindPropertyByName(TEXT("Tag")))) Tag->SetPropertyValue_InContainer(Memory.GetStructMemory(), FName(*Row.Key));
			if (FStrProperty* Comment = CastField<FStrProperty>(Struct->FindPropertyByName(TEXT("DevComment")))) Comment->SetPropertyValue_InContainer(Memory.GetStructMemory(), Row.Value);
			Table->AddRow(FName(*Row.Key), Memory.GetStructMemory(), Struct);
		}
		FAssetRegistryModule::AssetCreated(Table);
		return Table;
	}

	UCurveTable* MakeCurves(const FString& Path)
	{
		UPackage* Package = CreatePackage(*Path);
		UCurveTable* Table = NewObject<UCurveTable>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
		FRichCurve& Damage = Table->AddRichCurve(TEXT("Damage"));
		Damage.AddKey(1.f, 10.f);
		Damage.AddKey(5.f, 50.f);
		FAssetRegistryModule::AssetCreated(Table);
		return Table;
	}

	/** A test's table let go: out of the asset registry, nothing to save, collected. */
	void Drop(UObject* Asset)
	{
		if (!Asset) return;
		FAssetRegistryModule::AssetDeleted(Asset);
		Asset->ClearFlags(RF_Public | RF_Standalone);
		Asset->GetPackage()->SetDirtyFlag(false);
		Asset->MarkAsGarbage();
	}

	/** The plans' undo steps taken back, whatever the test found. */
	void UndoSince(const int32 UndoBefore)
	{
		if (!GEditor || !GEditor->Trans) return;
		const int32 Steps = GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount();
		for (int32 Index = 0; Index < Steps; ++Index) GEditor->UndoTransaction();
	}

	FHyperAIDataRegistryLookupReport Look(const FString& Asset, TArray<FString> Items, const TCHAR* Format = TEXT(""), const float Level = 1.f)
	{
		FHyperAIDataRegistryLookupRequest Request;
		Request.Asset = Asset;
		Request.Items = MoveTemp(Items);
		Request.Format = Format;
		Request.Level = Level;
		return C::Lookup(Request);
	}

	const FHyperAIDataRegistryItem* ItemOf(const FHyperAIDataRegistryLookupReport& Report, const TCHAR* Name)
	{
		return Report.Items.FindByPredicate([Name](const FHyperAIDataRegistryItem& Item) { return Item.Id.EndsWith(FString(TEXT(":")) + Name); });
	}

	bool HasField(const FHyperAIDataRegistryItem* Item, const FString& Text)
	{
		return Item && Item->Fields.ContainsByPredicate([&Text](const FString& Field) { return Field.Contains(Text); });
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDataRegistryLogicTest, "HyperAIStudio.NativeTools.DataRegistry.Logic", HyperAIStudio::DataRegistryTests::Flags)

bool FHyperAIStudioDataRegistryLogicTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DataRegistryTests;
	TArray<TPair<FString, FString>> Pairs;
	FString Error;
	TestTrue(TEXT("params split: ") + Error, Gate::SplitParams(TEXT("MaxNumberKept=10; bCacheIsAlwaysVolatile = true;"), Pairs, Error) && Pairs.Num() == 2
		&& Pairs[1].Key == TEXT("bCacheIsAlwaysVolatile") && Pairs[1].Value == TEXT("true"));
	TestFalse(TEXT("a part without = is refused"), Gate::SplitParams(TEXT("MaxNumberKept"), Pairs, Error));

	// Registry settings: the cache policy's fields by their own names, any case, a bool's b optional.
	const TArray<Gate::FLeaf> Registry = Gate::Leaves(*UDataRegistry::StaticClass());
	const Gate::FLeaf* Kept = Gate::FindLeaf(Registry, TEXT("maxnumberkept"));
	TestTrue(TEXT("MaxNumberKept is the cache policy's"), Kept && Kept->Path == TEXT("DefaultCachePolicy.MaxNumberKept"));
	TestNotNull(TEXT("and by its path"), Gate::FindLeaf(Registry, TEXT("DefaultCachePolicy.MaxNumberKept")));
	TestNotNull(TEXT("a bool without its b"), Gate::FindLeaf(Registry, TEXT("CacheIsAlwaysVolatile")));
	TestNotNull(TEXT("the id format's tag"), Gate::FindLeaf(Registry, TEXT("BaseGameplayTag")));
	TestNull(TEXT("the sources are the source ops' work, not a setting"), Gate::FindLeaf(Registry, TEXT("DataSources")));
	TestNull(TEXT("runtime sources are not settings"), Gate::FindLeaf(Registry, TEXT("RuntimeSources")));

	// Values coerced.
	const TArray<Gate::FLeaf> Meta = Gate::Leaves(*UMetaDataRegistrySource_DataTable::StaticClass());
	Gate::FAssign Assign;
	const Gate::FLeaf* Usage = Gate::FindLeaf(Meta, TEXT("AssetUsage"));
	TestTrue(TEXT("an enum by its short name: ") + Error, Usage && Gate::CoerceValue(*Usage->Property, TEXT("searchandregisterassets"), Assign, Error) && Assign.Import == TEXT("SearchAndRegisterAssets"));
	TestFalse(TEXT("an enum word it lacks is refused"), Usage && Gate::CoerceValue(*Usage->Property, TEXT("Everything"), Assign, Error));
	TestTrue(TEXT("naming the words: ") + Error, Error.Contains(TEXT("RegisterAssets")));
	const Gate::FLeaf* Paths = Gate::FindLeaf(Meta, TEXT("AssetScanPaths"));
	TestTrue(TEXT("a list of paths: ") + Error, Paths && Gate::CoerceValue(*Paths->Property, TEXT("/Game/A, /Game/B"), Assign, Error) && Assign.Import == TEXT("(\"/Game/A\",\"/Game/B\")"));
	TestFalse(TEXT("a search path that is not a path is refused"), Paths && Gate::CoerceValue(*Paths->Property, TEXT("Items"), Assign, Error));
	const Gate::FLeaf* Precache = Gate::FindLeaf(Meta, TEXT("PrecacheTable"));
	TestTrue(TEXT("a bool: ") + Error, Precache && Gate::CoerceValue(*Precache->Property, TEXT("no"), Assign, Error) && Assign.Import == TEXT("False"));
	TestFalse(TEXT("a bool takes true or false"), Precache && Gate::CoerceValue(*Precache->Property, TEXT("maybe"), Assign, Error));
	TestTrue(TEXT("a whole number: ") + Error, Kept && Gate::CoerceValue(*Kept->Property, TEXT("12"), Assign, Error) && Assign.Import == TEXT("12"));
	TestFalse(TEXT("a fraction is not a whole number"), Kept && Gate::CoerceValue(*Kept->Property, TEXT("1.5"), Assign, Error));
	const Gate::FLeaf* Tag = Gate::FindLeaf(Registry, TEXT("BaseGameplayTag"));
	TestFalse(TEXT("a tag the project does not have is refused"), Tag && Gate::CoerceValue(*Tag->Property, TEXT("HyperAI.No.Such.Tag.Here"), Assign, Error));
	TestTrue(TEXT("saying how to add it: ") + Error, Error.Contains(TEXT("AddTag")));
	const Gate::FLeaf* Struct = Gate::FindLeaf(Registry, TEXT("ItemStruct"));
	TestTrue(TEXT("a struct by its C++ name with the F: ") + Error, Struct && Gate::CoerceValue(*Struct->Property, TEXT("/Script/GameplayTags.FGameplayTagTableRow"), Assign, Error)
		&& Assign.Resolved && Assign.Resolved->GetPathName() == TagRow);
	TestNotNull(TEXT("a struct by its bare name"), Gate::FindStruct(TEXT("GameplayTagTableRow"), Error));
	TestNull(TEXT("no struct by a made-up name"), Gate::FindStruct(TEXT("/Script/Engine.NoSuchRowHere"), Error));
	TArray<Gate::FAssign> Assigns;
	TestFalse(TEXT("an unknown setting is refused"), Gate::ParseAssigns(*UDataRegistry::StaticClass(), TEXT("Explode=true"), Assigns, Error));
	TestTrue(TEXT("listing the settings: ") + Error, Error.Contains(TEXT("TimerUpdateFrequency")) && Error.Contains(TEXT("MaxNumberKept")));

	// Kinds.
	TestTrue(TEXT("the op kinds"), Gate::OpKinds() == TArray<FString>({TEXT("create"), TEXT("set"), TEXT("source.add"), TEXT("source.set"), TEXT("source.remove")}));
	for (const FString& Kind : Gate::SourceKinds()) TestNotNull(FString::Printf(TEXT("%s has an engine class"), *Kind), Gate::SourceClassOf(Kind));
	TestNull(TEXT("no class for a made-up kind"), Gate::SourceClassOf(TEXT("web_table")));

	// Inspect without an asset: every kind, with settings an agent can name.
	const FHyperAIDataRegistryInspectReport Listed = C::Inspect(FHyperAIDataRegistryInspectRequest());
	TestTrue(TEXT("inspect without an asset lists the kinds: ") + Listed.Diagnostic, Listed.bOk && Listed.Kinds.Num() == 5 && Listed.SourceKinds.Num() == 4);
	const FHyperAIDataRegistryKind* MetaKind = Listed.SourceKinds.FindByPredicate([](const FHyperAIDataRegistryKind& Kind) { return Kind.Kind == TEXT("data_table_meta"); });
	TestTrue(TEXT("with the meta source's search rules and table rules"), MetaKind && MetaKind->Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("AssetScanPaths:")); })
		&& MetaKind->Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("bPrecacheTable: true or false, default True")); }));
	const FHyperAIDataRegistryKind* Set = Listed.Kinds.FindByPredicate([](const FHyperAIDataRegistryKind& Kind) { return Kind.Kind == TEXT("set"); });
	TestTrue(TEXT("and the registry's settings for set"), Set && Set->Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("TimerUpdateFrequency:")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDataRegistryDryRunTest, "HyperAIStudio.NativeTools.DataRegistry.DryRun", HyperAIStudio::DataRegistryTests::Flags)

bool FHyperAIStudioDataRegistryDryRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DataRegistryTests;
	const FString Folder = TestFolder();
	UDataTable* Table = MakeTable(TestAsset(Folder, TEXT("DT_Items")), TagRow, {{TEXT("Sword"), TEXT("sharp")}});
	ON_SCOPE_EXIT { Drop(Table); };
	if (!TestNotNull(TEXT("the test table is made"), Table)) return false;
	const FString Asset = TestAsset(Folder, TEXT("DR_Dry"));
	const FString Type = TEXT("HyperAIDry") + Unique();
	const int32 UndoBefore = GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0;
	const FHyperAIDataRegistryPlanReport Done = Run(Asset, {Create(Type, TagRow), AddTable(TEXT("data_table"), Table->GetPathName()), Op(TEXT("set"), TEXT("MaxNumberKept=5"))}, true);
	TestTrue(TEXT("the dry run checks: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("checked"));
	TestEqual(TEXT("one source"), Done.Sources.Num(), 1);
	TestTrue(TEXT("naming the table"), Done.Sources.Num() == 1 && Done.Sources[0].Contains(Table->GetPathName()));
	TestEqual(TEXT("a note per op"), Done.Notes.Num(), 3);
	TestEqual(TEXT("the type"), Done.RegistryType, Type);
	TestNull(TEXT("a dry run makes nothing"), Find(Asset));
	TestFalse(TEXT("nothing on disk"), FPackageName::DoesPackageExist(Asset));
	TestEqual(TEXT("and no undo step"), GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0, UndoBefore);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDataRegistryApplyUndoTest, "HyperAIStudio.NativeTools.DataRegistry.ApplyAndUndo", HyperAIStudio::DataRegistryTests::Flags)

bool FHyperAIStudioDataRegistryApplyUndoTest::RunTest(const FString& Parameters)
{
	// Undo and redo call UDataRegistry::PostEditChangeProperty, which logs this error whenever the Data Registry subsystem has
	// no registries to look after (no Directories To Scan, as in this bench): the engine's own noise, not a failure here.
	AddExpectedMessagePlain(TEXT("subsystem not registered"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1);
	using namespace HyperAIStudio::DataRegistryTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor, its undo buffer, and no play session."));
		return false;
	}
	const FString Folder = TestFolder();
	UDataTable* Table = MakeTable(TestAsset(Folder, TEXT("DT_Items")), TagRow, {{TEXT("Sword"), TEXT("sharp")}, {TEXT("Shield"), TEXT("round")}});
	ON_SCOPE_EXIT { Drop(Table); };
	if (!TestNotNull(TEXT("the test table is made"), Table)) return false;
	const FString Asset = TestAsset(Folder, TEXT("DR_Items"));
	const FString Type = TEXT("HyperAIItems") + Unique();
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	const FHyperAIDataRegistryPlanReport Done = Run(Asset, {Create(Type, TagRow), AddTable(TEXT("data_table"), Table->GetPathName()),
		Op(TEXT("set"), TEXT("TimerUpdateFrequency=2; MaxNumberKept=7"))}, false);
	const int32 Steps = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - UndoBefore;
	ON_SCOPE_EXIT { UndoSince(UndoBefore); };
	if (!TestTrue(TEXT("the plan runs: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"))) return false;
	TestEqual(TEXT("in one undo step"), Steps, 1);
	TestTrue(TEXT("named for the plan"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("Data Registry Plan")));
	UDataRegistry* Made = Find(Asset);
	if (!TestNotNull(TEXT("the registry is made"), Made)) return false;
	TestEqual(TEXT("its type"), Made->GetRegistryType(), FName(*Type));
	TestTrue(TEXT("its item struct"), Made->GetItemStruct() && Made->GetItemStruct()->GetPathName() == TagRow);
	TestEqual(TEXT("one source"), Made->GetDataSources().Num(), 1);
	const UDataRegistrySource_DataTable* Source = Made->GetDataSources().Num() == 1 ? Cast<UDataRegistrySource_DataTable>(Made->GetDataSources()[0]) : nullptr;
	TestTrue(TEXT("a data table source reading the table"), Source && Source->SourceTable.ToSoftObjectPath() == FSoftObjectPath(Table));
	const TArray<Gate::FLeaf> Settings = Gate::Leaves(*UDataRegistry::StaticClass());
	const Gate::FLeaf* Kept = Gate::FindLeaf(Settings, TEXT("MaxNumberKept"));
	TestTrue(TEXT("the default cache policy is set"), Kept && Gate::LeafText(*Kept, *Made) == TEXT("7"));
	TestEqual(TEXT("the reply counts the items"), Done.ItemCount, 2);
	TestTrue(TEXT("and, outside the scanned folders, says how to add the folder"), Done.bInScannedDirectory
		|| Done.Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("Directories To Scan")); }));

	// Read back through inspect.
	FHyperAIDataRegistryInspectRequest Ask;
	Ask.Asset = Asset;
	const FHyperAIDataRegistryInspectReport Read = C::Inspect(Ask);
	TestTrue(TEXT("inspect reads it: ") + Read.Diagnostic, Read.bOk && Read.RegistryType == Type && Read.ItemCount == 2 && Read.Items.Contains(TEXT("Sword")));
	TestTrue(TEXT("its source and the table it reads"), Read.Sources.Num() == 1 && Read.Sources[0].Kind == TEXT("data_table") && Read.Sources[0].Tables.Num() == 1);
	TestTrue(TEXT("its timer and cache settings"), Read.Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("TimerUpdateFrequency:")) && Line.Contains(TEXT("now 2")); })
		&& Read.Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("MaxNumberKept:")) && Line.Contains(TEXT("now 7")); }));

	// Lookup returns the row, with a missing id reported on its own.
	const FHyperAIDataRegistryLookupReport Found = Look(Asset, {TEXT("Sword"), Type + TEXT(":Shield"), TEXT("Spoon")});
	TestTrue(TEXT("lookup answers: ") + Found.Diagnostic, Found.bOk && Found.Items.Num() == 3);
	const FHyperAIDataRegistryItem* Sword = ItemOf(Found, TEXT("Sword"));
	TestTrue(TEXT("the sword's fields, from the table"), Sword && Sword->bFound && HasField(Sword, TEXT("DevComment: sharp")) && Sword->Source.Contains(Table->GetPathName()));
	TestTrue(TEXT("an id with the type"), ItemOf(Found, TEXT("Shield")) && ItemOf(Found, TEXT("Shield"))->bFound && HasField(ItemOf(Found, TEXT("Shield")), TEXT("round")));
	TestTrue(TEXT("the spoon is missing, on its own"), Found.Missing.Num() == 1 && Found.Missing[0] == TEXT("Spoon") && ItemOf(Found, TEXT("Spoon")) && !ItemOf(Found, TEXT("Spoon"))->bFound);
	const FHyperAIDataRegistryLookupReport Json = Look(Asset, {TEXT("Sword")}, TEXT("json"));
	TestTrue(TEXT("or as JSON: ") + (Json.Items.Num() ? Json.Items[0].Json : FString()), Json.bOk && Json.Items.Num() == 1 && Json.Items[0].Json.Contains(TEXT("sharp")) && Json.Items[0].Json.StartsWith(TEXT("{")));
	FHyperAIDataRegistryLookupRequest ByType;
	ByType.RegistryType = Type;
	ByType.Items = {TEXT("Shield")};
	const FHyperAIDataRegistryLookupReport Typed = C::Lookup(ByType);
	TestTrue(TEXT("and by registry type: ") + Typed.Diagnostic, Typed.bOk && Typed.Items.Num() == 1 && Typed.Items[0].bFound);

	// One undo takes it all back; redo brings it again.
	if (!TestTrue(TEXT("undo"), GEditor->UndoTransaction())) return false;
	TestFalse(TEXT("undo removes the registry"), IsValid(Made));
	TestFalse(TEXT("leaving nothing to save"), Made->GetPackage()->IsDirty());
	if (!TestTrue(TEXT("redo"), GEditor->RedoTransaction())) return false;
	TestTrue(TEXT("redo brings it back with its source"), IsValid(Made) && Made->GetDataSources().Num() == 1 && Made->GetRegistryType() == FName(*Type));
	TestTrue(TEXT("and its items"), Look(Asset, {TEXT("Sword")}).Missing.IsEmpty());
	GEditor->UndoTransaction();
	// Undone, the path and the type are free to make again.
	const FHyperAIDataRegistryPlanReport Again = Run(Asset, {Create(Type, TagRow)}, false);
	TestTrue(TEXT("the same Asset and type can be made again after undo: ") + Again.Diagnostic, Again.bOk);
	TestTrue(TEXT("as a new registry"), Find(Asset) && IsValid(Find(Asset)) && Find(Asset) != Made);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDataRegistrySourceEditsTest, "HyperAIStudio.NativeTools.DataRegistry.SourceEdits", HyperAIStudio::DataRegistryTests::Flags)

bool FHyperAIStudioDataRegistrySourceEditsTest::RunTest(const FString& Parameters)
{
	// Undo and redo call UDataRegistry::PostEditChangeProperty, which logs this error whenever the Data Registry subsystem has
	// no registries to look after (no Directories To Scan, as in this bench): the engine's own noise, not a failure here.
	AddExpectedMessagePlain(TEXT("subsystem not registered"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1);
	using namespace HyperAIStudio::DataRegistryTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor, its undo buffer, and no play session."));
		return false;
	}
	const FString Folder = TestFolder();
	UDataTable* Base = MakeTable(TestAsset(Folder, TEXT("DT_Base")), TagRow, {{TEXT("Sword"), TEXT("base sword")}, {TEXT("Bow"), TEXT("base bow")}});
	UDataTable* Patch = MakeTable(TestAsset(Folder, TEXT("DT_Patch")), TagRow, {{TEXT("Sword"), TEXT("patched sword")}, {TEXT("Axe"), TEXT("patch axe")}});
	UDataTable* Other = MakeTable(TestAsset(Folder, TEXT("DT_Mirror")), MirrorRow, {});
	ON_SCOPE_EXIT { Drop(Base); Drop(Patch); Drop(Other); };
	if (!TestTrue(TEXT("the test tables are made"), Base && Patch && Other)) return false;
	const FString Asset = TestAsset(Folder, TEXT("DR_Edits"));
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	ON_SCOPE_EXIT { UndoSince(UndoBefore); };
	const FHyperAIDataRegistryPlanReport Made = Run(Asset, {Create(TEXT("HyperAIEdits") + Unique(), TagRow), AddTable(TEXT("data_table"), Base->GetPathName())}, false);
	if (!TestTrue(TEXT("the registry is made: ") + Made.Diagnostic, Made.bOk)) return false;
	UDataRegistry* Registry = Find(Asset);
	if (!TestNotNull(TEXT("and found"), Registry)) return false;

	// A second table placed first wins the names both hold; the first gets a setting.
	const int32 BeforeEdit = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	const FHyperAIDataRegistryPlanReport Edited = Run(Asset, {AddTable(TEXT("data_table"), Patch->GetPathName(), 0), AtIndex(TEXT("source.set"), 1, TEXT("PrecacheTable=false"))}, false);
	if (!TestTrue(TEXT("the edit runs: ") + Edited.Diagnostic, Edited.bOk)) return false;
	TestEqual(TEXT("in one undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - BeforeEdit, 1);
	TestEqual(TEXT("two sources"), Registry->GetDataSources().Num(), 2);
	const UDataRegistrySource_DataTable* First = Cast<UDataRegistrySource_DataTable>(Registry->GetDataSources()[0]);
	const UDataRegistrySource_DataTable* Second = Registry->GetDataSources().Num() > 1 ? Cast<UDataRegistrySource_DataTable>(Registry->GetDataSources()[1]) : nullptr;
	TestTrue(TEXT("the patch first"), First && First->SourceTable.ToSoftObjectPath() == FSoftObjectPath(Patch));
	TestTrue(TEXT("the base second, no longer precached"), Second && Second->SourceTable.ToSoftObjectPath() == FSoftObjectPath(Base) && !Second->TableRules.bPrecacheTable);
	TestEqual(TEXT("three names in all"), Edited.ItemCount, 3);
	const FHyperAIDataRegistryLookupReport Patched = Look(Asset, {TEXT("Sword"), TEXT("Bow"), TEXT("Axe")});
	const FHyperAIDataRegistryItem* Sword = ItemOf(Patched, TEXT("Sword"));
	TestTrue(TEXT("the sword comes from the patch"), HasField(Sword, TEXT("patched sword")) && Sword->Source.Contains(Patch->GetPathName()));
	TestTrue(TEXT("hiding the base's"), Sword && Sword->Shadowed.Num() == 1 && Sword->Shadowed[0].Contains(Base->GetPathName()));
	TestTrue(TEXT("the bow from the base, the axe from the patch"), HasField(ItemOf(Patched, TEXT("Bow")), TEXT("base bow")) && HasField(ItemOf(Patched, TEXT("Axe")), TEXT("patch axe")));

	// Removed, the base answers again.
	const FHyperAIDataRegistryPlanReport Removed = Run(Asset, {AtIndex(TEXT("source.remove"), 0)}, false);
	TestTrue(TEXT("source.remove runs: ") + Removed.Diagnostic, Removed.bOk && Registry->GetDataSources().Num() == 1);
	TestTrue(TEXT("and the sword is the base's again"), HasField(ItemOf(Look(Asset, {TEXT("Sword")}), TEXT("Sword")), TEXT("base sword")));
	TestTrue(TEXT("the axe is gone"), Look(Asset, {TEXT("Axe")}).Missing.Num() == 1);

	// A meta source over the folder finds both matching tables and skips the mirror table.
	const FHyperAIDataRegistryPlanReport Meta = Run(Asset, {AtIndex(TEXT("source.remove"), 0), AddMeta(Folder)}, false);
	if (!TestTrue(TEXT("the meta source is added: ") + Meta.Diagnostic, Meta.bOk)) return false;
	const UMetaDataRegistrySource_DataTable* Search = Registry->GetDataSources().Num() == 1 ? Cast<UMetaDataRegistrySource_DataTable>(Registry->GetDataSources()[0]) : nullptr;
	TestTrue(TEXT("searching the folder"), Search && Search->SearchRules.AssetScanPaths == TArray<FString>({Folder}) && Search->AssetUsage == EMetaDataRegistrySourceAssetUsage::SearchAssets);
	FHyperAIDataRegistryInspectRequest Ask;
	Ask.Asset = Asset;
	const FHyperAIDataRegistryInspectReport Read = C::Inspect(Ask);
	FString Tables;
	for (const FHyperAIDataRegistrySource& Each : Read.Sources) Tables += FString::Join(Each.Tables, TEXT(" | "));
	TestTrue(TEXT("inspect lists the two tables it found: ") + Tables, Read.Sources.Num() == 1 && Read.Sources[0].Kind == TEXT("data_table_meta") && Read.Sources[0].Tables.Num() == 2);
	TestTrue(TEXT("and says it skipped the other"), Read.Warnings.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("skips 1 table")); }));
	TestEqual(TEXT("three names through the meta source"), Read.ItemCount, 3);

	// Undo walks back each plan in turn.
	GEditor->UndoTransaction();
	TestTrue(TEXT("undoing the meta plan puts the base source back"), Registry->GetDataSources().Num() == 1 && Cast<UDataRegistrySource_DataTable>(Registry->GetDataSources()[0]) != nullptr);
	GEditor->UndoTransaction();
	TestEqual(TEXT("undoing the removal puts the patch back"), Registry->GetDataSources().Num(), 2);
	GEditor->UndoTransaction();
	TestTrue(TEXT("undoing the edit leaves the base alone and precached"), Registry->GetDataSources().Num() == 1
		&& Cast<UDataRegistrySource_DataTable>(Registry->GetDataSources()[0]) && Cast<UDataRegistrySource_DataTable>(Registry->GetDataSources()[0])->TableRules.bPrecacheTable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDataRegistryRefusalsTest, "HyperAIStudio.NativeTools.DataRegistry.Refusals", HyperAIStudio::DataRegistryTests::Flags)

bool FHyperAIStudioDataRegistryRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DataRegistryTests;
	const FString Folder = TestFolder();
	UDataTable* Items = MakeTable(TestAsset(Folder, TEXT("DT_Items")), TagRow, {{TEXT("Sword"), TEXT("sharp")}});
	UDataTable* Mirror = MakeTable(TestAsset(Folder, TEXT("DT_Mirror")), MirrorRow, {});
	UCurveTable* Curves = MakeCurves(TestAsset(Folder, TEXT("CT_Damage")));
	ON_SCOPE_EXIT { Drop(Items); Drop(Mirror); Drop(Curves); };
	if (!TestTrue(TEXT("the test tables are made"), Items && Mirror && Curves)) return false;
	const FString Asset = TestAsset(Folder, TEXT("DR_Refused"));
	const FString Type = TEXT("HyperAIRefused") + Unique();
	const int32 UndoBefore = GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0;
	TestTrue(TEXT("Engine content is refused"), Refused(Run(TEXT("/Engine/DR_Items"), {Create(Type, TagRow)}, false), TEXT("not project content")));
	TestTrue(TEXT("so is plugin content"), Refused(Run(TEXT("/HyperAISb3f9f07c5a33V1/DR_Items"), {Create(Type, TagRow)}, false), TEXT("not project content")));
	TestTrue(TEXT("an unknown kind is refused, with the kinds"), Refused(Run(Asset, {Create(Type, TagRow), Op(TEXT("source.explode"))}, false), TEXT("source.remove")));
	TestTrue(TEXT("create comes first"), Refused(Run(Asset, {Op(TEXT("set"), TEXT("MaxNumberKept=1")), Create(Type, TagRow)}, false), TEXT("create comes first")));
	TestTrue(TEXT("a missing registry is refused"), Refused(Run(Asset, {Op(TEXT("set"), TEXT("MaxNumberKept=1"))}, false), TEXT("start the plan with create")));
	TestTrue(TEXT("a made-up struct is refused"), Refused(Run(Asset, {Create(Type, TEXT("/Script/Engine.NoSuchRowHere"))}, false), TEXT("is not a struct")));
	TestTrue(TEXT("create needs a type"), Refused(Run(Asset, {Create(TEXT(""), TagRow)}, false), TEXT("Give RegistryType")));
	TestTrue(TEXT("a table whose rows do not match is refused"), Refused(Run(Asset, {Create(Type, TagRow), AddTable(TEXT("data_table"), Mirror->GetPathName())}, false),
		TEXT("rows are MirrorTableRow")));
	TestTrue(TEXT("a curve table for items that are not curves is refused"), Refused(Run(Asset, {Create(Type, TagRow), AddTable(TEXT("curve_table"), Curves->GetPathName())}, false),
		TEXT("curve items only")));
	TestTrue(TEXT("a missing table is refused"), Refused(Run(Asset, {Create(Type, TagRow), AddTable(TEXT("data_table"), TEXT("/Game/Nowhere/DT_Missing"))}, false), TEXT("is not a data table")));
	TestTrue(TEXT("a data table source needs its table"), Refused(Run(Asset, {Create(Type, TagRow), AddTable(TEXT("data_table"), TEXT(""))}, false), TEXT("needs Table")));
	FHyperAIDataRegistryOp NoPaths = AddMeta(Folder);
	NoPaths.Paths.Reset();
	TestTrue(TEXT("a meta source needs paths"), Refused(Run(Asset, {Create(Type, TagRow), NoPaths}, false), TEXT("needs Paths")));
	TestTrue(TEXT("an unknown source kind is refused, with the kinds"), Refused(Run(Asset, {Create(Type, TagRow), AddTable(TEXT("web_table"), Items->GetPathName())}, false), TEXT("curve_table_meta")));
	TestTrue(TEXT("an unknown setting is refused, with the settings"), Refused(Run(Asset, {Create(Type, TagRow), Op(TEXT("set"), TEXT("Explode=true"))}, false), TEXT("TimerUpdateFrequency")));
	TestTrue(TEXT("a bad value is refused"), Refused(Run(Asset, {Create(Type, TagRow), AddTable(TEXT("data_table"), Items->GetPathName(), -1, TEXT("PrecacheTable=maybe"))}, false),
		TEXT("true or false")));
	TestTrue(TEXT("an index past the sources is refused"), Refused(Run(Asset, {Create(Type, TagRow), AtIndex(TEXT("source.remove"), 0)}, false), TEXT("is not a source")));
	TestTrue(TEXT("a table on a meta source is refused"), Refused(Run(Asset, {Create(Type, TagRow), AddMeta(Folder), [&]() { FHyperAIDataRegistryOp Set = AtIndex(TEXT("source.set"), 0);
		Set.Table = Items->GetPathName(); return Set; }()}, false), TEXT("takes Paths")));
	TestTrue(TEXT("refused plans make nothing"), !Find(Asset) && !FPackageName::DoesPackageExist(Asset));
	TestEqual(TEXT("and leave no undo step"), GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0, UndoBefore);

	// A type another registry has is refused.
	const FString Taken = TestAsset(Folder, TEXT("DR_Taken"));
	const FHyperAIDataRegistryPlanReport First = Run(Taken, {Create(Type, TagRow)}, false);
	ON_SCOPE_EXIT { UndoSince(UndoBefore); };
	TestTrue(TEXT("the first registry is made: ") + First.Diagnostic, First.bOk);
	TestTrue(TEXT("a second with its type is refused"), Refused(Run(Asset, {Create(Type, TagRow)}, false), TEXT("already")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDataRegistryLookupTest, "HyperAIStudio.NativeTools.DataRegistry.Lookup", HyperAIStudio::DataRegistryTests::Flags)

bool FHyperAIStudioDataRegistryLookupTest::RunTest(const FString& Parameters)
{
	// Undo and redo call UDataRegistry::PostEditChangeProperty, which logs this error whenever the Data Registry subsystem has
	// no registries to look after (no Directories To Scan, as in this bench): the engine's own noise, not a failure here.
	AddExpectedMessagePlain(TEXT("subsystem not registered"), ELogVerbosity::Error, EAutomationExpectedMessageFlags::Contains, -1);
	using namespace HyperAIStudio::DataRegistryTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor, its undo buffer, and no play session."));
		return false;
	}
	const FString Folder = TestFolder();
	UCurveTable* Curves = MakeCurves(TestAsset(Folder, TEXT("CT_Damage")));
	ON_SCOPE_EXIT { Drop(Curves); };
	if (!TestNotNull(TEXT("the test curve table is made"), Curves)) return false;
	const FString Asset = TestAsset(Folder, TEXT("DR_Curves"));
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	ON_SCOPE_EXIT { UndoSince(UndoBefore); };
	const FHyperAIDataRegistryPlanReport Done = Run(Asset, {Create(TEXT("HyperAICurves") + Unique(), TEXT("/Script/Engine.RichCurve")), AddTable(TEXT("curve_table"), Curves->GetPathName())}, false);
	if (!TestTrue(TEXT("a curve registry is made: ") + Done.Diagnostic, Done.bOk && Done.ItemCount == 1)) return false;
	const FHyperAIDataRegistryLookupReport AtOne = Look(Asset, {TEXT("Damage")});
	TestTrue(TEXT("the curve is found: ") + AtOne.Diagnostic, AtOne.bOk && AtOne.Items.Num() == 1 && AtOne.Items[0].bFound && AtOne.Items[0].bCurve);
	TestTrue(FString::Printf(TEXT("worth 10 at level 1 (%g)"), AtOne.Items.Num() ? AtOne.Items[0].Value : -1.f), AtOne.Items.Num() == 1 && FMath::IsNearlyEqual(AtOne.Items[0].Value, 10.f, 0.01f));
	TestTrue(TEXT("with its keys listed"), AtOne.Items.Num() == 1 && HasField(&AtOne.Items[0], TEXT("Keys: 2")));
	const FHyperAIDataRegistryLookupReport AtFive = Look(Asset, {TEXT("Damage"), TEXT("Healing")}, TEXT(""), 5.f);
	TestTrue(TEXT("50 at level 5"), AtFive.Items.Num() == 2 && FMath::IsNearlyEqual(AtFive.Items[0].Value, 50.f, 0.01f));
	TestTrue(TEXT("a missing curve is reported on its own, the call still succeeds"), AtFive.bOk && AtFive.Missing == TArray<FString>({TEXT("Healing")})
		&& AtFive.Items[1].Diagnostic.Contains(TEXT("Damage")));
	TestTrue(TEXT("an id of another registry type is not found here"), Look(Asset, {TEXT("Weapons:Damage")}).Missing.Num() == 1);
	TestFalse(TEXT("a format it lacks is refused"), Look(Asset, {TEXT("Damage")}, TEXT("xml")).bOk);
	TestFalse(TEXT("no items is refused"), Look(Asset, {}).bOk);
	TestFalse(TEXT("a registry that is not there is refused"), Look(TEXT("/Game/Nowhere/DR_Missing"), {TEXT("Damage")}).bOk);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioDataRegistrySkillTest, "HyperAIStudio.NativeTools.DataRegistry.Skill", HyperAIStudio::DataRegistryTests::Flags)

bool FHyperAIStudioDataRegistrySkillTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::DataRegistryTests;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	FString Skill;
	if (!TestTrue(TEXT("the data registry skill ships with the plugin"), Plugin.IsValid()
		&& FFileHelper::LoadFileToString(Skill, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Skills/unreal-data-registry/SKILL.md")))))
	{
		return false;
	}
	TestTrue(TEXT("it is a skill"), Skill.StartsWith(TEXT("---")) && Skill.Contains(TEXT("name: unreal-data-registry")));
	for (const FString& Name : C::GetToolNames())
	{
		TestTrue(FString::Printf(TEXT("it names %s"), *Name), Skill.Contains(Name));
	}
	for (const FString& Kind : Gate::OpKinds())
	{
		TestTrue(FString::Printf(TEXT("it explains %s"), *Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), *Kind)));
	}
	for (const FString& Kind : Gate::SourceKinds())
	{
		TestTrue(FString::Printf(TEXT("and the source kind %s"), *Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), *Kind)));
	}
	for (const TCHAR* Word : {TEXT("HyperAIStudioDataRegistry.HyperAIStudioDataRegistryToolset"), TEXT("Directories To Scan"), TEXT("GetCachedItem"), TEXT("AcquireItem"),
		TEXT("FDataRegistryId"), TEXT("DataRegistryToolset")})
	{
		TestTrue(FString::Printf(TEXT("and %s"), Word), Skill.Contains(Word));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
