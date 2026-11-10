// Copyright Epic Games, Inc. All Rights Reserved.

#include "ConsoleToolset.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Features/IModularFeatures.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/DefaultValueHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(ConsoleToolset)

namespace UE::ConsoleToolset::Private
{
	class FCaptureDevice : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type, const FName&) override
		{
			Entries.Add(V);
		}

		TArray<FString> Entries;
	};

	class FCategoryProbe : public FOutputDevice
	{
	public:
		virtual void Serialize(const TCHAR*, ELogVerbosity::Type, const FName& Category) override
		{
			Seen.Add(Category);
		}

		TSet<FName> Seen;
	};

	// Whether Command mentions Word as a whole word, anywhere (case-insensitive). Only letters and digits continue a
	// word, matching FParse::Command (which accepts "py_x" and "py.x" as the PY command), so py__import__(...) and
	// script.py match too.
	bool ContainsWord(const FString& Command, const TCHAR* Word)
	{
		const auto IsWordChar = [](TCHAR Char)
		{
			return FChar::IsAlnum(Char);
		};

		const int32 WordLen = FCString::Strlen(Word);
		int32 SearchFrom = 0;
		while (true)
		{
			const int32 Found = Command.Find(Word, ESearchCase::IgnoreCase, ESearchDir::FromStart, SearchFrom);
			if (Found == INDEX_NONE)
			{
				return false;
			}
			const int32 End = Found + WordLen;
			const bool bWordStart = Found == 0 || !IsWordChar(Command[Found - 1]);
			const bool bWordEnd = End >= Command.Len() || !IsWordChar(Command[End]);
			if (bWordStart && bWordEnd)
			{
				return true;
			}
			SearchFrom = Found + 1;
		}
	}

	// Whether Command mentions Python: "py" as a whole word, anywhere. This is deliberately broader than matching
	// the PY command at the start of each command. Commands that wrap or queue other commands (DEFER,
	// REDIRECTTOFILE, KE, ...) would otherwise let Python run outside the Python scripting tool, which returns the
	// script's result and errors. A false positive only costs the caller an error pointing at that tool.
	bool RunsPython(const FString& Command)
	{
		return ContainsWord(Command, TEXT("py"));
	}

	// Whether Command mentions EXEC as a whole word, anywhere. EXEC runs every line of a file as a console command,
	// which would let a command (including Python) reach the console without passing through the checks here.
	bool RunsExec(const FString& Command)
	{
		return ContainsWord(Command, TEXT("exec"));
	}

	// The plain console command executor (UnrealEd's FConsoleCommandExecutor), never a Python one,
	// whichever registered first. Null without the editor.
	IConsoleCommandExecutor* FindConsoleCommandExecutor()
	{
		static const FName CmdExecutorName(TEXT("Cmd"));
		for (IConsoleCommandExecutor* Executor : IModularFeatures::Get().GetModularFeatureImplementations<IConsoleCommandExecutor>(IConsoleCommandExecutor::ModularFeatureName()))
		{
			if (Executor && Executor->GetName() == CmdExecutorName)
			{
				return Executor;
			}
		}
		return nullptr;
	}
}

FString UConsoleToolset::SearchCVars(const FString& Name)
{
	TSharedPtr<FJsonObject> Results = MakeShared<FJsonObject>();
	const auto OnConsoleVariable = [&Results](const TCHAR* Name, IConsoleObject* CVar)
	{
		if (TSharedPtr<FJsonObject> CVarJson = CVarToJson(CVar->AsVariable()))
		{
			Results->SetObjectField(Name, CVarJson);
		}
	};
	IConsoleManager& ConsoleManager = IConsoleManager::Get();
	ConsoleManager.ForEachConsoleObjectThatContains(FConsoleObjectVisitor::CreateLambda(OnConsoleVariable), *Name);
	FString JsonString;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> JsonWriter =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);
	FJsonSerializer::Serialize(Results.ToSharedRef(), JsonWriter);
	return JsonString;
}

TMap<FString, FString> UConsoleToolset::SearchConsoleCommands(const FString& Name)
{
	TMap<FString, FString> Results;

#if !UE_BUILD_SHIPPING
	if (!GEngine)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SearchConsoleCommands: GEngine unavailable."));
		return Results;
	}

	// Union of registered console commands and unregistered FParse::Command handlers, same list as DumpConsoleCommands.
	// The engine only exposes this data via ConsoleCommandLibrary_DumpLibrary (Parse.cpp:72). The implementation is kind of sketchy, so making it into a proper read only API is not trivial
	// So we use ConsoleCommandLibrary_DumpLibrary and scrape the log for the output. This could also be done by the LLM directly, but this is trying to save it some effort
	UE::ConsoleToolset::Private::FCaptureDevice Capture;
	ConsoleCommandLibrary_DumpLibrary(GEngine->GetCurrentPlayWorld(), *GEngine, TEXT(""), Capture);

	IConsoleManager& ConsoleManager = IConsoleManager::Get();
	for (const FString& Entry : Capture.Entries)
	{
		if (Entry.IsEmpty())
		{
			continue;
		}
		// DumpLibrary's two-line ERROR trailer (Parse.cpp:108), only emitted when the exec probe bypassed FParse::Command.
		if (Entry.StartsWith(TEXT("ERROR: The function was supposed to only find matching commands"))
			|| Entry.StartsWith(TEXT("However Exec() returned true")))
		{
			continue;
		}
		if (!Name.IsEmpty() && !Entry.Contains(Name, ESearchCase::IgnoreCase))
		{
			continue;
		}
		FString Help;
		if (IConsoleObject* Obj = ConsoleManager.FindConsoleObject(*Entry))
		{
			Help = Obj->GetHelp();
		}
		Results.Add(Entry, MoveTemp(Help));
	}
#else
	// ConsoleCommandLibrary_DumpLibrary is stripped, so unregistered FParse::Command handlers are not enumerable.
	// Fall back to IConsoleManager-registered commands only. Empty prefix matches everything (MatchPartialName returns true immediately);
	// substring filtering by Name is applied in the visitor.
	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda(
		[&Results, &Name](const TCHAR* CmdName, IConsoleObject* Obj)
		{
			if (!Obj || !Obj->IsEnabled() || !Obj->AsCommand())
			{
				return;
			}
			if (!Name.IsEmpty() && !FCString::Stristr(CmdName, *Name))
			{
				return;
			}
			Results.Add(CmdName, Obj->GetHelp());
		}), TEXT(""));
#endif

	return Results;
}

FString UConsoleToolset::GetCVarValue(const FString& Name)
{
	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
	if (!CVar || !CVar->IsEnabled())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Console variable not found: %s"), *Name));
		return FString();
	}
	return CVar->GetString();
}

void UConsoleToolset::SetCVarValue(const FString& Name, const FString& Value)
{
	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
	if (!CVar || !CVar->IsEnabled())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Console variable not found: %s"), *Name));
		return;
	}
	if ((CVar->GetFlags() & ECVF_ReadOnly) != 0)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Console variable is read-only: %s"), *Name));
		return;
	}

	// Type-check the input against the cvar's declared type before letting Set()
	// silently coerce (e.g. "abc" -> 0 on an int).
	const TCHAR* ExpectedType = nullptr;
	if (CVar->IsVariableBool())
	{
		bool Parsed; if (!FDefaultValueHelper::ParseBool(Value, Parsed)) ExpectedType = TEXT("bool");
	}
	else if (CVar->IsVariableInt())
	{
		// ParseInt also accepts hex ("0x10") and C-style octal ("010"), but Set() below reads the text as decimal, so
		// such text would report success and store a different value. Only text that reads the same as decimal is accepted.
		int32 Parsed; if (!FDefaultValueHelper::ParseInt(Value, Parsed) || FCString::Atoi(*Value) != Parsed) ExpectedType = TEXT("int");
	}
	else if (CVar->IsVariableFloat())
	{
		float Parsed; if (!FDefaultValueHelper::ParseFloat(Value, Parsed)) ExpectedType = TEXT("float");
	}
	else
	{
		// string / other: accept any input.
	}

	if (ExpectedType)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Expected %s value for %s, got '%s'."), ExpectedType, *Name, *Value));
		return;
	}

	CVar->Set(*Value, ECVF_SetByConsole);
}

TArray<FString> UConsoleToolset::RunConsoleCommand(const FString& Command)
{
#if UE_BUILD_SHIPPING && !WITH_EDITOR
	// Arbitrary-command exec is a dev-time escape hatch. Allowed in editor builds
	// (including UEFN, where UE_BUILD_SHIPPING=1 but WITH_EDITOR=1) and disabled
	// in cooked shipping game builds where an LLM client should not be able to
	// dispatch destructive commands (quit/restartlevel/obj gc/...).
	UKismetSystemLibrary::RaiseScriptError(TEXT("RunConsoleCommand: disabled in shipping non-editor builds."));
	return {};
#else
	if (!GEngine)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RunConsoleCommand: GEngine unavailable."));
		return {};
	}

	if (UE::ConsoleToolset::Private::RunsPython(Command))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RunConsoleCommand: running Python through the console isn't supported. Run Python with the Python scripting tool instead, which returns the script's result and its errors."));
		return {};
	}

	if (UE::ConsoleToolset::Private::RunsExec(Command))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RunConsoleCommand: EXEC isn't supported, because it runs a file of console commands that bypass this tool's checks. Run the commands individually instead."));
		return {};
	}

	UE::ConsoleToolset::Private::FCategoryProbe Probe;
	GLog->AddOutputDevice(&Probe);
	if (IConsoleCommandExecutor* Executor = UE::ConsoleToolset::Private::FindConsoleCommandExecutor())
	{
		Executor->Exec(*Command);
	}
	else
	{
		GEngine->Exec(GEngine->GetCurrentPlayWorld(), *Command, *GLog);
	}
	GLog->FlushThreadedLogs();
	GLog->RemoveOutputDevice(&Probe);

	// Empty string marks uncategorized Ar.Logf output; it's the value a caller would pass to
	// read those lines back, so returning it as-is keeps the mapping direct.
	TArray<FString> Categories;
	Categories.Reserve(Probe.Seen.Num());
	for (const FName& Category : Probe.Seen)
	{
		Categories.Add(Category.IsNone() ? FString() : Category.ToString());
	}
	return Categories;
#endif
}

TSharedPtr<FJsonObject> UConsoleToolset::CVarToJson(IConsoleObject* CVar)
{
	if (!CVar || !CVar->IsEnabled())
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> CVarData = MakeShared<FJsonObject>();
	CVarData->SetStringField(FString(TEXT("help")), CVar->GetHelp());
	if (IConsoleVariable* Variable = CVar->AsVariable())
	{
		if (Variable->IsVariableBool())
		{
			CVarData->SetBoolField(
				FString(TEXT("value")), Variable->GetBool());
		}
		else if (Variable->IsVariableInt())
		{
			CVarData->SetNumberField(
				FString(TEXT("value")), Variable->GetInt());
		}
		else if (Variable->IsVariableFloat())
		{
			CVarData->SetNumberField(
				FString(TEXT("value")), Variable->GetFloat());
		}
		else if (Variable->IsVariableString())
		{
			CVarData->SetStringField(
				FString(TEXT("value")), Variable->GetString());
		}
		else
		{
			CVarData->SetStringField(TEXT("value"), Variable->GetDefaultValue());
		}
	}
	return CVarData;
}

bool UConsoleToolset::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted();
}
