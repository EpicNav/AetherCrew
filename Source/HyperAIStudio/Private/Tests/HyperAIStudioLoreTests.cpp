// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/AllOf.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Curves/CurveFloat.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioLore.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSourceControl.h"
#include "HyperAIStudioSourceControlToolset.h"
#include "HyperAIStudioWidgetShot.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "SHyperAIStudioLoreSetupPanel.h"
#include "SHyperAIStudioSettingsPage.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::LoreTests
{
	/** The lore command as far as an agent's edits go, recording each call. */
	const TCHAR* const FakeLore = TEXT(R"PY(import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
args = sys.argv[1:]
with open(os.path.join(HERE, "calls.jsonl"), "a", encoding="utf-8") as f:
    f.write(json.dumps(args) + "\n")
command = args[args.index("--non-interactive") + 1:] if "--non-interactive" in args else args
if command[:2] == ["lock", "acquire"]:
    sys.stderr.write("[Error] No remote configured\n  at lore-revision/src/lock/file/acquire.rs - Unable to acquire lock while offline\n")
    sys.exit(111)
if command[:1] == ["status"]:
    print("On branch main revision 3")
print("ok")
)PY");

	/** A folder of its own under the bench's Saved folder, never the user's project. */
	FString TempFolder(const TCHAR* Label)
	{
		const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAITests") / FString::Printf(TEXT("%s-%s"), Label,
			*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12)));
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		return Folder;
	}

	void Touch(const FString& Folder, const TCHAR* Relative)
	{
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("%s\n"), Relative), *(Folder / Relative));
	}

	bool HasUnrealProvider()
	{
		return ISourceControlModule::Get().IsEnabled() && ISourceControlModule::Get().GetProvider().GetName() != FName(TEXT("None"));
	}

	TArray<EHyperAIStudioLoreStepKind> Kinds(const TArray<FHyperAIStudioLoreSetupStep>& Steps)
	{
		TArray<EHyperAIStudioLoreStepKind> Out;
		for (const FHyperAIStudioLoreSetupStep& Step : Steps)
		{
			Out.Add(Step.Kind);
		}
		return Out;
	}

	const FHyperAIStudioLoreSetupStep* Find(const TArray<FHyperAIStudioLoreSetupStep>& Steps, const EHyperAIStudioLoreStepKind Kind)
	{
		return Steps.FindByPredicate([Kind](const FHyperAIStudioLoreSetupStep& Step) { return Step.Kind == Kind; });
	}

	/** Runs the game thread's queued tasks until an approved Lore submit has finished, for up to a minute. */
	bool WaitForSubmit()
	{
		const double End = FPlatformTime::Seconds() + 60.0;
		while (FHyperAIStudioSourceControl::IsSubmitting() && FPlatformTime::Seconds() < End)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.02f);
		}
		return !FHyperAIStudioSourceControl::IsSubmitting();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLoreCliTest,
	"HyperAIStudio.SourceControl.LoreCli",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLoreCliTest::RunTest(const FString& Parameters)
{
	FString Version;
	FString Build;
	TestTrue(TEXT("lore 0.10.1's own --version line"), FHyperAIStudioLore::ParseVersion(TEXT("lore 0.10.1+1476-lore_v0.10.1__urc_main\r\n"), Version, Build));
	TestEqual(TEXT("is release 0.10.1"), Version, FString(TEXT("0.10.1")));
	TestEqual(TEXT("built 1476-lore_v0.10.1__urc_main"), Build, FString(TEXT("1476-lore_v0.10.1__urc_main")));
	TestTrue(TEXT("An older release"), FHyperAIStudioLore::ParseVersion(TEXT("lore 0.8.3"), Version, Build) && Version == TEXT("0.8.3") && Build.IsEmpty());
	TestTrue(TEXT("A build from source keeps its pre-release"), FHyperAIStudioLore::ParseVersion(TEXT("lore 0.10.1-nightly+local"), Version, Build)
		&& Version == TEXT("0.10.1-nightly") && Build == TEXT("local"));
	TestTrue(TEXT("The server's line, after a warning"), FHyperAIStudioLore::ParseVersion(TEXT("warning: config not found\nloreserver 0.10.1"), Version, Build) && Version == TEXT("0.10.1"));
	TestFalse(TEXT("An error is no version"), FHyperAIStudioLore::ParseVersion(TEXT("[Error] Repository not found\n  at lore\\src\\call.rs:236:19"), Version, Build));
	TestFalse(TEXT("Nor is nothing"), FHyperAIStudioLore::ParseVersion(FString(), Version, Build));

	FString Remote;
	FString Identity;
	TestTrue(TEXT("A local repository's config names its remote_url"),
		FHyperAIStudioLore::ReadWorkspaceConfig(TEXT("remote_url = \"\"\n\n[store]\nmax_capacity = 2000000\n"), Remote, Identity));
	TestTrue(TEXT("as empty"), Remote.IsEmpty() && Identity.IsEmpty());
	TestTrue(TEXT("A server repository's config names its remote_url"), FHyperAIStudioLore::ReadWorkspaceConfig(
		TEXT("remote_url = \"lore://127.0.0.1:41337\"\r\nidentity = \"alex@example.com\"\r\n\r\n[vfs]\r\nidentity = \"not-top-level\"\r\n"), Remote, Identity));
	TestEqual(TEXT("as the server"), Remote, FString(TEXT("lore://127.0.0.1:41337")));
	TestEqual(TEXT("and its identity, from the top level only"), Identity, FString(TEXT("alex@example.com")));
	TestFalse(TEXT("A config with remote_url only under a table does not say"),
		FHyperAIStudioLore::ReadWorkspaceConfig(TEXT("[remote]\nremote_url = \"lore://host:41337\"\n"), Remote, Identity));
	TestFalse(TEXT("Nor does an empty one"), FHyperAIStudioLore::ReadWorkspaceConfig(FString(), Remote, Identity));

	TestEqual(TEXT("A project name becomes a repository name"), FHyperAIStudioLore::RepositoryName(TEXT("My Project (v2)!")), FString(TEXT("My-Project-v2")));
	TestEqual(TEXT("one Lore accepts as it is"), FHyperAIStudioLore::RepositoryName(TEXT("HyperAIFixBench3")), FString(TEXT("HyperAIFixBench3")));
	TestTrue(TEXT("Names: letters, digits, - _ . /"), FHyperAIStudioLore::IsValidRepositoryName(TEXT("team/My-Game_1.0")));
	TestFalse(TEXT("not spaces"), FHyperAIStudioLore::IsValidRepositoryName(TEXT("My Project")));
	TestFalse(TEXT("nor a leading dash, read as an option"), FHyperAIStudioLore::IsValidRepositoryName(TEXT("-f")));
	TestFalse(TEXT("nor ..") , FHyperAIStudioLore::IsValidRepositoryName(TEXT("a/../b")));

	FString Error;
	EHyperAIStudioLoreProblem Problem = EHyperAIStudioLoreProblem::BadName;
	TestTrue(TEXT("A loreserver on this computer"), FHyperAIStudioLore::IsValidServerUrl(TEXT("lore://127.0.0.1:41337"), Error, &Problem) && Problem == EHyperAIStudioLoreProblem::None);
	TestTrue(TEXT("A hosted one by name"), FHyperAIStudioLore::IsValidServerUrl(TEXT(" lore://lore.example.com/ "), Error));
	TestFalse(TEXT("No scheme"), FHyperAIStudioLore::IsValidServerUrl(TEXT("127.0.0.1:41337"), Error, &Problem));
	TestTrue(TEXT("which the page can word in the user's language"), Problem == EHyperAIStudioLoreProblem::ServerNoScheme && !Error.IsEmpty());
	TestFalse(TEXT("No password in the address"), FHyperAIStudioLore::IsValidServerUrl(TEXT("lore://bob:secret@host:41337"), Error, &Problem) || Problem != EHyperAIStudioLoreProblem::ServerBadHost);
	TestFalse(TEXT("No spaces"), FHyperAIStudioLore::IsValidServerUrl(TEXT("lore://my host"), Error, &Problem) || Problem != EHyperAIStudioLoreProblem::ServerSpacesOrQuotes);
	TestFalse(TEXT("No option for a scheme"), FHyperAIStudioLore::IsValidServerUrl(TEXT("-x://host"), Error, &Problem) || Problem != EHyperAIStudioLoreProblem::ServerBadScheme);
	TestEqual(TEXT("A repository's address on its server"), FHyperAIStudioLore::RepositoryUrl(TEXT("lore://127.0.0.1:41337/"), TEXT("Game")), FString(TEXT("lore://127.0.0.1:41337/Game")));
	TestEqual(TEXT("The sign-in command the user runs"), FHyperAIStudioLore::LoginCommand(TEXT("lore://127.0.0.1:41337")), FString(TEXT("lore login lore://127.0.0.1:41337")));

	TestEqual(TEXT("Arguments with spaces are quoted"), FHyperAIStudioLore::JoinArguments({TEXT("commit"), TEXT("Add the level"), TEXT("E:/Unreal Projects/X/")}),
		FString(TEXT("commit \"Add the level\" \"E:/Unreal Projects/X/\"")));
	TestEqual(TEXT("and a quote or a trailing backslash kept"), FHyperAIStudioLore::JoinArguments({TEXT("say \"hi\""), TEXT("C:\\a b\\")}),
		FString(TEXT("\"say \\\"hi\\\"\" \"C:\\a b\\\\\"")));

	const TArray<FString> SignIns = FHyperAIStudioLore::ParseSignIns(TEXT("alex@example.com  lore://lore.example.com\n")
		TEXT("bob eyJhbGciOiJSUzI1NiJ9.eyJzdWIiOiJib2IifQ.c2lnbmF0dXJlLXZhbHVlLWhlcmU\n\n[Error] x\n"));
	TestEqual(TEXT("Two sign-ins listed"), SignIns.Num(), 2);
	TestTrue(TEXT("by name"), SignIns.Num() == 2 && SignIns[0].StartsWith(TEXT("alex@example.com")) && SignIns[1] == TEXT("bob"));
	TestFalse(TEXT("and never a token"), FString::Join(SignIns, TEXT(" ")).Contains(TEXT("eyJ")));

	bool bChanged = false;
	const FString Fresh = FHyperAIStudioLore::MergeIgnore(FString(), bChanged);
	TestTrue(TEXT("A new .loreignore has Unreal's folders"), bChanged && Fresh.Contains(TEXT("/Binaries/\n")) && Fresh.Contains(TEXT("/Intermediate/\n"))
		&& Fresh.Contains(TEXT("/Saved/\n")) && Fresh.Contains(TEXT("/DerivedDataCache/\n")) && Fresh.Contains(TEXT("Plugins/**/Binaries/\n"))
		&& Fresh.Contains(TEXT("/.vs/\n")) && Fresh.Contains(TEXT("*.sln\n")));
	TestTrue(TEXT("and HyperAI's per-machine runtime folder, which only .gitignore kept out before"), Fresh.Contains(TEXT("/.hyperai/runtime/\n")));
	TestTrue(TEXT("and does not hide Content, or the rest of .hyperai"), !Fresh.Contains(TEXT("Content")) && !Fresh.Contains(TEXT("/.hyperai/\n")));
	const FString Mine = TEXT("# mine\r\n/Saved/\r\n*.psd\r\n!keep.psd");
	const FString Merged = FHyperAIStudioLore::MergeIgnore(Mine, bChanged);
	TestTrue(TEXT("An existing one keeps every line, in order"), bChanged && Merged.StartsWith(Mine));
	TestEqual(TEXT("and gains only the patterns it lacks"), Merged.Find(TEXT("/Saved/")), Merged.Find(TEXT("/Saved/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd));
	FHyperAIStudioLore::MergeIgnore(Merged, bChanged);
	TestFalse(TEXT("Merging again changes nothing"), bChanged);

	// Lore reads only .loreignore, so what a project keeps out of Git through .gitignore alone is copied in.
	const FString GitIgnore = TEXT("# BEGIN HyperAIStudio\r\n.hyperai/runtime/\r\n# END HyperAIStudio\r\n/Saved/\r\n.claude/settings.local.json\r\n*.pdb\r\n!Keep.pdb\r\n\r\n");
	TestTrue(TEXT("A .gitignore's rules are its lines that are not blank or comments"), FHyperAIStudioLore::IgnoreRules(GitIgnore)
		== TArray<FString>{TEXT(".hyperai/runtime/"), TEXT("/Saved/"), TEXT(".claude/settings.local.json"), TEXT("*.pdb"), TEXT("!Keep.pdb")});
	TestEqual(TEXT("Of those, the merged file lacks four"), FHyperAIStudioLore::CountMissingGitRules(Merged, GitIgnore), 4);
	const FString WithGit = FHyperAIStudioLore::MergeIgnore(Merged, bChanged, GitIgnore);
	TestTrue(TEXT("They are copied in after the Unreal patterns, under their own heading"), bChanged && WithGit.StartsWith(Merged)
		&& WithGit.Contains(TEXT("# From the project's .gitignore")) && WithGit.Find(TEXT(".claude/settings.local.json")) > WithGit.Find(TEXT("/.hyperai/runtime/")));
	TestTrue(TEXT("in their order, so a ! rule still follows its pattern"), WithGit.Find(TEXT("!Keep.pdb")) > WithGit.Find(TEXT("*.pdb")));
	TestEqual(TEXT("and a rule .loreignore has is not copied twice"), WithGit.Find(TEXT("/Saved/")), WithGit.Find(TEXT("/Saved/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd));
	FHyperAIStudioLore::MergeIgnore(WithGit, bChanged, GitIgnore);
	TestFalse(TEXT("Merging the same .gitignore again changes nothing"), bChanged);

	int32 Tools = 0;
	for (TFieldIterator<UFunction> It(UHyperAIStudioSourceControlToolset::StaticClass(), EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (It->HasMetaData(TEXT("AICallable")))
		{
			++Tools;
			const FString ToolTip = It->GetMetaData(TEXT("ToolTip"));
			TestTrue(FString::Printf(TEXT("%s has a description within 400 characters (%d)"), *It->GetName(), ToolTip.Len()), !ToolTip.IsEmpty() && ToolTip.Len() <= 400);
		}
	}
	TestEqual(TEXT("The source control tools are still two"), Tools, 2);

	TestTrue(TEXT("The lore command is lore.exe"), FHyperAIStudioLore::IsLoreFileName(TEXT("C:/Tools/Lore/LORE.EXE")));
	TestFalse(TEXT("not a script"), FHyperAIStudioLore::IsLoreFileName(TEXT("C:/Tools/lore.cmd")) || FHyperAIStudioLore::IsLoreFileName(TEXT("C:/Tools/lore.bat")));
	TestFalse(TEXT("nor another program"), FHyperAIStudioLore::IsLoreFileName(TEXT("C:/Windows/System32/cmd.exe")) || FHyperAIStudioLore::IsLoreFileName(TEXT("C:/Tools/notlore.exe")));

	const FHyperAIStudioLoreCli Cli = FHyperAIStudioLore::DetectCli();
	if (!Cli.bFound)
	{
		AddWarning(TEXT("The lore command is not installed here, so its detection was not exercised: ") + Cli.Error);
		TestTrue(TEXT("and the page is told why"), Cli.Problem == EHyperAIStudioLoreProblem::CliNotFound);
		return true;
	}
	AddInfo(FString::Printf(TEXT("lore %s at %s"), *Cli.Version, *Cli.Path));
	TestTrue(TEXT("The installed lore reports a version: ") + Cli.Error, !Cli.Version.IsEmpty() && FPaths::FileExists(Cli.Path) && Cli.Problem == EHyperAIStudioLoreProblem::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLoreSetupPlanTest,
	"HyperAIStudio.SourceControl.LoreSetupPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLoreSetupPlanTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LoreTests;
	using EKind = EHyperAIStudioLoreStepKind;
	const FString Folder = TempFolder(TEXT("LorePlan"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Folder, false, true); };
	Touch(Folder, TEXT("Game.uproject"));

	FHyperAIStudioLoreSetupOptions Options;
	Options.Folder = Folder;
	Options.Name = TEXT("Game");
	TArray<FHyperAIStudioLoreSetupStep> Steps;
	FString Error;
	TestTrue(TEXT("A local setup plans: ") + Error, FHyperAIStudioLore::PlanSetup(Options, Steps, Error));
	TestTrue(TEXT("ignore rules, the repository, stage, commit; no server, no push"), Kinds(Steps) == TArray<EKind>{EKind::WriteIgnore, EKind::Create, EKind::Stage, EKind::Commit});
	const FHyperAIStudioLoreSetupStep* Create = Find(Steps, EKind::Create);
	TestTrue(TEXT("The repository is made with --offline under its name"), Create && Create->Arguments == TArray<FString>{TEXT("repository"), TEXT("create"), TEXT("--offline"), TEXT("Game")}
		&& Create->Command == TEXT("lore repository create --offline Game"));
	TestTrue(TEXT("The ignore step names HyperAI's runtime folder: ") + Steps[0].Detail, Steps[0].Detail.Contains(TEXT(".hyperai/runtime")) && !Steps[0].Detail.Contains(TEXT(".gitignore")));

	// A project kept in Git too: Lore does not read .gitignore, so its rules are copied in, or the step says they are not.
	FFileHelper::SaveStringToFile(TEXT(".claude/settings.local.json\n*.pdb\n"), *(Folder / TEXT(".gitignore")));
	TestTrue(TEXT("With a .gitignore it still plans: ") + Error, FHyperAIStudioLore::PlanSetup(Options, Steps, Error));
	TestTrue(TEXT("saying its two rules are copied, as Lore does not read it: ") + Steps[0].Detail, Steps[0].Detail.Contains(TEXT("2 rule(s) of the project's .gitignore")));
	Options.bCopyGitIgnore = false;
	TestTrue(TEXT("Not copying them plans too"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error));
	TestTrue(TEXT("and says what that leaves in: ") + Steps[0].Detail, Steps[0].Detail.Contains(TEXT("only the project's .gitignore keeps out will be committed")));
	Options.bCopyGitIgnore = true;
	IFileManager::Get().Delete(*(Folder / TEXT(".gitignore")));
	TestTrue(TEXT("each step says how to undo it"), Algo::AllOf(Steps, [](const FHyperAIStudioLoreSetupStep& Step) { return !Step.Undo.IsEmpty() && !Step.Title.IsEmpty(); }));
	TestTrue(TEXT("Staging walks the folder"), Find(Steps, EKind::Stage) && Find(Steps, EKind::Stage)->Arguments == TArray<FString>{TEXT("stage"), TEXT("--scan"), TEXT(".")});
	TestTrue(TEXT("The commit carries the default message"), Find(Steps, EKind::Commit) && Find(Steps, EKind::Commit)->Arguments.Last() == TEXT("Add the Unreal project"));
	TestFalse(TEXT("Planning writes nothing"), FPaths::FileExists(Folder / TEXT(".loreignore")) || IFileManager::Get().DirectoryExists(*(Folder / TEXT(".lore"))));

	Options.ServerUrl = TEXT("lore://127.0.0.1:41337");
	Options.Identity = TEXT("alex@example.com");
	Options.CommitMessage = TEXT("--amend is not an option here");
	TestTrue(TEXT("A server setup plans: ") + Error, FHyperAIStudioLore::PlanSetup(Options, Steps, Error));
	TestTrue(TEXT("checking the server first and pushing last"), Kinds(Steps) == TArray<EKind>{EKind::CheckServer, EKind::WriteIgnore, EKind::Create, EKind::Stage, EKind::Commit, EKind::Push});
	TestTrue(TEXT("The check only lists the server's repositories"), Steps[0].Arguments == TArray<FString>{TEXT("repository"), TEXT("list"), TEXT("lore://127.0.0.1:41337")} && !Steps[0].bChangesFolder);
	Create = Find(Steps, EKind::Create);
	TestTrue(TEXT("The repository is made on the server, as the identity"), Create && Create->Arguments == TArray<FString>{TEXT("--identity"), TEXT("alex@example.com"),
		TEXT("repository"), TEXT("create"), TEXT("lore://127.0.0.1:41337/Game")});
	TestTrue(TEXT("A message that starts with '-' follows --"), Find(Steps, EKind::Commit) && Find(Steps, EKind::Commit)->Arguments
		== TArray<FString>{TEXT("commit"), TEXT("--"), TEXT("--amend is not an option here")});

	Options.ServerUrl.Reset();
	Options.bWriteIgnore = false;
	Options.bFirstCommit = false;
	TestTrue(TEXT("Only the repository, when asked for nothing else"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error) && Kinds(Steps) == TArray<EKind>{EKind::Create});
	Options.bWriteIgnore = true;
	// Lore stages a .git or .plastic folder like any other unless told not to, at any depth (a plugin's own repository).
	TestTrue(TEXT("Other version control's folders stay out at any depth"), FHyperAIStudioLore::UnrealIgnoreText().Contains(TEXT("\n.git/\n"))
		&& FHyperAIStudioLore::UnrealIgnoreText().Contains(TEXT("\n.plastic/\n")));
	FFileHelper::SaveStringToFile(FHyperAIStudioLore::UnrealIgnoreText(), *(Folder / TEXT(".loreignore")));
	TestTrue(TEXT("An ignore file that has every pattern is left alone"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error) && !Find(Steps, EKind::WriteIgnore));

	// Each refusal also says which it is, for the page to word in the user's language.
	EHyperAIStudioLoreProblem Problem = EHyperAIStudioLoreProblem::None;
	Options.Name = TEXT("My Game");
	TestFalse(TEXT("A name Lore refuses is refused here"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error, &Problem));
	TestTrue(TEXT("saying what names may hold"), Error.Contains(TEXT("letters")) && Steps.IsEmpty() && Problem == EHyperAIStudioLoreProblem::BadName);
	Options.Name = TEXT("Game");
	Options.ServerUrl = TEXT("127.0.0.1:41337");
	TestFalse(TEXT("So is a server address without its scheme"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error, &Problem) || Problem != EHyperAIStudioLoreProblem::ServerNoScheme);
	Options.ServerUrl.Reset();
	Options.Identity = TEXT("--force");
	TestFalse(TEXT("And an identity that reads as an option"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error, &Problem) || Problem != EHyperAIStudioLoreProblem::BadIdentity);
	Options.Identity.Reset();
	Options.Folder = Folder / TEXT("NotThere");
	TestFalse(TEXT("A folder that does not exist is refused"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error, &Problem) || Problem != EHyperAIStudioLoreProblem::NotAFolder);

	IFileManager::Get().MakeDirectory(*(Folder / TEXT(".lore")), true);
	IFileManager::Get().MakeDirectory(*(Folder / TEXT("Content")), true);
	Options.Folder = Folder;
	TestFalse(TEXT("A folder that is a workspace already is refused"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error, &Problem));
	TestTrue(TEXT("saying so"), Error.Contains(TEXT("already")) && Problem == EHyperAIStudioLoreProblem::AlreadyWorkspace);
	Options.Folder = Folder / TEXT("Content");
	TestFalse(TEXT("So is one inside a workspace"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error, &Problem));
	TestTrue(TEXT("naming the workspace"), Error.Contains(TEXT("inside")) && Problem == EHyperAIStudioLoreProblem::InsideWorkspace);
	FHyperAIStudioLoreWorkspace Workspace = FHyperAIStudioLore::FindWorkspace(Folder / TEXT("Content"));
	TestTrue(TEXT("The workspace is found from a folder inside it"), FPaths::IsSamePath(Workspace.Root, Folder));
	// Local only on the config's word: a workspace HyperAI cannot read is taken for a server one, whose locks protect teammates.
	TestTrue(TEXT("With no config.toml its mode is unknown, not local"), Workspace.Mode == EHyperAIStudioLoreMode::Unknown);
	FFileHelper::SaveStringToFile(TEXT("[store]\nmax_capacity = 2000000\n"), *(Folder / TEXT(".lore/config.toml")));
	TestTrue(TEXT("as with a config that names no remote_url"), FHyperAIStudioLore::FindWorkspace(Folder).Mode == EHyperAIStudioLoreMode::Unknown);
	FFileHelper::SaveStringToFile(TEXT("remote_url = \"\"\n\n[store]\nmax_capacity = 2000000\n"), *(Folder / TEXT(".lore/config.toml")));
	TestTrue(TEXT("A config whose remote_url is empty is local"), FHyperAIStudioLore::FindWorkspace(Folder).Mode == EHyperAIStudioLoreMode::Local);
	FFileHelper::SaveStringToFile(TEXT("remote_url = \"lore://127.0.0.1:41337\"\n"), *(Folder / TEXT(".lore/config.toml")));
	Workspace = FHyperAIStudioLore::FindWorkspace(Folder);
	TestTrue(TEXT("and one that names a server is on it"), Workspace.Mode == EHyperAIStudioLoreMode::Remote && Workspace.RemoteUrl == TEXT("lore://127.0.0.1:41337"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLoreSetupLocalTest,
	"HyperAIStudio.SourceControl.LoreSetupLocal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLoreSetupLocalTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LoreTests;
	const FHyperAIStudioLoreCli Cli = FHyperAIStudioLore::DetectCli();
	if (!Cli.bFound)
	{
		AddWarning(TEXT("Skipped: the lore command is not on PATH, so a real local setup cannot run here. ") + Cli.Error);
		return true;
	}
	// A fresh folder under the bench's Saved folder: a made-up project, never the user's.
	const FString Folder = TempFolder(TEXT("LoreSetup"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Folder, false, true); };
	TestTrue(TEXT("The test folder is under this project's Saved folder"), Folder.StartsWith(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())));
	for (const TCHAR* File : {TEXT("Game.uproject"), TEXT("Content/Maps/Arena.umap"), TEXT("Content/Hero.uasset"), TEXT("Binaries/Win64/Game.dll"),
		TEXT("Intermediate/Build/x.obj"), TEXT("Saved/Logs/Game.log"), TEXT("DerivedDataCache/a.ddp"), TEXT("Plugins/Tool/Binaries/Win64/Tool.dll"),
		TEXT("Plugins/Tool/Content/Icon.uasset"), TEXT("Game.sln"), TEXT(".vs/Game/v17/x"),
		// HyperAI's shared workspace is tracked; its per-machine runtime folder is not.
		TEXT(".hyperai/README.md"), TEXT(".hyperai/runtime/jobs/chat-1.md"), TEXT(".hyperai/runtime/hyperai-status.json"),
		// A per-user agent setting that only the project's .gitignore keeps out.
		TEXT(".claude/settings.json"), TEXT(".claude/settings.local.json")})
	{
		Touch(Folder, File);
	}
	FFileHelper::SaveStringToFile(TEXT("# BEGIN HyperAIStudio\n.hyperai/runtime/\n# END HyperAIStudio\n.claude/settings.local.json\n"), *(Folder / TEXT(".gitignore")));
	FHyperAIStudioLoreSetupOptions Options;
	Options.Folder = Folder;
	Options.Name = TEXT("HyperAI-Lore-Test");
	Options.Identity = TEXT("hyperai-test@example.com");
	Options.CommitMessage = TEXT("Add the test project");
	TArray<FHyperAIStudioLoreSetupStep> Steps;
	FString Error;
	if (!TestTrue(TEXT("The local setup plans: ") + Error, FHyperAIStudioLore::PlanSetup(Options, Steps, Error)))
	{
		return true;
	}
	FString StagedStatus;
	for (const FHyperAIStudioLoreSetupStep& Step : Steps)
	{
		FString Text;
		const bool bOk = FHyperAIStudioLore::RunStep(Cli.Path, Options, Step, Text);
		AddInfo(FString::Printf(TEXT("%s -> %s: %s"), *Step.Command, bOk ? TEXT("ok") : TEXT("failed"), *Text.Left(300)));
		if (!TestTrue(FString::Printf(TEXT("Step \"%s\" runs: %s"), *Step.Title, *Text.Left(600)), bOk))
		{
			return true;
		}
		if (Step.Kind == EHyperAIStudioLoreStepKind::Stage)
		{
			FHyperAIStudioLore::Run(Cli.Path, Folder, {TEXT("--repository"), Folder, TEXT("status")}, 60.0, StagedStatus);
		}
	}
	TestTrue(TEXT("The project's own files were staged"), StagedStatus.Contains(TEXT("Content/Hero.uasset")) && StagedStatus.Contains(TEXT("Game.uproject"))
		&& StagedStatus.Contains(TEXT("Plugins/Tool/Content/Icon.uasset")) && StagedStatus.Contains(TEXT(".loreignore")));
	TestTrue(TEXT("with HyperAI's shared workspace, .gitignore and the shared agent settings"), StagedStatus.Contains(TEXT(".hyperai/README.md"))
		&& StagedStatus.Contains(TEXT(".gitignore")) && StagedStatus.Contains(TEXT(".claude/settings.json")));
	TestFalse(TEXT("and none of Unreal's build output, caches or IDE files: ") + StagedStatus, StagedStatus.Contains(TEXT("Binaries")) || StagedStatus.Contains(TEXT("Intermediate"))
		|| StagedStatus.Contains(TEXT("Saved/")) || StagedStatus.Contains(TEXT("DerivedDataCache")) || StagedStatus.Contains(TEXT(".sln")) || StagedStatus.Contains(TEXT(".vs/")));
	TestFalse(TEXT("nor HyperAI's per-machine runtime files (chat jobs, status), which Lore would push to a server"),
		StagedStatus.Contains(TEXT(".hyperai/runtime")) || StagedStatus.Contains(TEXT("chat-1.md")) || StagedStatus.Contains(TEXT("hyperai-status.json")));
	TestFalse(TEXT("nor what only .gitignore kept out, as Lore does not read it"), StagedStatus.Contains(TEXT("settings.local.json")));

	const FHyperAIStudioLoreWorkspace Workspace = FHyperAIStudioLore::FindWorkspace(Folder);
	TestTrue(TEXT("The folder is a Lore workspace now"), FPaths::IsSamePath(Workspace.Root, Folder));
	TestTrue(TEXT("with a local repository"), Workspace.Mode == EHyperAIStudioLoreMode::Local && Workspace.RemoteUrl.IsEmpty());
	TestEqual(TEXT("recording commits as the identity given"), Workspace.Identity, Options.Identity);
	FString History;
	FHyperAIStudioLore::Run(Cli.Path, Folder, {TEXT("--repository"), Folder, TEXT("history"), TEXT("1"), TEXT("--oneline")}, 60.0, History);
	TestTrue(TEXT("whose first revision is the commit: ") + History, History.Contains(TEXT("Add the test project")));
	FString Scan;
	FHyperAIStudioLore::Run(Cli.Path, Folder, {TEXT("--repository"), Folder, TEXT("status"), TEXT("--scan")}, 60.0, Scan);
	TestFalse(TEXT("Nothing is left to add, though the ignored files are still there: ") + Scan, Scan.Contains(TEXT("\nA ")) || Scan.Contains(TEXT("Untracked")));
	TestTrue(TEXT("and they are"), FPaths::FileExists(Folder / TEXT("Binaries/Win64/Game.dll")));

	TestFalse(TEXT("Setting it up again is refused"), FHyperAIStudioLore::PlanSetup(Options, Steps, Error));

	FString Check;
	const double Start = FPlatformTime::Seconds();
	TestFalse(TEXT("A server that is not there does not answer"), FHyperAIStudioLore::CheckServer(Cli.Path, TEXT("lore://127.0.0.1:1"), Check, 20.0));
	AddInfo(FString::Printf(TEXT("An absent server was reported in %.1f s: %s"), FPlatformTime::Seconds() - Start, *Check.Left(200)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLoreSetupServerTest,
	"HyperAIStudio.SourceControl.LoreSetupServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLoreSetupServerTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LoreTests;
	const FHyperAIStudioLoreCli Cli = FHyperAIStudioLore::DetectCli();
	// Only against a loreserver someone started on this computer (demo mode will do); nothing is started here.
	const FString Server = TEXT("lore://127.0.0.1:41337");
	FString Check;
	if (!Cli.bFound || !FHyperAIStudioLore::CheckServer(Cli.Path, Server, Check, 10.0))
	{
		// A warning, not a note: a run with no server has not checked server repositories, and should say so.
		AddWarning(TEXT("Skipped: no lore command, or no loreserver answers at lore://127.0.0.1:41337 (start one with loreserver to run this)."));
		return true;
	}
	const FString Folder = TempFolder(TEXT("LoreServer"));
	FHyperAIStudioLoreSetupOptions Options;
	Options.Folder = Folder;
	Options.Name = TEXT("hyperai-test-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	Options.ServerUrl = Server;
	ON_SCOPE_EXIT
	{
		FString Ignored;
		FHyperAIStudioLore::Run(Cli.Path, Folder, {TEXT("repository"), TEXT("delete"), FHyperAIStudioLore::RepositoryUrl(Server, Options.Name)}, 30.0, Ignored);
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	};
	Touch(Folder, TEXT("Net.uproject"));
	Touch(Folder, TEXT("Content/Gate.uasset"));
	Touch(Folder, TEXT("Binaries/Win64/Net.dll"));
	TArray<FHyperAIStudioLoreSetupStep> Steps;
	FString Error;
	if (!TestTrue(TEXT("A server setup plans: ") + Error, FHyperAIStudioLore::PlanSetup(Options, Steps, Error)))
	{
		return true;
	}
	for (const FHyperAIStudioLoreSetupStep& Step : Steps)
	{
		FString Text;
		const bool bOk = FHyperAIStudioLore::RunStep(Cli.Path, Options, Step, Text);
		AddInfo(FString::Printf(TEXT("%s -> %s: %s"), *Step.Command, bOk ? TEXT("ok") : TEXT("failed"), *Text.Left(300)));
		if (!TestTrue(FString::Printf(TEXT("Step \"%s\" runs: %s"), *Step.Title, *Text.Left(600)), bOk))
		{
			return true;
		}
	}
	const FHyperAIStudioLoreWorkspace Workspace = FHyperAIStudioLore::FindWorkspace(Folder);
	TestTrue(TEXT("The workspace pushes to the server: ") + Workspace.RemoteUrl, Workspace.Mode == EHyperAIStudioLoreMode::Remote && Workspace.RemoteUrl.StartsWith(Server));
	FString Status;
	FHyperAIStudioLore::Run(Cli.Path, Folder, {TEXT("--repository"), Folder, TEXT("status"), TEXT("--revision-only")}, 30.0, Status);
	TestTrue(TEXT("and the server has the first revision: ") + Status, Status.Contains(TEXT("in sync with remote")));
	// What agent edits rely on in a server repository: a file lock, taken and let go.
	FString Lock;
	TestTrue(TEXT("A file can be locked: ") + Lock, FHyperAIStudioLore::Run(Cli.Path, Folder, {TEXT("--repository"), Folder, TEXT("lock"), TEXT("acquire"), Folder / TEXT("Content/Gate.uasset")}, 30.0, Lock));
	TestTrue(TEXT("and released: ") + Lock, FHyperAIStudioLore::Run(Cli.Path, Folder, {TEXT("--repository"), Folder, TEXT("lock"), TEXT("release"), Folder / TEXT("Content/Gate.uasset")}, 30.0, Lock));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLoreSetupPanelRunTest,
	"HyperAIStudio.SourceControl.LoreSetupPanelRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLoreSetupPanelRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LoreTests;
	if (!FHyperAIStudioLore::DetectCli().bFound)
	{
		AddWarning(TEXT("Skipped: the lore command is not on PATH, so the page cannot set a folder up here."));
		return true;
	}
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const FString OldServer = Settings->LoreServerUrl;
	const FString Folder = TempFolder(TEXT("LorePage"));
	ON_SCOPE_EXIT
	{
		Settings->LoreServerUrl = OldServer;
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	};
	Settings->LoreServerUrl.Reset();
	for (const TCHAR* File : {TEXT("Page.uproject"), TEXT("Content/Door.uasset"), TEXT("Binaries/Win64/Page.dll"), TEXT("Saved/Logs/Page.log")})
	{
		Touch(Folder, File);
	}
	// A project kept in Git too: the page offers to copy its .gitignore, which Lore does not read, and does by default.
	FFileHelper::SaveStringToFile(TEXT(".claude/settings.local.json\n"), *(Folder / TEXT(".gitignore")));
	// The page as Settings > Version control shows it, pointed at a made-up project; Set up's question is answered here.
	const TSharedRef<SHyperAIStudioLoreSetupPanel> Panel = SNew(SHyperAIStudioLoreSetupPanel).Folder(Folder);
	TestEqual(TEXT("It plans a local setup"), Panel->GetPlan().Num(), 4);
	TestTrue(TEXT("copying the .gitignore's rules into .loreignore"), Panel->GetOptions().bCopyGitIgnore && Panel->GetPlan()[0].Detail.Contains(TEXT("1 rule(s) of the project's .gitignore")));
	Panel->RunPlanForTests();
	TestTrue(TEXT("and runs it in the background"), Panel->IsRunning());
	const double End = FPlatformTime::Seconds() + 120.0;
	while (Panel->IsRunning() && FPlatformTime::Seconds() < End)
	{
		FTSTicker::GetCoreTicker().Tick(0.02f);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::Sleep(0.02f);
	}
	int32 Done = 0;
	int32 Failed = 0;
	Panel->CountSteps(Done, Failed);
	TestFalse(TEXT("It finishes"), Panel->IsRunning());
	TestEqual(TEXT("every step done"), Done, 4);
	TestEqual(TEXT("none failed: ") + Panel->GetResult().ToString(), Failed, 0);
	TestTrue(TEXT("and says so: ") + Panel->GetResult().ToString(), Panel->GetResult().ToString().Contains(TEXT("Lore workspace")));
	TestTrue(TEXT("The folder is a local Lore workspace"), FHyperAIStudioLore::FindWorkspace(Folder).Mode == EHyperAIStudioLoreMode::Local);
	FString LoreIgnore;
	FFileHelper::LoadFileToString(LoreIgnore, *(Folder / TEXT(".loreignore")));
	TestTrue(TEXT("Its .loreignore keeps out HyperAI's runtime folder and what .gitignore did: ") + LoreIgnore,
		LoreIgnore.Contains(TEXT("/.hyperai/runtime/")) && LoreIgnore.Contains(TEXT(".claude/settings.local.json")));
	HyperAIStudio::Tests::SaveWidgetShot(Panel, FIntPoint(1000, 1600), TEXT("lore-setup-done"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLoreLocalModeTest,
	"HyperAIStudio.SourceControl.LoreLocalMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLoreLocalModeTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LoreTests;
	if (HasUnrealProvider())
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
	const FString Folder = FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir() / (TEXT("LoreLocal-") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
	const FString Script = Folder / TEXT("fake_lore.py");
	FFileHelper::SaveStringToFile(FakeLore, *Script, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	// A local repository: no server in its config, so lore refuses locks ("Unable to acquire lock while offline").
	FHyperAIStudioSourceControl::SetLoreForTests(TEXT("python"), FString::Printf(TEXT("-u \"%s\""), *Script), Folder, FString());
	const FString Suffix = FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
	const FString Existing = TEXT("/Game/HyperAITests/SourceControl/SC_LocalOld_") + Suffix;
	const FString New = TEXT("/Game/HyperAITests/SourceControl/SC_LocalNew_") + Suffix;
	TArray<UObject*> Made;
	auto SaveCurve = [&Made](const FString& PackageName)
	{
		UPackage* Package = CreatePackage(*PackageName);
		UCurveFloat* Curve = NewObject<UCurveFloat>(Package, *FPackageName::GetShortName(PackageName), RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Curve);
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::SavePackage(Package, Curve, *FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension()), Args);
		Made.Add(Curve);
	};
	ON_SCOPE_EXIT
	{
		FHyperAIStudioSourceControl::SetLoreForTests(FString(), FString(), FString(), FString());
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
		return Text;
	};
	SaveCurve(Existing);

	FString Error;
	TestTrue(TEXT("An agent may edit an existing asset in a local repository: ") + Error, FHyperAIStudioSourceControl::MakeEditable({Existing}, Error));
	TestFalse(TEXT("without asking for a lock, which a local repository cannot give"), Calls().Contains(TEXT("\"lock\"")));
	TestTrue(TEXT("New assets too"), FHyperAIStudioSourceControl::MakeEditable({New}, Error));
	SaveCurve(New);
	for (int32 Tick = 0; Tick < 5; ++Tick)
	{
		FTSTicker::GetCoreTicker().Tick(0.02f);
	}
	TestTrue(TEXT("A saved agent file is marked dirty for the next commit"), Calls().Contains(TEXT("\"dirty\"")));

	const FHyperAISourceControlStatusReport Status = UHyperAIStudioSourceControlToolset::hyper_source_control_status(FHyperAISourceControlStatusRequest());
	TestEqual(TEXT("Status reports the local mode"), Status.Lore.Mode, FString(TEXT("local")));
	TestTrue(TEXT("with the workspace, and no server"), FPaths::IsSamePath(Status.Lore.Workspace, Folder) && Status.Lore.Server.IsEmpty());
	TestTrue(TEXT("each agent file changed, not locked"), Status.Files.Num() == 2 && Algo::AllOf(Status.Files, [](const FHyperAISourceControlFile& File) { return File.State == TEXT("changed"); }));
	TestTrue(TEXT("and says a submit only commits here: ") + Status.Diagnostic, Status.Diagnostic.Contains(TEXT("local")) && Status.Diagnostic.Contains(TEXT("no push")));

	FHyperAISourceControlSubmitRequest Submit;
	Submit.Description = TEXT("Add the dash curves");
	const FHyperAISourceControlSubmitReport Checked = UHyperAIStudioSourceControlToolset::hyper_source_control_submit(Submit);
	TestTrue(TEXT("A dry run says it commits to the local repository: ") + Checked.Diagnostic, Checked.Status == TEXT("checked") && Checked.Diagnostic.Contains(TEXT("local Lore repository")));
	Submit.bDryRun = false;
	const FHyperAISourceControlSubmitReport Asked = UHyperAIStudioSourceControlToolset::hyper_source_control_submit(Submit);
	TestEqual(TEXT("For real it still waits for the user"), Asked.Status, FString(TEXT("awaiting_user_approval")));
	TestTrue(TEXT("Approved, it goes: ") + Error, FHyperAIStudioApprovalGate::Approve(Asked.OperationId, Error));
	// The lore commands run on a worker thread: the approval returns at once, and the outcome lands on a later tick.
	TestTrue(TEXT("in the background, so the editor is not held while it commits"), FHyperAIStudioSourceControl::IsSubmitting() && !FHyperAIStudioSourceControl::GetAgentFiles().IsEmpty());
	const FHyperAISourceControlSubmitReport Again = UHyperAIStudioSourceControlToolset::hyper_source_control_submit(Submit);
	TestEqual(TEXT("A second submit waits for the first"), Again.Status, FString(TEXT("submit_running")));
	TestTrue(TEXT("It finishes"), WaitForSubmit());
	const FString Log = Calls();
	TestTrue(TEXT("staged and committed with the description"), Log.Contains(TEXT("\"stage\"")) && Log.Contains(TEXT("\"commit\", \"Add the dash curves\"")));
	TestFalse(TEXT("with no push and no lock to let go"), Log.Contains(TEXT("\"push\"")) || Log.Contains(TEXT("\"release\"")));
	TestFalse(TEXT("A local repository's status asked for no sign-ins"), Log.Contains(TEXT("\"auth\"")));
	TestTrue(TEXT("and nothing of the agents' is left to submit"), FHyperAIStudioSourceControl::GetAgentFiles().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioLoreSettingsTest,
	"HyperAIStudio.SourceControl.LoreSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioLoreSettingsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::LoreTests;
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const FString OldServer = Settings->LoreServerUrl;
	const FString OldIdentity = Settings->LoreIdentity;
	const FString OldCommand = FHyperAIStudioLore::GetCommandOverride();
	const FString Folder = TempFolder(TEXT("LorePanel"));
	auto SetCommandInConfig = [](const FString& Value)
	{
		GConfig->SetString(TEXT("HyperAIStudio.Lore"), TEXT("Command"), *Value, GEditorPerProjectIni);
	};
	ON_SCOPE_EXIT
	{
		Settings->LoreServerUrl = OldServer;
		Settings->LoreIdentity = OldIdentity;
		Settings->SaveConfig();
		SetCommandInConfig(OldCommand);
		IFileManager::Get().DeleteDirectory(*Folder, false, true);
	};
	SetCommandInConfig(FString());

	// The remote settings survive a save and a load from the user's config.
	Settings->LoreServerUrl = TEXT("lore://lore.example.com:41337");
	Settings->LoreIdentity = TEXT("alex@example.com");
	Settings->SaveConfig();
	Settings->LoreServerUrl = TEXT("changed");
	Settings->LoreIdentity = TEXT("changed");
	Settings->LoadConfig();
	TestEqual(TEXT("The Lore Server comes back from the config"), Settings->LoreServerUrl, FString(TEXT("lore://lore.example.com:41337")));
	TestEqual(TEXT("as does the Lore Identity"), Settings->LoreIdentity, FString(TEXT("alex@example.com")));

	const FHyperAISourceControlStatusReport Status = UHyperAIStudioSourceControlToolset::hyper_source_control_status(FHyperAISourceControlStatusRequest());
	TestEqual(TEXT("Status reports the configured server"), Status.Lore.ConfiguredServer, FString(TEXT("lore://lore.example.com:41337")));
	if (!HasUnrealProvider() && Status.Lore.Workspace.IsEmpty())
	{
		TestEqual(TEXT("This bench is in no Lore workspace"), Status.Lore.Mode, FString(TEXT("none")));
		TestTrue(TEXT("and status says where to set one up: ") + Status.Lore.Next, Status.Lore.Next.Contains(TEXT("Settings > Version control")));
	}

	// The panel takes the server from the settings and plans a server setup for the folder it is given.
	Touch(Folder, TEXT("Panel.uproject"));
	const TSharedRef<SHyperAIStudioLoreSetupPanel> Panel = SNew(SHyperAIStudioLoreSetupPanel).Folder(Folder);
	const FHyperAIStudioLoreSetupOptions Options = Panel->GetOptions();
	TestEqual(TEXT("The panel offers the configured server"), Options.ServerUrl, FString(TEXT("lore://lore.example.com:41337")));
	TestEqual(TEXT("and the identity"), Options.Identity, FString(TEXT("alex@example.com")));
	TestTrue(TEXT("and lists the steps before anything runs: ") + Panel->GetPlanError(), Panel->GetPlan().Num() == 6 && Panel->GetPlan()[0].Kind == EHyperAIStudioLoreStepKind::CheckServer);
	TestFalse(TEXT("Showing them changes nothing"), IFileManager::Get().DirectoryExists(*(Folder / TEXT(".lore"))) || FPaths::FileExists(Folder / TEXT(".loreignore")));
	// Deleting .lore undoes neither the repository a server setup makes nor the upload, so the question says what does.
	const FString ServerQuestion = Panel->GetConfirmQuestion().ToString();
	TestTrue(TEXT("Set up's question for a server says the project is uploaded, and how to remove it there: ") + ServerQuestion,
		ServerQuestion.Contains(TEXT("uploads")) && ServerQuestion.Contains(FString::Printf(TEXT("lore repository delete lore://lore.example.com:41337/%s"), *Options.Name))
		&& !ServerQuestion.Contains(TEXT("your project files are not changed")));
	HyperAIStudio::Tests::SaveWidgetShot(Panel, FIntPoint(1000, 1500), TEXT("lore-setup-server"));

	Settings->LoreServerUrl.Reset();
	const TSharedRef<SHyperAIStudioLoreSetupPanel> Local = SNew(SHyperAIStudioLoreSetupPanel).Folder(Folder);
	TestTrue(TEXT("With no server it plans a local repository"), Local->GetOptions().ServerUrl.IsEmpty() && Local->GetPlan().Num() == 4
		&& Local->GetPlan()[1].Arguments.Contains(TEXT("--offline")));
	const FString LocalQuestion = Local->GetConfirmQuestion().ToString();
	TestTrue(TEXT("whose question says deleting .lore undoes it: ") + LocalQuestion, LocalQuestion.Contains(TEXT("delete the .lore folder")) && !LocalQuestion.Contains(TEXT("repository delete")));
	HyperAIStudio::Tests::SaveWidgetShot(Local, FIntPoint(1000, 1500), TEXT("lore-setup-local"));

	// Which program HyperAI runs as lore is the user's choice, made on this page: no setting agents' tools can write names it.
	TestNull(TEXT("HyperAI's settings object has no Lore Command"), UHyperAIStudioSettings::StaticClass()->FindPropertyByName(TEXT("LoreCommand")));
	EHyperAIStudioLoreProblem Problem = EHyperAIStudioLoreProblem::None;
	TestFalse(TEXT("A lore command that does not exist is refused"), FHyperAIStudioLore::SetCommandOverride(Folder / TEXT("lore.exe"), Problem));
	TestTrue(TEXT("saying so"), Problem == EHyperAIStudioLoreProblem::CliCommandMissing);
	const FString Script = Folder / TEXT("lore.cmd");
	FFileHelper::SaveStringToFile(TEXT("@echo lore 9.9.9\r\n"), *Script);
	TestFalse(TEXT("So is a file that is not lore.exe, such as a script"), FHyperAIStudioLore::SetCommandOverride(Script, Problem));
	TestTrue(TEXT("saying so"), Problem == EHyperAIStudioLoreProblem::CliCommandNotLore);
	SetCommandInConfig(Script);
	FString Path;
	TestFalse(TEXT("Written into the config by other means, it is still never run"), FHyperAIStudioLore::FindExecutable(Path, &Problem));
	const FHyperAIStudioLoreCli Refused = FHyperAIStudioLore::DetectCli();
	const TSharedRef<SHyperAIStudioLoreSetupPanel> RefusedPage = SNew(SHyperAIStudioLoreSetupPanel).Folder(Folder);
	TestTrue(TEXT("and the page says why, in its own words"), !Refused.bFound && Refused.Problem == EHyperAIStudioLoreProblem::CliCommandNotLore
		&& !RefusedPage->GetCli().bFound && RefusedPage->GetCli().Problem == EHyperAIStudioLoreProblem::CliCommandNotLore);
	SetCommandInConfig(FString());

	// A lore.exe that answered is not started again until its file changes.
	const FHyperAIStudioLoreCli Real = FHyperAIStudioLore::DetectCli();
	if (!Real.bFound || Real.Version.IsEmpty())
	{
		AddWarning(TEXT("The lore command is not installed here, so a chosen lore.exe was not exercised."));
	}
	else
	{
		const FString Copy = Folder / TEXT("bin") / TEXT("lore.exe");
		IFileManager::Get().MakeDirectory(*(Folder / TEXT("bin")), /*Tree=*/true);
		TestTrue(TEXT("A copy of lore.exe is made"), IFileManager::Get().Copy(*Copy, *Real.Path) == COPY_OK);
		TestTrue(TEXT("and kept as the lore command"), FHyperAIStudioLore::SetCommandOverride(Copy, Problem) && FPaths::IsSamePath(FHyperAIStudioLore::GetCommandOverride(), Copy));
		const FHyperAIStudioLoreCli First = FHyperAIStudioLore::DetectCli();
		TestTrue(TEXT("It is found there, with its version: ") + First.Error, First.bFound && FPaths::IsSamePath(First.Path, Copy) && First.Version == Real.Version);
		// The same size and time, but no longer a program: an answer now can only come from what was remembered.
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Copy);
		TArray<uint8> Zeros;
		Zeros.SetNumZeroed(IFileManager::Get().FileSize(*Copy));
		FFileHelper::SaveArrayToFile(Zeros, *Copy);
		IFileManager::Get().SetTimeStamp(*Copy, Stamp);
		if (IFileManager::Get().GetTimeStamp(*Copy) != Stamp)
		{
			AddWarning(TEXT("This file system did not keep the copy's time, so remembering the version was not checked."));
		}
		else
		{
			TestEqual(TEXT("Asking again starts nothing: the version is remembered"), FHyperAIStudioLore::DetectCli().Version, Real.Version);
		}
		IFileManager::Get().SetTimeStamp(*Copy, Stamp + FTimespan::FromMinutes(1.0));
		const FHyperAIStudioLoreCli Changed = FHyperAIStudioLore::DetectCli();
		TestTrue(TEXT("Once the file changes it is asked again, and this one is no program"), Changed.bFound && Changed.Version.IsEmpty() && Changed.Problem == EHyperAIStudioLoreProblem::CliNoVersion);
		SetCommandInConfig(FString());
	}

	const TSharedRef<SHyperAIStudioSettingsPage> Page = SNew(SHyperAIStudioSettingsPage);
	Page->ShowSection(TEXT("VersionControl"));
	TestEqual(TEXT("Settings has a Version control section"), Page->GetSection(), FName(TEXT("VersionControl")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
