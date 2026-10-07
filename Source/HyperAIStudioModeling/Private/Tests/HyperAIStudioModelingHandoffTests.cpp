// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "BaseTools/SingleClickTool.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "EditorModeManager.h"
#include "EditorModes.h"
#include "Engine/Level.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioModelingGate.h"
#include "HyperAIStudioModelingToolset.h"
#include "HyperAIStudioSettings.h"
#include "InteractiveToolManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "StaticMeshCompiler.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Tools/Modes.h"
#include "Tools/UEdMode.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"

namespace HyperAIStudio::ModelingHandoffTests
{
	using C = FHyperAIStudioModelingContracts;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/**
	 * A hand-off edits the open level and switches its mode, so it runs only in unattended test runs, never in an editor
	 * someone is using, and only with a level editor and Modeling Mode to hand to.
	 */
	bool Ready(FAutomationTestBase& Test)
	{
		FEditorModeInfo Info;
		const UAssetEditorSubsystem* Editors = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
		if (!FApp::IsUnattended())
		{
			Test.AddInfo(TEXT("Runs only in unattended test runs, so it never changes the level or mode you have open."));
			return false;
		}
		if (!GEditor || !GEditor->GetEditorWorldContext().World() || GEditor->GetLevelViewportClients().IsEmpty() || !FSlateApplication::IsInitialized()
			|| !Editors || !Editors->FindEditorModeInfo(FName(C::ModelingModeId), Info))
		{
			Test.AddInfo(TEXT("No level editor with Modeling Mode here; skipped."));
			return false;
		}
		return true;
	}

	/** Lets the unattended run through, for the test's length. */
	struct FHooks : FNoncopyable
	{
		explicit FHooks(const FName ModeId = NAME_None) { C::SetHandoffTestHooks(true, ModeId); }
		~FHooks() { C::SetHandoffTestHooks(false); }
	};

	/**
	 * The mode's toolbox tabs open and close, and Slate saves the layout 5 s later (TabManager.cpp:1103-1124 in 5.8) into
	 * a file every project on this machine reads; saving comes back on only after those saves have been refused.
	 */
	struct FLayoutGuard : FNoncopyable
	{
		bool bCould = true;
		FLayoutGuard()
		{
			bCould = FGlobalTabmanager::Get()->CanSavePersistentLayouts();
			FGlobalTabmanager::Get()->SetCanSavePersistentLayouts(false);
		}
		~FLayoutGuard()
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Could = bCould](float)
			{
				FGlobalTabmanager::Get()->SetCanSavePersistentLayouts(Could);
				return false;
			}), 10.f);
		}
	};

	/** Approval on or off for the test's length. */
	struct FApproval : FNoncopyable
	{
		bool bWas = false;
		explicit FApproval(const bool bOn)
		{
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			bWas = Settings->bRequireApprovalForAgentEdits;
			Settings->bRequireApprovalForAgentEdits = bOn;
		}
		~FApproval() { GetMutableDefault<UHyperAIStudioSettings>()->bRequireApprovalForAgentEdits = bWas; }
	};

	FName ModeId()
	{
		return FName(C::ModelingModeId);
	}

	/** Modeling Mode left at once (DeactivateMode would wait for a tick), and the editor's default mode back. */
	void LeaveMode()
	{
		GLevelEditorModeTools().DestroyMode(ModeId());
		GLevelEditorModeTools().ActivateDefaultMode();
		GEditor->SelectNone(/*bNoteSelectionChange=*/true, /*bDeselectBSPSurfs=*/true, /*WarnAboutManyActors=*/false);
	}

	/** Steps Edit > Undo can take back; undone ones an earlier test left for redo go when the next step is made. */
	int32 UndoLength()
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount() : 0;
	}

	AActor* FindByLabel(const FString& Label)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel() == Label) return *It;
		}
		return nullptr;
	}

	FString TestFolder()
	{
		return TEXT("/Game/__HyperAIStudioTests/Handoff_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	}

	FString TestLabel()
	{
		return TEXT("HyperAI_TestHandoff_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	}

	void DeleteFolder(const FString& Folder)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FAssetData> Assets;
		Registry.GetAssetsByPath(FName(*Folder), Assets, /*bRecursive=*/true);
		TArray<UObject*> Objects;
		for (const FAssetData& Asset : Assets)
		{
			if (UObject* Object = Asset.GetAsset()) Objects.Add(Object);
		}
		if (!Objects.IsEmpty()) ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
	}

	/** A 100 cm box written as a static mesh under Folder, with approval off. */
	UStaticMesh* WriteBox(const FString& Folder, FString& OutError)
	{
		FApproval Off(false);
		FHyperAIModelOp Box;
		Box.Kind = TEXT("add.box");
		Box.Mesh = TEXT("Box");
		Box.Size = TEXT("100");
		FHyperAIModelOp Write;
		Write.Kind = TEXT("write_asset");
		Write.Mesh = TEXT("Box");
		Write.Asset = Folder + TEXT("/SM_Handoff");
		FHyperAIModelPlanRequest Request;
		Request.Operations = {Box, Write};
		const FHyperAIModelPlanReport Report = C::ApplyPlan(Request);
		OutError = Report.Diagnostic;
		return Report.bOk ? LoadObject<UStaticMesh>(nullptr, *(Write.Asset + TEXT(".SM_Handoff")), nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
	}

	FHyperAIModelHandoffRequest Request(const FString& Asset, const FString& Label = FString(), const FString& Location = FString())
	{
		FHyperAIModelHandoffRequest Out;
		Out.Asset = Asset;
		Out.Label = Label;
		Out.Location = Location;
		// The bench's tabs are never moved by a test.
		Out.bLeaveTabsAlone = true;
		return Out;
	}

	bool Refused(const FHyperAIModelHandoffReport& Report, const TCHAR* Status, const TCHAR* Words)
	{
		return !Report.bOk && Report.Status == Status && Report.Diagnostic.Contains(Words);
	}

	/** Modeling Mode's palette setting, set for the test's length by name as the hand-off reads it; invalid without the plugin. */
	struct FLegacyPalette : FNoncopyable
	{
		UObject* Settings = nullptr;
		const FBoolProperty* Property = nullptr;
		bool bWas = false;
		explicit FLegacyPalette(const bool bOn)
		{
			const UClass* Class = FindObject<UClass>(nullptr, C::ModelingPaletteSettingsClass);
			Property = Class ? FindFProperty<FBoolProperty>(Class, TEXT("bUseLegacyModelingPalette")) : nullptr;
			Settings = Class ? Class->GetDefaultObject() : nullptr;
			if (!IsValid()) return;
			bWas = Property->GetPropertyValue_InContainer(Settings);
			Set(bOn);
		}
		~FLegacyPalette()
		{
			if (IsValid()) Set(bWas);
		}
		bool IsValid() const { return Property && Settings; }
		void Set(const bool bOn) { Property->SetPropertyValue_InContainer(Settings, bOn); }
	};

	/** The Completed or Failed entry an approved hand-off recorded; unset when it recorded none. */
	TOptional<FHyperAIStudioActivityEntry> Outcome(const FString& OperationId)
	{
		const TArray<FHyperAIStudioActivityEntry> Entries = FHyperAIStudioAgentActivityLog::GetSnapshot();
		for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
		{
			const FHyperAIStudioActivityEntry& Entry = Entries[Index];
			if (Entry.OperationId == OperationId && (Entry.Kind == EHyperAIStudioActivityKind::Completed || Entry.Kind == EHyperAIStudioActivityKind::Failed)) return Entry;
		}
		return {};
	}

	/** A copy of Mesh named Name under Folder, in memory only (never saved), listed in the registry so DeleteFolder finds it. */
	UStaticMesh* CopyAs(UStaticMesh& Mesh, const FString& Folder, const FString& Name)
	{
		UStaticMesh* Copy = DuplicateObject<UStaticMesh>(&Mesh, CreatePackage(*(Folder + TEXT("/") + Name)), FName(*Name));
		if (!Copy) return nullptr;
		Copy->SetFlags(RF_Public | RF_Standalone);
		// Built as the content browser's Duplicate builds it: a duplicate has no render data of its own.
		Copy->PostEditChange();
		const TArray<UStaticMesh*> Compile = {Copy};
		FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
		FAssetRegistryModule::AssetCreated(Copy);
		return Copy;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingHandoffRefusalsTest, "HyperAIStudio.NativeTools.Modeling.Handoff.Refusals", HyperAIStudio::ModelingHandoffTests::Flags)

bool FHyperAIStudioModelingHandoffRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingHandoffTests;
	if (!Ready(*this)) return true;
	const FString Folder = TestFolder();
	FString Error;
	UStaticMesh* Mesh = WriteBox(Folder, Error);
	if (!TestNotNull(TEXT("the box to hand over is written: ") + Error, Mesh))
	{
		DeleteFolder(Folder);
		return false;
	}
	ON_SCOPE_EXIT
	{
		DeleteFolder(Folder);
	};
	const FString Asset = Mesh->GetPackage()->GetName();
	const FString Label = TestLabel();
	GEditor->SelectNone(true, true, false);
	const int32 UndoBefore = UndoLength();
	auto ChangedNothing = [&](const TCHAR* Case)
	{
		TestEqual(FString::Printf(TEXT("%s: nothing in the undo history"), Case), UndoLength(), UndoBefore);
		TestEqual(FString::Printf(TEXT("%s: nothing selected"), Case), GEditor->GetSelectedActorCount(), 0);
		TestFalse(FString::Printf(TEXT("%s: Modeling Mode not opened"), Case), GLevelEditorModeTools().IsModeActive(ModeId()));
		TestNull(FString::Printf(TEXT("%s: nothing put in the level"), Case), FindByLabel(Label));
	};

	const FHyperAIModelHandoffReport Unattended = C::Handoff(Request(Asset, Label));
	TestTrue(TEXT("an unattended editor is refused, nobody being at it: ") + Unattended.Diagnostic, Refused(Unattended, TEXT("unavailable"), TEXT("unattended")));
	ChangedNothing(TEXT("unattended"));
	{
		FHooks Hooks;
		FHyperAIModelHandoffRequest Both = Request(Asset);
		Both.Actor = TEXT("Anything");
		TestTrue(TEXT("Asset and Actor together are refused"), Refused(C::Handoff(Both), TEXT("invalid_request"), TEXT("Give Asset")));
		TestTrue(TEXT("and neither"), Refused(C::Handoff(Request(FString())), TEXT("invalid_request"), TEXT("Give Asset")));
		TestTrue(TEXT("an engine mesh is refused"), Refused(C::Handoff(Request(TEXT("/Engine/BasicShapes/Cube"))), TEXT("invalid_request"), TEXT("not under /Game")));
		TestTrue(TEXT("a missing asset is refused"), Refused(C::Handoff(Request(Folder + TEXT("/SM_Nope"))), TEXT("invalid_request"), TEXT("No asset at")));
		const FHyperAIModelHandoffReport Material = C::Handoff(Request(C::VertexColorMaterial));
		TestTrue(TEXT("a material is refused: ") + Material.Diagnostic, Refused(Material, TEXT("invalid_request"), TEXT("not a static mesh")));
		FHyperAIModelHandoffRequest Hammer = Request(Asset, Label);
		Hammer.Tool = TEXT("Hammer");
		TestTrue(TEXT("a tool not on the list is refused, naming the list"), Refused(C::Handoff(Hammer), TEXT("invalid_request"), TEXT("is not one of PolyEdit")));
		TestTrue(TEXT("a Location of two numbers is refused"), Refused(C::Handoff(Request(Asset, Label, TEXT("1,2"))), TEXT("invalid_request"), TEXT("is not x,y,z")));
		TestTrue(TEXT("a Location far out is refused"), Refused(C::Handoff(Request(Asset, Label, TEXT("0,0,9e9"))), TEXT("invalid_request"), TEXT("20 km")));
		TestTrue(TEXT("a Label with a space is refused"), Refused(C::Handoff(Request(Asset, TEXT("Two words"))), TEXT("invalid_request"), TEXT("is not 1 to 64")));
		FHyperAIModelHandoffRequest Moved = Request(FString(), Label);
		Moved.Actor = TEXT("Anything");
		TestTrue(TEXT("an Actor with a Label is refused"), Refused(C::Handoff(Moved), TEXT("invalid_request"), TEXT("Location and Label put a new actor")));
		FHyperAIModelHandoffRequest Nobody = Request(FString());
		Nobody.Actor = TEXT("NoSuchActor_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		TestTrue(TEXT("an actor not in the level is refused"), Refused(C::Handoff(Nobody), TEXT("invalid_request"), TEXT("No actor in the open level")));
		// A pasted blob or a malformed path is refused before the registry or a lookup makes an FName of it: an FName past
		// 1023 characters, or a load's package with a double slash, stops the editor. A dry run reads them too.
		const FString Blob = TEXT("/Game/") + FString::ChrN(1100, TEXT('a'));
		FHyperAIModelHandoffRequest LongAsset = Request(Blob);
		LongAsset.bDryRun = true;
		TestTrue(TEXT("an Asset past an FName's length is refused"), Refused(C::Handoff(LongAsset), TEXT("invalid_request"), TEXT("characters long")));
		TestTrue(TEXT("an Asset with a double slash is refused"), Refused(C::Handoff(Request(TEXT("/Game//SM_Nope"))), TEXT("invalid_request"), TEXT("double slash")));
		FHyperAIModelHandoffRequest LongActor = Request(FString());
		LongActor.Actor = Blob;
		LongActor.bDryRun = true;
		TestTrue(TEXT("an Actor past an FName's length is refused"), Refused(C::Handoff(LongActor), TEXT("invalid_request"), TEXT("characters long")));
		FHyperAIModelHandoffRequest SlashActor = Request(FString());
		SlashActor.Actor = TEXT("/Game//Map.Map:PersistentLevel.Nobody");
		TestTrue(TEXT("an Actor path with a double slash is looked up by label only"), Refused(C::Handoff(SlashActor), TEXT("invalid_request"), TEXT("No actor in the open level")));
		// Play In Editor running: refused before the request is read.
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!GEditor->PlayWorld)
		{
			GEditor->PlayWorld = World;
			const FHyperAIModelHandoffReport Playing = C::Handoff(Request(Asset, Label));
			GEditor->PlayWorld = nullptr;
			TestTrue(TEXT("a running PIE session is refused: ") + Playing.Diagnostic, Refused(Playing, TEXT("invalid_request"), TEXT("Play in Editor")));
		}
		// A tool open in another mode: opening Modeling Mode would exit that mode, which cancels the tool. A click tool in
		// the Selection mode stands in for a half-drawn spline or a paint stroke; starting it leaves no undo step here.
		UEdMode* Selection = GLevelEditorModeTools().GetActiveScriptableMode(FBuiltinEditorModes::EM_Default);
		UEditorInteractiveToolsContext* Context = Selection ? Selection->GetInteractiveToolsContext(EToolsContextScope::EdMode) : nullptr;
		if (Context && !Context->HasActiveTool())
		{
			UInteractiveToolManager* Manager = Context->ToolManager;
			const EToolChangeTrackingMode Tracking = Manager->GetChangeTrackingMode();
			Manager->ConfigureChangeTrackingMode(EToolChangeTrackingMode::NoChangeTracking);
			Manager->RegisterToolType(TEXT("HyperAITestClickTool"), NewObject<USingleClickToolBuilder>(Context));
			Context->UInteractiveToolsContext::StartTool(EToolSide::Mouse, TEXT("HyperAITestClickTool"));
			const bool bOpen = Context->HasActiveTool();
			FHyperAIModelHandoffRequest Dry = Request(Asset, Label);
			Dry.bDryRun = true;
			const FHyperAIModelHandoffReport Busy = C::Handoff(Dry);
			Manager->DeactivateTool(EToolSide::Mouse, EToolShutdownType::Cancel);
			Manager->UnregisterToolType(TEXT("HyperAITestClickTool"));
			Manager->ConfigureChangeTrackingMode(Tracking);
			TestTrue(TEXT("the stand-in tool is open in the Selection mode"), bOpen);
			TestTrue(TEXT("a tool open in another mode is refused, naming the mode and the tool: ") + Busy.Diagnostic,
				Refused(Busy, TEXT("unavailable"), TEXT("The Selection mode has a tool open (HyperAITestClickTool)")));
		}
		else
		{
			AddInfo(TEXT("The Selection mode is not active with a free tools context; the other-mode tool case is skipped."));
		}
		ChangedNothing(TEXT("refused requests"));
	}
	{
		// As without the plugin: no mode by that id is registered.
		FHooks Hooks(TEXT("EM_HyperAINoSuchMode"));
		const FHyperAIModelHandoffReport Missing = C::Handoff(Request(Asset, Label));
		TestTrue(TEXT("with no Modeling Mode registered it is unavailable: ") + Missing.Diagnostic, Refused(Missing, TEXT("unavailable"), TEXT("Modeling Mode is not available")));
		ChangedNothing(TEXT("no mode"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingHandoffDryRunTest, "HyperAIStudio.NativeTools.Modeling.Handoff.DryRunChangesNothing", HyperAIStudio::ModelingHandoffTests::Flags)

bool FHyperAIStudioModelingHandoffDryRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingHandoffTests;
	if (!Ready(*this)) return true;
	const FString Folder = TestFolder();
	FString Error;
	UStaticMesh* Mesh = WriteBox(Folder, Error);
	if (!TestNotNull(TEXT("the box to hand over is written: ") + Error, Mesh))
	{
		DeleteFolder(Folder);
		return false;
	}
	ON_SCOPE_EXIT
	{
		DeleteFolder(Folder);
	};
	FHooks Hooks;
	GEditor->SelectNone(true, true, false);
	const int32 UndoBefore = UndoLength();
	const FString Label = TestLabel();
	FHyperAIModelHandoffRequest Dry = Request(Mesh->GetPackage()->GetName(), Label, TEXT("0,0,20000"));
	Dry.bDryRun = true;
	const FHyperAIModelHandoffReport Checked = C::Handoff(Dry);
	TestTrue(TEXT("a dry run is checked: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked") && Checked.bPlaced);
	TestTrue(TEXT("and says what it would do: ") + Checked.Diagnostic, Checked.Diagnostic.Contains(TEXT("Would put")) && Checked.Diagnostic.Contains(TEXT("Nothing was changed")));
	TestEqual(TEXT("where it would stand"), Checked.Location, FString(TEXT("0,0,20000")));
	TestEqual(TEXT("with the label asked for"), Checked.ActorLabel, Label);
	TestEqual(TEXT("and the mesh's fingerprint"), Checked.Fingerprint, HyperAIStudio::Modeling::Gate::Fingerprint(*Mesh).Hash);
	TestNull(TEXT("nothing is put in the level"), FindByLabel(Label));
	TestEqual(TEXT("nothing goes in the undo history"), UndoLength(), UndoBefore);
	TestEqual(TEXT("nothing is selected"), GEditor->GetSelectedActorCount(), 0);
	TestFalse(TEXT("and Modeling Mode is not opened"), GLevelEditorModeTools().IsModeActive(ModeId()));

	// The tool hint names the palette the person's Modeling Mode shows: the ToolkitBuilder one by default, the legacy
	// tabs with their short button names when the setting asks for them.
	{
		FLegacyPalette Palette(false);
		if (Palette.IsValid())
		{
			FHyperAIModelHandoffRequest Hinted = Dry;
			Hinted.Tool = TEXT("PolyEdit");
			TestEqual(TEXT("PolyEdit on the default palette"), C::Handoff(Hinted).ToolHint, FString(TEXT("Model > PolyGroup Edit")));
			Hinted.Tool = TEXT("Remesh");
			TestEqual(TEXT("Remesh on the default palette"), C::Handoff(Hinted).ToolHint, FString(TEXT("Mesh > Remesh")));
			Hinted.Tool = TEXT("PolyGroups");
			TestEqual(TEXT("PolyGroups on the default palette"), C::Handoff(Hinted).ToolHint, FString(TEXT("Attribs > Generate PolyGroups")));
			Palette.Set(true);
			Hinted.Tool = TEXT("PolyEdit");
			TestEqual(TEXT("PolyEdit on the legacy palette"), C::Handoff(Hinted).ToolHint, FString(TEXT("PolyModel > PolyEd")));
			Hinted.Tool = TEXT("Simplify");
			TestEqual(TEXT("Simplify on the legacy palette"), C::Handoff(Hinted).ToolHint, FString(TEXT("MeshOps > Simplfy")));
		}
		else
		{
			AddInfo(TEXT("Modeling Mode's palette setting is not loaded; the palette names are not checked."));
		}
	}

	// An asset on disk the registry has not scanned yet (as during the editor's startup scan) is still found: a copy of
	// the saved box's file, which nothing has loaded or listed.
	const FString Unscanned = Folder + TEXT("/Unscanned/SM_Handoff");
	const FString CopyFile = FPackageName::LongPackageNameToFilename(Unscanned, FPackageName::GetAssetPackageExtension());
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	const bool bCopied = IFileManager::Get().Copy(*CopyFile, *FPackageName::LongPackageNameToFilename(Mesh->GetPackage()->GetName(), FPackageName::GetAssetPackageExtension()))
		== COPY_OK;
	ON_SCOPE_EXIT
	{
		// Let go and its file deleted here, never listed: a force delete of this copy beside the box it was copied from
		// trips the editor's referencer cross-check, which then switches to its legacy search for the whole session.
		if (UPackage* Loaded = FindPackage(nullptr, *Unscanned))
		{
			TArray<UStaticMesh*> Compiling;
			ForEachObjectWithPackage(Loaded, [&Compiling](UObject* Object)
			{
				if (UStaticMesh* Copy = Cast<UStaticMesh>(Object)) Compiling.Add(Copy);
				return true;
			});
			FStaticMeshCompilingManager::Get().FinishCompilation(Compiling);
			ResetLoaders(Loaded);
			ForEachObjectWithPackage(Loaded, [](UObject* Object)
			{
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->MarkAsGarbage();
				return true;
			});
			Loaded->MarkAsGarbage();
		}
		if (bCopied) IFileManager::Get().Delete(*CopyFile, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
	};
	if (TestTrue(TEXT("the saved box's file is copied"), bCopied))
	{
		TestFalse(TEXT("the registry does not list the copy"), Registry.GetAssetByObjectPath(FSoftObjectPath(Unscanned + TEXT(".SM_Handoff"))).IsValid());
		FHyperAIModelHandoffRequest Fresh = Request(Unscanned, TestLabel(), TEXT("0,0,20000"));
		Fresh.bDryRun = true;
		const FHyperAIModelHandoffReport Found = C::Handoff(Fresh);
		TestTrue(TEXT("an asset on disk but not yet scanned is found and checked: ") + Found.Diagnostic, Found.bOk && Found.Status == TEXT("checked")
			&& Found.Asset == Unscanned + TEXT(".SM_Handoff"));
	}

	// A hi-res source is what Modeling Mode edits, so the reply counts it, by the count saved with it: one of millions
	// (as recorded here) is reported heavy without being unpacked.
	Mesh->CreateHiResMeshDescription(*Mesh->GetMeshDescription(0));
	Mesh->CommitHiResMeshDescription();
	Mesh->ClearHiResMeshDescription();
	const int32 Millions = C::MaxFingerprintTriangles + 1;
	Mesh->GetHiResSourceModel().CacheMeshDescriptionTrianglesCount = Millions;
	const FHyperAIModelHandoffReport Heavy = C::Handoff(Dry);
	const FString HeavyNotes = FString::Join(Heavy.Notes, TEXT(" | "));
	TestTrue(TEXT("a heavy hi-res source is reported, with LOD 0's count beside it: ") + HeavyNotes, Heavy.bOk && Heavy.HiResTriangles == Millions && Heavy.Triangles == 12
		&& HeavyNotes.Contains(FString::Printf(TEXT("%d triangles in its hi-res source, which Modeling Mode edits (LOD 0 has 12)"), Millions))
		&& HeavyNotes.Contains(TEXT("Simplify or Remesh first")));
	TestNull(TEXT("and it is not unpacked to count it"), Mesh->GetHiResSourceModel().GetCachedMeshDescription());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingHandoffOpensTheModeTest, "HyperAIStudio.NativeTools.Modeling.Handoff.PlacesSelectsAndOpensTheMode",
	HyperAIStudio::ModelingHandoffTests::Flags)

bool FHyperAIStudioModelingHandoffOpensTheModeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingHandoffTests;
	if (!Ready(*this)) return true;
	const FString Folder = TestFolder();
	FString Error;
	UStaticMesh* Mesh = WriteBox(Folder, Error);
	if (!TestNotNull(TEXT("the box to hand over is written: ") + Error, Mesh))
	{
		DeleteFolder(Folder);
		return false;
	}
	FHooks Hooks;
	FLayoutGuard Layout;
	// The default palette, whatever this machine's setting.
	FLegacyPalette Palette(false);
	const FString Asset = Mesh->GetPackage()->GetName();
	const FString Label = TestLabel();
	GEditor->SelectNone(true, true, false);
	const int32 UndoBefore = UndoLength();
	bool bUndone = false;
	ON_SCOPE_EXIT
	{
		// Whatever failed above: the mode left, the placement undone, the asset deleted.
		if (GLevelEditorModeTools().IsModeActive(ModeId())) LeaveMode();
		if (!bUndone && FindByLabel(Label) && UndoLength() > UndoBefore) GEditor->UndoTransaction();
		DeleteFolder(Folder);
	};

	FHyperAIModelHandoffRequest First = Request(Asset, Label, TEXT("0,0,20000"));
	First.Tool = TEXT("PolyEdit");
	FHyperAIModelHandoffReport Placed;
	{
		FApproval Off(false);
		Placed = C::Handoff(First);
	}
	if (!TestTrue(TEXT("the box is handed over: ") + Placed.Diagnostic, Placed.bOk && Placed.Status == TEXT("completed"))) return false;
	TestTrue(TEXT("put in the level, selected, in Modeling Mode"), Placed.bPlaced && Placed.bSelected && Placed.bModeActive);
	TestTrue(TEXT("Modeling Mode is active in the level editor"), GLevelEditorModeTools().IsModeActive(ModeId()));
	AActor* Actor = FindByLabel(Label);
	const AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor);
	TestTrue(TEXT("one actor is selected, the one put there, showing the box"), GEditor->GetSelectedActorCount() == 1 && Actor && Actor->IsSelected() && MeshActor
		&& MeshActor->GetStaticMeshComponent()->GetStaticMesh() == Mesh);
	TestEqual(TEXT("where it was asked to stand"), Placed.Location, FString(TEXT("0,0,20000")));
	TestEqual(TEXT("the tabs were left alone"), Placed.Viewport, FString(TEXT("left_alone")));
	TestTrue(TEXT("in HyperAI's outliner folder"), Actor && Actor->GetFolderPath() == FName(C::HandoffFolder));
	TestEqual(TEXT("the actor's path is reported"), Placed.ActorPath, Actor ? Actor->GetPathName() : FString());
	// The palette's own check, against Modeling Mode's private id for PolyGroup Edit in this engine.
	TestTrue(TEXT("PolyGroup Edit would start on this selection: ") + FString::Join(Placed.Notes, TEXT(" | ")), Placed.bToolReady && Placed.ToolHint == TEXT("Model > PolyGroup Edit"));
	TestTrue(TEXT("and the next steps name the default palette: ") + FString::Join(Placed.NextSteps, TEXT(" | ")), Placed.NextSteps.Num() > 1
		&& Placed.NextSteps[1] == TEXT("Open the Model palette and click PolyGroup Edit (faces, edges and vertices by polygroup: extrude, inset, bevel, cut)."));
	FHyperAIModelInspectRequest Look;
	Look.Asset = Asset;
	const FHyperAIModelInspectReport Before = C::Inspect(Look);
	TestTrue(TEXT("the fingerprint handed over is inspect's: ") + Placed.Fingerprint, !Placed.Fingerprint.IsEmpty() && Placed.Fingerprint == Before.Fingerprint);
	TestTrue(TEXT("inspect remembers the hand-off, unchanged"), Before.Details.ContainsByPredicate([](const FString& Line)
		{
			return Line.StartsWith(TEXT("Handed to Modeling Mode")) && Line.Contains(TEXT("unchanged since"));
		}));
	TestEqual(TEXT("placing is one undo step"), UndoLength(), UndoBefore + 1);
	TestTrue(TEXT("named for the hand-off"), GEditor->Trans && GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("Place for Modeling Mode")));
	TestEqual(TEXT("the reply says what the person does next"), Placed.NextSteps.Num(), 6);

	// Asked again without Label or Location: the actor already showing it is used, and nothing is placed.
	FHyperAIModelHandoffReport Again;
	{
		FApproval Off(false);
		Again = C::Handoff(Request(Asset));
	}
	TestTrue(TEXT("asked again, the same actor is used: ") + Again.Diagnostic, Again.bOk && !Again.bPlaced && Again.ActorPath == Placed.ActorPath);
	TestTrue(TEXT("with the mode open already"), Again.Notes.Contains(TEXT("Modeling Mode was open already.")));
	TestTrue(TEXT("without Tool, the next steps suggest tools on the default palette: ") + FString::Join(Again.NextSteps, TEXT(" | ")), Again.NextSteps.Num() > 1
		&& Again.NextSteps[1].Contains(TEXT("Model > PolyGroup Edit")) && Again.NextSteps[1].Contains(TEXT("Mesh > Remesh or Simplify")));
	TestEqual(TEXT("and nothing more to undo"), UndoLength(), UndoBefore + 1);
	FHyperAIModelHandoffRequest ByActor = Request(FString());
	ByActor.Actor = Label;
	const FHyperAIModelHandoffReport Named = C::Handoff(ByActor);
	TestTrue(TEXT("the actor can be named instead: ") + Named.Diagnostic, Named.bOk && Named.ActorPath == Placed.ActorPath && Named.Asset == Mesh->GetPathName());
	// Using an actor that is there changes nothing in the level, so it needs no approval.
	{
		FApproval On(true);
		const FHyperAIModelHandoffReport Reused = C::Handoff(Request(Asset));
		TestTrue(TEXT("with approval on, using the actor there runs straight away: ") + Reused.Status, Reused.bOk && Reused.Status == TEXT("completed"));
	}

	// A hand edit, as an Accept writes it: the source mesh changes and the package is dirty.
	const TStrongObjectPtr<UDynamicMesh> Ball(NewObject<UDynamicMesh>(GetTransientPackage()));
	UGeometryScriptLibrary_MeshPrimitiveFunctions::AppendSphereLatLong(Ball.Get(), FGeometryScriptPrimitiveOptions(), FTransform::Identity, 50, 12, 12);
	FGeometryScriptCopyMeshToAssetOptions Options;
	Options.bEmitTransaction = false;
	EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
	UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(Ball.Get(), Mesh, Options, FGeometryScriptMeshWriteLOD(), Outcome, /*bUseSectionMaterials=*/false);
	Mesh->MarkPackageDirty();
	const FHyperAIModelInspectReport After = C::Inspect(Look);
	TestTrue(TEXT("after the edit the fingerprint differs: ") + After.Fingerprint, Outcome == EGeometryScriptOutcomePins::Success && !After.Fingerprint.IsEmpty()
		&& After.Fingerprint != Placed.Fingerprint);
	TestTrue(TEXT("inspect says it changed since the hand-off: ") + FString::Join(After.Details, TEXT(" | ")), After.Details.ContainsByPredicate([](const FString& Line)
		{
			return Line.Contains(TEXT("changed since"));
		}));
	TestTrue(TEXT("and that it is unsaved"), After.Details.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("unsaved changes")); }));

	// Leaving: the mode goes at once, the default mode comes back, and one undo takes the actor away.
	LeaveMode();
	TestFalse(TEXT("Modeling Mode is left"), GLevelEditorModeTools().IsModeActive(ModeId()));
	TestTrue(TEXT("the editor's default mode is back"), GLevelEditorModeTools().IsDefaultModeActive());
	GEditor->UndoTransaction();
	bUndone = true;
	TestNull(TEXT("one undo takes the placed actor away"), FindByLabel(Label));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingHandoffOwnLabelTest, "HyperAIStudio.NativeTools.Modeling.Handoff.OwnLabelFollowsTheLabelRule",
	HyperAIStudio::ModelingHandoffTests::Flags)

bool FHyperAIStudioModelingHandoffOwnLabelTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingHandoffTests;
	if (!Ready(*this)) return true;
	const FString Folder = TestFolder();
	FString Error;
	UStaticMesh* Box = WriteBox(Folder, Error);
	// An asset name the Label rule does not allow (+ and letters past ASCII), long enough that the label is cut.
	const FString Name = FString(TEXT("SM_Rock+Moss_")) + FString(TEXT("岩石")) + TEXT("_") + FString::ChrN(60, TEXT('x'));
	UStaticMesh* Mesh = Box ? CopyAs(*Box, Folder, Name) : nullptr;
	if (!TestNotNull(TEXT("a mesh with that name: ") + Error, Mesh))
	{
		DeleteFolder(Folder);
		return false;
	}
	FHooks Hooks;
	FLayoutGuard Layout;
	const FString Asset = Mesh->GetPathName();
	const FString Expected = (TEXT("HyperAI_Edit_SM_Rock_Moss____") + FString::ChrN(60, TEXT('x'))).Left(64);
	GEditor->SelectNone(true, true, false);
	int32 Placed = 0;
	ON_SCOPE_EXIT
	{
		if (GLevelEditorModeTools().IsModeActive(ModeId())) LeaveMode();
		for (; Placed > 0; --Placed) GEditor->UndoTransaction();
		DeleteFolder(Folder);
	};

	FHyperAIModelHandoffRequest Dry = Request(Asset, FString(), TEXT("0,0,20000"));
	Dry.bDryRun = true;
	const FHyperAIModelHandoffReport Checked = C::Handoff(Dry);
	TestTrue(TEXT("the dry run is checked: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked") && Checked.bPlaced);
	TestEqual(TEXT("with a label made within the Label rule, and cut to 64"), Checked.ActorLabel, Expected);
	FHyperAIModelHandoffReport Own;
	{
		FApproval Off(false);
		Own = C::Handoff(Request(Asset, FString(), TEXT("0,0,20000")));
	}
	Placed += Own.bPlaced && !Own.ActorPath.IsEmpty() ? 1 : 0;
	TestTrue(TEXT("and the real run puts it there by that label, as the dry run said: ") + Own.Diagnostic, Own.bOk && Own.Status == TEXT("completed") && Own.ActorLabel == Expected
		&& FindByLabel(Expected));
	// Another actor showing it, labelled to sort first: a later hand-off still prefers its own.
	const FString First = TEXT("AAA_HyperAITest_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	FHyperAIModelHandoffReport Second;
	{
		FApproval Off(false);
		Second = C::Handoff(Request(Asset, First, TEXT("500,0,20000")));
	}
	Placed += Second.bPlaced && !Second.ActorPath.IsEmpty() ? 1 : 0;
	TestTrue(TEXT("a second actor is put there: ") + Second.Diagnostic, Second.bOk && Second.bPlaced && FindByLabel(First));
	const FHyperAIModelHandoffReport Again = C::Handoff(Request(Asset));
	TestTrue(TEXT("asked again, the hand-off's own actor is used, though another sorts first: ") + Again.ActorLabel, Again.bOk && !Again.bPlaced && Again.ActorLabel == Expected
		&& Again.ActorPath == Own.ActorPath);
	TestEqual(TEXT("and it sees the other actor showing it"), Again.SharedBy, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioModelingHandoffApprovalTest, "HyperAIStudio.NativeTools.Modeling.Handoff.ApprovalHoldsThePlacement",
	HyperAIStudio::ModelingHandoffTests::Flags)

bool FHyperAIStudioModelingHandoffApprovalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::ModelingHandoffTests;
	if (!Ready(*this)) return true;
	const FString Folder = TestFolder();
	FString Error;
	UStaticMesh* Mesh = WriteBox(Folder, Error);
	if (!TestNotNull(TEXT("the box to hand over is written: ") + Error, Mesh))
	{
		DeleteFolder(Folder);
		return false;
	}
	ON_SCOPE_EXIT
	{
		DeleteFolder(Folder);
	};
	FHooks Hooks;
	GEditor->SelectNone(true, true, false);
	const int32 UndoBefore = UndoLength();
	const FString Label = TestLabel();
	FHyperAIModelHandoffReport Held;
	{
		FApproval On(true);
		Held = C::Handoff(Request(Mesh->GetPackage()->GetName(), Label, TEXT("0,0,20000")));
	}
	TestTrue(TEXT("with approval on, putting an actor in the level waits: ") + Held.Diagnostic, Held.bOk && Held.Status == TEXT("awaiting_user_approval"));
	TestTrue(TEXT("in the approval queue"), FHyperAIStudioApprovalGate::HasPending(Held.OperationId));
	const TArray<FHyperAIStudioPendingApproval> Pending = FHyperAIStudioApprovalGate::GetPending();
	const FHyperAIStudioPendingApproval* Mine = Pending.FindByPredicate([&Held](const FHyperAIStudioPendingApproval& Item) { return Item.OperationId == Held.OperationId; });
	TestTrue(TEXT("as a hand-off that says what it puts where"), Mine && Mine->Summary.ToolName == C::HandoffToolName && !Mine->Summary.Effects.IsEmpty()
		&& Mine->Summary.Effects[0].Contains(TEXT("put")) && Mine->Summary.Effects[0].Contains(TEXT("0,0,20000")));
	TestNull(TEXT("nothing is put in the level yet"), FindByLabel(Label));
	TestFalse(TEXT("Modeling Mode is not opened yet"), GLevelEditorModeTools().IsModeActive(ModeId()));
	TestEqual(TEXT("nothing goes in the undo history"), UndoLength(), UndoBefore);
	FHyperAIStudioApprovalGate::Reject(Held.OperationId, TEXT("test"));
	TestFalse(TEXT("rejected, it is gone"), FHyperAIStudioApprovalGate::HasPending(Held.OperationId));
	TestNull(TEXT("and still nothing is in the level"), FindByLabel(Label));

	// Approved after another level was opened: the placement was checked in this one, so it is refused there. The other
	// level is a scratch world, current for the moment of the click only.
	auto Hold = [&]()
	{
		FApproval On(true);
		return C::Handoff(Request(Mesh->GetPackage()->GetName(), Label, TEXT("0,0,20000")));
	};
	FWorldContext& Editor = GEditor->GetEditorWorldContext();
	UWorld* Open = Editor.World();
	const FHyperAIModelHandoffReport Moved = Hold();
	UWorld* Other = UWorld::CreateWorld(EWorldType::Inactive, /*bInformEngineOfWorld=*/false, TEXT("HyperAIHandoffOtherLevel"));
	if (TestTrue(TEXT("held again: ") + Moved.Diagnostic, Moved.Status == TEXT("awaiting_user_approval")) && TestNotNull(TEXT("a scratch level"), Other))
	{
		FString ApproveError;
		Editor.SetCurrentWorld(Other);
		const bool bApproved = FHyperAIStudioApprovalGate::Approve(Moved.OperationId, ApproveError);
		Editor.SetCurrentWorld(Open);
		const TOptional<FHyperAIStudioActivityEntry> Ran = Outcome(Moved.OperationId);
		TestTrue(TEXT("approved in another level, it is refused, naming both: ") + (Ran ? Ran->Detail : ApproveError), bApproved && Ran && Ran->Kind == EHyperAIStudioActivityKind::Failed
			&& Ran->Detail.Contains(FString::Printf(TEXT("checked in %s, but %s is open now"), *Open->GetOutermost()->GetName(), *Other->GetOutermost()->GetName())));
		TestFalse(TEXT("nothing is put in the scratch level"), Other->GetCurrentLevel()->Actors.ContainsByPredicate([&Label](const AActor* Actor)
		{
			return Actor && Actor->GetActorLabel() == Label;
		}));
	}
	if (Other) Other->DestroyWorld(/*bInformEngineOfWorld=*/false);
	// Approved while Play in Editor runs: refused too.
	const FHyperAIModelHandoffReport Playing = Hold();
	if (TestTrue(TEXT("held a third time: ") + Playing.Diagnostic, Playing.Status == TEXT("awaiting_user_approval")) && !GEditor->PlayWorld)
	{
		FString ApproveError;
		GEditor->PlayWorld = Open;
		const bool bApproved = FHyperAIStudioApprovalGate::Approve(Playing.OperationId, ApproveError);
		GEditor->PlayWorld = nullptr;
		const TOptional<FHyperAIStudioActivityEntry> Ran = Outcome(Playing.OperationId);
		TestTrue(TEXT("approved during Play in Editor, it is refused: ") + (Ran ? Ran->Detail : ApproveError), bApproved && Ran && Ran->Kind == EHyperAIStudioActivityKind::Failed
			&& Ran->Detail.Contains(TEXT("Play in Editor was running")));
	}
	TestNull(TEXT("after both, nothing is in the level"), FindByLabel(Label));
	TestFalse(TEXT("Modeling Mode was never opened"), GLevelEditorModeTools().IsModeActive(ModeId()));
	TestEqual(TEXT("and nothing went in the undo history"), UndoLength(), UndoBefore);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
