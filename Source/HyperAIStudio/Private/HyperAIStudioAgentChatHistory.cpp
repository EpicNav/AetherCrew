// Games by Hyper 2026.

#include "HyperAIStudioAgentChatHistory.h"

#include "Algo/AllOf.h"
#include "HyperAIStudioCredentials.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shellapi.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace HyperAIStudio::ChatHistory
{
namespace
{
	constexpr int64 HeadBytes = 512 * 1024;
	constexpr int64 TailBytes = 256 * 1024;
	constexpr int32 MaxTitleChars = 90;

	FString ApiProvider(const FString& Agent)
	{
		return Agent == TEXT("Claude API") ? FString(TEXT("anthropic")) : Agent == TEXT("OpenAI API") ? FString(TEXT("openai"))
			: Agent == TEXT("Gemini API") ? FString(TEXT("gemini")) : FString();
	}

	FString ApiDirectory(const FString& Root, const FString& Agent)
	{
		return FPaths::Combine(Root, TEXT(".hyperai/runtime/api-chats"), ApiProvider(Agent));
	}

	/** What a session file says, kept while it stays the same size and age. */
	struct FFileFacts
	{
		int64 Size = -1;
		FDateTime Stamp;
		FString Title;
		bool bUsedUnrealMcp = false;
		/** Codex: the thread id, its working directory, and whether Codex ran it for itself. */
		FString Id;
		FString Cwd;
		bool bSubagent = false;
	};

	FCriticalSection FactsLock;

	TMap<FString, FFileFacts>& KnownFacts()
	{
		static TMap<FString, FFileFacts> Facts;
		return Facts;
	}

	/** Facts about Path, read again only when the file changed since they were last read. */
	FFileFacts FactsFor(const FString& Path, const FDateTime& Stamp, TFunctionRef<FFileFacts()> Read)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		{
			FScopeLock Lock(&FactsLock);
			if (const FFileFacts* Known = KnownFacts().Find(Path); Known && Known->Size == Size && Known->Stamp == Stamp)
			{
				return *Known;
			}
		}
		FFileFacts Fresh = Read();
		Fresh.Size = Size;
		Fresh.Stamp = Stamp;
		FScopeLock Lock(&FactsLock);
		KnownFacts().Add(Path, Fresh);
		return Fresh;
	}

	FString NormalizeRoot(const FString& Path)
	{
		FString Result = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeDirectoryName(Result);
		return Result;
	}

	FString HomeDirectory()
	{
		FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		return Home.IsEmpty() ? FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")) : Home;
	}

	FString ConfigDirectory(const TCHAR* OverrideVariable, const TCHAR* DefaultName)
	{
		const FString Override = FPlatformMisc::GetEnvironmentVariable(OverrideVariable);
		return Override.IsEmpty() ? HomeDirectory() / DefaultName : Override;
	}

	/** Agents append to these files while running, so open with shared write access. */
	FString ReadRange(const FString& Path, const int64 Offset, const int64 Bytes)
	{
		TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path, /*bAllowWrite=*/true));
		if (!Handle || Bytes <= 0 || !Handle->Seek(Offset))
		{
			return FString();
		}
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(static_cast<int32>(Bytes));
		if (!Handle->Read(Buffer.GetData(), Bytes))
		{
			return FString();
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), Buffer.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	/** The start of a session plus its end, where renames land. Lines cut at the seams simply fail to parse. */
	FString ReadHeadAndTail(const FString& Path)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		if (Size <= 0)
		{
			return FString();
		}
		if (Size <= HeadBytes + TailBytes)
		{
			return ReadRange(Path, 0, Size);
		}
		return ReadRange(Path, 0, HeadBytes) + TEXT("\n") + ReadRange(Path, Size - TailBytes, TailBytes);
	}

	TSharedPtr<FJsonObject> ParseLine(const FString& Line)
	{
		TSharedPtr<FJsonObject> Object;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Line);
		return FJsonSerializer::Deserialize(Reader, Object) ? Object : nullptr;
	}

	FString OneLine(FString Text)
	{
		Text.TrimStartAndEndInline();
		int32 Break = INDEX_NONE;
		if (Text.FindChar(TEXT('\n'), Break))
		{
			Text.LeftInline(Break);
		}
		Text.ReplaceInline(TEXT("\r"), TEXT(""));
		Text.TrimEndInline();
		return Text.Len() > MaxTitleChars ? Text.Left(MaxTitleChars - 3) + TEXT("...") : Text;
	}

	/** Text of a message content field that is either a string or an array of {type, text} parts. */
	FString ContentText(const TSharedPtr<FJsonObject>& Owner, const TCHAR* TextPartType)
	{
		FString Text;
		if (Owner->TryGetStringField(TEXT("content"), Text))
		{
			return Text;
		}
		const TArray<TSharedPtr<FJsonValue>>* Parts = nullptr;
		if (Owner->TryGetArrayField(TEXT("content"), Parts))
		{
			for (const TSharedPtr<FJsonValue>& Part : *Parts)
			{
				const TSharedPtr<FJsonObject> PartObject = Part.IsValid() ? Part->AsObject() : nullptr;
				FString Type;
				if (PartObject && PartObject->TryGetStringField(TEXT("type"), Type) && Type == TextPartType
					&& PartObject->TryGetStringField(TEXT("text"), Text))
				{
					return Text;
				}
			}
		}
		return FString();
	}

	bool IsTypedPrompt(const FString& Text)
	{
		const FString Trimmed = Text.TrimStart();
		// Injected instructions, environment blocks, command caveats and slash commands are not what the user asked.
		return !Trimmed.IsEmpty() && !Trimmed.StartsWith(TEXT("<")) && !Trimmed.StartsWith(TEXT("#"))
			&& !Trimmed.StartsWith(TEXT("/"));
	}

	/** Files with their last-write times, newest first. */
	TArray<TPair<FString, FDateTime>> FilesNewestFirst(const TArray<FString>& Files)
	{
		TArray<TPair<FString, FDateTime>> Stamped;
		for (const FString& File : Files)
		{
			Stamped.Emplace(File, IFileManager::Get().GetTimeStamp(*File));
		}
		Stamped.Sort([](const TPair<FString, FDateTime>& A, const TPair<FString, FDateTime>& B) { return A.Value > B.Value; });
		return Stamped;
	}

	FString ClaudeProjectsDirectory()
	{
		return ConfigDirectory(TEXT("CLAUDE_CONFIG_DIR"), TEXT(".claude")) / TEXT("projects");
	}

	FString CodexSessionsDirectory()
	{
		return ConfigDirectory(TEXT("CODEX_HOME"), TEXT(".codex")) / TEXT("sessions");
	}

	void AddClaudeSessions(const FString& Root, const int32 MaxSessions, TArray<FHyperAIStudioChatSession>& Out)
	{
		const FString Directory = ClaudeProjectsDirectory() / ClaudeProjectDirectoryName(Root);
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Directory / TEXT("*.jsonl")), /*Files=*/true, /*Directories=*/false);
		TArray<FString> Paths;
		for (const FString& Name : Names)
		{
			Paths.Add(Directory / Name);
		}
		int32 Found = 0;
		for (const TPair<FString, FDateTime>& File : FilesNewestFirst(Paths))
		{
			const FString SessionId = FPaths::GetBaseFilename(File.Key);
			if (!IsSafeSessionId(SessionId))
			{
				continue;
			}
			const FFileFacts Facts = FactsFor(File.Key, File.Value, [&File]()
			{
				const FString Text = ReadHeadAndTail(File.Key);
				FFileFacts Read;
				Read.Title = ExtractClaudeTitle(Text);
				Read.bUsedUnrealMcp = MentionsUnrealMcpCall(Text);
				return Read;
			});
			// No title means no typed prompt: an aborted launch, not a conversation worth resuming.
			if (Facts.Title.IsEmpty())
			{
				continue;
			}
			Out.Add({TEXT("Claude Code"), SessionId, Facts.Title, File.Value, Facts.bUsedUnrealMcp});
			Out.Last().Path = File.Key;
			if (++Found >= MaxSessions)
			{
				return;
			}
		}
	}

	void AddCodexSessions(const FString& Root, const int32 MaxSessions, TArray<FHyperAIStudioChatSession>& Out)
	{
		const FString CodexHome = ConfigDirectory(TEXT("CODEX_HOME"), TEXT(".codex"));
		TMap<FString, FString> ThreadNames;
		FString Index;
		if (FFileHelper::LoadFileToString(Index, *(CodexHome / TEXT("session_index.jsonl"))))
		{
			TArray<FString> Lines;
			Index.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				FString Id;
				FString Name;
				const TSharedPtr<FJsonObject> Object = ParseLine(Line);
				if (Object && Object->TryGetStringField(TEXT("id"), Id) && Object->TryGetStringField(TEXT("thread_name"), Name))
				{
					ThreadNames.Add(Id, Name);
				}
			}
		}

		TArray<FString> Paths;
		IFileManager::Get().FindFilesRecursive(Paths, *(CodexHome / TEXT("sessions")), TEXT("rollout-*.jsonl"), true, false);
		int32 Found = 0;
		for (const TPair<FString, FDateTime>& File : FilesNewestFirst(Paths))
		{
			const FFileFacts Facts = FactsFor(File.Key, File.Value, [&File]()
			{
				FFileFacts Read;
				const FString Head = ReadRange(File.Key, 0, FMath::Min(HeadBytes, IFileManager::Get().FileSize(*File.Key)));
				int32 Break = INDEX_NONE;
				const TSharedPtr<FJsonObject> Meta = Head.FindChar(TEXT('\n'), Break) ? ParseLine(Head.Left(Break)) : nullptr;
				const TSharedPtr<FJsonObject>* Payload = nullptr;
				if (!Meta || !Meta->TryGetObjectField(TEXT("payload"), Payload))
				{
					return Read;
				}
				(*Payload)->TryGetStringField(TEXT("id"), Read.Id);
				(*Payload)->TryGetStringField(TEXT("cwd"), Read.Cwd);
				// Threads Codex starts for itself, such as the reviewer that judges its own approvals.
				FString ThreadSource;
				const TSharedPtr<FJsonObject>* Source = nullptr;
				Read.bSubagent = ((*Payload)->TryGetStringField(TEXT("thread_source"), ThreadSource) && ThreadSource == TEXT("subagent"))
					|| ((*Payload)->TryGetObjectField(TEXT("source"), Source) && (*Source)->HasField(TEXT("subagent")));
				Read.Title = ExtractCodexFirstPrompt(Head);
				Read.bUsedUnrealMcp = MentionsUnrealMcpCall(Head);
				return Read;
			});
			if (Facts.bSubagent || !IsSafeSessionId(Facts.Id) || !NormalizeRoot(Facts.Cwd).Equals(Root, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString* ThreadName = ThreadNames.Find(Facts.Id);
			const FString Title = OneLine(ThreadName ? *ThreadName : Facts.Title);
			if (Title.IsEmpty())
			{
				continue;
			}
			Out.Add({TEXT("Codex"), Facts.Id, Title, File.Value, Facts.bUsedUnrealMcp});
			Out.Last().Path = File.Key;
			if (++Found >= MaxSessions)
			{
				return;
			}
		}
	}

	/** Moves files and folders to the Recycle Bin, so a deleted chat can still be restored. */
	bool MoveToRecycleBin(const TArray<FString>& Paths, FString& OutError)
	{
#if PLATFORM_WINDOWS
		// The shell takes the paths as one buffer: each ended by a null, and the list by another.
		TArray<TCHAR> From;
		for (const FString& Path : Paths)
		{
			const FString Full = FPaths::ConvertRelativePathToFull(Path).Replace(TEXT("/"), TEXT("\\"));
			From.Append(*Full, Full.Len());
			From.Add(TEXT('\0'));
		}
		From.Add(TEXT('\0'));
		SHFILEOPSTRUCTW Operation = {};
		Operation.wFunc = FO_DELETE;
		Operation.pFrom = From.GetData();
		Operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
		const int32 Result = SHFileOperationW(&Operation);
		if (Result != 0 || Operation.fAnyOperationsAborted)
		{
			OutError = FString::Printf(TEXT("Windows could not move it to the Recycle Bin (error %d)."), Result);
			return false;
		}
		return true;
#else
		for (const FString& Path : Paths)
		{
			const bool bGone = IFileManager::Get().DirectoryExists(*Path) ? IFileManager::Get().DeleteDirectory(*Path, false, true)
				: IFileManager::Get().Delete(*Path, false, true);
			if (!bGone)
			{
				OutError = FString::Printf(TEXT("%s could not be deleted."), *Path);
				return false;
			}
		}
		return true;
#endif
	}
}

FString SessionFile(const FString& AgentName, const FString& ProjectRoot, const FString& SessionId)
{
	if (!IsSafeSessionId(SessionId))
	{
		return FString();
	}
	if (AgentName == TEXT("Claude Code"))
	{
		const FString Path = ClaudeProjectsDirectory() / ClaudeProjectDirectoryName(ProjectRoot) / (SessionId + TEXT(".jsonl"));
		return IFileManager::Get().FileExists(*Path) ? Path : FString();
	}
	if (!ApiProvider(AgentName).IsEmpty())
	{
		const FString Path = ApiDirectory(ProjectRoot, AgentName) / (SessionId + TEXT(".jsonl"));
		return IFileManager::Get().FileExists(*Path) ? Path : FString();
	}
	if (AgentName == TEXT("Codex"))
	{
		// A rollout's name ends with its thread id.
		TArray<FString> Paths;
		IFileManager::Get().FindFilesRecursive(Paths, *CodexSessionsDirectory(), *(TEXT("rollout-*") + SessionId + TEXT(".jsonl")), true, false);
		return Paths.IsEmpty() ? FString() : Paths[0];
	}
	return FString();
}

bool DeleteSession(const FHyperAIStudioChatSession& Session, FString& OutError)
{
	// Only a log inside the agent's own store, named for the chat, is ever touched.
	const FString Store = Session.AgentName == TEXT("Claude Code") ? ClaudeProjectsDirectory()
		: Session.AgentName == TEXT("Codex") ? CodexSessionsDirectory()
		: !ApiProvider(Session.AgentName).IsEmpty() && !Session.ProjectRoot.IsEmpty() ? ApiDirectory(Session.ProjectRoot, Session.AgentName) : FString();
	const FString Full = FPaths::ConvertRelativePathToFull(Session.Path);
	if (Store.IsEmpty() || !IsSafeSessionId(Session.SessionId) || !FPaths::IsUnderDirectory(Full, FPaths::ConvertRelativePathToFull(Store))
		|| !FPaths::GetBaseFilename(Full).EndsWith(Session.SessionId) || !IFileManager::Get().FileExists(*Full))
	{
		OutError = TEXT("Its log is not where this chat list read it from.");
		return false;
	}
	TArray<FString> Paths = {Full};
	if (!ApiProvider(Session.AgentName).IsEmpty())
	{
		const FString State = FPaths::ChangeExtension(Full, TEXT("json"));
		if (IFileManager::Get().FileExists(*State)) { Paths.Add(State); }
	}
	// Claude Code keeps a chat's subagent and tool logs in a folder named like it.
	const FString Folder = FPaths::GetPath(Full) / Session.SessionId;
	if (Session.AgentName == TEXT("Claude Code") && IFileManager::Get().DirectoryExists(*Folder))
	{
		Paths.Add(Folder);
	}
	if (!MoveToRecycleBin(Paths, OutError))
	{
		return false;
	}
	FScopeLock Lock(&FactsLock);
	KnownFacts().Remove(Session.Path);
	return true;
}

TArray<FString> ReadRecentLines(const FString& Path, const int64 MaxBytes, bool& bOutCut)
{
	const int64 Size = IFileManager::Get().FileSize(*Path);
	bOutCut = Size > MaxBytes;
	TArray<FString> Lines;
	if (Size <= 0 || MaxBytes <= 0)
	{
		return Lines;
	}
	ReadRange(Path, bOutCut ? Size - MaxBytes : 0, FMath::Min(Size, MaxBytes)).ParseIntoArrayLines(Lines);
	// Reading from the middle of the log starts partway through a line.
	if (bOutCut && !Lines.IsEmpty())
	{
		Lines.RemoveAt(0);
	}
	return Lines;
}

FString StripInjectedContext(const FString& Message)
{
	FString Typed = Message;
	for (const TCHAR* Marker : {TEXT("\n\n<project-memory>"), TEXT("\n\n<agents>"), TEXT("\n\n<skills>"), TEXT("\n\n<tools>")})
	{
		const int32 At = Typed.Find(Marker, ESearchCase::CaseSensitive);
		if (At != INDEX_NONE)
		{
			Typed.LeftInline(At);
		}
	}
	return Typed.TrimEnd();
}

TArray<FHyperAIStudioChatSession> ListSessions(const FString& ProjectRoot, const int32 MaxSessions)
{
	TArray<FHyperAIStudioChatSession> Sessions;
	if (ProjectRoot.IsEmpty() || MaxSessions <= 0)
	{
		return Sessions;
	}
	const FString Root = NormalizeRoot(ProjectRoot);
	AddClaudeSessions(Root, MaxSessions, Sessions);
	AddCodexSessions(Root, MaxSessions, Sessions);
	for (const TCHAR* Agent : {TEXT("Claude API"), TEXT("OpenAI API"), TEXT("Gemini API")})
	{
		const FString Directory = ApiDirectory(Root, Agent);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.jsonl")), true, false);
		for (const FString& File : Files)
		{
			const FString Id = FPaths::GetBaseFilename(File);
			if (!IsSafeSessionId(Id)) { continue; }
			const FString Full = Directory / File;
			const FString Head = ReadHeadAndTail(Full);
			FHyperAIStudioChatSession Session;
			Session.AgentName = Agent;
			Session.SessionId = Id;
			Session.Title = ExtractClaudeTitle(Head);
			Session.LastActiveUtc = IFileManager::Get().GetTimeStamp(*Full);
			Session.Path = Full;
			Session.ProjectRoot = Root;
			Session.bUsedUnrealMcp = Head.Contains(TEXT("unreal_call_tool"));
			Sessions.Add(MoveTemp(Session));
		}
	}
	Sessions.Sort([](const FHyperAIStudioChatSession& A, const FHyperAIStudioChatSession& B) { return A.LastActiveUtc > B.LastActiveUtc; });
	if (Sessions.Num() > MaxSessions)
	{
		Sessions.SetNum(MaxSessions);
	}
	return Sessions;
}

FString BuildResumeArguments(const FString& AgentName, const FString& SessionId)
{
	if (!IsSafeSessionId(SessionId))
	{
		return FString();
	}
	if (AgentName == TEXT("Claude Code"))
	{
		return TEXT("--resume ") + SessionId;
	}
	if (!ApiProvider(AgentName).IsEmpty()) { return TEXT("--session ") + SessionId; }
	// Codex applies the root -c overrides to its resume subcommand.
	return AgentName == TEXT("Codex") ? TEXT("resume ") + SessionId : FString();
}

FString NewClaudeSessionId()
{
	// ponytail: FGuid's own text order is not RFC 4122, so print its bits in UUID order with the v4 and variant bits set.
	const FGuid Guid = FGuid::NewGuid();
	return FString::Printf(TEXT("%08x-%04x-%04x-%04x-%04x%08x"), Guid.A, Guid.B >> 16, (Guid.B & 0x0FFFu) | 0x4000u,
		((Guid.C >> 16) & 0x3FFFu) | 0x8000u, Guid.C & 0xFFFFu, Guid.D);
}

FString BuildClaudeRemoteArguments(const FString& ProjectName, const FString& ChatTitle)
{
	// The name lands inside a quoted cmd.exe argument: keep plain ASCII words, so no quote, %, ^, & or | survives.
	auto Clean = [](const FString& Text)
	{
		FString Out;
		for (const TCHAR C : Text)
		{
			const bool bKeep = C < 128 && (FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_') || C == TEXT('.') || C == TEXT(','));
			Out.AppendChar(bKeep ? C : TEXT(' '));
		}
		while (Out.ReplaceInline(TEXT("  "), TEXT(" ")) > 0) {}
		Out.TrimStartAndEndInline();
		return Out.Left(60).TrimEnd();
	};
	const FString Project = Clean(ProjectName);
	const FString Title = Clean(ChatTitle);
	if (Title.IsEmpty())
	{
		return Project.IsEmpty() ? TEXT("--remote-control")
			: FString::Printf(TEXT("--remote-control --remote-control-session-name-prefix \"%s\""), *Project);
	}
	return FString::Printf(TEXT("--remote-control \"%s\""), *(Project.IsEmpty() ? Title : Project + TEXT(" - ") + Title));
}

FString ParseBackgroundedId(const FString& Output)
{
	const int32 Start = Output.Find(TEXT("backgrounded"));
	if (Start == INDEX_NONE)
	{
		return FString();
	}
	// The first run of exactly eight hex digits after the marker; the separators around it are decoration.
	FString Run;
	for (int32 Index = Start + 12; Index <= Output.Len(); ++Index)
	{
		const TCHAR C = Index < Output.Len() ? Output[Index] : TEXT(' ');
		if (FChar::IsHexDigit(C))
		{
			Run.AppendChar(C);
			continue;
		}
		if (Run.Len() == 8)
		{
			return Run.ToLower();
		}
		if (C == TEXT('\n'))
		{
			return FString();
		}
		Run.Reset();
	}
	return FString();
}

TArray<FHyperAIStudioLiveChat> ParseLiveClaudeChats(const FString& Json, const FString& ProjectRoot)
{
	TArray<FHyperAIStudioLiveChat> Chats;
	TArray<TSharedPtr<FJsonValue>> Entries;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Entries))
	{
		return Chats;
	}
	const FString Root = NormalizeRoot(ProjectRoot);
	for (const TSharedPtr<FJsonValue>& Entry : Entries)
	{
		const TSharedPtr<FJsonObject> Object = Entry.IsValid() ? Entry->AsObject() : nullptr;
		FHyperAIStudioLiveChat Chat;
		FString Cwd;
		if (!Object || !Object->TryGetStringField(TEXT("sessionId"), Chat.SessionId) || !IsSafeSessionId(Chat.SessionId)
			|| !Object->TryGetStringField(TEXT("cwd"), Cwd) || !NormalizeRoot(Cwd).Equals(Root, ESearchCase::IgnoreCase))
		{
			continue;
		}
		// Interactive sessions carry no short id; attach and stop only apply to background ones, which do.
		if (!Object->TryGetStringField(TEXT("id"), Chat.ShortId) || !IsSafeSessionId(Chat.ShortId))
		{
			Chat.ShortId = Chat.SessionId.Left(8);
		}
		Object->TryGetStringField(TEXT("kind"), Chat.Kind);
		Object->TryGetStringField(TEXT("status"), Chat.Status);
		Chats.Add(MoveTemp(Chat));
	}
	return Chats;
}

void MergeLiveChats(TArray<FHyperAIStudioChatSession>& Sessions, const TArray<FHyperAIStudioLiveChat>& Live)
{
	for (FHyperAIStudioChatSession& Session : Sessions)
	{
		const FHyperAIStudioLiveChat* Running = Session.AgentName == TEXT("Claude Code")
			? Live.FindByPredicate([&Session](const FHyperAIStudioLiveChat& Chat) { return Chat.SessionId == Session.SessionId; })
			: nullptr;
		Session.Live = Running ? *Running : FHyperAIStudioLiveChat();
	}
}

bool ClaudeSessionExists(const FString& ProjectRoot, const FString& SessionId)
{
	return IsSafeSessionId(SessionId) && IFileManager::Get().FileExists(*(ConfigDirectory(TEXT("CLAUDE_CONFIG_DIR"), TEXT(".claude"))
		/ TEXT("projects") / ClaudeProjectDirectoryName(ProjectRoot) / (SessionId + TEXT(".jsonl"))));
}

namespace
{
	/** Runs one command line through cmd.exe, hidden and not tied to the editor's lifetime. */
	bool RunHidden(const FString& CommandLine, const FString& WorkingDirectory, FString& OutStdOut)
	{
		int32 ReturnCode = -1;
		FString StdErr;
		const FString Params = FString::Printf(TEXT("/d /s /c \"%s\""), *CommandLine);
		const bool bRan = FPlatformProcess::ExecProcess(TEXT("cmd.exe"), *Params, &ReturnCode, &OutStdOut, &StdErr,
			WorkingDirectory.IsEmpty() ? nullptr : *WorkingDirectory, /*bShouldEndWithParentProcess=*/false);
		OutStdOut += StdErr;
		return bRan && ReturnCode == 0;
	}
}

TArray<FHyperAIStudioLiveChat> ListLiveClaudeChats(const FString& ClaudeCommand, const FString& ProjectRoot)
{
	FString Output;
	if (ClaudeCommand.IsEmpty() || !RunHidden(ClaudeCommand + TEXT(" agents --json"), ProjectRoot, Output))
	{
		return {};
	}
	// Other output (an update notice) can precede the array.
	const int32 Start = Output.Find(TEXT("["));
	return Start == INDEX_NONE ? TArray<FHyperAIStudioLiveChat>() : ParseLiveClaudeChats(Output.Mid(Start), ProjectRoot);
}

FString StartClaudeBackgroundChat(const FString& ClaudeCommand, const FString& ProjectRoot, const FString& EnvPrefix,
	const FString& Arguments, FString& OutError, const TArray<TPair<FString, FString>>& KeyTargets, const TMap<FString, FString>& Overrides)
{
	FString Output;
	RunHidden(HyperAIStudio::Credentials::WrapCommandWithTargets(KeyTargets, EnvPrefix + ClaudeCommand + TEXT(" --bg ") + Arguments, Overrides), ProjectRoot, Output);
	const FString ShortId = ParseBackgroundedId(Output);
	if (ShortId.IsEmpty())
	{
		OutError = Output.TrimStartAndEnd().Left(400);
	}
	return ShortId;
}

bool StopClaudeBackgroundChat(const FString& ClaudeCommand, const FString& ShortId)
{
	FString Output;
	return !ClaudeCommand.IsEmpty() && IsSafeSessionId(ShortId) && RunHidden(ClaudeCommand + TEXT(" stop ") + ShortId, FString(), Output);
}

bool IsSafeSessionId(const FString& SessionId)
{
	// The id lands in a cmd.exe command line: hex digits and dashes only.
	return SessionId.Len() >= 8 && SessionId.Len() <= 64
		&& Algo::AllOf(SessionId, [](const TCHAR C) { return FChar::IsHexDigit(C) || C == TEXT('-'); });
}

bool MentionsUnrealMcpCall(const FString& JsonlText)
{
	// A called tool is a JSON name field. The catalogue an agent is shown lists the same names as escaped
	// text inside a prompt string, which this deliberately does not match.
	return JsonlText.Contains(TEXT("\"name\":\"mcp__unreal")) || JsonlText.Contains(TEXT("\"name\":\"mcp__hyper"));
}

FString ClaudeProjectDirectoryName(const FString& ProjectRoot)
{
	FString Name = NormalizeRoot(ProjectRoot);
	for (TCHAR& Character : Name)
	{
		if (!FChar::IsAlnum(Character))
		{
			Character = TEXT('-');
		}
	}
	return Name;
}

FString ExtractClaudeTitle(const FString& JsonlText)
{
	FString CustomTitle;
	FString AiTitle;
	FString FirstPrompt;
	TArray<FString> Lines;
	JsonlText.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		const bool bCustom = Line.Contains(TEXT("\"custom-title\""));
		const bool bAi = !bCustom && Line.Contains(TEXT("\"ai-title\""));
		const bool bUser = !bCustom && !bAi && FirstPrompt.IsEmpty() && Line.Contains(TEXT("\"type\":\"user\""));
		const TSharedPtr<FJsonObject> Object = bCustom || bAi || bUser ? ParseLine(Line) : nullptr;
		if (!Object)
		{
			continue;
		}
		if (bCustom)
		{
			Object->TryGetStringField(TEXT("customTitle"), CustomTitle);
		}
		else if (bAi)
		{
			Object->TryGetStringField(TEXT("aiTitle"), AiTitle);
		}
		else
		{
			const TSharedPtr<FJsonObject>* Message = nullptr;
			bool bMeta = false;
			Object->TryGetBoolField(TEXT("isMeta"), bMeta);
			if (!bMeta && Object->TryGetObjectField(TEXT("message"), Message))
			{
				const FString Prompt = ContentText(*Message, TEXT("text"));
				if (IsTypedPrompt(Prompt))
				{
					FirstPrompt = Prompt;
				}
			}
		}
	}
	return OneLine(!CustomTitle.IsEmpty() ? CustomTitle : !AiTitle.IsEmpty() ? AiTitle : FirstPrompt);
}

FString ExtractCodexFirstPrompt(const FString& JsonlText)
{
	TArray<FString> Lines;
	JsonlText.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		if (!Line.Contains(TEXT("\"response_item\"")) || !Line.Contains(TEXT("\"role\":\"user\"")))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Object = ParseLine(Line);
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (Object && Object->TryGetObjectField(TEXT("payload"), Payload))
		{
			const FString Prompt = ContentText(*Payload, TEXT("input_text"));
			if (IsTypedPrompt(Prompt))
			{
				return OneLine(Prompt);
			}
		}
	}
	return FString();
}
}
