// Games by Hyper 2026.

#include "HyperAIStudioLore.h"

#include "Algo/AllOf.h"
#include "Algo/AnyOf.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioLore, Log, All);

const TCHAR* FHyperAIStudioLore::InstallGuideUrl = TEXT("https://epicgames.github.io/lore/how-to/install-lore-cli/");
const TCHAR* FHyperAIStudioLore::HomeUrl = TEXT("https://lore.org");

namespace HyperAIStudio::Lore::Private
{
	constexpr int32 MaxOutputCharacters = 8000;
	/** Making the repository and checking a server are quick; staging and committing a large project are not. */
	constexpr double QuickSeconds = 60.0;
	constexpr double LongSeconds = 60.0 * 60.0;
	const TCHAR* const IgnoreHeader = TEXT("# Unreal Engine: build output, caches and IDE files (added by HyperAI Studio)");

	/** The lines of the Unreal ignore list that are patterns. */
	TArray<FString> UnrealPatterns()
	{
		TArray<FString> Lines;
		FHyperAIStudioLore::UnrealIgnoreText().ParseIntoArrayLines(Lines);
		return Lines.FilterByPredicate([](const FString& Line) { return !Line.IsEmpty() && !Line.StartsWith(TEXT("#")); });
	}

	/** A TOML string value on a top-level key's line: key = "value". */
	bool ReadTomlString(const FString& Line, const TCHAR* Key, FString& OutValue)
	{
		FString Left;
		FString Right;
		if (!Line.Split(TEXT("="), &Left, &Right) || Left.TrimStartAndEnd() != Key)
		{
			return false;
		}
		Right.TrimStartAndEndInline();
		if (Right.Len() >= 2 && (Right.StartsWith(TEXT("\"")) || Right.StartsWith(TEXT("'"))))
		{
			const TCHAR Quote = Right[0];
			int32 End = INDEX_NONE;
			for (int32 Index = 1; Index < Right.Len(); ++Index)
			{
				if (Right[Index] == TEXT('\\') && Quote == TEXT('"'))
				{
					++Index;
					continue;
				}
				if (Right[Index] == Quote)
				{
					End = Index;
					break;
				}
			}
			if (End != INDEX_NONE)
			{
				OutValue = Right.Mid(1, End - 1).Replace(TEXT("\\\\"), TEXT("\\")).Replace(TEXT("\\\""), TEXT("\""));
				return true;
			}
		}
		return false;
	}

	/** A token, as lore auth list could print one only when asked with --with-token: never shown, never kept. */
	bool LooksLikeToken(const FString& Word)
	{
		if (Word.StartsWith(TEXT("eyJ")) && Word.Contains(TEXT(".")))
		{
			return true;
		}
		int32 Run = 0;
		for (const TCHAR Char : Word)
		{
			Run = FChar::IsAlnum(Char) || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('.') || Char == TEXT('=') || Char == TEXT('+') || Char == TEXT('/') ? Run + 1 : 0;
			if (Run >= 40)
			{
				return true;
			}
		}
		return false;
	}
}

bool FHyperAIStudioLore::FindExecutable(FString& OutPath)
{
	OutPath.Reset();
	const FString Setting = GetDefault<UHyperAIStudioSettings>()->LoreCommand.TrimStartAndEnd().TrimQuotes();
	if (!Setting.IsEmpty())
	{
		if (FPaths::FileExists(Setting))
		{
			OutPath = FPaths::ConvertRelativePathToFull(Setting);
			return true;
		}
		return false;
	}
	return FHyperAIStudioService::FindExecutableOnPath(TEXT("lore"), OutPath);
}

bool FHyperAIStudioLore::ParseVersion(const FString& Output, FString& OutVersion, FString& OutBuild)
{
	OutVersion.Reset();
	OutBuild.Reset();
	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		TArray<FString> Words;
		Line.TrimStartAndEnd().ParseIntoArrayWS(Words);
		// "lore 0.10.1+1476-..." from the CLI, "loreserver 0.10.1" from the server, or the bare version.
		const FString Word = Words.Num() >= 2 && Words[0].StartsWith(TEXT("lore")) ? Words[1] : Words.Num() == 1 ? Words[0] : FString();
		FString Version = Word;
		FString Build;
		Word.Split(TEXT("+"), &Version, &Build);
		Version.RemoveFromStart(TEXT("v"));
		TArray<FString> Parts;
		FString Core = Version;
		Version.Split(TEXT("-"), &Core, nullptr);
		Core.ParseIntoArray(Parts, TEXT("."));
		if (Parts.Num() >= 2 && Parts.Num() <= 4 && Algo::AllOf(Parts, [](const FString& Part) { return !Part.IsEmpty() && Part.IsNumeric(); }))
		{
			OutVersion = Version;
			OutBuild = Build;
			return true;
		}
	}
	return false;
}

FHyperAIStudioLoreCli FHyperAIStudioLore::DetectCli()
{
	FHyperAIStudioLoreCli Cli;
	if (!FindExecutable(Cli.Path))
	{
		Cli.Error = GetDefault<UHyperAIStudioSettings>()->LoreCommand.TrimStartAndEnd().IsEmpty()
			? TEXT("The lore command is not on PATH, nor in %USERPROFILE%\\bin where Lore's installer puts it.")
			: TEXT("The Lore Command setting names a file that does not exist.");
		return Cli;
	}
	Cli.bFound = true;
	FString Text;
	int32 Code = -1;
	// --version takes no repository and asks nothing; the flags Run adds are harmless to it.
	if (!Run(Cli.Path, FString(), {TEXT("--version")}, 10.0, Text, &Code) || !ParseVersion(Text, Cli.Version, Cli.Build))
	{
		Cli.Error = FString::Printf(TEXT("lore --version did not answer as expected (exit %d): %s"), Code, *Text.Left(300));
	}
	return Cli;
}

void FHyperAIStudioLore::ReadWorkspaceConfig(const FString& ConfigToml, FString& OutRemoteUrl, FString& OutIdentity)
{
	OutRemoteUrl.Reset();
	OutIdentity.Reset();
	TArray<FString> Lines;
	ConfigToml.ParseIntoArrayLines(Lines);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		// Top-level keys only: a table header ends them.
		if (Line.StartsWith(TEXT("[")))
		{
			break;
		}
		HyperAIStudio::Lore::Private::ReadTomlString(Line, TEXT("remote_url"), OutRemoteUrl);
		HyperAIStudio::Lore::Private::ReadTomlString(Line, TEXT("identity"), OutIdentity);
	}
}

FHyperAIStudioLoreWorkspace FHyperAIStudioLore::FindWorkspace(const FString& InFolder)
{
	FHyperAIStudioLoreWorkspace Workspace;
	for (FString Folder = FPaths::ConvertRelativePathToFull(InFolder); !Folder.IsEmpty(); Folder = FPaths::GetPath(Folder))
	{
		Folder.RemoveFromEnd(TEXT("/"));
		if (IFileManager::Get().DirectoryExists(*(Folder / TEXT(".lore"))))
		{
			Workspace.Root = Folder;
			break;
		}
		if (FPaths::GetPath(Folder) == Folder)
		{
			break;
		}
	}
	if (Workspace.Root.IsEmpty())
	{
		return Workspace;
	}
	FString Config;
	FFileHelper::LoadFileToString(Config, *(Workspace.Root / TEXT(".lore") / TEXT("config.toml")));
	ReadWorkspaceConfig(Config, Workspace.RemoteUrl, Workspace.Identity);
	Workspace.Mode = Workspace.RemoteUrl.TrimStartAndEnd().IsEmpty() ? EHyperAIStudioLoreMode::Local : EHyperAIStudioLoreMode::Remote;
	return Workspace;
}

FString FHyperAIStudioLore::RepositoryName(const FString& ProjectName)
{
	FString Name;
	for (const TCHAR Char : ProjectName.TrimStartAndEnd())
	{
		const bool bAllowed = FChar::IsAlnum(Char) && Char < 128;
		Name.AppendChar(bAllowed || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('.') ? Char : TEXT('-'));
	}
	while (Name.ReplaceInline(TEXT("--"), TEXT("-")) > 0)
	{
	}
	Name = Name.TrimChar(TEXT('-')).TrimChar(TEXT('.'));
	return Name.IsEmpty() ? FString(TEXT("UnrealProject")) : Name.Left(100);
}

bool FHyperAIStudioLore::IsValidRepositoryName(const FString& Name)
{
	if (Name.IsEmpty() || Name.Len() > 100 || Name.StartsWith(TEXT("/")) || Name.StartsWith(TEXT("-")) || Name.StartsWith(TEXT(".")) || Name.EndsWith(TEXT("/"))
		|| Name.Contains(TEXT("..")) || Name.Contains(TEXT("//")))
	{
		return false;
	}
	return Algo::AllOf(Name, [](const TCHAR Char) { return (FChar::IsAlnum(Char) && Char < 128) || Char == TEXT('-') || Char == TEXT('_') || Char == TEXT('.') || Char == TEXT('/'); });
}

bool FHyperAIStudioLore::IsValidServerUrl(const FString& InUrl, FString& OutError)
{
	const FString Url = InUrl.TrimStartAndEnd();
	FString Scheme;
	FString Rest;
	if (!Url.Split(TEXT("://"), &Scheme, &Rest) || Scheme.IsEmpty() || Rest.IsEmpty())
	{
		OutError = TEXT("A Lore server is an address such as lore://127.0.0.1:41337 (scheme, host, and port when it is not the default).");
		return false;
	}
	if (!FChar::IsAlpha(Scheme[0]) || !Algo::AllOf(Scheme, [](const TCHAR Char) { return FChar::IsAlpha(Char) || Char == TEXT('+') || Char == TEXT('-'); }))
	{
		OutError = FString::Printf(TEXT("%s is not a URL scheme."), *Scheme);
		return false;
	}
	if (Algo::AnyOf(Url, [](const TCHAR Char) { return FChar::IsWhitespace(Char) || Char == TEXT('"') || Char == TEXT('\'') || Char == TEXT('`') || Char < 32; }))
	{
		OutError = TEXT("A server address has no spaces or quotes.");
		return false;
	}
	FString Host = Rest;
	Rest.Split(TEXT("/"), &Host, nullptr);
	if (Host.IsEmpty() || Host.StartsWith(TEXT(":")) || Host.Contains(TEXT("@")))
	{
		OutError = TEXT("A server address names its host, and carries no user name or password.");
		return false;
	}
	OutError.Reset();
	return true;
}

FString FHyperAIStudioLore::RepositoryUrl(const FString& ServerUrl, const FString& Name)
{
	FString Server = ServerUrl.TrimStartAndEnd();
	Server.RemoveFromEnd(TEXT("/"));
	return Server + TEXT("/") + Name;
}

FString FHyperAIStudioLore::LoginCommand(const FString& ServerUrl)
{
	const FString Server = ServerUrl.TrimStartAndEnd();
	return Server.IsEmpty() ? FString(TEXT("lore login")) : TEXT("lore login ") + JoinArguments({Server});
}

FString FHyperAIStudioLore::UnrealIgnoreText()
{
	return FString(HyperAIStudio::Lore::Private::IgnoreHeader) + TEXT("\n")
		TEXT("/Binaries/\n")
		TEXT("/Intermediate/\n")
		TEXT("/Saved/\n")
		TEXT("/DerivedDataCache/\n")
		TEXT("Plugins/**/Binaries/\n")
		TEXT("Plugins/**/Intermediate/\n")
		TEXT("/.vs/\n")
		TEXT("/.idea/\n")
		TEXT("/.vscode/\n")
		TEXT(".git/\n")
		TEXT("*.sln\n")
		TEXT("*.suo\n")
		TEXT("*.sdf\n")
		TEXT("*.opensdf\n")
		TEXT("*.VC.db\n")
		TEXT("*.VC.opendb\n")
		TEXT("*.xcodeproj\n")
		TEXT("*.xcworkspace\n")
		TEXT(".DS_Store\n")
		TEXT("Thumbs.db\n");
}

FString FHyperAIStudioLore::MergeIgnore(const FString& Existing, bool& bOutChanged)
{
	TArray<FString> Have;
	Existing.ParseIntoArrayLines(Have);
	for (FString& Line : Have)
	{
		Line.TrimStartAndEndInline();
	}
	TArray<FString> Missing;
	for (const FString& Pattern : HyperAIStudio::Lore::Private::UnrealPatterns())
	{
		if (!Have.Contains(Pattern))
		{
			Missing.Add(Pattern);
		}
	}
	bOutChanged = !Missing.IsEmpty();
	if (!bOutChanged)
	{
		return Existing;
	}
	FString Out = Existing;
	if (!Out.IsEmpty() && !Out.EndsWith(TEXT("\n")))
	{
		Out += TEXT("\n");
	}
	if (!Out.IsEmpty())
	{
		Out += TEXT("\n");
	}
	if (!Have.Contains(HyperAIStudio::Lore::Private::IgnoreHeader))
	{
		Out += FString(HyperAIStudio::Lore::Private::IgnoreHeader) + TEXT("\n");
	}
	return Out + FString::Join(Missing, TEXT("\n")) + TEXT("\n");
}

bool FHyperAIStudioLore::PlanSetup(const FHyperAIStudioLoreSetupOptions& Options, TArray<FHyperAIStudioLoreSetupStep>& OutSteps, FString& OutError)
{
	OutSteps.Reset();
	FString Root = Options.Folder.TrimStartAndEnd().IsEmpty() ? FString() : FPaths::ConvertRelativePathToFull(Options.Folder);
	Root.RemoveFromEnd(TEXT("/"));
	if (Root.IsEmpty() || !IFileManager::Get().DirectoryExists(*Root))
	{
		OutError = FString::Printf(TEXT("%s is not a folder."), *Options.Folder);
		return false;
	}
	const FHyperAIStudioLoreWorkspace Existing = FindWorkspace(Root);
	if (!Existing.Root.IsEmpty())
	{
		OutError = FPaths::IsSamePath(Existing.Root, Root)
			? FString::Printf(TEXT("%s is a Lore workspace already."), *Root)
			: FString::Printf(TEXT("%s is inside the Lore workspace %s already; set up the workspace's own folder, or none."), *Root, *Existing.Root);
		return false;
	}
	if (!IsValidRepositoryName(Options.Name))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a repository name Lore accepts: letters, digits, '/', '-', '_' and '.', up to 100."), *Options.Name);
		return false;
	}
	const FString Server = Options.ServerUrl.TrimStartAndEnd();
	const bool bRemote = !Server.IsEmpty();
	if (bRemote && !IsValidServerUrl(Server, OutError))
	{
		return false;
	}
	const FString Identity = Options.Identity.TrimStartAndEnd();
	if (Identity.StartsWith(TEXT("-")) || Identity.Len() > 200 || Algo::AnyOf(Identity, [](const TCHAR Char) { return Char == TEXT('"') || Char < 32; }))
	{
		OutError = TEXT("The identity is a name or email: no quotes or control characters, and not starting with '-'.");
		return false;
	}
	const FString CommitMessage = Options.CommitMessage.TrimStartAndEnd().IsEmpty() ? FString(TEXT("Add the Unreal project")) : Options.CommitMessage.TrimStartAndEnd();
	auto Lore = [](const TArray<FString>& Arguments) { return TEXT("lore ") + JoinArguments(Arguments); };

	if (bRemote)
	{
		FHyperAIStudioLoreSetupStep& Step = OutSteps.AddDefaulted_GetRef();
		Step.Kind = EHyperAIStudioLoreStepKind::CheckServer;
		Step.Arguments = {TEXT("repository"), TEXT("list"), Server};
		Step.Title = FString::Printf(TEXT("Check that the Lore server %s answers"), *Server);
		Step.Detail = TEXT("Reads the server's repository list. Changes nothing.");
		Step.Command = Lore(Step.Arguments);
		Step.Undo = TEXT("Nothing to undo.");
	}
	if (Options.bWriteIgnore)
	{
		FString Current;
		const FString IgnoreFile = Root / TEXT(".loreignore");
		const bool bHasFile = FFileHelper::LoadFileToString(Current, *IgnoreFile);
		bool bChanged = false;
		MergeIgnore(Current, bChanged);
		if (bChanged)
		{
			FHyperAIStudioLoreSetupStep& Step = OutSteps.AddDefaulted_GetRef();
			Step.Kind = EHyperAIStudioLoreStepKind::WriteIgnore;
			Step.Title = bHasFile ? TEXT("Add Unreal's build output and caches to .loreignore") : TEXT("Write .loreignore for Unreal's build output and caches");
			Step.Detail = FString::Printf(TEXT("%s %s, so Binaries, Intermediate, Saved, DerivedDataCache, plugin builds, IDE files and *.sln stay out of the repository."),
				bHasFile ? TEXT("Appends lines to") : TEXT("Writes"), *IgnoreFile);
			Step.Command = IgnoreFile;
			Step.Undo = bHasFile ? TEXT("Remove the lines under the HyperAI Studio heading in .loreignore.") : TEXT("Delete .loreignore.");
			Step.bChangesFolder = true;
		}
	}
	{
		FHyperAIStudioLoreSetupStep& Step = OutSteps.AddDefaulted_GetRef();
		Step.Kind = EHyperAIStudioLoreStepKind::Create;
		if (!Identity.IsEmpty())
		{
			Step.Arguments = {TEXT("--identity"), Identity};
		}
		Step.Arguments.Append({TEXT("repository"), TEXT("create")});
		if (bRemote)
		{
			Step.Arguments.Add(RepositoryUrl(Server, Options.Name));
			Step.Title = FString::Printf(TEXT("Create the repository %s on %s, with this folder as its workspace"), *Options.Name, *Server);
			Step.Detail = FString::Printf(TEXT("Registers %s on the server and adds a .lore folder to %s for Lore's own state. Your files are not changed."), *Options.Name, *Root);
			Step.Undo = FString::Printf(TEXT("Delete the .lore folder in %s; lore repository delete %s removes it from the server."), *Root, *RepositoryUrl(Server, Options.Name));
		}
		else
		{
			Step.Arguments.Append({TEXT("--offline"), Options.Name});
			Step.Title = FString::Printf(TEXT("Create the local repository %s in this folder"), *Options.Name);
			Step.Detail = FString::Printf(TEXT("Adds a .lore folder to %s, where Lore keeps the history on this computer. No server is used; your files are not changed."), *Root);
			Step.Undo = FString::Printf(TEXT("Delete the .lore folder in %s: the repository and its history go, your files stay."), *Root);
		}
		Step.Command = Lore(Step.Arguments);
		Step.bChangesFolder = true;
	}
	if (Options.bFirstCommit)
	{
		{
			FHyperAIStudioLoreSetupStep& Step = OutSteps.AddDefaulted_GetRef();
			Step.Kind = EHyperAIStudioLoreStepKind::Stage;
			Step.Arguments = {TEXT("stage"), TEXT("--scan"), TEXT(".")};
			Step.Title = TEXT("Stage every project file that is not ignored");
			Step.Detail = TEXT("Walks the folder and stages what .loreignore leaves in. Large projects take a while.");
			Step.Command = Lore(Step.Arguments);
			Step.Undo = TEXT("lore unstage . (or delete .lore to start over).");
			Step.bChangesFolder = true;
		}
		{
			FHyperAIStudioLoreSetupStep& Step = OutSteps.AddDefaulted_GetRef();
			Step.Kind = EHyperAIStudioLoreStepKind::Commit;
			Step.Arguments = CommitMessage.StartsWith(TEXT("-")) ? TArray<FString>{TEXT("commit"), TEXT("--"), CommitMessage} : TArray<FString>{TEXT("commit"), CommitMessage};
			Step.Title = FString::Printf(TEXT("Commit them as \"%s\""), *CommitMessage);
			Step.Detail = TEXT("Records the first revision in the repository. Works without a network.");
			Step.Command = Lore(Step.Arguments);
			Step.Undo = TEXT("The revision stays in history; delete .lore to start over.");
			Step.bChangesFolder = true;
		}
		if (bRemote)
		{
			FHyperAIStudioLoreSetupStep& Step = OutSteps.AddDefaulted_GetRef();
			Step.Kind = EHyperAIStudioLoreStepKind::Push;
			Step.Arguments = {TEXT("push")};
			Step.Title = FString::Printf(TEXT("Push the first revision to %s"), *Server);
			Step.Detail = TEXT("Uploads the project's files to the server, where the team can clone them.");
			Step.Command = Lore(Step.Arguments);
			Step.Undo = FString::Printf(TEXT("lore repository delete %s removes the repository from the server."), *RepositoryUrl(Server, Options.Name));
		}
	}
	OutError.Reset();
	return true;
}

bool FHyperAIStudioLore::RunStep(const FString& Executable, const FHyperAIStudioLoreSetupOptions& Options, const FHyperAIStudioLoreSetupStep& Step, FString& OutText)
{
	using namespace HyperAIStudio::Lore::Private;
	FString Root = FPaths::ConvertRelativePathToFull(Options.Folder);
	Root.RemoveFromEnd(TEXT("/"));
	if (Step.Kind == EHyperAIStudioLoreStepKind::WriteIgnore)
	{
		const FString IgnoreFile = Root / TEXT(".loreignore");
		FString Current;
		FFileHelper::LoadFileToString(Current, *IgnoreFile);
		bool bChanged = false;
		const FString Merged = MergeIgnore(Current, bChanged);
		if (!bChanged)
		{
			OutText = TEXT(".loreignore already has the Unreal patterns.");
			return true;
		}
		if (!FFileHelper::SaveStringToFile(Merged, *IgnoreFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutText = FString::Printf(TEXT("Could not write %s."), *IgnoreFile);
			return false;
		}
		OutText = FString::Printf(TEXT("Wrote %s."), *IgnoreFile);
		return true;
	}
	if (Executable.IsEmpty())
	{
		OutText = TEXT("The lore command was not found.");
		return false;
	}
	const bool bQuick = Step.Kind == EHyperAIStudioLoreStepKind::CheckServer || Step.Kind == EHyperAIStudioLoreStepKind::Create;
	// A step inside the workspace names it, so lore never walks up into another one.
	TArray<FString> Arguments;
	if (Step.Kind != EHyperAIStudioLoreStepKind::CheckServer)
	{
		Arguments = {TEXT("--repository"), Root};
	}
	Arguments.Append(Step.Arguments);
	return Run(Executable, Root, Arguments, bQuick ? QuickSeconds : LongSeconds, OutText);
}

bool FHyperAIStudioLore::CheckServer(const FString& Executable, const FString& ServerUrl, FString& OutText, const double TimeoutSeconds)
{
	FString Error;
	if (!IsValidServerUrl(ServerUrl, Error))
	{
		OutText = Error;
		return false;
	}
	if (Executable.IsEmpty())
	{
		OutText = TEXT("The lore command was not found.");
		return false;
	}
	return Run(Executable, FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()), {TEXT("repository"), TEXT("list"), ServerUrl.TrimStartAndEnd()}, TimeoutSeconds, OutText);
}

TArray<FString> FHyperAIStudioLore::ParseSignIns(const FString& Output)
{
	TArray<FString> Identities;
	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines);
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("[")) || Line.StartsWith(TEXT("at ")) || Line.EndsWith(TEXT(":")))
		{
			continue;
		}
		TArray<FString> Words;
		Line.ParseIntoArrayWS(Words);
		TArray<FString> Kept = Words.FilterByPredicate([](const FString& Word) { return !HyperAIStudio::Lore::Private::LooksLikeToken(Word); });
		if (!Kept.IsEmpty())
		{
			Identities.Add(FString::Join(Kept, TEXT(" ")).Left(200));
		}
	}
	return Identities;
}

bool FHyperAIStudioLore::ListSignIns(const FString& Executable, TArray<FString>& OutIdentities, FString& OutText)
{
	OutIdentities.Reset();
	if (Executable.IsEmpty())
	{
		OutText = TEXT("The lore command was not found.");
		return false;
	}
	FString Text;
	// Never --with-token: HyperAI reads who is signed in, not their tokens.
	const bool bOk = Run(Executable, FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()), {TEXT("auth"), TEXT("list")}, 15.0, Text);
	OutIdentities = bOk ? ParseSignIns(Text) : TArray<FString>();
	OutText = bOk ? FString::Join(OutIdentities, TEXT("\n")) : Text;
	return bOk;
}

FString FHyperAIStudioLore::JoinArguments(const TArray<FString>& Arguments)
{
	TArray<FString> Parts;
	for (const FString& Argument : Arguments)
	{
		const bool bQuote = Argument.IsEmpty() || Algo::AnyOf(Argument, [](const TCHAR Char) { return FChar::IsWhitespace(Char) || Char == TEXT('"') || Char == TEXT('&') || Char == TEXT('|') || Char == TEXT('<') || Char == TEXT('>') || Char == TEXT('^'); });
		if (!bQuote)
		{
			Parts.Add(Argument);
			continue;
		}
		// Windows command-line rules: a quote is escaped, and backslashes before one (or before the closing quote) doubled.
		FString Quoted = TEXT("\"");
		int32 Slashes = 0;
		for (const TCHAR Char : Argument)
		{
			if (Char == TEXT('\\'))
			{
				++Slashes;
				continue;
			}
			if (Char == TEXT('"'))
			{
				Quoted += FString::ChrN(Slashes * 2 + 1, TEXT('\\')) + TEXT("\"");
			}
			else
			{
				Quoted += FString::ChrN(Slashes, TEXT('\\'));
				Quoted.AppendChar(Char);
			}
			Slashes = 0;
		}
		Quoted += FString::ChrN(Slashes * 2, TEXT('\\')) + TEXT("\"");
		Parts.Add(Quoted);
	}
	return FString::Join(Parts, TEXT(" "));
}

bool FHyperAIStudioLore::Run(const FString& Executable, const FString& WorkingDirectory, const TArray<FString>& Arguments, const double TimeoutSeconds,
	FString& OutText, int32* OutCode)
{
	TArray<FString> All = {TEXT("--non-interactive"), TEXT("--no-pager")};
	All.Append(Arguments);
	const bool bOk = RunProcess(Executable, JoinArguments(All), WorkingDirectory, TimeoutSeconds, OutText, OutCode);
	UE_LOG(LogHyperAIStudioLore, Log, TEXT("lore %s -> %s"), *JoinArguments(Arguments).Left(300), bOk ? TEXT("ok") : TEXT("failed"));
	return bOk;
}

bool FHyperAIStudioLore::RunProcess(const FString& Executable, const FString& Params, const FString& WorkingDirectory, const double TimeoutSeconds,
	FString& OutText, int32* OutCode)
{
	using namespace HyperAIStudio::Lore::Private;
	OutText.Reset();
	if (OutCode)
	{
		*OutCode = -1;
	}
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
	{
		OutText = TEXT("Could not open a pipe for the lore command.");
		return false;
	}
	FProcHandle Process = FPlatformProcess::CreateProc(*Executable, *Params, /*bLaunchDetached=*/false, /*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true,
		nullptr, 0, WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory, WritePipe, nullptr, WritePipe);
	if (!Process.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		OutText = FString::Printf(TEXT("Could not start %s."), *Executable);
		return false;
	}
	const double Start = FPlatformTime::Seconds();
	bool bTimedOut = false;
	FString Output;
	while (FPlatformProcess::IsProcRunning(Process))
	{
		Output += FPlatformProcess::ReadPipe(ReadPipe);
		if (FPlatformTime::Seconds() - Start > TimeoutSeconds)
		{
			FPlatformProcess::TerminateProc(Process, /*KillTree=*/true);
			bTimedOut = true;
			break;
		}
		FPlatformProcess::Sleep(0.02f);
	}
	Output += FPlatformProcess::ReadPipe(ReadPipe);
	int32 Code = -1;
	if (!bTimedOut)
	{
		FPlatformProcess::GetProcReturnCode(Process, &Code);
	}
	FPlatformProcess::CloseProc(Process);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
	Output.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	OutText = Output.TrimStartAndEnd().Right(MaxOutputCharacters);
	if (bTimedOut)
	{
		OutText += FString::Printf(TEXT("\nStopped after %.0f seconds without an answer."), TimeoutSeconds);
	}
	if (OutCode)
	{
		*OutCode = Code;
	}
	return !bTimedOut && Code == 0;
}
