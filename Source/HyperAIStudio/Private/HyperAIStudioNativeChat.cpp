// Games by Hyper 2026.

#include "HyperAIStudioNativeChat.h"

#include "HAL/FileManager.h"
#include "HyperAIStudioService.h"
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

	FString& CommandForTests()
	{
		static FString Command;
		return Command;
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
	const FString Command = HyperAIStudio::NativeChat::Private::CommandForTests().IsEmpty()
		? CommandLine() : HyperAIStudio::NativeChat::Private::CommandForTests();
	// Through cmd so npm's .cmd shims start too; /s keeps the quoted executable path intact.
	Process = FPlatformProcess::CreateProc(TEXT("cmd.exe"), *FString::Printf(TEXT("/d /s /c \"%s\""), *Command),
		/*bLaunchDetached=*/false, /*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true, nullptr, 0, *ProjectRoot, StdOutWrite, StdInRead);
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
	return true;
}

void FHyperAIStudioNativeChat::Stop()
{
	if (bStopped)
	{
		return;
	}
	bStopped = true;
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

void FHyperAIStudioNativeChat::Send(const FString& Text, const TArray<FString>& Images)
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
		Queued.Add({Trimmed, MoveTemp(Sendable)});
		Changed();
		return;
	}
	AddItem(FHyperAIStudioChatItem::EKind::User, FString(), FString(), Trimmed)->Images = Sendable;
	TurnStarted();
	SendNow(Trimmed, Sendable);
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
		Send(Next.Text, Next.Images);
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
	if (!Queued.IsEmpty() && bReady)
	{
		const FQueuedMessage Next = Queued[0];
		Queued.RemoveAt(0);
		Send(Next.Text, Next.Images);
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
	const TSharedRef<FHyperAIStudioChatItem> Item = MakeShared<FHyperAIStudioChatItem>();
	Item->Kind = Kind;
	Item->Id = Id.IsEmpty() ? FGuid::NewGuid().ToString() : Id;
	Item->Title = Title;
	Item->Text = HyperAIStudio::NativeChat::Private::Clip(Text);
	Item->Status = Status;
	Items.Add(Item);
	if (Items.Num() > HyperAIStudio::NativeChat::Private::MaxItems)
	{
		Items.RemoveAt(0, Items.Num() - HyperAIStudio::NativeChat::Private::MaxItems);
	}
	Changed();
	return Item;
}

void FHyperAIStudioNativeChat::AskApproval(const TSharedPtr<FJsonValue>& RequestId, const FString& Id, const FString& Title,
	const FString& Text, const TArray<FString>& Choices, const TArray<FString>& ChoiceIds)
{
	const TSharedRef<FHyperAIStudioChatItem> Item = AddItem(FHyperAIStudioChatItem::EKind::Approval, Id + TEXT("#approval"), Title, Text);
	Item->Choices = Choices;
	Item->ChoiceIds = ChoiceIds;
	OpenApprovals.Add(Item->Id, RequestId);
	Changed();
}

void FHyperAIStudioNativeChat::Changed()
{
	OnChanged.Broadcast();
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
		AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(),
			FString::Printf(TEXT("%s stopped (exit code %d). Start a new chat, or switch this tab to the terminal to see why."), *AgentName, Code));
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
			return Command + TEXT(" app-server");
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
			if (!Model.IsEmpty())
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
				Ready(StringField(ObjectField(Result, TEXT("thread")), TEXT("id")));
			});
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
				if (Saved.IsEmpty() && bCompleted && !Data.IsEmpty())
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

		virtual TSharedRef<FJsonObject> AnswerPayload(const FHyperAIStudioChatItem& Item, const int32 Choice) override
		{
			if (const TSharedPtr<FJsonObject>* Requested = RequestedPermissions.Find(Item.Id))
			{
				const bool bAllow = Item.ChoiceIds[Choice] == TEXT("allow");
				return Object({{TEXT("permissions"), Obj(bAllow && Requested->IsValid() ? Requested->ToSharedRef() : MakeShared<FJsonObject>())}});
			}
			return Object({{TEXT("decision"), Str(Item.ChoiceIds[Choice])}});
		}

	private:
		FString TurnId;
		FString TokensText;
		FString LimitsText;
		TMap<FString, TSharedPtr<FJsonObject>> RequestedPermissions;
	};

	// ------------------------------------------------------------------------------------------------------------ Gemini

	/** Gemini through `gemini --acp`, the Agent Client Protocol. */
	class FGeminiChat final : public FHyperAIStudioNativeChat
	{
	public:
		FGeminiChat(const FString& InProjectRoot, const FString& InModel, const FString& InResumeSessionId)
			: FHyperAIStudioNativeChat(TEXT("Gemini"), InProjectRoot, InModel, InResumeSessionId)
		{
		}

	protected:
		virtual FString CommandLine() const override
		{
			return Executable(TEXT("gemini")) + (Model.IsEmpty() ? FString() : TEXT(" --model ") + Model) + TEXT(" --acp");
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
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), TEXT("Gemini refused to start: ") + Error);
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
				}
				OpenSession(bCanLoad && !ResumeSessionId.IsEmpty());
			});
		}

		void OpenSession(const bool bLoad)
		{
			const TSharedRef<FJsonObject> Params = Object({{TEXT("cwd"), Str(ProjectRoot)}, {TEXT("mcpServers"), Arr({})}});
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
						? TEXT("Gemini needs you to sign in: switch this tab to the terminal view and run gemini once.")
						: TEXT("Gemini could not start a chat: ") + Error);
					return;
				}
				Ready(bLoad ? ResumeSessionId : StringField(Result, TEXT("sessionId")));
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
					: FString(TEXT("Gemini did not say it takes pictures, so only the text went.")));
			}
			Request(TEXT("session/prompt"), Object({{TEXT("sessionId"), Str(SessionId)}, {TEXT("prompt"), Arr(Blocks)}}),
				[this](const TSharedPtr<FJsonObject>& Result, const FString& Error)
			{
				const FString StopReason = StringField(Result, TEXT("stopReason"));
				if (!Error.IsEmpty())
				{
					AddItem(FHyperAIStudioChatItem::EKind::Error, FString(), FString(), TEXT("Gemini: ") + Error);
				}
				else if (StopReason == TEXT("cancelled"))
				{
					AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), TEXT("Stopped."));
				}
				else if (StopReason == TEXT("max_tokens") || StopReason == TEXT("refusal"))
				{
					AddItem(FHyperAIStudioChatItem::EKind::Notice, FString(), FString(), TEXT("Gemini ended the turn early: ") + StopReason);
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
		/** A picture Gemini sent as data, shown as its own item; Id keeps a repeated update from adding it twice. */
		void AddPicture(const FString& Data, const FString& Id)
		{
			if (!Id.IsEmpty() && FindItem(Id).IsValid())
			{
				return;
			}
			const FString File = SaveImageData(Data, TEXT("gemini"));
			if (!File.IsEmpty())
			{
				AddItem(FHyperAIStudioChatItem::EKind::Image, Id, TEXT("Image"), FString(), TEXT("completed"))->Images = {File};
			}
		}

		TSharedPtr<FHyperAIStudioChatItem> CurrentAgent;
		TSharedPtr<FHyperAIStudioChatItem> CurrentThought;
		bool bTakesImages = false;
	};
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioNativeChat::SetCommandForTests(const FString& Command)
{
	HyperAIStudio::NativeChat::Private::CommandForTests() = Command;
}
#endif

bool FHyperAIStudioNativeChat::IsSupported(const FString& AgentName)
{
	return AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase) || AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase);
}

TSharedPtr<FHyperAIStudioNativeChat> FHyperAIStudioNativeChat::Create(const FString& AgentName, const FString& ProjectRoot, const FString& Model,
	const FString& ResumeSessionId)
{
	using namespace HyperAIStudio::NativeChat::Private;
	if (AgentName.Contains(TEXT("Codex"), ESearchCase::IgnoreCase))
	{
		return MakeShared<FCodexChat>(ProjectRoot, Model, ResumeSessionId);
	}
	if (AgentName.Contains(TEXT("Gemini"), ESearchCase::IgnoreCase))
	{
		return MakeShared<FGeminiChat>(ProjectRoot, Model, ResumeSessionId);
	}
	return nullptr;
}
