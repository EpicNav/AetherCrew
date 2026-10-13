// Games by Hyper 2026.

#include "HyperAIStudioChatModes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioSkills.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/PrettyJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::ChatModes
{
namespace
{
	struct FActiveMode
	{
		FString Label;
		FString Name;
		EHyperAIStudioModeApproval Approval = EHyperAIStudioModeApproval::Normal;
	};

	TMap<const void*, FActiveMode>& Active()
	{
		static TMap<const void*, FActiveMode> Modes;
		return Modes;
	}

	FHyperAIStudioChatMode Make(const TCHAR* Id, const TCHAR* Name, const TCHAR* Description, const EHyperAIStudioModeApproval Approval,
		const TCHAR* Instructions, TArray<FString> Packs = {}, TArray<FString> Skills = {})
	{
		FHyperAIStudioChatMode Mode;
		Mode.Id = Id;
		Mode.Name = Name;
		Mode.Description = Description;
		Mode.Approval = Approval;
		Mode.Instructions = Instructions;
		Mode.Packs = MoveTemp(Packs);
		Mode.Skills = MoveTemp(Skills);
		Mode.bBuiltIn = true;
		return Mode;
	}

	bool ReadStrings(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, TArray<FString>& Out, FString& OutError)
	{
		Out.Reset();
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->HasField(Field))
		{
			return true;
		}
		if (!Object->TryGetArrayField(Field, Values))
		{
			OutError = FString::Printf(TEXT("\"%s\" must be a list of names."), Field);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Name;
			if (!Value.IsValid() || !Value->TryGetString(Name) || Name.TrimStartAndEnd().IsEmpty() || Name.Len() > 128)
			{
				OutError = FString::Printf(TEXT("\"%s\" must hold names of up to 128 characters."), Field);
				return false;
			}
			Out.AddUnique(Name.TrimStartAndEnd());
		}
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	FString FileOf(const FString& ProjectRoot, const FString& Id)
	{
		return FPaths::Combine(Directory(ProjectRoot), Id + TEXT(".json"));
	}
}

FString Directory(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("modes"));
}

TArray<FHyperAIStudioChatMode> BuiltIns()
{
	using enum EHyperAIStudioModeApproval;
	return {
		Make(TEXT("plan"), TEXT("Plan"), TEXT("Read only: looks at the project and answers with a plan, changing nothing."), ReadOnly,
			TEXT("You are in Plan mode. Read the project and the editor with read and inspect tools only, then answer with a numbered plan: ")
			TEXT("each step, the assets and HyperAI tools it uses, the risks, and how the result will be checked. Do not change files, assets ")
			TEXT("or the level: HyperAI refuses edit plans while this mode is on. End by asking whether to build it.")),
		Make(TEXT("build"), TEXT("Build"), TEXT("Makes the change: plans checked with a dry run, applied, then verified."), Normal,
			TEXT("You are in Build mode. Make the change asked for: check each HyperAI plan with a dry run, apply it, then read the result ")
			TEXT("back (inspect tools, a capture or a playtest) and fix what does not match before you say it is done.")),
		Make(TEXT("review"), TEXT("Review"), TEXT("Read only: inspects what was asked and reports issues by severity."), ReadOnly,
			TEXT("You are in Review mode. Inspect what the user names (assets, Blueprints, the level, code, the last change) with read, ")
			TEXT("inspect and diagnose tools only. Report issues from most to least severe, each with where it is, why it matters and the ")
			TEXT("fix you suggest. Change nothing: HyperAI refuses edit plans while this mode is on."),
			{TEXT("diagnostics"), TEXT("blueprint")}, {}),
		Make(TEXT("art-pass"), TEXT("Art pass"), TEXT("Lighting, materials and set dressing, every edit approved by you."), Ask,
			TEXT("You are in Art pass mode: improve how the scene looks (lighting, post process, materials, set dressing) without changing ")
			TEXT("gameplay. Capture the view before and after each change and say what changed. Every edit waits for the user's approval."),
			{TEXT("lighting_lookdev"), TEXT("materials_dynamic_material"), TEXT("level_design")}, {TEXT("unreal-lighting"), TEXT("image-match")}),
		Make(TEXT("qa"), TEXT("QA"), TEXT("Plays and tests the game, then reports what fails, with steps to reproduce."), Ask,
			TEXT("You are in QA mode. Test what the user names in Play In Editor and with HyperAI's diagnose and playtest tools, and report ")
			TEXT("each failure with steps to reproduce, what was expected and what happened, and evidence (log lines, captures). Fix nothing ")
			TEXT("unless asked; any edit waits for the user's approval."),
			{TEXT("pie_runtime"), TEXT("diagnostics")}, {TEXT("unreal-playtest")})};
}

TArray<FHyperAIStudioChatMode> List(const FString& ProjectRoot)
{
	TArray<FHyperAIStudioChatMode> Own;
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Directory(ProjectRoot), TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
	Files.Sort();
	for (const FString& Name : Files)
	{
		const FString Path = FPaths::Combine(Directory(ProjectRoot), Name);
		FString Text;
		FHyperAIStudioChatMode Mode;
		FString Error;
		// A file whose id is not its name is skipped, so two files never claim one mode.
		if (FFileHelper::LoadFileToString(Text, *Path) && FromJson(Text, Mode, Error) && Mode.Id == FPaths::GetBaseFilename(Name))
		{
			Mode.File = FPaths::ConvertRelativePathToFull(Path);
			Own.Add(MoveTemp(Mode));
		}
	}
	TArray<FHyperAIStudioChatMode> Out;
	for (FHyperAIStudioChatMode& BuiltIn : BuiltIns())
	{
		const int32 Index = Own.IndexOfByPredicate([&BuiltIn](const FHyperAIStudioChatMode& Each) { return Each.Id == BuiltIn.Id; });
		if (Index == INDEX_NONE)
		{
			Out.Add(MoveTemp(BuiltIn));
			continue;
		}
		Own[Index].bBuiltIn = true;
		Out.Add(MoveTemp(Own[Index]));
		Own.RemoveAt(Index);
	}
	Own.Sort([](const FHyperAIStudioChatMode& A, const FHyperAIStudioChatMode& B) { return A.Name < B.Name; });
	Out.Append(MoveTemp(Own));
	return Out;
}

bool Find(const FString& ProjectRoot, const FString& Id, FHyperAIStudioChatMode& OutMode)
{
	for (FHyperAIStudioChatMode& Mode : List(ProjectRoot))
	{
		if (Mode.Id == Id)
		{
			OutMode = MoveTemp(Mode);
			return true;
		}
	}
	return false;
}

FString DefaultId()
{
	return TEXT("build");
}

FString ApprovalName(const EHyperAIStudioModeApproval Approval)
{
	switch (Approval)
	{
	case EHyperAIStudioModeApproval::Ask: return TEXT("ask");
	case EHyperAIStudioModeApproval::ReadOnly: return TEXT("read-only");
	default: return TEXT("normal");
	}
}

bool ParseApproval(const FString& Name, EHyperAIStudioModeApproval& OutApproval)
{
	const FString Clean = Name.TrimStartAndEnd().ToLower();
	if (Clean == TEXT("normal") || Clean.IsEmpty()) { OutApproval = EHyperAIStudioModeApproval::Normal; return true; }
	if (Clean == TEXT("ask")) { OutApproval = EHyperAIStudioModeApproval::Ask; return true; }
	if (Clean == TEXT("read-only") || Clean == TEXT("readonly")) { OutApproval = EHyperAIStudioModeApproval::ReadOnly; return true; }
	return false;
}

bool LooksLikeSecret(const FString& Text)
{
	// Providers' key shapes, then any long run of letters and digits mixed, which is what a token looks like.
	static const TCHAR* Patterns[] = {
		TEXT("sk-[A-Za-z0-9_\\-]{16,}"),
		TEXT("AIza[0-9A-Za-z_\\-]{30,}"),
		TEXT("(ghp|gho|ghu|ghs|github_pat)_[A-Za-z0-9_]{20,}"),
		TEXT("xox[abposr]-[A-Za-z0-9\\-]{10,}"),
		TEXT("AKIA[0-9A-Z]{16}"),
		TEXT("[A-Za-z0-9_\\-]{40,}")};
	for (const TCHAR* Pattern : Patterns)
	{
		FRegexMatcher Matcher(FRegexPattern(Pattern), Text);
		while (Matcher.FindNext())
		{
			const FString Found = Text.Mid(Matcher.GetMatchBeginning(), Matcher.GetMatchEnding() - Matcher.GetMatchBeginning());
			bool bLetter = false;
			bool bDigit = false;
			for (const TCHAR C : Found)
			{
				bLetter |= FChar::IsAlpha(C);
				bDigit |= FChar::IsDigit(C);
			}
			if (bLetter && bDigit)
			{
				return true;
			}
		}
	}
	return false;
}

FString ToJson(const FHyperAIStudioChatMode& Mode)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("id"), Mode.Id);
	Object->SetStringField(TEXT("name"), Mode.Name);
	Object->SetStringField(TEXT("description"), Mode.Description);
	Object->SetStringField(TEXT("instructions"), Mode.Instructions);
	Object->SetStringField(TEXT("approval"), ApprovalName(Mode.Approval));
	Object->SetArrayField(TEXT("packs"), Strings(Mode.Packs));
	Object->SetArrayField(TEXT("tools"), Strings(Mode.Tools));
	Object->SetArrayField(TEXT("skills"), Strings(Mode.Skills));
	Object->SetStringField(TEXT("agent"), Mode.Agent);
	Object->SetStringField(TEXT("model"), Mode.Model);
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out + TEXT("\n");
}

bool FromJson(const FString& Json, FHyperAIStudioChatMode& OutMode, FString& OutError)
{
	OutMode = FHyperAIStudioChatMode();
	OutError.Reset();
	TSharedPtr<FJsonObject> Object;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
	{
		OutError = TEXT("The file is not a JSON object.");
		return false;
	}
	static const TSet<FString> Known = {TEXT("id"), TEXT("name"), TEXT("description"), TEXT("instructions"), TEXT("approval"),
		TEXT("packs"), TEXT("tools"), TEXT("skills"), TEXT("agent"), TEXT("model")};
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Object->Values)
	{
		if (!Known.Contains(Field.Key))
		{
			OutError = FString::Printf(TEXT("Unknown field \"%s\". A mode has id, name, description, instructions, approval, packs, tools, skills, agent and model; keys belong in Settings > Agents."), *Field.Key);
			return false;
		}
	}
	FString Approval;
	Object->TryGetStringField(TEXT("id"), OutMode.Id);
	Object->TryGetStringField(TEXT("name"), OutMode.Name);
	Object->TryGetStringField(TEXT("description"), OutMode.Description);
	Object->TryGetStringField(TEXT("instructions"), OutMode.Instructions);
	Object->TryGetStringField(TEXT("approval"), Approval);
	Object->TryGetStringField(TEXT("agent"), OutMode.Agent);
	Object->TryGetStringField(TEXT("model"), OutMode.Model);
	OutMode.Name.TrimStartAndEndInline();
	if (!HyperAIStudio::Skills::IsValidName(OutMode.Id))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not a valid mode id: lowercase letters, digits and hyphens, up to 64."), *OutMode.Id);
		return false;
	}
	if (OutMode.Name.IsEmpty() || OutMode.Name.Len() > 48)
	{
		OutError = TEXT("A mode needs a name of up to 48 characters.");
		return false;
	}
	if (OutMode.Instructions.Len() > 8000)
	{
		OutError = TEXT("A mode's instructions are at most 8000 characters.");
		return false;
	}
	if (!ParseApproval(Approval, OutMode.Approval))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not an approval: use normal, ask or read-only."), *Approval);
		return false;
	}
	if (!ReadStrings(Object, TEXT("packs"), OutMode.Packs, OutError) || !ReadStrings(Object, TEXT("tools"), OutMode.Tools, OutError)
		|| !ReadStrings(Object, TEXT("skills"), OutMode.Skills, OutError))
	{
		return false;
	}
	for (const FString& Text : {OutMode.Name, OutMode.Description, OutMode.Instructions, OutMode.Agent, OutMode.Model})
	{
		if (LooksLikeSecret(Text))
		{
			OutError = TEXT("The mode holds something that looks like an API key or token. Modes are shared with the project, so keys go in Settings > Agents instead.");
			return false;
		}
	}
	return true;
}

bool Save(const FString& ProjectRoot, const FHyperAIStudioChatMode& Mode, const FString& PreviousId, FString& OutError)
{
	FHyperAIStudioChatMode Checked;
	if (!FromJson(ToJson(Mode), Checked, OutError))
	{
		return false;
	}
	const FString Path = FileOf(ProjectRoot, Mode.Id);
	const bool bRenamed = !PreviousId.IsEmpty() && PreviousId != Mode.Id;
	FHyperAIStudioChatMode Existing;
	if ((PreviousId.IsEmpty() || bRenamed) && Find(ProjectRoot, Mode.Id, Existing))
	{
		OutError = FString::Printf(TEXT("A mode with the id \"%s\" already exists."), *Mode.Id);
		return false;
	}
	if (!FFileHelper::SaveStringToFile(ToJson(Mode), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *Path);
		return false;
	}
	if (bRenamed)
	{
		IFileManager::Get().Delete(*FileOf(ProjectRoot, PreviousId));
	}
	return true;
}

bool Delete(const FString& ProjectRoot, const FString& Id, FString& OutError)
{
	const FString Path = FileOf(ProjectRoot, Id);
	if (!HyperAIStudio::Skills::IsValidName(Id) || !FPaths::FileExists(Path))
	{
		OutError = FString::Printf(TEXT("The project has no mode file for \"%s\"; a built-in mode as shipped cannot be deleted."), *Id);
		return false;
	}
	if (!IFileManager::Get().Delete(*Path))
	{
		OutError = FString::Printf(TEXT("Could not delete %s."), *Path);
		return false;
	}
	return true;
}

bool Import(const FString& ProjectRoot, const FString& SourceFile, FString& OutId, FString& OutError)
{
	OutId.Reset();
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *SourceFile))
	{
		OutError = FString::Printf(TEXT("Could not read %s."), *SourceFile);
		return false;
	}
	FHyperAIStudioChatMode Mode;
	if (!FromJson(Text, Mode, OutError))
	{
		OutError = FString::Printf(TEXT("%s is not a valid mode: %s"), *FPaths::GetCleanFilename(SourceFile), *OutError);
		return false;
	}
	if (FPaths::FileExists(FileOf(ProjectRoot, Mode.Id)))
	{
		OutError = FString::Printf(TEXT("The project already has a mode \"%s\"; delete or rename it first."), *Mode.Id);
		return false;
	}
	// Saved as an edit of its own id, so a file for a built-in mode replaces that mode in this project.
	if (!Save(ProjectRoot, Mode, Mode.Id, OutError))
	{
		return false;
	}
	OutId = Mode.Id;
	return true;
}

bool Export(const FString& ProjectRoot, const FString& Id, const FString& DestinationFile, FString& OutError)
{
	FHyperAIStudioChatMode Mode;
	if (!Find(ProjectRoot, Id, Mode))
	{
		OutError = FString::Printf(TEXT("There is no mode \"%s\"."), *Id);
		return false;
	}
	if (!FFileHelper::SaveStringToFile(ToJson(Mode), *DestinationFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *DestinationFile);
		return false;
	}
	return true;
}

FString InstructionsBlock(const FHyperAIStudioChatMode& Mode)
{
	FString Block = FString::Printf(TEXT("\n\n<mode name=\"%s\" approval=\"%s\">\n"), *Mode.Name, *ApprovalName(Mode.Approval));
	if (!Mode.Instructions.TrimStartAndEnd().IsEmpty())
	{
		Block += Mode.Instructions.TrimStartAndEnd() + TEXT("\n");
	}
	if (Mode.IsReadOnly())
	{
		Block += TEXT("HyperAI refuses edit plans while this chat is in this mode; do not try to work around it with scripts or file edits.\n");
	}
	else if (Mode.Approval == EHyperAIStudioModeApproval::Ask)
	{
		Block += TEXT("Every HyperAI edit plan waits for the user's click in this mode, so wait for each with hyper_operation_wait.\n");
	}
	if (!Mode.Skills.IsEmpty())
	{
		Block += TEXT("Skills for this mode: ") + FString::Join(Mode.Skills, TEXT(", ")) + TEXT(".\n");
	}
	return Block + TEXT("</mode>");
}

void SetActive(const void* Owner, const FHyperAIStudioChatMode* Mode, const FString& Label)
{
	if (!Owner)
	{
		return;
	}
	if (!Mode)
	{
		Active().Remove(Owner);
		return;
	}
	FActiveMode& Entry = Active().FindOrAdd(Owner);
	Entry.Label = Label;
	Entry.Name = Mode->Name;
	Entry.Approval = Mode->Approval;
}

void ClearActive(const void* Owner)
{
	Active().Remove(Owner);
}

FString ReadOnlyHolder()
{
	for (const TPair<const void*, FActiveMode>& Pair : Active())
	{
		if (Pair.Value.Approval == EHyperAIStudioModeApproval::ReadOnly)
		{
			return FString::Printf(TEXT("%s is in %s mode"), Pair.Value.Label.IsEmpty() ? TEXT("A chat") : *Pair.Value.Label, *Pair.Value.Name);
		}
	}
	return FString();
}

bool IsAskActive()
{
	for (const TPair<const void*, FActiveMode>& Pair : Active())
	{
		if (Pair.Value.Approval == EHyperAIStudioModeApproval::Ask)
		{
			return true;
		}
	}
	return false;
}

FString ReadOnlyRefusal()
{
	const FString Holder = ReadOnlyHolder();
	return Holder.IsEmpty() ? FString()
		: FString::Printf(TEXT("Refused: %s, which is read-only, so HyperAI refuses edit plans and nothing was changed. ")
			TEXT("Describe the change as a plan instead; the user switches that chat to Build mode (or another mode that allows edits) to make it."), *Holder);
}

#if WITH_DEV_AUTOMATION_TESTS
void ClearAllActiveForTests()
{
	Active().Reset();
}
#endif
}
