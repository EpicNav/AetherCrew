// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioEditorRestart.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace HyperAIStudio::RecoveryTests
{
	FString NewFolder()
	{
		const FString Folder = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("Recovery-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true);
		return Folder;
	}

	FString Native(const FString& Path)
	{
		FString Out = FPaths::ConvertRelativePathToFull(Path);
		FPaths::MakePlatformFilename(Out);
		return Out;
	}

	FString Escaped(const FString& Path)
	{
		return Native(Path).ReplaceCharWithEscapedChar();
	}

	FString PluginScript(const TCHAR* Name)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Resources/Scripts") / Name) : FString();
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) ? Object : nullptr;
	}

	/** This user as Windows names a process's owner. */
	FString Me()
	{
		return FPlatformMisc::GetEnvironmentVariable(TEXT("USERDOMAIN")) + TEXT("\\") + FPlatformMisc::GetEnvironmentVariable(TEXT("USERNAME"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioRecoveryNoSecondEditorTest,
	"HyperAIStudio.EditorRestart.NoSecondEditor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioRecoveryNoSecondEditorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::RecoveryTests;
	const FString Folder = NewFolder();
	const FString Project = Folder / TEXT("Fake Bench/FakeBench.uproject");
	FFileHelper::SaveStringToFile(TEXT("{}"), *Project);
	const uint32 Self = FPlatformProcess::GetCurrentProcessId();

	// A process list as Windows gives it: only the first and the fifth are this user's editors on this very project.
	FString Upper = Native(Project).ToUpper();
	Upper.ReplaceInline(TEXT("\\"), TEXT("/"));
	const FString List = FString::Printf(TEXT(R"JSON([
		{"ProcessId": 101, "Name": "UnrealEditor.exe", "CommandLine": "\"E:\\EpicGames\\UE_5.8\\Engine\\Binaries\\Win64\\UnrealEditor.exe\" \"%s\" -log", "Owner": "%s"},
		{"ProcessId": 102, "Name": "UnrealEditor.exe", "CommandLine": "UnrealEditor.exe \"D:\\Elsewhere\\FakeBench.uproject\"", "Owner": "%s"},
		{"ProcessId": 103, "Name": "UnrealEditor.exe", "CommandLine": "UnrealEditor.exe \"%s\"", "Owner": "OTHERPC\\someone"},
		{"ProcessId": 104, "Name": "UnrealEditor-Cmd.exe", "CommandLine": "UnrealEditor-Cmd.exe \"%s\" -run=cook", "Owner": "%s"},
		{"ProcessId": 105, "Name": "UnrealEditor.exe", "CommandLine": "UnrealEditor.exe FakeBench.uproject", "Owner": "%s"},
		{"ProcessId": %u, "Name": "UnrealEditor.exe", "CommandLine": "UnrealEditor.exe \"%s\"", "Owner": "%s"}])JSON"),
		*Upper, *Me().ReplaceCharWithEscapedChar(), *Me().ReplaceCharWithEscapedChar(), *Escaped(Project), *Escaped(Project), *Me().ReplaceCharWithEscapedChar(),
		*Me().ReplaceCharWithEscapedChar(), Self, *Escaped(Project), *Me().ReplaceCharWithEscapedChar());
	const FString ListFile = Folder / TEXT("processes.json");
	FFileHelper::SaveStringToFile(List, *ListFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	const TArray<uint32> Listed = FHyperAIStudioEditorRestart::FindOtherEditors(Project, ListFile);
	AddInfo(FString::Printf(TEXT("Found %d editor(s) in the fake list."), Listed.Num()));
	TestTrue(TEXT("Same path (case and slashes aside) or the name alone counts; another project of that name, another user, a commandlet and itself do not"),
		Listed == TArray<uint32>{101, 105});

	TestTrue(TEXT("An editor a restart reopened gives way to one open already"), FHyperAIStudioEditorRestart::ShouldGiveWay(TEXT("tok"), {101}));
	TestFalse(TEXT("but one opened by hand never closes itself"), FHyperAIStudioEditorRestart::ShouldGiveWay(FString(), {101}));
	TestFalse(TEXT("and none gives way to nobody"), FHyperAIStudioEditorRestart::ShouldGiveWay(TEXT("tok"), {}));

	// A real process named UnrealEditor.exe with the project on its command line, as an editor opened by hand has.
	const FString Dummy = Folder / TEXT("bin/UnrealEditor.exe");
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Dummy), /*Tree=*/true);
	if (IFileManager::Get().Copy(*Dummy, *(FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")) / TEXT("System32/cmd.exe"))) != COPY_OK)
	{
		AddError(TEXT("The stand-in editor could not be made."));
		return false;
	}
	// Started by Windows as a person's would be, through PowerShell's Start-Process; its id comes back to stop it after.
	int32 StartCode = -1;
	FString StartOut;
	FString StartErr;
	const FString Launch = FString::Printf(TEXT("-NoProfile -Command \"(Start-Process -FilePath '%s' -ArgumentList '/d /c ping -n 40 127.0.0.1 >nul & rem \\\"%s\\\"' -WindowStyle Hidden -PassThru).Id\""),
		*Native(Dummy), *Native(Project));
	FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *Launch, &StartCode, &StartOut, &StartErr);
	const uint32 DummyId = static_cast<uint32>(FCString::Atoi(*StartOut.TrimStartAndEnd()));
	FProcHandle Editor = DummyId ? FPlatformProcess::OpenProcess(DummyId) : FProcHandle();
	if (!TestTrue(TEXT("The stand-in editor runs"), Editor.IsValid()))
	{
		AddInfo(StartOut + StartErr);
		return false;
	}
	FPlatformProcess::Sleep(0.5f);
	TestTrue(TEXT("The stand-in editor is still running"), FPlatformProcess::IsProcRunning(Editor));
	const TArray<uint32> Live = FHyperAIStudioEditorRestart::FindOtherEditors(Project);
	if (!Live.Contains(DummyId))
	{
		int32 ListCode = -1;
		FString ListOut;
		FString ListErr;
		FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *FString::Printf(TEXT("-NoProfile -Command \"Get-CimInstance Win32_Process -Filter 'ProcessId=%u' | Format-List Name, CommandLine\""), DummyId),
			&ListCode, &ListOut, &ListErr);
		AddInfo(FString::Printf(TEXT("Stand-in %u as Windows lists it: %s %s"), DummyId, *ListOut, *ListErr));
	}
	TestTrue(TEXT("Among this user's processes, the stand-in is found open on the project"), Live.Contains(DummyId));
	TestFalse(TEXT("and an editor on another project is not counted for it"), FHyperAIStudioEditorRestart::FindOtherEditors(Folder / TEXT("Other/Other.uproject")).Contains(DummyId));

	// The helper, about to reopen the project after a crash, finds it open and hands that editor the chats instead.
	const FString Restart = Folder / TEXT("Restart");
	IFileManager::Get().MakeDirectory(*Restart, /*Tree=*/true);
	const FString Marker = Folder / TEXT("relaunched.txt");
	const FString Plan = FString::Printf(TEXT(R"JSON({"token": "tok", "project": "FakeBench", "projectFile": "%s", "editorProcessId": 0, "reason": "crash", "reopen": true,
		"crash": {"error": "Assertion failed", "report": "Saved/Crashes/UECC", "processId": 0, "folder": ""}, "lingerSeconds": 0, "delaySeconds": 0,
		"build": {"enabled": false},
		"relaunch": {"executable": "cmd.exe", "arguments": "/c echo relaunched> \"%s\"", "workingDirectory": "%s"},
		"chats": [{"tab": 1, "agent": "Codex", "sessionId": "thread-1", "working": true}]})JSON"), *Escaped(Project), *Escaped(Marker), *Escaped(Folder));
	FFileHelper::SaveStringToFile(Plan, *(Restart / TEXT("request.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	int32 Code = -1;
	FString Out;
	FString Err;
	FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -Request \"%s\""),
		*Native(PluginScript(TEXT("HyperAIStudioRestart.ps1"))), *Native(Restart / TEXT("request.json"))), &Code, &Out, &Err);
	FString ResultText;
	FFileHelper::LoadFileToString(ResultText, *(Restart / TEXT("result.json")));
	const TSharedPtr<FJsonObject> Result = ParseObject(ResultText);
	AddInfo(TEXT("Helper result: ") + ResultText.Replace(TEXT("\r\n"), TEXT(" ")));
	double HandedTo = 0.0;
	TestTrue(TEXT("The helper opens no second editor"), !FPaths::FileExists(Marker) && Result.IsValid() && Result->GetStringField(TEXT("state")) == TEXT("already_open"));
	TestTrue(TEXT("and names the editor it found, which takes the chats"), Result.IsValid() && Result->TryGetNumberField(TEXT("editorProcessId"), HandedTo)
		&& static_cast<uint32>(HandedTo) == DummyId);
	FPlatformProcess::TerminateProc(Editor, /*KillTree=*/true);
	FPlatformProcess::CloseProc(Editor);

	// What an open editor checks before it takes a plan the helper handed it.
	const FString Mine = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()).ReplaceCharWithEscapedChar();
	const TSharedPtr<FJsonObject> MyPlan = ParseObject(FString::Printf(TEXT(R"JSON({"token": "t1", "projectFile": "%s"})JSON"), *Mine));
	const TSharedPtr<FJsonObject> Handed = ParseObject(FString::Printf(TEXT(R"JSON({"token": "t1", "state": "already_open", "editorProcessId": %u})JSON"), Self));
	const TSharedPtr<FJsonObject> Other = ParseObject(FString::Printf(TEXT(R"JSON({"token": "t1", "state": "already_open", "editorProcessId": %u})JSON"), Self + 1));
	const TSharedPtr<FJsonObject> Relaunched = ParseObject(FString::Printf(TEXT(R"JSON({"token": "t1", "state": "relaunched", "editorProcessId": %u})JSON"), Self));
	TestTrue(TEXT("A plan handed to this editor is taken"), FHyperAIStudioEditorRestart::IsHandedTo(MyPlan, Handed, Self));
	TestFalse(TEXT("one handed to another is not"), FHyperAIStudioEditorRestart::IsHandedTo(MyPlan, Other, Self));
	TestFalse(TEXT("nor one the helper relaunched an editor for"), FHyperAIStudioEditorRestart::IsHandedTo(MyPlan, Relaunched, Self));
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioRecoveryCrashSummaryTest,
	"HyperAIStudio.EditorRestart.CrashSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioRecoveryCrashSummaryTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::RecoveryTests;
	const FString Folder = NewFolder();
	const FString Crash = Folder / TEXT("Crashes/UECC-Windows-FIXTURE_0000");
	IFileManager::Get().MakeDirectory(*Crash, /*Tree=*/true);
	// A crash folder as the editor leaves it: the context, and its log ending in the crash's own dump.
	FFileHelper::SaveStringToFile(TEXT(R"XML(<?xml version="1.0" encoding="UTF-8"?>
<FGenericCrashContext>
	<RuntimeProperties>
		<CrashType>Assert</CrashType>
		<ErrorMessage>Assertion failed: Ability.IsValid() [File:E:\Game\Jump.cpp] [Line: 40]

</ErrorMessage>
		<ProcessId>4321</ProcessId>
		<SecondsSinceStart>754</SecondsSinceStart>
		<IsRequestingExit>false</IsRequestingExit>
		<CallStack>UnrealEditor_Game!UJumpAbility::Activate() [E:\Game\Jump.cpp:40]
UnrealEditor_GameplayAbilities!UAbilitySystemComponent::TryActivateAbility() [E:\Engine\AbilitySystemComponent.cpp:1650]
UnrealEditor_HyperAIStudio!UHyperAIStudioPlaytest::Step() [E:\HyperAI\Playtest.cpp:88]</CallStack>
	</RuntimeProperties>
</FGenericCrashContext>
)XML"), *(Crash / TEXT("CrashContext.runtime-xml")));
	FFileHelper::SaveStringToFile(TEXT(
		"[2026.10.13-10.00.00:000][  1]LogInit: Display: Starting\r\n"
		"[2026.10.13-10.00.01:000][ 12]LogHyperAIStudio: Display: hyper_playtest started PIE\r\n"
		"\r\n"
		"[2026.10.13-10.00.02:000][ 13]LogTemp: Warning: Jump ability has no montage\r\n"
		"[2026.10.13-10.00.03:000][ 14]LogWindows: Error: === Critical error: ===\r\n"
		"[2026.10.13-10.00.03:000][ 14]LogWindows: Error: Assertion failed: Ability.IsValid()\r\n"
		"[2026.10.13-10.00.03:000][ 14]LogWindows: Error: [Callstack] 0x00007ff6 UnrealEditor-Game.dll!UJumpAbility::Activate()\r\n"),
		*(Crash / TEXT("FakeBench.log")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);

	auto Read = [&](const uint32 ProcessId)
	{
		int32 Code = -1;
		FString Out;
		FString Err;
		FPlatformProcess::ExecProcess(TEXT("powershell.exe"), *FString::Printf(TEXT("-NoProfile -ExecutionPolicy Bypass -File \"%s\" -CrashFolder \"%s\" -CrashProcessId %u"),
			*Native(PluginScript(TEXT("HyperAIStudioRecovery.ps1"))), *Native(Crash), ProcessId), &Code, &Out, &Err);
		if (!Err.IsEmpty())
		{
			AddInfo(TEXT("The script said: ") + Err);
		}
		return Out.TrimStartAndEnd();
	};
	const FString Json = Read(4321);
	AddInfo(TEXT("Summary: ") + Json);
	const TSharedPtr<FJsonObject> Summary = ParseObject(Json);
	if (!TestTrue(TEXT("The crash folder reads as a summary"), Summary.IsValid()))
	{
		return false;
	}
	TArray<FString> Frames;
	TArray<FString> Tail;
	Summary->TryGetStringArrayField(TEXT("callstack"), Frames);
	Summary->TryGetStringArrayField(TEXT("logTail"), Tail);
	TestTrue(TEXT("with its kind, error and how long the editor ran"), Summary->GetStringField(TEXT("type")) == TEXT("Assert")
		&& Summary->GetStringField(TEXT("error")).StartsWith(TEXT("Assertion failed: Ability.IsValid()")) && Summary->GetNumberField(TEXT("uptimeSeconds")) == 754.0);
	TestTrue(TEXT("the crashed thread's first frames"), Frames.Num() == 3 && Frames[0].Contains(TEXT("UJumpAbility::Activate()")));
	TestTrue(TEXT("and the last lines logged before it, without timestamps, blanks or the crash's own dump"), Tail.Num() == 3
		&& Tail.Last() == TEXT("LogTemp: Warning: Jump ability has no montage") && Tail[0] == TEXT("LogInit: Display: Starting")
		&& !Tail.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("Critical error")) || Line.Contains(TEXT("[Callstack]")); }));
	TestEqual(TEXT("Another process's crash is not this editor's"), Read(999), FString(TEXT("null")));

	// What a chat that was working is told.
	const TSharedRef<FJsonObject> Plan = MakeShared<FJsonObject>();
	Plan->SetStringField(TEXT("reason"), TEXT("crash"));
	Plan->SetObjectField(TEXT("crash"), Summary.ToSharedRef());
	const FString Told = FHyperAIStudioEditorRestart::DescribeRestart(Plan, nullptr);
	AddInfo(TEXT("The chat is told:\n") + Told);
	TestTrue(TEXT("The chat hears the crash, its kind and when"), Told.Contains(TEXT("- Crash: Assertion failed: Ability.IsValid()")) && Told.Contains(TEXT("Kind: Assert, 13 min after the editor started")));
	TestTrue(TEXT("where it happened, and what was logged just before"), Told.Contains(TEXT("UJumpAbility::Activate()")) && Told.Contains(TEXT("Last logged before it:"))
		&& Told.Contains(TEXT("Jump ability has no montage")) && Told.Contains(TEXT("Saved/Crashes/UECC-Windows-FIXTURE_0000")));
	IFileManager::Get().DeleteDirectory(*Folder, false, true);
	return true;
}

#endif
