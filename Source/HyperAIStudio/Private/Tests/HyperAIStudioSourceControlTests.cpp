// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Curves/CurveFloat.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioSourceControl.h"
#include "HyperAIStudioSourceControlToolset.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace HyperAIStudio::SourceControlTests
{
	/** The lore command as far as an agent's edits go: it records each call, and a file named Locked is someone else's. */
	const TCHAR* const FakeLore = TEXT(R"PY(import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
with open(os.path.join(HERE, "calls.jsonl"), "a", encoding="utf-8") as f:
    f.write(json.dumps(args) + "\n")
command = args[args.index("--non-interactive") + 1:] if "--non-interactive" in args else args
if command[:2] == ["lock", "acquire"] and any("Locked" in path for path in command[2:]):
    sys.stderr.write("error: SC_Locked.uasset is locked by bob@studio\n")
    sys.exit(1)
if command[:1] == ["status"]:
    print("On branch main, 2 files dirty")
print("ok")
)PY");

	UPackage* SaveCurve(const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		UCurveFloat* Curve = NewObject<UCurveFloat>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Curve);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, Curve, *FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args);
		return Package;
	}

	FString File(const FString& PackageName)
	{
		return FPaths::ConvertRelativePathToFull(FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSourceControlDecideTest,
	"HyperAIStudio.SourceControl.Decide",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSourceControlDecideTest::RunTest(const FString& Parameters)
{
	using EDecision = FHyperAIStudioSourceControl::EDecision;
	FString Error;
	FHyperAIStudioSourceControlFileState State;
	TestTrue(TEXT("A file new to source control is edited as it is"), FHyperAIStudioSourceControl::Decide(State, TEXT("Content/A.uasset"), Error) == EDecision::Edit);
	State.bSourceControlled = true;
	State.bCanCheckOut = true;
	TestTrue(TEXT("One that can be checked out is checked out first"), FHyperAIStudioSourceControl::Decide(State, TEXT("Content/A.uasset"), Error) == EDecision::CheckOut);
	State.bCheckedOutOther = true;
	State.Who = TEXT("bob");
	TestTrue(TEXT("One a teammate holds turns the edit away, naming them"), FHyperAIStudioSourceControl::Decide(State, TEXT("Content/A.uasset"), Error) == EDecision::Refuse
		&& Error.Contains(TEXT("bob")) && Error.Contains(TEXT("Content/A.uasset")));
	State.bCheckedOutOther = false;
	State.bCurrent = false;
	TestTrue(TEXT("One behind the latest revision turns it away, asking for the latest"), FHyperAIStudioSourceControl::Decide(State, TEXT("Content/A.uasset"), Error) == EDecision::Refuse
		&& Error.Contains(TEXT("latest")));
	State.bCurrent = true;
	State.bDeleted = true;
	TestTrue(TEXT("One marked for delete turns it away"), FHyperAIStudioSourceControl::Decide(State, TEXT("Content/A.uasset"), Error) == EDecision::Refuse);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSourceControlLoreTest,
	"HyperAIStudio.SourceControl.Lore",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSourceControlLoreTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::SourceControlTests;
	if (ISourceControlModule::Get().IsEnabled() && ISourceControlModule::Get().GetProvider().GetName() != FName(TEXT("None")))
	{
		AddInfo(TEXT("Skipped: this editor has an Unreal source control provider, which takes precedence over Lore."));
		return true;
	}
	int32 Code = -1;
	FString Out;
	FString Err;
	FPlatformProcess::ExecProcess(TEXT("python"), TEXT("--version"), &Code, &Out, &Err);
	if (Code != 0)
	{
		AddWarning(TEXT("python is not on PATH, so the scripted lore command cannot run."));
		return true;
	}
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / (TEXT("Lore-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
	const FString Script = Folder / TEXT("fake_lore.py");
	FFileHelper::SaveStringToFile(FakeLore, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	FHyperAIStudioSourceControl::SetLoreForTests(TEXT("python"), FString::Printf(TEXT("-u \"%s\""), *Script), Folder);
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Free = TEXT("/Game/HyperAITests/SourceControl/SC_Free_") + Suffix;
	const FString Locked = TEXT("/Game/HyperAITests/SourceControl/SC_Locked_") + Suffix;
	const FString New = TEXT("/Game/HyperAITests/SourceControl/SC_New_") + Suffix;
	TArray<UObject*> Made;
	ON_SCOPE_EXIT
	{
		FHyperAIStudioSourceControl::SetLoreForTests(FString(), FString(), FString());
		if (!Made.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Made, /*ShowConfirmation=*/false);
		}
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	};
	auto Calls = [&Folder]()
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *(Folder / TEXT("calls.jsonl")));
		return Text.Replace(TEXT("\\\\"), TEXT("/"));
	};
	auto Normal = [](FString Path) { return Path.Replace(TEXT("\\"), TEXT("/")); };
	for (const FString& Name : {Free, Locked})
	{
		Made.Add(SaveCurve(Name)->FindAssetInPackage());
	}

	FString Error;
	TestTrue(TEXT("A file nobody holds is locked for the edit: ") + Error, FHyperAIStudioSourceControl::MakeEditable({Free}, Error));
	TestTrue(TEXT("with lore lock acquire, never asking"), Calls().Contains(TEXT("\"--non-interactive\", \"lock\", \"acquire\"")) && Calls().Contains(Normal(File(Free))));
	TestFalse(TEXT("A file a teammate has locked turns the edit away"), FHyperAIStudioSourceControl::MakeEditable({Locked}, Error));
	TestTrue(TEXT("saying who holds it"), Error.Contains(TEXT("bob@studio")));
	TestTrue(TEXT("A file not made yet needs no lock"), FHyperAIStudioSourceControl::MakeEditable({New}, Error) && !Calls().Contains(Normal(File(New))));

	// Saving the new file tells Lore about it, on the next tick.
	Made.Add(SaveCurve(New)->FindAssetInPackage());
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.02f);
	}
	TestTrue(TEXT("An agent's new file is marked dirty for the next commit"), Calls().Contains(TEXT("\"dirty\"")) && Calls().Contains(Normal(File(New))));

	FHyperAISourceControlStatusRequest StatusRequest;
	const FHyperAISourceControlStatusReport Status = UHyperAIStudioSourceControlToolset::hyper_source_control_status(StatusRequest);
	TestTrue(TEXT("Status names the workspace and the agent's files, with what lore status says"), Status.Backend.StartsWith(TEXT("lore:"))
		&& Status.Files.Num() == 2 && Status.LoreStatus.Contains(TEXT("On branch main")));
	TestTrue(TEXT("and that the repository is on a server"), Status.Lore.Mode == TEXT("remote") && Status.Lore.Server == TEXT("lore://127.0.0.1:41337"));

	FHyperAISourceControlSubmitRequest Submit;
	Submit.Description = TEXT("Add the jump curves");
	const FHyperAISourceControlSubmitReport Checked = UHyperAIStudioSourceControlToolset::hyper_source_control_submit(Submit);
	TestTrue(TEXT("A dry run lists what would go"), Checked.bOk && Checked.Status == TEXT("checked") && Checked.Files.Num() == 2);
	Submit.bDryRun = false;
	const FHyperAISourceControlSubmitReport Asked = UHyperAIStudioSourceControlToolset::hyper_source_control_submit(Submit);
	TestEqual(TEXT("For real it waits for the user"), Asked.Status, FString(TEXT("awaiting_user_approval")));
	TestFalse(TEXT("and nothing is committed before that"), Calls().Contains(TEXT("\"commit\"")));
	TestTrue(TEXT("Approved, it goes: ") + Error, FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
	const FString Log = Calls();
	TestTrue(TEXT("staged, committed with the description, pushed, and the locks let go"), Log.Contains(TEXT("\"stage\"")) && Log.Contains(TEXT("\"commit\", \"Add the jump curves\""))
		&& Log.Contains(TEXT("\"push\"")) && Log.Contains(TEXT("\"lock\", \"release\"")));
	TestTrue(TEXT("and nothing of the agents' is left to submit"), FHyperAIStudioSourceControl::GetAgentFiles().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSourceControlSaveAssetTest,
	"HyperAIStudio.SourceControl.SaveAsset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSourceControlSaveAssetTest::RunTest(const FString& Parameters)
{
	const FString Name = TEXT("/Game/HyperAITests/SourceControl/SC_Save_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	UPackage* Package = CreatePackage(*Name);
	UCurveFloat* Curve = NewObject<UCurveFloat>(Package, *FPackageName::GetShortName(Name), RF_Public | RF_Standalone);
	FAssetRegistryModule::AssetCreated(Curve);
	ON_SCOPE_EXIT
	{
		ObjectTools::ForceDeleteObjects(TArray<UObject*>{Curve}, /*ShowConfirmation=*/false);
		// The full load still holds the package this frame, so the delete above leaves its file behind.
		IFileManager::Get().Delete(*HyperAIStudio::SourceControlTests::File(Name), /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
	};
	FString Error;
	const bool bMade = FHyperAIStudioSourceControl::SaveAsset(*Curve, Error);
	TestTrue(TEXT("A new asset is saved: ") + Error, bMade);
	// As another asset's load leaves a package it needed only a class from: Epic's save refuses it, by default through a
	// fatal error, which once crashed the editor as a Blueprint's layout was saved.
	Package->MarkAsUnloaded();
	TestFalse(TEXT("The package now reads as partly loaded"), Package->IsFullyLoaded());
	Curve->FloatCurve.AddKey(1.f, 2.f);
	Curve->MarkPackageDirty();
	Error.Reset();
	const bool bEdited = FHyperAIStudioSourceControl::SaveAsset(*Curve, Error);
	TestTrue(TEXT("An edit to it is still saved, the package loaded in full first: ") + Error, bEdited);
	TestEqual(TEXT("with the edit kept"), Curve->FloatCurve.GetNumKeys(), 1);
	TestFalse(TEXT("and nothing left unsaved"), Package->IsDirty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
