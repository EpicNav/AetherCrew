// Games by Hyper 2026.

#include "HyperAIStudioNativeChat.h"

#include "Async/Async.h"
#include "HyperAIStudioCredentials.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "CoreGlobals.h"
#include "HyperAIStudioAgentChatHistory.h"
#include "HyperAIStudioProjectMemory.h"
#include "HyperAIStudioSkills.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTaskRouter.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioNativeChat, Log, All);

namespace HyperAIStudio::NativeChat::Private
{
	constexpr int32 MaxItems = 600;
	/** How much of a resumed chat comes back: its latest items, read from at most the end of its log. */
	constexpr int32 MaxEarlierItems = 300;
	constexpr int64 MaxHistoryBytes = 3 * 1024 * 1024;

	FString& CommandForTests()
	{
		static FString Command;
		return Command;
	}

	bool& ContextInTests()
	{
		static bool bEnabled = false;
		return bEnabled;
	}
	constexpr int32 MaxOutputCharacters = 20000;

	FString Quote(const FString& Path)
	{
		return Path.Contains(TEXT(" ")) && !Path.StartsWith(TEXT("\"")) ? TEXT("\"") + Path + TEXT("\"") : Path;
	}

	FString Executable(const TCHAR* Name)
	{
		FString Path;
		return FHyperAIStudioService::FindExecutableOnPath(Name, Path) ? Quote(Path) : FString(Name);
	}

	FString StringField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found ? *Found : nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>& ArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		static const TArray<TSharedPtr<FJsonValue>> Empty;
		const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
		return Object.IsValid() && Object->TryGetArrayField(Field, Found) && Found ? *Found : Empty;
	}

	FString Compact(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Value, FString(), Writer);
		return Text;
	}

	FString Clip(const FString& Text)
	{
		return Text.Len() <= MaxOutputCharacters ? Text : TEXT("...") + Text.Right(MaxOutputCharacters);
	}

	struct FField
	{
		const TCHAR* Key;
		TSharedPtr<FJsonValue> Value;
	};

	TSharedRef<FJsonObject> Object(std::initializer_list<FField> Fields)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const FField& Field : Fields)
		{
			if (Field.Value.IsValid())
			{
				Out->SetField(Field.Key, Field.Value);
			}
		}
		return Out;
	}

	TSharedPtr<FJsonValue> Str(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
	TSharedPtr<FJsonValue> Obj(const TSharedRef<FJsonObject>& Value) { return MakeShared<FJsonValueObject>(Value); }
	TSharedPtr<FJsonValue> Arr(const TArray<TSharedPtr<FJsonValue>>& Value) { return MakeShared<FJsonValueArray>(Value); }
	TSharedPtr<FJsonValue> Num(const double Value) { return MakeShared<FJsonValueNumber>(Value); }
	TSharedPtr<FJsonValue> Bool(const bool Value) { return MakeShared<FJsonValueBoolean>(Value); }

	FString PluginVersion()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		return Plugin.IsValid() ? Plugin->GetDescriptor().VersionName : FString(TEXT("1.0"));
	}

	/** png, jpg or webp from the leading bytes; null when they are not a picture. */
	const TCHAR* ImageExtension(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() >= 8 && Bytes[0] == 0x89 && Bytes[1] == 'P' && Bytes[2] == 'N' && Bytes[3] == 'G')
		{
			return TEXT("png");
		}
		if (Bytes.Num() >= 3 && Bytes[0] == 0xFF && Bytes[1] == 0xD8 && Bytes[2] == 0xFF)
		{
			return TEXT("jpg");
		}
		if (Bytes.Num() >= 12 && Bytes[0] == 'R' && Bytes[1] == 'I' && Bytes[2] == 'F' && Bytes[3] == 'F'
			&& Bytes[8] == 'W' && Bytes[9] == 'E' && Bytes[10] == 'B' && Bytes[11] == 'P')
		{
			return TEXT("webp");
		}
		return nullptr;
	}

	FString MimeType(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		return Extension == TEXT("jpg") || Extension == TEXT("jpeg") ? FString(TEXT("image/jpeg"))
			: Extension == TEXT("webp") ? FString(TEXT("image/webp")) : FString(TEXT("image/png"));
	}

	/** An attachment the agent can be sent: a picture file that is there, and small enough to go as data. */
	bool IsSendableImage(const FString& Path)
	{
		const int64 Size = IFileManager::Get().FileSize(*Path);
		return Size > 0 && Size <= 20 * 1024 * 1024;
	}

	/** The largest picture Claude is sent as data: the API takes up to 5 MB of base64. */
	constexpr int64 MaxInlineImageBytes = 5 * 1024 * 1024 / 4 * 3;
}

// ---------------------------------------------------------------------------------------------------------------- base

FHyperAIStudioNativeChat::FHyperAIStudioNativeChat(const FString& InAgentName, const FString& InProjectRoot, const FString& InModel,
	const FString& InResumeSessionId)
	: AgentName(InAgentName)
	, ProjectRoot(InProjectRoot)
	, Model(InModel)
	, ResumeSessionId(InResumeSessionId)
{
}

FHyperAIStudioNativeChat::~FHyperAIStudioNativeChat()
{
	Stop();
}

void FHyperAIStudioNativeChat::SetHeadless(const FString& Instructions)
{
	bHeadless = true;
	HeadlessInstructions = Instructions;
}

FString FHyperAIStudioNativeChat::ImageFolder()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("ChatImages"));
}

FString FHyperAIStudioNativeChat::SaveImageData(const FString& Base64, const FString& Name)
{
	using namespace HyperAIStudio::NativeChat::Private;
	FString Data = Base64;
	int32 Comma = INDEX_NONE;
	if (Data.StartsWith(TEXT("data:")) && Data.FindChar(TEXT(','), Comma))
	{
		Data.RightChopInline(Comma + 1);
	}
	TArray<uint8> Bytes;
	const TCHAR* Extension = FBase64::Decode(Data, Bytes) ? ImageExtension(Bytes) : nullptr;
	if (!Extension)
	{
		return FString();
	}
	IFileManager::Get().MakeDirectory(*ImageFolder(), /*Tree=*/true);
	const FString File = ImageFolder() / FString::Printf(TEXT("%s-%s.%s"), *FPaths::MakeValidFileName(Name.Left(48), TEXT('_')),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8), Extension);
	return FFileHelper::SaveArrayToFile(Bytes, *File) ? File : FString();
}

bool FHyperAIStudioNativeChat::Start(FString& OutError)
{
	if (!FPlatformProcess::CreatePipe(StdOutRead, StdOutWrite) || !FPlatformProcess::CreatePipe(StdInRead, StdInWrite, /*bWritePipeLocal=*/true))
	{
		OutError = TEXT("Could not create the pipes to talk to the agent.");
		ClosePipes();
		return false;
	}
	FString Command = HyperAIStudio::NativeChat::Private::CommandForTests();
#if WITH_DEV_AUTOMATION_TESTS
	if (Command.IsEmpty())
	{
		// A stand-in for tests that span an editor restart, which outlive SetCommandForTests.
		Command = FPlatformMisc::GetEnvironmentVariable(TEXT("HYPERAI_TEST_NATIVE_CHAT_COMMAND"));
	}
#endif
	Command = Command.IsEmpty() ? CommandLine() : Command;
	// Through cmd so npm's .cmd shims start too; /s keeps the quoted executable path intact. The keys the user stored
	// for this agent go into its environment alone, not the editor's.
	const TMap<FString, FString> Keys = HyperAIStudio::Credentials::EnvironmentFor(AgentName);
	Process = Keys.IsEmpty()
		? FPlatformProcess::CreateProc(TEXT("cmd.exe"), *FString::Printf(TEXT("/d /s /c \"%s\""), *Command),
			/*bLaunchDetached=*/false, /*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true, nullptr, 0, *ProjectRoot, StdOutWrite, StdInRead)
		: HyperAIStudio::Credentials::CreateProcWithEnvironment(TEXT("cmd.exe"), FString::Printf(TEXT("/d /s /c \"%s\""), *Command), ProjectRoot,
			Keys, StdOutWrite, StdInRead);
	if (!Process.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not start %s."), *Command);
		ClosePipes();
		return false;
	}
	UE_LOG(LogHyperAIStudioNativeChat, Log, TEXT("Started %s for %s"), *Command, *AgentName);
	Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FHyperAIStudioNativeChat::Pump), 0.03f);
	AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(),
		ResumeSessionId.IsEmpty() ? FString::Printf(TEXT("Starting %s..."), *AgentName) : FString::Printf(TEXT("Resuming this %s chat..."), *AgentName));
	Handshake();
	if (!ResumeSessionId.IsEmpty())
	{
		LoadHistory();
	}
	return true;
}

void FHyperAIStudioNativeChat::ShowEarlier(TFunctionRef<void()> Replay, bool bMoreBefore)
{
	using namespace HyperAIStudio::NativeChat::Private;
	const int32 Before = Items.Num();
	bReplaying = true;
	Replay();
	bReplaying = false;
	TArray<TSharedPtr<FHyperAIStudioChatItem>> Earlier(Items.GetData() + Before, Items.Num() - Before);
	Items.SetNum(Before);
	if (!Earlier.IsEmpty())
	{
		// The latest of it fits on screen with room for the chat that goes on.
		if (Earlier.Num() > MaxEarlierItems)
		{
			Earlier.RemoveAt(0, Earlier.Num() - MaxEarlierItems);
			bMoreBefore = true;
		}
		for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Earlier)
		{
			// A step that was still going when that session ended never finished.
			if (Item->Status == TEXT("running"))
			{
				Item->Status = TEXT("stopped");
			}
		}
		if (bMoreBefore)
		{
			Earlier.Insert(MakeShared<FHyperAIStudioChatItem>(), 0);
			Earlier[0]->Id = FGuid::NewGuid().ToString();
			Earlier[0]->Text = FString::Printf(TEXT("Earlier messages of this chat are in %s's own history; the latest are shown."), *AgentName);
		}
		Items.Insert(Earlier, 0);
		if (Items.Num() > MaxItems)
		{
			Items.RemoveAt(0, Items.Num() - MaxItems);
		}
	}
	// Announced once, items or not: a replayed plan or context size is news too.
	Changed();
}

void FHyperAIStudioNativeChat::Stop()
{
	if (bStopped)
	{
		return;
	}
	bStopped = true;
	HyperAIStudio::ChatModes::ClearActive(this);
	if (Ticker.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
		Ticker.Reset();
	}
	if (Process.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(Process))
		{
			FPlatformProcess::TerminateProc(Process, /*KillTree=*/true);
		}
		FPlatformProcess::CloseProc(Process);
	}
	ClosePipes();
	Waiting.Reset();
	OpenApprovals.Reset();
	bBusy = false;
	bReady = false;
}

void FHyperAIStudioNativeChat::ClosePipes()
{
	if (StdOutRead || StdOutWrite)
	{
		FPlatformProcess::ClosePipe(StdOutRead, StdOutWrite);
	}
	if (StdInRead || StdInWrite)
	{
		FPlatformProcess::ClosePipe(StdInRead, StdInWrite);
	}
	StdOutRead = StdOutWrite = StdInRead = StdInWrite = nullptr;
}

bool FHyperAIStudioNativeChat::IsRunning() const
{
	return !bStopped && Process.IsValid() && FPlatformProcess::IsProcRunning(const_cast<FProcHandle&>(Process));
}

void FHyperAIStudioNativeChat::Send(const FString& Text, const TArray<FString>& Images, const FHyperAIStudioMessageHints& Hints)
{
	TArray<FString> Sendable = Images.FilterByPredicate(&HyperAIStudio::NativeChat::Private::IsSendableImage);
	FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty() && !Sendable.IsEmpty())
	{
		Trimmed = TEXT("Look at the attached image.");
	}
	if (Trimmed.IsEmpty() || bStopped)
	{
		return;
	}
	if (!bReady || bBusy)
	{
		// The hints wait with it, so a queued preset still reaches its tools.
		Queued.Add({Trimmed, MoveTemp(Sendable), Hints});
		Changed();
		return;
	}
	AddItem(FHyperAIStudioChatItem::EKind::User, FString(), FString(), Trimmed)->Images = Sendable;
	TurnStarted();
	// The mode's packs, tools and skills go with every message, after what the message names itself.
	FHyperAIStudioMessageHints WithMode = Hints;
	for (const FString& Pack : Mode.Packs) { WithMode.Packs.AddUnique(Pack); }
	for (const FString& Tool : Mode.Tools) { WithMode.Tools.AddUnique(Tool); }
	for (const FString& Skill : Mode.Skills) { WithMode.Skills.AddUnique(Skill); }
	// Its instructions go once, with the first message after the chat enters it.
	FString ModeBlock;
	const FString ModeKey = Mode.Id.IsEmpty() ? FString() : HyperAIStudio::ChatModes::ToJson(Mode);
	if (!bHeadless && ModeKey != SentModeKey && !Trimmed.StartsWith(TEXT("/")))
	{
		SentModeKey = ModeKey;
		ModeBlock = Mode.Id.IsEmpty() ? FString() : HyperAIStudio::ChatModes::InstructionsBlock(Mode);
	}
	// The agent also gets what project memory knows about the message; the transcript keeps what was typed.
	SendNow(Trimmed + ModeBlock + MemoryFor(Trimmed, WithMode), Sendable);
}

void FHyperAIStudioNativeChat::SetMode(const FHyperAIStudioChatMode& InMode, const FString& Label)
{
	Mode = InMode;
	if (bStopped || bHeadless)
	{
		return;
	}
	HyperAIStudio::ChatModes::SetActive(this, &Mode, Label);
	Changed();
}

FString FHyperAIStudioNativeChat::MemoryFor(const FString& Text, const FHyperAIStudioMessageHints& Hints)
{
	// A headless chat answers one question for a tool, a slash command has to stay a command, and tests script exact messages.
	if (bHeadless || Text.StartsWith(TEXT("/")) || (GIsAutomationTesting && !HyperAIStudio::NativeChat::Private::ContextInTests()))
	{
		return FString();
	}
	FString Summary;
	const FString Context = ContextFor(ProjectRoot, Text, Hints, AgentName, StartedAs, GivenMemory, Summary);
	if (!Summary.IsEmpty())
	{
		// The transcript keeps what was typed; this line says what went with it.
		AddNotice(TEXT("Sent with it: ") + Summary + TEXT("."));
	}
	return Context;
}

FString FHyperAIStudioNativeChat::ContextFor(const FString& Root, const FString& Text, const FString& ChatAgent, const FString& StartedAs,
	TSet<FString>& Given, FString& OutSummary)
{
	return ContextFor(Root, Text, FHyperAIStudioMessageHints(), ChatAgent, StartedAs, Given, OutSummary);
}

FString FHyperAIStudioNativeChat::ContextFor(const FString& Root, const FString& Text, const FHyperAIStudioMessageHints& Hints, const FString& ChatAgent,
	const FString& StartedAs, TSet<FString>& Given, FString& OutSummary)
{
	using namespace HyperAIStudio;
	const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
	const TSet<FString> Before = Given;
	// Packs a preset names are certain; the words are only a guess.
	TArray<TaskRouter::FPackFit> Packs;
	for (const FString& Pack : Hints.Packs)
	{
		Packs.Add({Pack, 100.0});
	}
	if (Packs.IsEmpty())
	{
		Packs = TaskRouter::PacksFor(Text);
	}
	TArray<FString> Preferred = Hints.Skills;
	for (const FString& Skill : TaskRouter::SkillsForPacks(Packs))
	{
		Preferred.AddUnique(Skill);
	}
	// Each piece marks what it adds in Given under its own prefix, so the order they run in does not matter.
	const FString Recalled = Memory::ContextFor(Root, Text, Given, Settings->AgentMemory);
	FString HandOff;
	if (Settings->bHandTasksToProjectAgents)
	{
		HandOff = Hints.Agent.IsEmpty() ? TaskRouter::AgentHintFor(Text, Root, ChatAgent, StartedAs, Given)
			: TaskRouter::AgentHintNamed(Hints.Agent, Text, Root, ChatAgent, StartedAs, Given);
	}
	const FString Context = Recalled + HandOff + Skills::HintFor(Root, Text, Given, Preferred) + TaskRouter::ToolHintFor(Packs, Given, Hints.Tools);
	FString HandedTo;
	TArray<FString> SkillNames;
	TArray<FString> PackIds;
	int32 Notes = 0;
	for (const FString& Key : Given)
	{
		if (Before.Contains(Key))
		{
			continue;
		}
		if (Key.StartsWith(TEXT("agent:")))
		{
			HandedTo = Key.RightChop(6);
		}
		else if (Key.StartsWith(TEXT("skill:")))
		{
			SkillNames.Add(Key.RightChop(6));
		}
		else if (Key.StartsWith(TEXT("tools:")))
		{
			PackIds.Add(Key.RightChop(6));
		}
		else if (Key.StartsWith(TEXT("tool:")))
		{
			// One line of a tools block, which its pack (or the preset) already names.
			continue;
		}
		else
		{
			++Notes;
		}
	}
	SkillNames.Sort();
	PackIds.Sort();
	TArray<FString> Parts;
	if (!Hints.PresetId.IsEmpty())
	{
		// The transcript says which preset the message came from, every time one does.
		Parts.Add(TEXT("preset:") + Hints.PresetId);
	}
	if (!HandedTo.IsEmpty())
	{
		Parts.Add(HandedTo == TEXT("team") ? FString(TEXT("a hand-off to HyperAI's team")) : FString::Printf(TEXT("a hand-off to the %s agent"), *HandedTo));
	}
	if (!SkillNames.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("the %s skill%s"), *FString::Join(SkillNames, TEXT(" and ")), SkillNames.Num() > 1 ? TEXT("s") : TEXT("")));
	}
	if (!PackIds.IsEmpty())
	{
		Parts.Add(FString::Printf(TEXT("the %s tools"), *FString::Join(PackIds, TEXT(" and "))));
	}
	if (Notes > 0)
	{
		Parts.Add(Notes == 1 ? FString(TEXT("1 note from project memory")) : FString::Printf(TEXT("%d notes from project memory"), Notes));
	}
	OutSummary = FString::Join(Parts, TEXT(", "));
	return Context;
}

void FHyperAIStudioNativeChat::AddNotice(const FString& Text)
{
	AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), Text);
}

void FHyperAIStudioNativeChat::RecordTurn()
{
	if (bHeadless || GIsAutomationTesting)
	{
		return;
	}
	int32 Asked = Items.Num() - 1;
	while (Asked >= 0 && Items[Asked]->Kind != FHyperAIStudioChatItem::EKind::User)
	{
		--Asked;
	}
	if (Asked < 0)
	{
		return;
	}
	FHyperAIStudioMemoryTask Task;
	Task.Utc = FDateTime::UtcNow();
	Task.Agent = Model.IsEmpty() ? AgentName : FString::Printf(TEXT("%s (%s)"), *AgentName, *Model);
	Task.Request = Items[Asked]->Text;
	for (int32 Index = Asked + 1; Index < Items.Num(); ++Index)
	{
		const FHyperAIStudioChatItem& Item = *Items[Index];
		if (Item.Kind == FHyperAIStudioChatItem::EKind::Agent && !Item.Text.TrimStartAndEnd().IsEmpty())
		{
			// The last reply says how the task ended.
			Task.Outcome = Item.Text;
		}
		else if ((Item.Kind == FHyperAIStudioChatItem::EKind::Tool || Item.Kind == FHyperAIStudioChatItem::EKind::FileChange) && !Item.Title.IsEmpty())
		{
			Task.Tools.AddUnique(Item.Title);
		}
	}
	// The chat holds this task in its own conversation, so its later messages are not reminded of it.
	const FString Id = HyperAIStudio::Memory::RecordTask(ProjectRoot, Task);
	if (!Id.IsEmpty())
	{
		GivenMemory.Add(Id);
	}
}

void FHyperAIStudioNativeChat::Interrupt()
{
	Queued.Reset();
	if (bBusy)
	{
		InterruptNow();
	}
	Changed();
}

void FHyperAIStudioNativeChat::Answer(const FString& ItemId, const int32 Choice)
{
	const TSharedPtr<FHyperAIStudioChatItem> Item = FindItem(ItemId);
	const TSharedPtr<FJsonValue>* RequestId = OpenApprovals.Find(ItemId);
	if (!Item.IsValid() || !RequestId || Item->bAnswered || !Item->Choices.IsValidIndex(Choice))
	{
		return;
	}
	Respond(*RequestId, AnswerPayload(*Item, Choice));
	OpenApprovals.Remove(ItemId);
	Item->bAnswered = true;
	Item->Status = Item->Choices[Choice];
	Changed();
}

void FHyperAIStudioNativeChat::Ready(const FString& InSessionId)
{
	SessionId = InSessionId;
	bReady = true;
	AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), FString::Printf(TEXT("%s is ready."), *AgentName));
	if (!Queued.IsEmpty())
	{
		const FQueuedMessage Next = Queued[0];
		Queued.RemoveAt(0);
		Send(Next.Text, Next.Images, Next.Hints);
	}
}

void FHyperAIStudioNativeChat::TurnStarted()
{
	bBusy = true;
	Changed();
}

void FHyperAIStudioNativeChat::TurnFinished()
{
	bBusy = false;
	// A command the user answered while it ran would otherwise stay open for ever.
	for (const TSharedPtr<FHyperAIStudioChatItem>& Item : Items)
	{
		if (Item->Status == TEXT("running"))
		{
			Item->Status = TEXT("stopped");
		}
	}
	RecordTurn();
	if (!Queued.IsEmpty() && bReady)
	{
		const FQueuedMessage Next = Queued[0];
		Queued.RemoveAt(0);
		Send(Next.Text, Next.Images, Next.Hints);
		return;
	}
	Changed();
}

TSharedPtr<FHyperAIStudioChatItem> FHyperAIStudioNativeChat::FindItem(const FString& Id) const
{
	if (Id.IsEmpty())
	{
		return nullptr;
	}
	for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
	{
		if (Items[Index]->Id == Id)
		{
			return Items[Index];
		}
	}
	return nullptr;
}

TSharedRef<FHyperAIStudioChatItem> FHyperAIStudioNativeChat::AddItem(const FHyperAIStudioChatItem::EKind Kind, const FString& Id,
	const FString& Title, const FString& Text, const FString& Status)
{
	const TSharedRef<FHyperAIStudioChatItem> Item = NewItem(Kind, Id, Title, Text, Status);
	Changed();
	return Item;
}

TSharedRef<FHyperAIStudioChatItem> FHyperAIStudioNativeChat::NewItem(const FHyperAIStudioChatItem::EKind Kind, const FString& Id,
	const FString& Title, const FString& Text, const FString& Status)
{
	const TSharedRef<FHyperAIStudioChatItem> Item = MakeShared<FHyperAIStudioChatItem>();
	Item->Kind = Kind;
	Item->Id = Id.IsEmpty() ? FGuid::NewGuid().ToString() : Id;
	Item->Title = Title;
	Item->Text = HyperAIStudio::NativeChat::Private::Clip(Text);
	Item->Status = Status;
	Items.Add(Item);
	// ShowEarlier keeps the length in check once its replay is moved into place.
	if (!bReplaying && Items.Num() > HyperAIStudio::NativeChat::Private::MaxItems)
	{
		Items.RemoveAt(0, Items.Num() - HyperAIStudio::NativeChat::Private::MaxItems);
	}
	return Item;
}

void FHyperAIStudioNativeChat::AskApproval(const TSharedPtr<FJsonValue>& RequestId, const FString& Id, const FString& Title,
	const FString& Text, const TArray<FString>& Choices, const TArray<FString>& ChoiceIds)
{
	const TSharedRef<FHyperAIStudioChatItem> Item = NewItem(FHyperAIStudioChatItem::EKind::Approval, Id + TEXT("#approval"), Title, Text, FString());
	Item->Choices = Choices;
	Item->ChoiceIds = ChoiceIds;
	OpenApprovals.Add(Item->Id, RequestId);
	Changed();
}

void FHyperAIStudioNativeChat::CloseApproval(const FString& ItemId, const FString& Status)
{
	OpenApprovals.Remove(ItemId);
	if (const TSharedPtr<FHyperAIStudioChatItem> Item = FindItem(ItemId))
	{
		Item->bAnswered = true;
		Item->Status = Status;
	}
	Changed();
}

void FHyperAIStudioNativeChat::Changed()
{
	if (!bReplaying)
	{
		OnChanged.Broadcast();
	}
}

void FHyperAIStudioNativeChat::Request(const FString& Method, const TSharedRef<FJsonObject>& Params, FOnResult OnResult)
{
	const int64 Id = NextId++;
	const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	if (WritesJsonRpcVersion())
	{
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	}
	Message->SetNumberField(TEXT("id"), static_cast<double>(Id));
	Message->SetStringField(TEXT("method"), Method);
	Message->SetObjectField(TEXT("params"), Params);
	if (OnResult)
	{
		Waiting.Add(Id, MoveTemp(OnResult));
	}
	Write(Message);
}

void FHyperAIStudioNativeChat::Notify(const FString& Method, const TSharedRef<FJsonObject>& Params)
{
	const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	if (WritesJsonRpcVersion())
	{
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	}
	Message->SetStringField(TEXT("method"), Method);
	Message->SetObjectField(TEXT("params"), Params);
	Write(Message);
}

void FHyperAIStudioNativeChat::Respond(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
{
	const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	if (WritesJsonRpcVersion())
	{
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	}
	Message->SetField(TEXT("id"), Id);
	Message->SetObjectField(TEXT("result"), Result);
	Write(Message);
}

void FHyperAIStudioNativeChat::RespondError(const TSharedPtr<FJsonValue>& Id, const FString& ErrorMessage)
{
	const TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	if (WritesJsonRpcVersion())
	{
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
	}
	Message->SetField(TEXT("id"), Id);
	const TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
	Error->SetNumberField(TEXT("code"), -32601);
	Error->SetStringField(TEXT("message"), ErrorMessage);
	Message->SetObjectField(TEXT("error"), Error);
	Write(Message);
}

void FHyperAIStudioNativeChat::Write(const TSharedRef<FJsonObject>& Message)
{
	if (!StdInWrite)
	{
		return;
	}
	FString Line;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Line);
	FJsonSerializer::Serialize(Message, Writer);
	Line += TEXT("\n");
	const FTCHARToUTF8 Utf8(*Line);
	int32 Written = 0;
	if (!FPlatformProcess::WritePipe(StdInWrite, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), &Written) || Written != Utf8.Length())
	{
		UE_LOG(LogHyperAIStudioNativeChat, Warning, TEXT("%s: could not write to the agent."), *AgentName);
	}
}

bool FHyperAIStudioNativeChat::Pump(float)
{
	// A callback below can stop this chat, and the view holding it can let it go.
	const TSharedRef<FHyperAIStudioNativeChat> Keep = AsShared();
	TArray<uint8> Bytes;
	while (StdOutRead && FPlatformProcess::ReadPipeToArray(StdOutRead, Bytes) && Bytes.Num() > 0)
	{
		Pending.Append(Bytes);
		Bytes.Reset();
	}
	int32 Newline = INDEX_NONE;
	while (!bStopped && Pending.Find(static_cast<uint8>('\n'), Newline))
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Pending.GetData()), Newline);
		const FString Line = FString(Converted.Length(), Converted.Get()).TrimEnd();
		Pending.RemoveAt(0, Newline + 1, EAllowShrinking::No);
		if (!Line.IsEmpty())
		{
			Dispatch(Line);
		}
	}
	if (bStopped)
	{
		return false;
	}
	if (!FPlatformProcess::IsProcRunning(Process))
	{
		int32 Code = 0;
		FPlatformProcess::GetProcReturnCode(Process, &Code);
		AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), FString::Printf(
			TEXT("%s stopped (exit code %d)%s. Start a new chat, or switch this tab to the terminal to see why."), *AgentName, Code,
			LastLogLine.IsEmpty() ? TEXT("") : *(TEXT(": ") + LastLogLine.Left(300))));
		Stop();
		Changed();
		return false;
	}
	return true;
}

void FHyperAIStudioNativeChat::Dispatch(const FString& Line)
{
	using namespace HyperAIStudio::NativeChat::Private;
	TSharedPtr<FJsonObject> Message;
	if (!Line.StartsWith(TEXT("{")) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Message) || !Message.IsValid())
	{
		// Log lines on stderr share the pipe.
		UE_LOG(LogHyperAIStudioNativeChat, Verbose, TEXT("%s: %s"), *AgentName, *Line);
		LastLogLine = Line;
		return;
	}
	if (HandleLine(Message.ToSharedRef()))
	{
		return;
	}
	FString Method;
	const bool bHasMethod = Message->TryGetStringField(TEXT("method"), Method);
	const TSharedPtr<FJsonValue> Id = Message->TryGetField(TEXT("id"));
	if (!bHasMethod)
	{
		if (!Id.IsValid())
		{
			return;
		}
		const int64 Number = static_cast<int64>(Id->AsNumber());
		FOnResult Callback;
		if (Waiting.RemoveAndCopyValue(Number, Callback) && Callback)
		{
			const TSharedPtr<FJsonObject> Error = ObjectField(Message, TEXT("error"));
			if (Error.IsValid())
			{
				Callback(nullptr, StringField(Error, TEXT("message")));
			}
			else
			{
				const TSharedPtr<FJsonObject> Result = ObjectField(Message, TEXT("result"));
				Callback(Result.IsValid() ? Result : MakeShared<FJsonObject>(), FString());
			}
		}
		return;
	}
	const TSharedPtr<FJsonObject> Params = ObjectField(Message, TEXT("params"));
	HandleMessage(Method, Params.IsValid() ? Params : MakeShared<FJsonObject>(), Id);
}

// ---------------------------------------------------------------------------------------------------------------- Codex

namespace HyperAIStudio::NativeChat::Private
{
	/** Codex through `codex app-server`: threads, turns, streamed items and approval requests. */
	class FCodexChat final : public FHyperAIStudioNativeChat
	{
	public:
		FCodexChat(const FString& InProjectRoot, const FString& InModel, const FString& InResumeSessionId)
			: FHyperAIStudioNativeChat(TEXT("Codex"), InProjectRoot, InModel, InResumeSessionId)
		{
		}

	protected:
		virtual FString CommandLine() const override
		{
			FString Command = Executable(TEXT("codex"));
			if (!FHyperAIStudioService::FindUnsupportedCodexServiceTierConfigPath().IsEmpty())
			{
				Command += TEXT(" -c service_tier=fast");
			}
			// Unreal MCP and a stored OpenAI key, as the terminal's Codex gets them.
			return Command + FHyperAIStudioService::GetCodexRootArguments() + TEXT(" app-server");
		}

		virtual bool WritesJsonRpcVersion() const override { return false; }

		virtual void Handshake() override
		{
			const TSharedRef<FJsonObject> Client = Object({
				{TEXT("name"), Str(TEXT("hyperai_studio"))}, {TEXT("title"), Str(TEXT("HyperAI Studio"))}, {TEXT("version"), Str(PluginVersion())}});
			Request(TEXT("initialize"), Object({{TEXT("clientInfo"), Obj(Client)}}), [this](const TSharedPtr<FJsonObject>&, const FString& Error)
			{
				if (!Error.IsEmpty())
				{
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), TEXT("Codex refused to start: ") + Error);
					return;
				}
				Notify(TEXT("initialized"), MakeShared<FJsonObject>());
				StartThread(!ResumeSessionId.IsEmpty());
			});
		}

		void StartThread(const bool bResume)
		{
			const TSharedRef<FJsonObject> Params = Object({{TEXT("cwd"), Str(ProjectRoot)}});
			FString LocalServer;
			FString LocalName;
			if (UHyperAIStudioSettings::ParseLocalModelId(Model, LocalServer, LocalName))
			{
				// A model Ollama or LM Studio serves here, through Codex's own provider for it.
				Params->SetStringField(TEXT("modelProvider"), LocalServer);
				Params->SetStringField(TEXT("model"), LocalName);
			}
			else if (!Model.IsEmpty())
			{
				Params->SetStringField(TEXT("model"), Model);
			}
			if (bHeadless)
			{
				// Nobody is there to answer a question, and nothing this thread does should last.
				Params->SetBoolField(TEXT("ephemeral"), true);
				Params->SetStringField(TEXT("approvalPolicy"), TEXT("never"));
				Params->SetStringField(TEXT("sandbox"), TEXT("read-only"));
				Params->SetStringField(TEXT("developerInstructions"), HeadlessInstructions);
			}
			else if (GetDefault<UHyperAIStudioSettings>()->bAgentAutoMode)
			{
				// Auto Mode: Codex's own reviewer answers its approvals, so the chat does not stop at each one.
				Params->SetStringField(TEXT("approvalsReviewer"), TEXT("auto_review"));
			}
			if (bResume)
			{
				Params->SetStringField(TEXT("threadId"), ResumeSessionId);
			}
			Request(bResume ? TEXT("thread/resume") : TEXT("thread/start"), Params, [this, bResume](const TSharedPtr<FJsonObject>& Result, const FString& Error)
			{
				if (!Error.IsEmpty())
				{
					if (bResume)
					{
						AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(),
							FString::Printf(TEXT("That chat could not be resumed (%s); this is a new one."), *Error));
						StartThread(false);
						return;
					}
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), TEXT("Codex could not start a chat: ") + Error);
					return;
				}
				const TSharedPtr<FJsonObject> Thread = ObjectField(Result, TEXT("thread"));
				if (bResume)
				{
					// A resumed thread comes back with its turns, items as they streamed live.
					ShowEarlier([this, &Thread]()
					{
						for (const TSharedPtr<FJsonValue>& Turn : ArrayField(Thread, TEXT("turns")))
						{
							for (const TSharedPtr<FJsonValue>& Item : ArrayField(Turn.IsValid() && Turn->Type == EJson::Object ? Turn->AsObject() : nullptr, TEXT("items")))
							{
								ReplayItem(Item.IsValid() && Item->Type == EJson::Object ? Item->AsObject() : nullptr);
							}
						}
					}, /*bMoreBefore=*/false);
				}
				Ready(StringField(Thread, TEXT("id")));
			});
		}

		/** One item of a resumed thread: what the user sent, or anything else as it showed live. */
		void ReplayItem(const TSharedPtr<FJsonObject>& Item)
		{
			if (!Item.IsValid())
			{
				return;
			}
			if (StringField(Item, TEXT("type")) != TEXT("userMessage"))
			{
				UpdateItem(Item, /*bCompleted=*/true);
				return;
			}
			FString Text;
			TArray<FString> Images;
			for (const TSharedPtr<FJsonValue>& Input : ArrayField(Item, TEXT("content")))
			{
				const TSharedPtr<FJsonObject> Part = Input.IsValid() && Input->Type == EJson::Object ? Input->AsObject() : nullptr;
				const FString Kind = StringField(Part, TEXT("type"));
				if (Kind == TEXT("text"))
				{
					Text += StringField(Part, TEXT("text"));
				}
				else if (Kind == TEXT("localImage"))
				{
					Images.Add(StringField(Part, TEXT("path")));
				}
			}
			Text = HyperAIStudio::ChatHistory::StripInjectedContext(Text).TrimStartAndEnd();
			if (!Text.IsEmpty() || !Images.IsEmpty())
			{
				AddItem(FHyperAIStudioChatItem::EKind::User, StringField(Item, TEXT("id")), FString(), Text)->Images = Images;
			}
		}

		virtual void SendNow(const FString& Text, const TArray<FString>& Images) override
		{
			TArray<TSharedPtr<FJsonValue>> Input = {Obj(Object({
				{TEXT("type"), Str(TEXT("text"))}, {TEXT("text"), Str(Text)}, {TEXT("text_elements"), Arr({})}}))};
			for (const FString& Image : Images)
			{
				// Codex reads the file itself.
				Input.Add(Obj(Object({{TEXT("type"), Str(TEXT("localImage"))}, {TEXT("path"), Str(Image)}})));
			}
			const TSharedRef<FJsonObject> Params = Object({{TEXT("threadId"), Str(SessionId)}, {TEXT("input"), Arr(Input)}});
			Request(TEXT("turn/start"), Params, [this](const TSharedPtr<FJsonObject>& Result, const FString& Error)
			{
				if (!Error.IsEmpty())
				{
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), TEXT("Codex did not take the message: ") + Error);
					TurnFinished();
					return;
				}
				TurnId = StringField(ObjectField(Result, TEXT("turn")), TEXT("id"));
			});
		}

		virtual void InterruptNow() override
		{
			if (!TurnId.IsEmpty())
			{
				Request(TEXT("turn/interrupt"), Object({{TEXT("threadId"), Str(SessionId)}, {TEXT("turnId"), Str(TurnId)}}));
			}
		}

		static FString StatusOf(const FString& Status)
		{
			return Status == TEXT("inProgress") ? FString(TEXT("running")) : Status;
		}

		/** Codex's failure for a spent image allowance, as a sentence with the time it comes back. */
		static FString LimitText(const TSharedPtr<FJsonObject>& Failure)
		{
			double ResetsAt = 0.0;
			Failure->TryGetNumberField(TEXT("resetsAt"), ResetsAt);
			if (ResetsAt > 1e11)
			{
				ResetsAt /= 1000.0;
			}
			return ResetsAt > 0.0
				? FString::Printf(TEXT("Codex's image generation limit is used up until %s."),
					*FDateTime::FromUnixTimestamp(static_cast<int64>(ResetsAt)).ToString(TEXT("%Y-%m-%d %H:%M UTC")))
				: FString(TEXT("Codex's image generation limit is used up."));
		}

		void UpdateItem(const TSharedPtr<FJsonObject>& Item, const bool bCompleted)
		{
			const FString Type = StringField(Item, TEXT("type"));
			const FString Id = StringField(Item, TEXT("id"));
			TSharedPtr<FHyperAIStudioChatItem> Existing = FindItem(Id);
			auto Ensure = [&](const FHyperAIStudioChatItem::EKind Kind, const FString& Title)
			{
				if (!Existing.IsValid())
				{
					Existing = AddItem(Kind, Id, Title, FString(), TEXT("running"));
				}
				return Existing;
			};
			if (Type == TEXT("agentMessage"))
			{
				Ensure(FHyperAIStudioChatItem::EKind::Agent, FString());
				const FString Text = StringField(Item, TEXT("text"));
				if (bCompleted || !Text.IsEmpty())
				{
					Existing->Text = Clip(Text);
				}
			}
			else if (Type == TEXT("reasoning"))
			{
				Ensure(FHyperAIStudioChatItem::EKind::Reasoning, FString());
				TArray<FString> Parts;
				for (const TSharedPtr<FJsonValue>& Part : ArrayField(Item, TEXT("summary")))
				{
					Parts.Add(Part->Type == EJson::String ? Part->AsString()
						: Part->Type == EJson::Object ? StringField(Part->AsObject(), TEXT("text")) : FString());
				}
				if (!Parts.IsEmpty())
				{
					Existing->Text = Clip(FString::Join(Parts, TEXT("\n\n")));
				}
			}
			else if (Type == TEXT("commandExecution"))
			{
				Ensure(FHyperAIStudioChatItem::EKind::Command, StringField(Item, TEXT("command")));
				Existing->Status = StatusOf(StringField(Item, TEXT("status")));
				FString Output;
				if (Item->TryGetStringField(TEXT("aggregatedOutput"), Output) && !Output.IsEmpty())
				{
					Existing->Text = Clip(Output);
				}
				double ExitCode = 0.0;
				if (bCompleted && Item->TryGetNumberField(TEXT("exitCode"), ExitCode) && ExitCode != 0.0)
				{
					Existing->Status = FString::Printf(TEXT("exit code %d"), static_cast<int32>(ExitCode));
				}
			}
			else if (Type == TEXT("fileChange"))
			{
				TArray<FString> Paths;
				FString Diffs;
				for (const TSharedPtr<FJsonValue>& Change : ArrayField(Item, TEXT("changes")))
				{
					const TSharedPtr<FJsonObject> Entry = Change->AsObject();
					Paths.Add(FPaths::GetCleanFilename(StringField(Entry, TEXT("path"))));
					Diffs += StringField(Entry, TEXT("path")) + TEXT("\n") + StringField(Entry, TEXT("diff")) + TEXT("\n");
				}
				Ensure(FHyperAIStudioChatItem::EKind::FileChange, TEXT("Edit ") + FString::Join(Paths, TEXT(", ")));
				Existing->Title = TEXT("Edit ") + FString::Join(Paths, TEXT(", "));
				Existing->Text = Clip(Diffs);
				Existing->Status = StatusOf(StringField(Item, TEXT("status")));
			}
			else if (Type == TEXT("mcpToolCall") || Type == TEXT("dynamicToolCall"))
			{
				const FString Server = StringField(Item, TEXT("server"));
				const FString Tool = StringField(Item, TEXT("tool"));
				TSharedPtr<FJsonValue> Arguments = Item->TryGetField(TEXT("arguments"));
				// Unreal's generic call_tool names the real tool in its arguments.
				FString Title = Server.IsEmpty() ? Tool : Server + TEXT(" / ") + Tool;
				if (const TSharedPtr<FJsonObject> Args = Arguments.IsValid() && Arguments->Type == EJson::Object ? Arguments->AsObject() : nullptr;
					Args.IsValid() && Args->HasField(TEXT("tool_name")))
				{
					Title = StringField(Args, TEXT("toolset_name")) + TEXT(".") + StringField(Args, TEXT("tool_name"));
				}
				Ensure(FHyperAIStudioChatItem::EKind::Tool, Title);
				Existing->Status = StatusOf(StringField(Item, TEXT("status")));
				FString Detail = TEXT("Arguments: ") + Compact(Arguments);
				if (const TSharedPtr<FJsonObject> Error = ObjectField(Item, TEXT("error")); Error.IsValid())
				{
					Detail += TEXT("\n\nError: ") + StringField(Error, TEXT("message"));
					Existing->Status = TEXT("failed");
				}
				if (const TSharedPtr<FJsonObject> Result = ObjectField(Item, TEXT("result")); Result.IsValid())
				{
					for (const TSharedPtr<FJsonValue>& Content : ArrayField(Result, TEXT("content")))
					{
						Detail += TEXT("\n\n") + StringField(Content->AsObject(), TEXT("text"));
					}
				}
				Existing->Text = Clip(Detail);
			}
			else if (Type == TEXT("webSearch"))
			{
				Ensure(FHyperAIStudioChatItem::EKind::Tool, TEXT("Web search: ") + StringField(Item, TEXT("query")));
				Existing->Status = bCompleted ? TEXT("completed") : TEXT("running");
			}
			else if (Type == TEXT("imageGeneration"))
			{
				Ensure(FHyperAIStudioChatItem::EKind::Image, TEXT("Generated image"));
				const FString Revised = StringField(Item, TEXT("revisedPrompt"));
				if (!Revised.IsEmpty())
				{
					Existing->Text = Clip(Revised);
				}
				FString Saved = StringField(Item, TEXT("savedPath"));
				const FString Data = StringField(Item, TEXT("result"));
				if (Saved.IsEmpty() && bCompleted && !Data.IsEmpty() && !bReplaying)
				{
					Saved = SaveImageData(Data, TEXT("codex-") + Id);
				}
				if (!Saved.IsEmpty())
				{
					Existing->Images = {Saved};
				}
				if (const TSharedPtr<FJsonObject> Failure = ObjectField(Item, TEXT("failure")); Failure.IsValid())
				{
					Existing->Status = TEXT("failed");
					Existing->Text = LimitText(Failure);
				}
				else
				{
					Existing->Status = !bCompleted ? FString(TEXT("running")) : Existing->Images.IsEmpty() ? FString(TEXT("failed")) : FString(TEXT("completed"));
				}
			}
			else if (Type == TEXT("imageView"))
			{
				Ensure(FHyperAIStudioChatItem::EKind::Image, TEXT("Looked at an image"));
				const FString Path = StringField(Item, TEXT("path"));
				Existing->Images = {Path};
				Existing->Text = Path;
				Existing->Status = TEXT("completed");
			}
			else if (Type == TEXT("contextCompaction") && bCompleted)
			{
				AddItem(FHyperAIStudioChatItem::EKind::Notice, Id, FString(), TEXT("Codex compacted the conversation to free context."));
			}
			if (Existing.IsValid() && bCompleted && Existing->Status == TEXT("running"))
			{
				Existing->Status = TEXT("completed");
			}
		}

		virtual void HandleMessage(const FString& Method, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& RequestId) override
		{
			if (Method == TEXT("item/started") || Method == TEXT("item/completed"))
			{
				UpdateItem(ObjectField(Params, TEXT("item")), Method == TEXT("item/completed"));
				Changed();
			}
			else if (Method == TEXT("item/agentMessage/delta") || Method == TEXT("item/reasoning/summaryTextDelta")
				|| Method == TEXT("item/commandExecution/outputDelta") || Method == TEXT("item/fileChange/outputDelta"))
			{
				if (const TSharedPtr<FHyperAIStudioChatItem> Item = FindItem(StringField(Params, TEXT("itemId"))))
				{
					Item->Text = Clip(Item->Text + StringField(Params, TEXT("delta")));
					Changed();
				}
			}
			else if (Method == TEXT("turn/plan/updated"))
			{
				Plan.Reset();
				for (const TSharedPtr<FJsonValue>& Step : ArrayField(Params, TEXT("plan")))
				{
					Plan.Add({StringField(Step->AsObject(), TEXT("step")), StringField(Step->AsObject(), TEXT("status"))});
				}
				Changed();
			}
			else if (Method == TEXT("thread/tokenUsage/updated"))
			{
				const TSharedPtr<FJsonObject> TokenUsage = ObjectField(Params, TEXT("tokenUsage"));
				double Total = 0.0;
				double Last = 0.0;
				double Window = 0.0;
				if (const TSharedPtr<FJsonObject> TotalUsage = ObjectField(TokenUsage, TEXT("total")))
				{
					TotalUsage->TryGetNumberField(TEXT("totalTokens"), Total);
				}
				if (const TSharedPtr<FJsonObject> LastUsage = ObjectField(TokenUsage, TEXT("last")))
				{
					LastUsage->TryGetNumberField(TEXT("totalTokens"), Last);
				}
				if (TokenUsage.IsValid())
				{
					TokenUsage->TryGetNumberField(TEXT("modelContextWindow"), Window);
				}
				TokensText = Window > 0.0
					? FString::Printf(TEXT("%.1fk tokens, %d%% of context used"), Total / 1000.0, FMath::RoundToInt(100.0 * Last / Window))
					: FString::Printf(TEXT("%.1fk tokens"), Total / 1000.0);
				Usage = LimitsText.IsEmpty() ? TokensText : TokensText + TEXT(" | ") + LimitsText;
				Changed();
			}
			else if (Method == TEXT("account/rateLimits/updated"))
			{
				const TSharedPtr<FJsonObject> Limits = ObjectField(Params, TEXT("rateLimits"));
				TArray<FString> Parts;
				for (const TCHAR* Window : {TEXT("primary"), TEXT("secondary")})
				{
					if (const TSharedPtr<FJsonObject> Slot = ObjectField(Limits, Window))
					{
						double Used = 0.0;
						double Minutes = 0.0;
						Slot->TryGetNumberField(TEXT("usedPercent"), Used);
						Slot->TryGetNumberField(TEXT("windowDurationMins"), Minutes);
						Parts.Add(FString::Printf(TEXT("%s limit %d%% used"), Minutes >= 1440.0 ? TEXT("weekly") : *FString::Printf(TEXT("%dh"), FMath::Max(1, FMath::RoundToInt(Minutes / 60.0))),
							FMath::RoundToInt(Used)));
					}
				}
				LimitsText = FString::Join(Parts, TEXT(", "));
				Usage = TokensText.IsEmpty() ? LimitsText : TokensText + TEXT(" | ") + LimitsText;
				Changed();
			}
			else if (Method == TEXT("turn/completed"))
			{
				const TSharedPtr<FJsonObject> Turn = ObjectField(Params, TEXT("turn"));
				const FString Status = StringField(Turn, TEXT("status"));
				if (Status == TEXT("failed"))
				{
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(),
						TEXT("The turn failed: ") + StringField(ObjectField(Turn, TEXT("error")), TEXT("message")));
				}
				else if (Status == TEXT("interrupted"))
				{
					AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), TEXT("Stopped."));
				}
				TurnId.Reset();
				TurnFinished();
			}
			else if (Method == TEXT("error"))
			{
				bool bWillRetry = false;
				Params->TryGetBoolField(TEXT("willRetry"), bWillRetry);
				AddItem(bWillRetry ? FHyperAIStudioChatItem::EKind::Notice : FHyperAIStudioChatItem::EKind::Error, FString(), FString(),
					(bWillRetry ? TEXT("Retrying: ") : TEXT("Error: ")) + StringField(ObjectField(Params, TEXT("error")), TEXT("message")));
			}
			else if (RequestId.IsValid())
			{
				HandleRequest(Method, Params, RequestId);
			}
		}

		void HandleRequest(const FString& Method, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& RequestId)
		{
			const FString ItemId = StringField(Params, TEXT("itemId"));
			const FString Reason = StringField(Params, TEXT("reason"));
			if (Method == TEXT("item/commandExecution/requestApproval"))
			{
				AskApproval(RequestId, ItemId, TEXT("Run this command?"),
					StringField(Params, TEXT("command")) + (Reason.IsEmpty() ? FString() : TEXT("\n\n") + Reason),
					{TEXT("Run"), TEXT("Run, and stop asking this session"), TEXT("Don't run")},
					{TEXT("accept"), TEXT("acceptForSession"), TEXT("decline")});
			}
			else if (Method == TEXT("item/fileChange/requestApproval"))
			{
				AskApproval(RequestId, ItemId, TEXT("Apply these file changes?"), Reason,
					{TEXT("Apply"), TEXT("Apply, and stop asking this session"), TEXT("Don't apply")},
					{TEXT("accept"), TEXT("acceptForSession"), TEXT("decline")});
			}
			else if (Method == TEXT("item/permissions/requestApproval"))
			{
				RequestedPermissions.Add(ItemId + TEXT("#approval"), ObjectField(Params, TEXT("permissions")));
				AskApproval(RequestId, ItemId, TEXT("Give Codex more access?"),
					Compact(Params->TryGetField(TEXT("permissions"))) + (Reason.IsEmpty() ? FString() : TEXT("\n\n") + Reason),
					{TEXT("Allow"), TEXT("Deny")}, {TEXT("allow"), TEXT("deny")});
			}
			else if (Method == TEXT("mcpServer/elicitation/request")
				&& StringField(ObjectField(Params, TEXT("_meta")), TEXT("codex_approval_kind")) == TEXT("mcp_tool_call"))
			{
				AskToolApproval(RequestId, Params);
			}
			else if (Method == TEXT("mcpServer/elicitation/request"))
			{
				Respond(RequestId, Object({{TEXT("action"), Str(TEXT("decline"))}}));
				AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(),
					TEXT("An MCP server asked for input this view cannot collect yet; it was declined. Use the terminal view for it."));
			}
			else
			{
				// Anything this view cannot show must still be answered, or the turn waits for ever.
				RespondError(RequestId, TEXT("HyperAI's native chat view does not support ") + Method);
				AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(),
					FString::Printf(TEXT("Codex asked for something this view cannot show (%s); use the terminal view for it."), *Method));
			}
		}

		/**
		 * Codex asks before an MCP tool runs, as an elicitation that carries the tool and its arguments. It is shown as an
		 * approval, with "for this chat" and "always" where Codex says it can remember the answer.
		 */
		void AskToolApproval(const TSharedPtr<FJsonValue>& RequestId, const TSharedPtr<FJsonObject>& Params)
		{
			const TSharedPtr<FJsonObject> Meta = ObjectField(Params, TEXT("_meta"));
			TArray<FString> Persist;
			if (const TSharedPtr<FJsonValue> Field = Meta->TryGetField(TEXT("persist")); Field.IsValid() && Field->Type == EJson::String)
			{
				Persist.Add(Field->AsString());
			}
			for (const TSharedPtr<FJsonValue>& Value : ArrayField(Meta, TEXT("persist")))
			{
				if (Value.IsValid() && Value->Type == EJson::String)
				{
					Persist.Add(Value->AsString());
				}
			}
			TArray<FString> Choices = {TEXT("Allow")};
			TArray<FString> ChoiceIds = {TEXT("accept")};
			if (Persist.Contains(TEXT("session")))
			{
				Choices.Add(TEXT("Allow, and stop asking this chat"));
				ChoiceIds.Add(TEXT("session"));
			}
			if (Persist.Contains(TEXT("always")))
			{
				Choices.Add(TEXT("Always allow this tool"));
				ChoiceIds.Add(TEXT("always"));
			}
			Choices.Add(TEXT("Don't allow"));
			ChoiceIds.Add(TEXT("decline"));
			// Unreal's call_tool names the tool it runs, and that tool's own arguments, in its arguments.
			const TSharedPtr<FJsonObject> ToolParams = ObjectField(Meta, TEXT("toolParams"));
			const bool bCallTool = ToolParams.IsValid() && ToolParams->HasField(TEXT("tool_name"));
			const FString Tool = bCallTool
				? StringField(ToolParams, TEXT("toolset_name")) + TEXT(".") + StringField(ToolParams, TEXT("tool_name"))
				: StringField(Meta, TEXT("toolTitle"));
			TArray<FString> Lines;
			if (!Tool.IsEmpty())
			{
				Lines.Add(TEXT("Tool: ") + Tool);
			}
			const FString Description = StringField(Meta, TEXT("toolDescription"));
			if (!Description.IsEmpty())
			{
				Lines.Add(Description.Left(300));
			}
			const FString Arguments = Compact(bCallTool ? ToolParams->TryGetField(TEXT("arguments")) : Meta->TryGetField(TEXT("toolParams")));
			if (!Arguments.IsEmpty())
			{
				Lines.Add(TEXT("Arguments: ") + (Arguments.Len() > 2000 ? Arguments.Left(2000) + TEXT("...") : Arguments));
			}
			const FString Id = TEXT("mcp-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
			ToolApprovals.Add(Id + TEXT("#approval"));
			const FString Question = StringField(Params, TEXT("message"));
			AskApproval(RequestId, Id, Question.IsEmpty() ? FString(TEXT("Allow this MCP tool to run?")) : Question, FString::Join(Lines, TEXT("\n\n")), Choices, ChoiceIds);
		}

		virtual TSharedRef<FJsonObject> AnswerPayload(const FHyperAIStudioChatItem& Item, const int32 Choice) override
		{
			if (const TSharedPtr<FJsonObject>* Requested = RequestedPermissions.Find(Item.Id))
			{
				const bool bAllow = Item.ChoiceIds[Choice] == TEXT("allow");
				return Object({{TEXT("permissions"), Obj(bAllow && Requested->IsValid() ? Requested->ToSharedRef() : MakeShared<FJsonObject>())}});
			}
			if (ToolApprovals.Remove(Item.Id) > 0)
			{
				// An approval answers an elicitation: accepted with an empty form, and how long Codex should remember it.
				const FString Picked = Item.ChoiceIds[Choice];
				const bool bAccept = Picked != TEXT("decline");
				const bool bRemember = Picked == TEXT("session") || Picked == TEXT("always");
				const TSharedPtr<FJsonValue> None = MakeShared<FJsonValueNull>();
				return Object({{TEXT("action"), Str(bAccept ? TEXT("accept") : TEXT("decline"))},
					{TEXT("content"), bAccept ? Obj(MakeShared<FJsonObject>()) : None},
					{TEXT("_meta"), bRemember ? Obj(Object({{TEXT("persist"), Str(Picked)}})) : None}});
			}
			return Object({{TEXT("decision"), Str(Item.ChoiceIds[Choice])}});
		}

	private:
		FString TurnId;
		FString TokensText;
		FString LimitsText;
		TMap<FString, TSharedPtr<FJsonObject>> RequestedPermissions;
		/** Open approvals that answer Codex's MCP tool elicitations. */
		TSet<FString> ToolApprovals;
	};

	// ------------------------------------------------------------------------------------------------------ ACP agents

	/** An agent HyperAI hosts through the Agent Client Protocol, as the ACP registry starts it. */
	struct FAcpAgent
	{
		const TCHAR* Name;
		const TCHAR* Executable;
		const TCHAR* Arguments;
		/** Its own settings name Unreal MCP (HyperAI writes Gemini's), so the session does not hand it over again. */
		bool bConfiguresMcp = false;
		/** A launch flag for the model, where the agent takes one; others are set through the session. */
		const TCHAR* ModelFlag = nullptr;
	};

	const TArray<FAcpAgent>& AcpAgents()
	{
		static const TArray<FAcpAgent> Agents = {
			{TEXT("Gemini"), TEXT("gemini"), TEXT("--acp"), true, TEXT("--model")},
			{TEXT("GitHub Copilot"), TEXT("copilot"), TEXT("--acp")},
			{TEXT("Cursor Agent"), TEXT("cursor-agent"), TEXT("acp")},
			{TEXT("OpenCode"), TEXT("opencode"), TEXT("acp")},
			{TEXT("Goose"), TEXT("goose"), TEXT("acp")},
			{TEXT("Qwen Code"), TEXT("qwen"), TEXT("--acp --experimental-skills")},
			{TEXT("Kimi"), TEXT("kimi"), TEXT("acp")},
			{TEXT("Cline"), TEXT("cline"), TEXT("--acp")},
			{TEXT("Auggie"), TEXT("auggie"), TEXT("--acp")},
		};
		return Agents;
	}

	const FAcpAgent* FindAcpAgent(const FString& Name)
	{
		return AcpAgents().FindByPredicate([&Name](const FAcpAgent& Agent) { return Name.Equals(Agent.Name, ESearchCase::IgnoreCase); });
	}

	/** Most model lists fit; OpenCode can offer every model of every provider it knows. */
	constexpr int32 MaxAgentModels = 100;

	/** An agent through the Agent Client Protocol: Gemini, GitHub Copilot, OpenCode and the rest of AcpAgents. */
	class FAcpChat final : public FHyperAIStudioNativeChat
	{
	public:
		FAcpChat(const FAcpAgent& InAgent, const FString& InProjectRoot, const FString& InModel, const FString& InResumeSessionId)
			: FHyperAIStudioNativeChat(InAgent.Name, InProjectRoot, InModel, InResumeSessionId)
			, Agent(InAgent)
		{
		}

	protected:
		virtual FString CommandLine() const override
		{
			// The model lands on a cmd.exe line, so only a plain id goes.
			const bool bModelFlag = Agent.ModelFlag && UHyperAIStudioSettings::IsSafeModelId(Model);
			return Executable(Agent.Executable) + (bModelFlag ? FString::Printf(TEXT(" %s %s"), Agent.ModelFlag, *Model) : FString()) + TEXT(" ") + Agent.Arguments;
		}

		virtual bool WritesJsonRpcVersion() const override { return true; }

		virtual void Handshake() override
		{
			const TSharedRef<FJsonObject> FileSystem = Object({{TEXT("readTextFile"), Bool(false)}, {TEXT("writeTextFile"), Bool(false)}});
			const TSharedRef<FJsonObject> Params = Object({
				{TEXT("protocolVersion"), Num(1)}, {TEXT("clientCapabilities"), Obj(Object({{TEXT("fs"), Obj(FileSystem)}}))}});
			Request(TEXT("initialize"), Params, [this](const TSharedPtr<FJsonObject>& Result, const FString& Error)
			{
				if (!Error.IsEmpty())
				{
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), FString::Printf(TEXT("%s refused to start: %s"), *AgentName, *Error));
					return;
				}
				bool bCanLoad = false;
				if (const TSharedPtr<FJsonObject> Capabilities = ObjectField(Result, TEXT("agentCapabilities")))
				{
					Capabilities->TryGetBoolField(TEXT("loadSession"), bCanLoad);
					if (const TSharedPtr<FJsonObject> Prompt = ObjectField(Capabilities, TEXT("promptCapabilities")))
					{
						Prompt->TryGetBoolField(TEXT("image"), bTakesImages);
					}
					if (const TSharedPtr<FJsonObject> Mcp = ObjectField(Capabilities, TEXT("mcpCapabilities")))
					{
						Mcp->TryGetBoolField(TEXT("http"), bTakesHttpMcp);
					}
				}
				if (!Agent.bConfiguresMcp && !bTakesHttpMcp)
				{
					AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), FString::Printf(
						TEXT("%s cannot take Unreal MCP from HyperAI (it does not accept HTTP servers), so it has only the tools in its own settings."), *AgentName));
				}
				OpenSession(bCanLoad && !ResumeSessionId.IsEmpty());
			});
		}

		/** Unreal MCP for this session, unless the agent's own settings name it already. */
		TArray<TSharedPtr<FJsonValue>> McpServers() const
		{
			if (Agent.bConfiguresMcp || !bTakesHttpMcp)
			{
				return {};
			}
			const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
			return {Obj(Object({{TEXT("type"), Str(TEXT("http"))}, {TEXT("name"), Str(TEXT("unreal-mcp"))},
				{TEXT("url"), Str(FHyperAIStudioService::GetEndpoint(Settings->UnrealMCPPort, Settings->UnrealMCPUrlPath))}, {TEXT("headers"), Arr({})}}))};
		}

		void OpenSession(const bool bLoad)
		{
			const TSharedRef<FJsonObject> Params = Object({{TEXT("cwd"), Str(ProjectRoot)}, {TEXT("mcpServers"), Arr(McpServers())}});
			if (bLoad)
			{
				Params->SetStringField(TEXT("sessionId"), ResumeSessionId);
			}
			Request(bLoad ? TEXT("session/load") : TEXT("session/new"), Params, [this, bLoad](const TSharedPtr<FJsonObject>& Result, const FString& Error)
			{
				if (!Error.IsEmpty())
				{
					if (bLoad)
					{
						OpenSession(false);
						return;
					}
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), Error.Contains(TEXT("auth"), ESearchCase::IgnoreCase)
						? FString::Printf(TEXT("%s needs you to sign in: switch this tab to the terminal view and sign in there."), *AgentName)
						: FString::Printf(TEXT("%s could not start a chat: %s"), *AgentName, *Error));
					return;
				}
				const FString Session = bLoad ? ResumeSessionId : StringField(Result, TEXT("sessionId"));
				ReadModels(Result);
				ChooseModel(Session, [this, Session]() { Ready(Session); });
			});
		}

		/**
		 * The agent's own models, from its model config option or the older models field, kept as its Model menu. A
		 * chat that loads an old session may not send them; the menu keeps what it had.
		 */
		void ReadModels(const TSharedPtr<FJsonObject>& Result)
		{
			TArray<FString> Values;
			for (const TSharedPtr<FJsonValue>& Value : ArrayField(Result, TEXT("configOptions")))
			{
				const TSharedPtr<FJsonObject> Option = Value->AsObject();
				if (StringField(Option, TEXT("category")) != TEXT("model") || !ModelConfigId.IsEmpty())
				{
					continue;
				}
				ModelConfigId = StringField(Option, TEXT("id"));
				CurrentModel = StringField(Option, TEXT("currentValue"));
				for (const TSharedPtr<FJsonValue>& Choice : ArrayField(Option, TEXT("options")))
				{
					// Choices may come in groups, each with its own options.
					const TArray<TSharedPtr<FJsonValue>> Grouped = ArrayField(Choice->AsObject(), TEXT("options"));
					for (const TSharedPtr<FJsonValue>& Entry : Grouped.IsEmpty() ? TArray<TSharedPtr<FJsonValue>>{Choice} : Grouped)
					{
						Values.AddUnique(StringField(Entry->AsObject(), TEXT("value")));
					}
				}
			}
			if (ModelConfigId.IsEmpty())
			{
				const TSharedPtr<FJsonObject> Models = ObjectField(Result, TEXT("models"));
				CurrentModel = StringField(Models, TEXT("currentModelId"));
				for (const TSharedPtr<FJsonValue>& Entry : ArrayField(Models, TEXT("availableModels")))
				{
					Values.AddUnique(StringField(Entry->AsObject(), TEXT("modelId")));
				}
			}
			Values.Remove(FString());
			Values.SetNum(FMath::Min(Values.Num(), MaxAgentModels));
			OfferedModels = Values;
			UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
			if (Values.IsEmpty() || bHeadless)
			{
				return;
			}
			FHyperAIStudioAgentModelRoute* Route = Settings->FindAgentModelRoute(AgentName);
			if (!Route)
			{
				Route = &Settings->AgentModelRoutes.AddDefaulted_GetRef();
				Route->AgentName = AgentName;
			}
			if (Route->Models != Values)
			{
				Route->Models = Values;
				Settings->SaveConfig();
			}
		}

		/** Switches the session to the chosen model when the agent offers it and it is not already on it. */
		void ChooseModel(const FString& Session, TFunction<void()> Then)
		{
			if (Model.IsEmpty() || Model == CurrentModel || Agent.ModelFlag)
			{
				Then();
				return;
			}
			if (!OfferedModels.Contains(Model))
			{
				AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), FString::Printf(TEXT("%s does not offer %s, so it uses %s."),
					*AgentName, *Model, CurrentModel.IsEmpty() ? TEXT("its own default") : *CurrentModel));
				Then();
				return;
			}
			const bool bOption = !ModelConfigId.IsEmpty();
			const TSharedRef<FJsonObject> Params = bOption
				? Object({{TEXT("sessionId"), Str(Session)}, {TEXT("configId"), Str(ModelConfigId)}, {TEXT("value"), Str(Model)}})
				: Object({{TEXT("sessionId"), Str(Session)}, {TEXT("modelId"), Str(Model)}});
			Request(bOption ? TEXT("session/set_config_option") : TEXT("session/set_model"), Params, [this, Then](const TSharedPtr<FJsonObject>&, const FString& Error)
			{
				if (!Error.IsEmpty())
				{
					AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), FString::Printf(TEXT("%s kept its model: %s"), *AgentName, *Error));
				}
				else
				{
					CurrentModel = Model;
				}
				Then();
			});
		}

		virtual void SendNow(const FString& Text, const TArray<FString>& Images) override
		{
			CurrentAgent.Reset();
			CurrentThought.Reset();
			TArray<TSharedPtr<FJsonValue>> Blocks = {Obj(Object({{TEXT("type"), Str(TEXT("text"))}, {TEXT("text"), Str(Text)}}))};
			int32 LeftOut = 0;
			for (const FString& Image : Images)
			{
				TArray<uint8> Bytes;
				if (!bTakesImages || !FFileHelper::LoadFileToArray(Bytes, *Image))
				{
					++LeftOut;
					continue;
				}
				Blocks.Add(Obj(Object({{TEXT("type"), Str(TEXT("image"))}, {TEXT("mimeType"), Str(MimeType(Image))},
					{TEXT("data"), Str(FBase64::Encode(Bytes))}})));
			}
			if (LeftOut > 0)
			{
				AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), bTakesImages
					? FString(TEXT("An attached picture could not be read, so it was left out."))
					: FString::Printf(TEXT("%s did not say it takes pictures, so only the text went."), *AgentName));
			}
			Request(TEXT("session/prompt"), Object({{TEXT("sessionId"), Str(SessionId)}, {TEXT("prompt"), Arr(Blocks)}}),
				[this](const TSharedPtr<FJsonObject>& Result, const FString& Error)
			{
				const FString StopReason = StringField(Result, TEXT("stopReason"));
				if (!Error.IsEmpty())
				{
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), FString::Printf(TEXT("%s: %s"), *AgentName, *Error));
				}
				else if (StopReason == TEXT("cancelled"))
				{
					AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), TEXT("Stopped."));
				}
				else if (StopReason == TEXT("max_tokens") || StopReason == TEXT("refusal"))
				{
					AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), FString::Printf(TEXT("%s ended the turn early: %s"), *AgentName, *StopReason));
				}
				TurnFinished();
			});
		}

		virtual void InterruptNow() override
		{
			Notify(TEXT("session/cancel"), Object({{TEXT("sessionId"), Str(SessionId)}}));
		}

		static FString StatusOf(const FString& Status)
		{
			return Status == TEXT("in_progress") || Status == TEXT("pending") ? FString(TEXT("running")) : Status;
		}

		static FString ContentText(const TArray<TSharedPtr<FJsonValue>>& Contents)
		{
			FString Text;
			for (const TSharedPtr<FJsonValue>& Value : Contents)
			{
				const TSharedPtr<FJsonObject> Entry = Value->AsObject();
				const FString Type = StringField(Entry, TEXT("type"));
				if (Type == TEXT("content"))
				{
					Text += StringField(ObjectField(Entry, TEXT("content")), TEXT("text")) + TEXT("\n");
				}
				else if (Type == TEXT("diff"))
				{
					Text += StringField(Entry, TEXT("path")) + TEXT("\n") + StringField(Entry, TEXT("newText")) + TEXT("\n");
				}
			}
			return Text;
		}

		virtual void HandleMessage(const FString& Method, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& RequestId) override
		{
			if (Method == TEXT("session/update"))
			{
				const TSharedPtr<FJsonObject> Update = ObjectField(Params, TEXT("update"));
				const FString Kind = StringField(Update, TEXT("sessionUpdate"));
				const TSharedPtr<FJsonObject> Content = ObjectField(Update, TEXT("content"));
				if (Kind == TEXT("agent_message_chunk") && StringField(Content, TEXT("type")) == TEXT("image"))
				{
					AddPicture(StringField(Content, TEXT("data")), FString());
					// Text after a picture is a new message.
					CurrentAgent.Reset();
				}
				else if (Kind == TEXT("agent_message_chunk") || Kind == TEXT("agent_thought_chunk"))
				{
					const bool bThought = Kind == TEXT("agent_thought_chunk");
					TSharedPtr<FHyperAIStudioChatItem>& Current = bThought ? CurrentThought : CurrentAgent;
					if (!Current.IsValid())
					{
						Current = AddItem(bThought ? FHyperAIStudioChatItem::EKind::Reasoning : FHyperAIStudioChatItem::EKind::Agent, FString(), FString(), FString());
					}
					Current->Text = Clip(Current->Text + StringField(ObjectField(Update, TEXT("content")), TEXT("text")));
					if (!bThought)
					{
						CurrentThought.Reset();
					}
				}
				else if (Kind == TEXT("tool_call") || Kind == TEXT("tool_call_update"))
				{
					const FString Id = StringField(Update, TEXT("toolCallId"));
					TSharedPtr<FHyperAIStudioChatItem> Item = FindItem(Id);
					if (!Item.IsValid())
					{
						Item = AddItem(FHyperAIStudioChatItem::EKind::Tool, Id, StringField(Update, TEXT("title")), FString(), TEXT("running"));
					}
					const FString Title = StringField(Update, TEXT("title"));
					if (!Title.IsEmpty())
					{
						Item->Title = Title;
					}
					const FString Status = StringField(Update, TEXT("status"));
					if (!Status.IsEmpty())
					{
						Item->Status = StatusOf(Status);
					}
					const FString Text = ContentText(ArrayField(Update, TEXT("content")));
					if (!Text.IsEmpty())
					{
						Item->Text = Clip(Text);
					}
					int32 Picture = 0;
					for (const TSharedPtr<FJsonValue>& Value : ArrayField(Update, TEXT("content")))
					{
						const TSharedPtr<FJsonObject> Block = ObjectField(Value->AsObject(), TEXT("content"));
						if (StringField(Block, TEXT("type")) == TEXT("image"))
						{
							AddPicture(StringField(Block, TEXT("data")), FString::Printf(TEXT("%s#image%d"), *Id, Picture++));
						}
					}
					// Text after a tool call is a new message.
					CurrentAgent.Reset();
				}
				else if (Kind == TEXT("plan"))
				{
					Plan.Reset();
					for (const TSharedPtr<FJsonValue>& Entry : ArrayField(Update, TEXT("entries")))
					{
						const FString Status = StringField(Entry->AsObject(), TEXT("status"));
						Plan.Add({StringField(Entry->AsObject(), TEXT("content")), Status == TEXT("in_progress") ? FString(TEXT("inProgress")) : Status});
					}
				}
				Changed();
			}
			else if (Method == TEXT("session/request_permission") && RequestId.IsValid() && bHeadless)
			{
				// Nobody is watching, so the chat answers: a picture tool may run, and nothing else.
				const FString Title = StringField(ObjectField(Params, TEXT("toolCall")), TEXT("title"));
				const bool bPictureTool = Title.Contains(TEXT("image"), ESearchCase::IgnoreCase);
				FString Picked;
				for (const TSharedPtr<FJsonValue>& Option : ArrayField(Params, TEXT("options")))
				{
					if (StringField(Option->AsObject(), TEXT("kind")) == (bPictureTool ? TEXT("allow_once") : TEXT("reject_once")))
					{
						Picked = StringField(Option->AsObject(), TEXT("optionId"));
						break;
					}
				}
				Respond(RequestId, Object({{TEXT("outcome"), Obj(Picked.IsEmpty()
					? Object({{TEXT("outcome"), Str(TEXT("cancelled"))}})
					: Object({{TEXT("outcome"), Str(TEXT("selected"))}, {TEXT("optionId"), Str(Picked)}}))}}));
				AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(),
					FString::Printf(TEXT("%s %s, with nobody watching."), bPictureTool ? TEXT("Allowed") : TEXT("Refused"), *Title));
			}
			else if (Method == TEXT("session/request_permission") && RequestId.IsValid())
			{
				const TSharedPtr<FJsonObject> ToolCall = ObjectField(Params, TEXT("toolCall"));
				TArray<FString> Names;
				TArray<FString> Ids;
				for (const TSharedPtr<FJsonValue>& Option : ArrayField(Params, TEXT("options")))
				{
					Names.Add(StringField(Option->AsObject(), TEXT("name")));
					Ids.Add(StringField(Option->AsObject(), TEXT("optionId")));
				}
				AskApproval(RequestId, StringField(ToolCall, TEXT("toolCallId")), TEXT("Allow this?"), StringField(ToolCall, TEXT("title")), Names, Ids);
			}
			else if (RequestId.IsValid())
			{
				RespondError(RequestId, TEXT("HyperAI's native chat view does not support ") + Method);
			}
		}

		virtual TSharedRef<FJsonObject> AnswerPayload(const FHyperAIStudioChatItem& Item, const int32 Choice) override
		{
			const TSharedRef<FJsonObject> Outcome = Object({{TEXT("outcome"), Str(TEXT("selected"))}, {TEXT("optionId"), Str(Item.ChoiceIds[Choice])}});
			return Object({{TEXT("outcome"), Obj(Outcome)}});
		}

	private:
		/** A picture the agent sent as data, shown as its own item; Id keeps a repeated update from adding it twice. */
		void AddPicture(const FString& Data, const FString& Id)
		{
			if (!Id.IsEmpty() && FindItem(Id).IsValid())
			{
				return;
			}
			const FString File = SaveImageData(Data, Agent.Executable);
			if (!File.IsEmpty())
			{
				AddItem(FHyperAIStudioChatItem::EKind::Image, Id, TEXT("Image"), FString(), TEXT("completed"))->Images = {File};
			}
		}

		const FAcpAgent& Agent;
		TSharedPtr<FHyperAIStudioChatItem> CurrentAgent;
		TSharedPtr<FHyperAIStudioChatItem> CurrentThought;
		bool bTakesImages = false;
		bool bTakesHttpMcp = false;
		/** The model config option's id; empty when the agent lists models the older way, or not at all. */
		FString ModelConfigId;
		FString CurrentModel;
		TArray<FString> OfferedModels;
	};

	// ------------------------------------------------------------------------------------------------------------ Claude

	/**
	 * Claude Code as its Agent SDK runs it: stream-json both ways, with control requests for the handshake, permission
	 * prompts and interrupts. Its lines are not JSON-RPC, so HandleLine takes each one whole.
	 */
	class FClaudeChat final : public FHyperAIStudioNativeChat
	{
		using EKind = FHyperAIStudioChatItem::EKind;

	public:
		FClaudeChat(const FString& InProjectRoot, const FString& InModel, const FString& InResumeSessionId, const FString& InSubagent)
			: FHyperAIStudioNativeChat(TEXT("Claude Code"), InProjectRoot, InModel, InResumeSessionId)
		{
			StartedAs = InSubagent;
			// A chat Claude never wrote down (nothing was sent yet) cannot be resumed, so it starts again. The id is
			// chosen here, so the tab has it before the first message, as it does in the terminal.
			if (!HyperAIStudio::ChatHistory::ClaudeSessionExists(ProjectRoot, ResumeSessionId))
			{
				ResumeSessionId.Reset();
			}
			SessionId = ResumeSessionId.IsEmpty() ? HyperAIStudio::ChatHistory::NewClaudeSessionId() : ResumeSessionId;
		}

	protected:
		virtual FString CommandLine() const override
		{
			const UHyperAIStudioSettings* Settings = GetDefault<UHyperAIStudioSettings>();
			FString Prefix;
			FString ModelArgument;
			FString Error;
			if (const FHyperAIStudioAgentModelRoute* Saved = Settings->FindAgentModelRoute(AgentName))
			{
				// The model the terminal would pass, Smart's plan and build models included.
				FHyperAIStudioAgentModelRoute Route = *Saved;
				Route.SelectedModel = Model;
				if (!Settings->TryResolveModelLaunch(Route, Prefix, ModelArgument, Error))
				{
					Prefix.Reset();
					ModelArgument.Reset();
				}
			}
			// An editor started from a Claude Code session inherits its marker, which stops a nested claude from starting.
			// AskUserQuestion needs a form this view does not have, so Claude asks in its reply instead.
			FString Command = TEXT("set \"CLAUDECODE=\" && ") + Prefix + FHyperAIStudioService::GetAgentTerminalLaunchCommand(AgentName)
				+ TEXT(" --output-format stream-json --input-format stream-json --verbose --include-partial-messages")
				+ TEXT(" --permission-prompt-tool stdio --disallowedTools AskUserQuestion");
			if (!ModelArgument.IsEmpty())
			{
				Command += TEXT(" ") + ModelArgument;
			}
			// Auto Mode: Claude's own safety check approves routine actions, so the chat does not stop at each one.
			const FString AutoMode = UHyperAIStudioSettings::GetAutoModeArguments(AgentName);
			if (!AutoMode.IsEmpty())
			{
				Command += TEXT(" ") + AutoMode;
			}
			Command += (ResumeSessionId.IsEmpty() ? TEXT(" --session-id ") : TEXT(" --resume ")) + SessionId;
			// A resumed chat keeps what it started as.
			if (ResumeSessionId.IsEmpty() && !StartedAs.IsEmpty())
			{
				Command += TEXT(" --agent ") + StartedAs;
			}
			return Command;
		}

		virtual bool WritesJsonRpcVersion() const override { return false; }

		virtual void Handshake() override
		{
			Control(TEXT("hyperai-initialize"), Object({{TEXT("subtype"), Str(TEXT("initialize"))}, {TEXT("hooks"), MakeShared<FJsonValueNull>()}}));
		}

		virtual void SendNow(const FString& Text, const TArray<FString>& Images) override
		{
			bReplied = false;
			FString Prompt = Text;
			TArray<TSharedPtr<FJsonValue>> Content;
			for (const FString& Image : Images)
			{
				TArray<uint8> Bytes;
				// A picture too big to go as data goes as its path; Claude opens it itself.
				if (IFileManager::Get().FileSize(*Image) > MaxInlineImageBytes || !FFileHelper::LoadFileToArray(Bytes, *Image))
				{
					Prompt += TEXT("\n\nAttached picture: ") + Image;
					continue;
				}
				const TSharedRef<FJsonObject> Source = Object({{TEXT("type"), Str(TEXT("base64"))}, {TEXT("media_type"), Str(MimeType(Image))},
					{TEXT("data"), Str(FBase64::Encode(Bytes))}});
				Content.Add(Obj(Object({{TEXT("type"), Str(TEXT("image"))}, {TEXT("source"), Obj(Source)}})));
			}
			Content.Add(Obj(Object({{TEXT("type"), Str(TEXT("text"))}, {TEXT("text"), Str(Prompt)}})));
			const TSharedRef<FJsonObject> Message = Object({{TEXT("role"), Str(TEXT("user"))}, {TEXT("content"), Arr(Content)}});
			Write(Object({{TEXT("type"), Str(TEXT("user"))}, {TEXT("message"), Obj(Message)},
				{TEXT("parent_tool_use_id"), MakeShared<FJsonValueNull>()}, {TEXT("session_id"), Str(SessionId)}}));
		}

		virtual void InterruptNow() override
		{
			bInterrupted = true;
			Control(FString::Printf(TEXT("hyperai-interrupt-%d"), ++Interrupts), Object({{TEXT("subtype"), Str(TEXT("interrupt"))}}));
		}

		virtual void HandleMessage(const FString&, const TSharedPtr<FJsonObject>&, const TSharedPtr<FJsonValue>&) override
		{
		}

		virtual void Respond(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result) override
		{
			Write(Object({{TEXT("type"), Str(TEXT("control_response"))}, {TEXT("response"), Obj(Object({
				{TEXT("subtype"), Str(TEXT("success"))}, {TEXT("request_id"), Id}, {TEXT("response"), Obj(Result)}}))}}));
		}

		virtual void RespondError(const TSharedPtr<FJsonValue>& Id, const FString& Message) override
		{
			Write(Object({{TEXT("type"), Str(TEXT("control_response"))}, {TEXT("response"), Obj(Object({
				{TEXT("subtype"), Str(TEXT("error"))}, {TEXT("request_id"), Id}, {TEXT("error"), Str(Message)}}))}}));
		}

		virtual TSharedRef<FJsonObject> AnswerPayload(const FHyperAIStudioChatItem& Item, const int32 Choice) override
		{
			FAsk Asked;
			Asks.RemoveAndCopyValue(Item.Id, Asked);
			const FString Picked = Item.ChoiceIds[Choice];
			if (Picked == TEXT("deny"))
			{
				return Object({{TEXT("behavior"), Str(TEXT("deny"))},
					{TEXT("message"), Str(TEXT("The user said no to this. Don't try it another way: say what you wanted to do, and wait for them."))}});
			}
			const TSharedRef<FJsonObject> Allow = Object({{TEXT("behavior"), Str(TEXT("allow"))}, {TEXT("updatedInput"), Asked.Input}});
			if (Picked == TEXT("always"))
			{
				// Claude's own suggestions, such as a rule for this command, as the terminal's "don't ask again" writes them.
				Allow->SetArrayField(TEXT("updatedPermissions"), Asked.Suggestions);
			}
			return Allow;
		}

		virtual bool HandleLine(const TSharedRef<FJsonObject>& Line) override
		{
			const FString Type = StringField(Line, TEXT("type"));
			if (Type == TEXT("control_request"))
			{
				AskPermission(StringField(Line, TEXT("request_id")), ObjectField(Line, TEXT("request")));
			}
			else if (Type == TEXT("control_response"))
			{
				const TSharedPtr<FJsonObject> Reply = ObjectField(Line, TEXT("response"));
				if (StringField(Reply, TEXT("request_id")) == TEXT("hyperai-initialize"))
				{
					if (StringField(Reply, TEXT("subtype")) == TEXT("success"))
					{
						Ready(SessionId);
					}
					else
					{
						AddItem(EKind::Error, FString(), FString(), TEXT("Claude Code refused to start: ") + StringField(Reply, TEXT("error")));
					}
				}
			}
			else if (Type == TEXT("control_cancel_request"))
			{
				Withdraw(StringField(Line, TEXT("request_id")));
			}
			else if (Line->HasTypedField<EJson::String>(TEXT("parent_tool_use_id")))
			{
				// A subagent's own steps stay inside its card, as the terminal folds them.
			}
			else if (Type == TEXT("stream_event"))
			{
				Stream(ObjectField(Line, TEXT("event")));
			}
			else if (Type == TEXT("assistant"))
			{
				ShowMessage(ObjectField(Line, TEXT("message")));
			}
			else if (Type == TEXT("user"))
			{
				ShowResults(Line);
			}
			else if (Type == TEXT("system"))
			{
				ShowSystem(Line);
			}
			else if (Type == TEXT("result"))
			{
				EndTurn(Line);
			}
			return true;
		}

		/** Claude Code resumes without sending the conversation back, so it is read from the end of the chat's log. */
		virtual void LoadHistory() override
		{
			const FString File = HyperAIStudio::ChatHistory::SessionFile(AgentName, ProjectRoot, ResumeSessionId);
			if (File.IsEmpty())
			{
				return;
			}
			const TWeakPtr<FHyperAIStudioNativeChat> Weak = AsShared();
			Async(EAsyncExecution::ThreadPool, [Weak, This = this, File]()
			{
				// A long chat's log runs to many megabytes: only its end is read, and it is parsed here, off the game thread.
				bool bCut = false;
				TArray<TSharedPtr<FJsonObject>> Records;
				for (const FString& Line : HyperAIStudio::ChatHistory::ReadRecentLines(File, MaxHistoryBytes, bCut))
				{
					// The conversation's own records; a subagent's steps stay inside its card, as they do live.
					const bool bWanted = Line.Contains(TEXT("\"type\":\"user\"")) || Line.Contains(TEXT("\"type\":\"assistant\""))
						|| Line.Contains(TEXT("\"subtype\":\"compact_boundary\""));
					TSharedPtr<FJsonObject> Record;
					if (bWanted && !Line.Contains(TEXT("\"isSidechain\":true"))
						&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line), Record) && Record.IsValid())
					{
						Records.Add(Record);
					}
				}
				AsyncTask(ENamedThreads::GameThread, [Weak, This, Records = MoveTemp(Records), bCut]()
				{
					if (const TSharedPtr<FHyperAIStudioNativeChat> Alive = Weak.Pin())
					{
						This->Replay(Records, bCut);
					}
				});
			});
		}

	private:
		/** The log's records shown as they were live: what was typed, Claude's replies, its tools and their results. */
		void Replay(const TArray<TSharedPtr<FJsonObject>>& Records, const bool bCut)
		{
			ShowEarlier([this, &Records]()
			{
				for (const TSharedPtr<FJsonObject>& Record : Records)
				{
					const FString Type = StringField(Record, TEXT("type"));
					if (Type == TEXT("assistant"))
					{
						ShowMessage(ObjectField(Record, TEXT("message")));
					}
					else if (Type == TEXT("system") && StringField(Record, TEXT("subtype")) == TEXT("compact_boundary"))
					{
						ShowSystem(Record.ToSharedRef());
					}
					else if (Type == TEXT("user"))
					{
						const FString Typed = TypedPrompt(Record);
						if (Typed.StartsWith(TEXT("[Request interrupted")))
						{
							AddItem(EKind::Notice, FString(), FString(), TEXT("Stopped by the user."));
						}
						else if (!Typed.IsEmpty())
						{
							AddItem(EKind::User, FString(), FString(), Typed);
						}
						else
						{
							ShowResults(Record.ToSharedRef());
						}
					}
				}
			}, bCut);
		}

		/** What the user typed in a log record; empty for a tool result, injected context or a slash command. */
		static FString TypedPrompt(const TSharedPtr<FJsonObject>& Record)
		{
			bool bMeta = false;
			Record->TryGetBoolField(TEXT("isMeta"), bMeta);
			const TSharedPtr<FJsonObject> Message = ObjectField(Record, TEXT("message"));
			if (bMeta || !Message.IsValid())
			{
				return FString();
			}
			FString Text;
			if (!Message->TryGetStringField(TEXT("content"), Text))
			{
				for (const TSharedPtr<FJsonValue>& Part : ArrayField(Message, TEXT("content")))
				{
					const TSharedPtr<FJsonObject> Piece = Part.IsValid() && Part->Type == EJson::Object ? Part->AsObject() : nullptr;
					const FString PieceType = StringField(Piece, TEXT("type"));
					if (PieceType == TEXT("tool_result"))
					{
						return FString();
					}
					if (PieceType == TEXT("text"))
					{
						Text += (Text.IsEmpty() ? FString() : FString(TEXT("\n"))) + StringField(Piece, TEXT("text"));
					}
				}
			}
			Text = HyperAIStudio::ChatHistory::StripInjectedContext(Text).TrimStartAndEnd();
			// Command output, reminders and the like arrive as tags, not as anything typed.
			return Text.StartsWith(TEXT("<")) ? FString() : Text;
		}

		struct FAsk
		{
			FString RequestId;
			TSharedPtr<FJsonValue> Input;
			TArray<TSharedPtr<FJsonValue>> Suggestions;
		};

		struct FTask
		{
			FString Id;
			FHyperAIStudioChatPlanStep Step;
		};

		void Control(const FString& RequestId, const TSharedRef<FJsonObject>& Body)
		{
			Write(Object({{TEXT("type"), Str(TEXT("control_request"))}, {TEXT("request_id"), Str(RequestId)}, {TEXT("request"), Obj(Body)}}));
		}

		static bool IsCommand(const FString& Tool)
		{
			return Tool == TEXT("Bash") || Tool == TEXT("PowerShell");
		}

		static bool IsEdit(const FString& Tool)
		{
			return Tool == TEXT("Edit") || Tool == TEXT("Write") || Tool == TEXT("NotebookEdit");
		}

		static bool IsPlanTool(const FString& Tool)
		{
			return Tool == TEXT("TodoWrite") || Tool == TEXT("TaskCreate") || Tool == TEXT("TaskUpdate") || Tool == TEXT("TaskList")
				|| Tool == TEXT("TaskGet");
		}

		static FString PlanStatus(const FString& Status)
		{
			return Status == TEXT("in_progress") ? FString(TEXT("inProgress")) : Status;
		}

		/** Each line of Text behind Mark, as a diff shows it. */
		static FString Marked(const FString& Text, const TCHAR* Mark)
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
			FString Out;
			for (const FString& Line : Lines)
			{
				Out += Mark + Line + TEXT("\n");
			}
			return Out;
		}

		/** An edit as a diff: the file, then the lines it takes out and puts in. */
		static FString EditText(const FString& Tool, const TSharedPtr<FJsonObject>& Input)
		{
			const bool bNotebook = Tool == TEXT("NotebookEdit");
			const FString Added = StringField(Input, Tool == TEXT("Write") ? TEXT("content") : bNotebook ? TEXT("new_source") : TEXT("new_string"));
			return StringField(Input, bNotebook ? TEXT("notebook_path") : TEXT("file_path")) + TEXT("\n")
				+ Marked(StringField(Input, TEXT("old_string")), TEXT("-")) + Marked(Added, TEXT("+"));
		}

		/** An MCP tool as server / tool (Unreal's call_tool as the tool it runs), else the tool and what it works on. */
		static FString ToolTitle(const FString& Tool, const TSharedPtr<FJsonObject>& Input)
		{
			FString Server;
			FString Name;
			if (Tool.StartsWith(TEXT("mcp__")) && Tool.Mid(5).Split(TEXT("__"), &Server, &Name))
			{
				return Input.IsValid() && Input->HasField(TEXT("tool_name"))
					? StringField(Input, TEXT("toolset_name")) + TEXT(".") + StringField(Input, TEXT("tool_name"))
					: Server + TEXT(" / ") + Name;
			}
			for (const TCHAR* Field : {TEXT("file_path"), TEXT("notebook_path"), TEXT("pattern"), TEXT("url"), TEXT("query"), TEXT("description"), TEXT("skill")})
			{
				const FString Value = StringField(Input, Field);
				if (!Value.IsEmpty())
				{
					return Tool + TEXT(" ") + (FCString::Strstr(Field, TEXT("_path")) ? FPaths::GetCleanFilename(Value) : Value);
				}
			}
			return Tool;
		}

		/** Claude's reasons can carry terminal colour codes. */
		static FString StripAnsi(const FString& Text)
		{
			FString Out;
			for (int32 Index = 0; Index < Text.Len(); ++Index)
			{
				if (Text[Index] == TEXT('\x1b'))
				{
					// ESC, then parameters up to the letter that ends the code.
					while (++Index < Text.Len() && !FChar::IsAlpha(Text[Index]))
					{
					}
					continue;
				}
				Out.AppendChar(Text[Index]);
			}
			return Out;
		}

		void AddText(const EKind Kind, const FString& Id, const FString& Text)
		{
			AddItem(Kind, Id, FString(), Text);
			bReplied = bReplied || Kind == EKind::Agent;
		}

		void AskPermission(const FString& RequestId, const TSharedPtr<FJsonObject>& Body)
		{
			const FString Subtype = StringField(Body, TEXT("subtype"));
			if (Subtype != TEXT("can_use_tool"))
			{
				// Anything this view cannot show must still be answered, or Claude waits for ever.
				RespondError(Str(RequestId), TEXT("HyperAI's native chat view does not support ") + Subtype);
				AddItem(EKind::Notice, FString(), FString(),
					FString::Printf(TEXT("Claude Code asked for something this view cannot show (%s); use the terminal view for it."), *Subtype));
				return;
			}
			bool bOnlyInSession = false;
			Body->TryGetBoolField(TEXT("requires_user_interaction"), bOnlyInSession);
			if (bOnlyInSession)
			{
				Respond(Str(RequestId), Object({{TEXT("behavior"), Str(TEXT("deny"))},
					{TEXT("message"), Str(TEXT("This chat view cannot show that. Ask the user in your reply instead."))}}));
				return;
			}
			const FString Tool = StringField(Body, TEXT("tool_name"));
			const TSharedPtr<FJsonValue> Input = Body->TryGetField(TEXT("input"));
			const TSharedPtr<FJsonObject> Fields = Input.IsValid() && Input->Type == EJson::Object ? Input->AsObject() : nullptr;
			FString Title = StringField(Body, TEXT("title"));
			if (Title.IsEmpty())
			{
				Title = IsCommand(Tool) ? FString(TEXT("Run this command?")) : IsEdit(Tool) ? ToolTitle(Tool, Fields) + TEXT("?")
					: FString::Printf(TEXT("Allow %s?"), *ToolTitle(Tool, Fields));
			}
			// What it wants to do: the command, the edit as a diff, the address or the plan, else its input.
			FString Detail = IsEdit(Tool) ? EditText(Tool, Fields) : FString();
			for (const TCHAR* Field : {TEXT("command"), TEXT("url"), TEXT("plan")})
			{
				Detail = Detail.IsEmpty() ? StringField(Fields, Field) : Detail;
			}
			Detail = Detail.IsEmpty() ? Compact(Input) : Detail;
			TArray<FString> Lines;
			for (const FString& Part : {StringField(Body, TEXT("description")), IsCommand(Tool) ? StringField(Fields, TEXT("description")) : FString(),
				Detail.Len() > 2000 ? Detail.Left(2000) + TEXT("...") : Detail,
				StripAnsi(StringField(Body, TEXT("decision_reason")))})
			{
				if (!Part.IsEmpty() && !Lines.Contains(Part))
				{
					Lines.Add(Part);
				}
			}
			const TArray<TSharedPtr<FJsonValue>>& Suggestions = ArrayField(Body, TEXT("permission_suggestions"));
			bool bNoRule = false;
			Body->TryGetBoolField(TEXT("suppress_always_allow_rule"), bNoRule);
			TArray<FString> Choices = {TEXT("Allow")};
			TArray<FString> ChoiceIds = {TEXT("allow")};
			if (!Suggestions.IsEmpty() && !bNoRule)
			{
				Choices.Add(TEXT("Allow, and don't ask again"));
				ChoiceIds.Add(TEXT("always"));
			}
			Choices.Add(TEXT("Deny"));
			ChoiceIds.Add(TEXT("deny"));
			const FString ToolUseId = StringField(Body, TEXT("tool_use_id"));
			Asks.Add(ToolUseId + TEXT("#approval"), FAsk{RequestId, Input, Suggestions});
			AskApproval(Str(RequestId), ToolUseId, Title, FString::Join(Lines, TEXT("\n\n")), Choices, ChoiceIds);
		}

		/** Claude no longer waits for an answer, as after an interrupt. */
		void Withdraw(const FString& RequestId)
		{
			for (auto It = Asks.CreateIterator(); It; ++It)
			{
				if (It->Value.RequestId == RequestId)
				{
					CloseApproval(It->Key, TEXT("Cancelled"));
					It.RemoveCurrent();
					return;
				}
			}
		}

		/** Text and thinking as they stream in, and each tool call as soon as it starts. */
		void Stream(const TSharedPtr<FJsonObject>& Event)
		{
			const FString Type = StringField(Event, TEXT("type"));
			if (Type == TEXT("message_start"))
			{
				MessageId = StringField(ObjectField(Event, TEXT("message")), TEXT("id"));
				Streamed.Add(MessageId);
				return;
			}
			double Index = 0.0;
			if (Event.IsValid())
			{
				Event->TryGetNumberField(TEXT("index"), Index);
			}
			const FString Key = FString::Printf(TEXT("%s#%d"), *MessageId, static_cast<int32>(Index));
			if (Type == TEXT("content_block_start"))
			{
				const TSharedPtr<FJsonObject> Block = ObjectField(Event, TEXT("content_block"));
				if (StringField(Block, TEXT("type")) == TEXT("tool_use"))
				{
					ShowTool(StringField(Block, TEXT("id")), StringField(Block, TEXT("name")), nullptr);
				}
			}
			else if (Type == TEXT("content_block_delta"))
			{
				const TSharedPtr<FJsonObject> Delta = ObjectField(Event, TEXT("delta"));
				const FString Kind = StringField(Delta, TEXT("type"));
				if (Kind != TEXT("text_delta") && Kind != TEXT("thinking_delta"))
				{
					return;
				}
				const bool bText = Kind == TEXT("text_delta");
				const FString Chunk = StringField(Delta, bText ? TEXT("text") : TEXT("thinking"));
				if (const TSharedPtr<FHyperAIStudioChatItem> Item = FindItem(Key))
				{
					Item->Text = Clip(Item->Text + Chunk);
					Changed();
				}
				else if (!Chunk.IsEmpty())
				{
					// Made on the first words, so a block that stays empty, such as hidden thinking, shows nothing.
					AddText(bText ? EKind::Agent : EKind::Reasoning, Key, Chunk);
				}
			}
		}

		/**
		 * A whole assistant message. One that streamed is already on screen, and only its tools' inputs are new; one that
		 * did not shows here. Its usage is the size of the context.
		 */
		void ShowMessage(const TSharedPtr<FJsonObject>& Message)
		{
			const bool bStreamed = Streamed.Contains(StringField(Message, TEXT("id")));
			for (const TSharedPtr<FJsonValue>& Value : ArrayField(Message, TEXT("content")))
			{
				const TSharedPtr<FJsonObject> Block = Value->AsObject();
				const FString Kind = StringField(Block, TEXT("type"));
				if (Kind == TEXT("tool_use"))
				{
					ShowTool(StringField(Block, TEXT("id")), StringField(Block, TEXT("name")), ObjectField(Block, TEXT("input")));
				}
				else if (!bStreamed && (Kind == TEXT("text") || Kind == TEXT("thinking")) && !StringField(Block, *Kind).IsEmpty())
				{
					AddText(Kind == TEXT("text") ? EKind::Agent : EKind::Reasoning, FString(), StringField(Block, *Kind));
				}
			}
			if (const TSharedPtr<FJsonObject> Used = ObjectField(Message, TEXT("usage")))
			{
				double Tokens = 0.0;
				for (const TCHAR* Field : {TEXT("input_tokens"), TEXT("cache_creation_input_tokens"), TEXT("cache_read_input_tokens"), TEXT("output_tokens")})
				{
					double Part = 0.0;
					Used->TryGetNumberField(Field, Part);
					Tokens += Part;
				}
				ContextTokens = Tokens;
			}
		}

		/** A tool call's card, or the plan for Claude's to-do tools. Input is null while the call is still streaming in. */
		void ShowTool(const FString& Id, const FString& Tool, const TSharedPtr<FJsonObject>& Input)
		{
			if (IsPlanTool(Tool))
			{
				if (Input.IsValid())
				{
					UpdatePlan(Id, Tool, Input);
				}
				return;
			}
			const bool bCommand = IsCommand(Tool);
			TSharedPtr<FHyperAIStudioChatItem> Item = FindItem(Id);
			if (!Item.IsValid())
			{
				Item = AddItem(bCommand ? EKind::Command : IsEdit(Tool) ? EKind::FileChange : EKind::Tool, Id, Tool, FString(), TEXT("running"));
			}
			if (!Input.IsValid())
			{
				return;
			}
			if (bCommand)
			{
				Item->Title = StringField(Input, TEXT("command"));
			}
			else if (IsEdit(Tool))
			{
				const FString Path = StringField(Input, Tool == TEXT("NotebookEdit") ? TEXT("notebook_path") : TEXT("file_path"));
				Item->Title = (Tool == TEXT("Write") ? TEXT("Write ") : TEXT("Edit ")) + FPaths::GetCleanFilename(Path);
				Item->Text = Clip(EditText(Tool, Input));
			}
			else
			{
				Item->Title = ToolTitle(Tool, Input);
				Item->Text = Clip(TEXT("Arguments: ") + Compact(MakeShared<FJsonValueObject>(Input)));
			}
			Changed();
		}

		/** TodoWrite sends the whole list; the task tools add and change one task at a time. */
		void UpdatePlan(const FString& ToolUseId, const FString& Tool, const TSharedPtr<FJsonObject>& Input)
		{
			if (Tool == TEXT("TodoWrite"))
			{
				Plan.Reset();
				for (const TSharedPtr<FJsonValue>& Todo : ArrayField(Input, TEXT("todos")))
				{
					Plan.Add({StringField(Todo->AsObject(), TEXT("content")), PlanStatus(StringField(Todo->AsObject(), TEXT("status")))});
				}
				Changed();
			}
			else if (Tool == TEXT("TaskCreate"))
			{
				// Its id comes with the result.
				NewTasks.Add(ToolUseId, StringField(Input, TEXT("subject")));
			}
			else if (Tool == TEXT("TaskUpdate"))
			{
				const FString TaskId = StringField(Input, TEXT("taskId"));
				const int32 Index = Tasks.IndexOfByPredicate([&TaskId](const FTask& Task) { return Task.Id == TaskId; });
				if (Index == INDEX_NONE)
				{
					return;
				}
				const FString Status = StringField(Input, TEXT("status"));
				const FString Subject = StringField(Input, TEXT("subject"));
				if (Status == TEXT("deleted"))
				{
					Tasks.RemoveAt(Index);
				}
				else
				{
					Tasks[Index].Step.Status = Status.IsEmpty() ? Tasks[Index].Step.Status : PlanStatus(Status);
					Tasks[Index].Step.Text = Subject.IsEmpty() ? Tasks[Index].Step.Text : Subject;
				}
				ShowTasks();
			}
		}

		void ShowTasks()
		{
			Plan.Reset();
			for (const FTask& Task : Tasks)
			{
				Plan.Add(Task.Step);
			}
			Changed();
		}

		/** Tool results: each card gets its answer and status, and pictures in an answer show as their own items. */
		void ShowResults(const TSharedRef<FJsonObject>& Line)
		{
			for (const TSharedPtr<FJsonValue>& Value : ArrayField(ObjectField(Line, TEXT("message")), TEXT("content")))
			{
				const TSharedPtr<FJsonObject> Block = Value->AsObject();
				if (!Block.IsValid() || StringField(Block, TEXT("type")) != TEXT("tool_result"))
				{
					continue;
				}
				const FString Id = StringField(Block, TEXT("tool_use_id"));
				bool bError = false;
				Block->TryGetBoolField(TEXT("is_error"), bError);
				FString Text;
				int32 Pictures = 0;
				const TSharedPtr<FJsonValue> Content = Block->TryGetField(TEXT("content"));
				if (Content.IsValid() && Content->Type == EJson::String)
				{
					Text = Content->AsString();
				}
				else if (Content.IsValid() && Content->Type == EJson::Array)
				{
					for (const TSharedPtr<FJsonValue>& Part : Content->AsArray())
					{
						const TSharedPtr<FJsonObject> Piece = Part->AsObject();
						if (StringField(Piece, TEXT("type")) == TEXT("text"))
						{
							Text += (Text.IsEmpty() ? FString() : FString(TEXT("\n"))) + StringField(Piece, TEXT("text"));
						}
						else if (StringField(Piece, TEXT("type")) == TEXT("image") && !bReplaying)
						{
							const FString PictureId = FString::Printf(TEXT("%s#image%d"), *Id, Pictures++);
							const FString File = FindItem(PictureId).IsValid() ? FString()
								: SaveImageData(StringField(ObjectField(Piece, TEXT("source")), TEXT("data")), TEXT("claude"));
							if (!File.IsEmpty())
							{
								AddItem(EKind::Image, PictureId, TEXT("Looked at an image"), FString(), TEXT("completed"))->Images = {File};
							}
						}
					}
				}
				if (const FString* Subject = NewTasks.Find(Id))
				{
					// "Task #7 created successfully: ..." when the structured result is missing.
					FString TaskId = StringField(ObjectField(ObjectField(Line, TEXT("tool_use_result")), TEXT("task")), TEXT("id"));
					if (TaskId.IsEmpty() && Text.StartsWith(TEXT("Task #")))
					{
						Text.Mid(6).Split(TEXT(" "), &TaskId, nullptr);
					}
					if (!TaskId.IsEmpty() && !bError)
					{
						Tasks.Add(FTask{TaskId, {*Subject, TEXT("pending")}});
						ShowTasks();
					}
					NewTasks.Remove(Id);
					continue;
				}
				const TSharedPtr<FHyperAIStudioChatItem> Item = FindItem(Id);
				if (!Item.IsValid())
				{
					continue;
				}
				Item->Status = bError ? TEXT("failed") : TEXT("completed");
				if (Item->Kind == EKind::Command)
				{
					Item->Text = Clip(Text);
				}
				else if (Item->Kind == EKind::FileChange)
				{
					Item->Text = bError ? Clip(TEXT("Error: ") + Text + TEXT("\n\n") + Item->Text) : Item->Text;
				}
				else
				{
					Item->Text = Clip(Item->Text + TEXT("\n\n") + (bError ? TEXT("Error: ") : TEXT("")) + Text);
				}
				Changed();
			}
		}

		void ShowSystem(const TSharedRef<FJsonObject>& Line)
		{
			const FString Subtype = StringField(Line, TEXT("subtype"));
			if (Subtype == TEXT("init"))
			{
				const FString Id = StringField(Line, TEXT("session_id"));
				SessionId = Id.IsEmpty() ? SessionId : Id;
				TArray<FString> Down;
				for (const TSharedPtr<FJsonValue>& Server : ArrayField(Line, TEXT("mcp_servers")))
				{
					const FString Status = StringField(Server->AsObject(), TEXT("status"));
					if (Status == TEXT("failed") || Status == TEXT("needs-auth"))
					{
						Down.Add(FString::Printf(TEXT("%s (%s)"), *StringField(Server->AsObject(), TEXT("name")), *Status));
					}
				}
				if (!Down.IsEmpty() && !bToldServers)
				{
					bToldServers = true;
					AddItem(EKind::Notice, FString(), FString(), TEXT("MCP servers not connected: ") + FString::Join(Down, TEXT(", "))
						+ TEXT(". Check them with /mcp in the terminal view."));
				}
			}
			else if (Subtype == TEXT("compact_boundary"))
			{
				AddItem(EKind::Notice, FString(), FString(), TEXT("Claude Code compacted the conversation to free context."));
			}
		}

		void EndTurn(const TSharedRef<FJsonObject>& Line)
		{
			double Window = 0.0;
			if (const TSharedPtr<FJsonObject> Models = ObjectField(Line, TEXT("modelUsage")))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Models->Values)
				{
					double Size = 0.0;
					if (Entry.Value.IsValid() && Entry.Value->Type == EJson::Object && Entry.Value->AsObject()->TryGetNumberField(TEXT("contextWindow"), Size))
					{
						Window = FMath::Max(Window, Size);
					}
				}
			}
			if (ContextTokens > 0.0)
			{
				Usage = Window > 0.0
					? FString::Printf(TEXT("%.1fk tokens, %d%% of context used"), ContextTokens / 1000.0, FMath::RoundToInt(100.0 * ContextTokens / Window))
					: FString::Printf(TEXT("%.1fk tokens"), ContextTokens / 1000.0);
			}
			bool bError = false;
			Line->TryGetBoolField(TEXT("is_error"), bError);
			FString Text = StringField(Line, TEXT("result"));
			for (const TSharedPtr<FJsonValue>& Value : ArrayField(Line, TEXT("errors")))
			{
				Text += (Text.IsEmpty() ? FString() : FString(TEXT(" "))) + Value->AsString();
			}
			if (bInterrupted)
			{
				AddItem(EKind::Notice, FString(), FString(), TEXT("Stopped."));
			}
			else if (bError)
			{
				AddItem(EKind::Error, FString(), FString(), Text.Contains(TEXT("/login"))
					? FString(TEXT("Claude Code needs you to sign in: switch this tab to the terminal view and run /login once."))
					: TEXT("Claude Code: ") + (Text.IsEmpty() ? StringField(Line, TEXT("subtype")) : Text));
			}
			else if (!bReplied && !Text.IsEmpty())
			{
				// A turn that answers in its result alone, such as a local command.
				AddText(EKind::Agent, FString(), Text);
			}
			bInterrupted = false;
			Streamed.Reset();
			TurnFinished();
		}

		/** The message now streaming, and every message this turn that streamed. */
		FString MessageId;
		TSet<FString> Streamed;
		/** Open permission requests, by approval item. */
		TMap<FString, FAsk> Asks;
		/** TaskCreate calls waiting for their task's id, by tool call. */
		TMap<FString, FString> NewTasks;
		TArray<FTask> Tasks;
		double ContextTokens = 0.0;
		int32 Interrupts = 0;
		bool bInterrupted = false;
		bool bReplied = false;
		bool bToldServers = false;
	};
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioNativeChat::SetCommandForTests(const FString& Command)
{
	HyperAIStudio::NativeChat::Private::CommandForTests() = Command;
}

void FHyperAIStudioNativeChat::SetContextInTestsForTests(const bool bEnabled)
{
	HyperAIStudio::NativeChat::Private::ContextInTests() = bEnabled;
}
#endif

bool FHyperAIStudioNativeChat::IsSupported(const FString& AgentName)
{
	return AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase) || AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase)
		|| HyperAIStudio::NativeChat::Private::FindAcpAgent(AgentName) != nullptr;
}

TArray<FString> FHyperAIStudioNativeChat::GetInstalledAcpAgents()
{
	// ponytail: a PATH search per agent, at most once a minute; an agent installed meanwhile shows after that.
	static TArray<FString> Installed;
	static double CheckedAt = -1.0;
	if (CheckedAt < 0.0 || FPlatformTime::Seconds() - CheckedAt > 60.0)
	{
		CheckedAt = FPlatformTime::Seconds();
		Installed.Reset();
		for (const HyperAIStudio::NativeChat::Private::FAcpAgent& Agent : HyperAIStudio::NativeChat::Private::AcpAgents())
		{
			FString Path;
			if (!Agent.bConfiguresMcp && FHyperAIStudioService::FindExecutableOnPath(Agent.Executable, Path))
			{
				Installed.Add(Agent.Name);
			}
		}
	}
	return Installed;
}

FString FHyperAIStudioNativeChat::GetAcpTerminalCommand(const FString& AgentName)
{
	const HyperAIStudio::NativeChat::Private::FAcpAgent* Agent = HyperAIStudio::NativeChat::Private::FindAcpAgent(AgentName);
	return Agent ? HyperAIStudio::NativeChat::Private::Executable(Agent->Executable) : FString();
}

TSharedPtr<FHyperAIStudioNativeChat> FHyperAIStudioNativeChat::Create(const FString& AgentName, const FString& ProjectRoot, const FString& Model,
	const FString& ResumeSessionId, const FString& Subagent)
{
	using namespace HyperAIStudio::NativeChat::Private;
	if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
	{
		return MakeShared<FCodexChat>(ProjectRoot, Model, ResumeSessionId);
	}
	if (AgentName.Contains(TEXT("Claude"), ESearchCase::IgnoreCase))
	{
		return MakeShared<FClaudeChat>(ProjectRoot, Model, ResumeSessionId, Subagent);
	}
	if (const FAcpAgent* Agent = FindAcpAgent(AgentName))
	{
		return MakeShared<FAcpChat>(*Agent, ProjectRoot, Model, ResumeSessionId);
	}
	return nullptr;
}
