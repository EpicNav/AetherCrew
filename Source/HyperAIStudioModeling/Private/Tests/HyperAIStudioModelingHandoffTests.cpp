// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "EditorModeManager.h"
#include "Engine/Selection.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioModelingGate.h"
#include "HyperAIStudioModelingToolset.h"
#include "HyperAIStudioSettings.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tools/Modes.h"
#include "UDynamicMesh.h"
#include "UObject/StrongObjectPtr.h"

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

	int32 UndoLength()
	{
		return GEditor && GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
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
		// Play In Editor running: refused before the request is read.
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (!GEditor->PlayWorld)
		{
			GEditor->PlayWorld = World;
			const FHyperAIModelHandoffReport Playing = C::Handoff(Request(Asset, Label));
			GEditor->PlayWorld = nullptr;
			TestTrue(TEXT("a running PIE session is refused: ") + Playing.Diagnostic, Refused(Playing, TEXT("invalid_request"), TEXT("Play in Editor")));
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
	TestTrue(TEXT("PolyGroup Edit would start on this selection: ") + FString::Join(Placed.Notes, TEXT(" | ")), Placed.bToolReady && Placed.ToolHint == TEXT("PolyModel > PolyGroup Edit"));
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
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
