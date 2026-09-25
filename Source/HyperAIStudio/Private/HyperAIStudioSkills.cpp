// Games by Hyper 2026.

#include "HyperAIStudioSkills.h"

#include "HyperAIStudioProjectMemory.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Blueprint/BlueprintSupport.h"
#include "Engine/Blueprint.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "ToolsetRegistry/AgentSkill.h"
#include "UObject/UObjectIterator.h"

namespace HyperAIStudio::Skills
{
namespace
{
	const TCHAR* MirrorMarker = TEXT(".hyperai-mirror");
	const TCHAR* BuiltInMarker = TEXT(".hyperai-builtin");
	const TCHAR* CodexPolicyFile = TEXT("agents/openai.yaml");
	const TCHAR* MenuOnlyPolicy = TEXT("policy:\n  allow_implicit_invocation: false\n");

	FString HomeDirectory()
	{
		const FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		return Home.IsEmpty() ? FPlatformMisc::GetEnvironmentVariable(TEXT("HOME")) : Home;
	}

	bool IsMarker(const FString& FileName)
	{
		const FString Clean = FPaths::GetCleanFilename(FileName);
		return Clean == MirrorMarker || Clean == BuiltInMarker;
	}

	/** Files under Folder, as paths relative to it with forward slashes, sorted. */
	TArray<FString> RelativeFiles(const FString& Folder)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Folder, TEXT("*"), /*Files=*/true, /*Directories=*/false);
		const FString Prefix = FPaths::ConvertRelativePathToFull(Folder).Replace(TEXT("\\"), TEXT("/")) + TEXT("/");
		TArray<FString> Relative;
		for (const FString& File : Files)
		{
			const FString Full = FPaths::ConvertRelativePathToFull(File).Replace(TEXT("\\"), TEXT("/"));
			if (!IsMarker(Full) && Full.StartsWith(Prefix))
			{
				Relative.Add(Full.RightChop(Prefix.Len()));
			}
		}
		Relative.Sort();
		return Relative;
	}

	FString ReadMarker(const FString& Folder, const TCHAR* Marker)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, Marker));
		return Text.TrimStartAndEnd();
	}

	bool WriteMarker(const FString& Folder, const TCHAR* Marker, const FString& Hash)
	{
		return FFileHelper::SaveStringToFile(Hash, *FPaths::Combine(Folder, Marker), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** Replaces Destination with a copy of Source, markers left out. Callers decide it is safe to replace. */
	bool CopyFolder(const FString& Source, const FString& Destination)
	{
		IFileManager& Files = IFileManager::Get();
		if (Files.DirectoryExists(*Destination) && !Files.DeleteDirectory(*Destination, /*RequireExists=*/false, /*Tree=*/true))
		{
			return false;
		}
		for (const FString& Relative : RelativeFiles(Source))
		{
			if (Files.Copy(*FPaths::Combine(Destination, Relative), *FPaths::Combine(Source, Relative)) != COPY_OK)
			{
				return false;
			}
		}
		return Files.MakeDirectory(*Destination, /*Tree=*/true);
	}

	/** Skill folders directly under Directory (each holding a SKILL.md). */
	TArray<FString> SkillFolders(const FString& Directory)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Directory, TEXT("*")), /*Files=*/false, /*Directories=*/true);
		TArray<FString> Folders;
		for (const FString& Name : Names)
		{
			const FString Folder = FPaths::Combine(Directory, Name);
			if (FPaths::FileExists(FPaths::Combine(Folder, TEXT("SKILL.md"))))
			{
				Folders.Add(Folder);
			}
		}
		Folders.Sort();
		return Folders;
	}

	bool ReadSkill(const FString& Folder, const FString& Scope, FHyperAIStudioSkill& OutSkill)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, TEXT("SKILL.md"))))
		{
			return false;
		}
		FString Name;
		FString Description;
		bool bMenuOnly = false;
		if (!ParseSkill(Text, Name, Description, bMenuOnly))
		{
			return false;
		}
		FString Policy;
		if (FFileHelper::LoadFileToString(Policy, *FPaths::Combine(Folder, CodexPolicyFile)) && Policy.Contains(TEXT("allow_implicit_invocation: false")))
		{
			bMenuOnly = true;
		}
		OutSkill.Name = FPaths::GetCleanFilename(Folder);
		OutSkill.Description = Description;
		OutSkill.Folder = FPaths::ConvertRelativePathToFull(Folder);
		OutSkill.Scope = Scope;
		OutSkill.bMenuOnly = bMenuOnly;
		return true;
	}

	/** Every SKILL.md below Directory: personal folders nest skills in groups. */
	void AddNestedSkills(const FString& Directory, const FString& Scope, TArray<FHyperAIStudioSkill>& Out)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *Directory, TEXT("SKILL.md"), /*Files=*/true, /*Directories=*/false);
		Files.Sort();
		for (const FString& File : Files)
		{
			FHyperAIStudioSkill Skill;
			if (ReadSkill(FPaths::GetPath(File), Scope, Skill))
			{
				Out.Add(MoveTemp(Skill));
			}
		}
	}

	FString KebabCase(const FString& Camel)
	{
		FString Out;
		for (int32 Index = 0; Index < Camel.Len(); ++Index)
		{
			const TCHAR C = Camel[Index];
			if (FChar::IsUpper(C) && Index > 0 && !Out.EndsWith(TEXT("-")))
			{
				Out.AppendChar(TEXT('-'));
			}
			Out.AppendChar(C < 128 && FChar::IsAlnum(C) ? FChar::ToLower(C) : TEXT('-'));
		}
		while (Out.ReplaceInline(TEXT("--"), TEXT("-")) > 0) {}
		Out.RemoveFromStart(TEXT("-"));
		Out.RemoveFromEnd(TEXT("-"));
		return Out;
	}

	/** A call that changes something, so a run containing it is worth a skill. */
	bool IsChange(const FString& Signature)
	{
		static const TCHAR* Verbs[] = {TEXT("apply"), TEXT("create"), TEXT("add"), TEXT("set"), TEXT("write"), TEXT("save"),
			TEXT("build"), TEXT("spawn"), TEXT("delete"), TEXT("remove"), TEXT("execute"), TEXT("import"), TEXT("compile"),
			TEXT("duplicate"), TEXT("rename"), TEXT("place"), TEXT("run")};
		FString Tool;
		Signature.Split(TEXT("."), nullptr, &Tool, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		for (const TCHAR* Verb : Verbs)
		{
			if (Tool.Contains(Verb))
			{
				return true;
			}
		}
		return false;
	}
}

FString CanonicalDirectory(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".agents"), TEXT("skills"));
}

FString ClaudeDirectory(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".claude"), TEXT("skills"));
}

FString PersonalClaudeDirectory()
{
	const FString Config = FPlatformMisc::GetEnvironmentVariable(TEXT("CLAUDE_CONFIG_DIR"));
	return FPaths::Combine(Config.IsEmpty() ? FPaths::Combine(HomeDirectory(), TEXT(".claude")) : Config, TEXT("skills"));
}

TArray<FString> PersonalAgentsDirectories()
{
	const FString CodexHome = FPlatformMisc::GetEnvironmentVariable(TEXT("CODEX_HOME"));
	return {FPaths::Combine(HomeDirectory(), TEXT(".agents"), TEXT("skills")),
		FPaths::Combine(CodexHome.IsEmpty() ? FPaths::Combine(HomeDirectory(), TEXT(".codex")) : CodexHome, TEXT("skills"))};
}

FString BuiltInDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Skills")) : FString();
}

bool IsValidName(const FString& Name)
{
	if (Name.IsEmpty() || Name.Len() > 64 || Name.StartsWith(TEXT("-")) || Name.EndsWith(TEXT("-")))
	{
		return false;
	}
	for (const TCHAR C : Name)
	{
		if (!((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('-')))
		{
			return false;
		}
	}
	return true;
}

bool ParseSkill(const FString& SkillText, FString& OutName, FString& OutDescription, bool& bOutMenuOnly)
{
	TArray<FString> Lines;
	SkillText.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
	OutName.Reset();
	OutDescription.Reset();
	bOutMenuOnly = false;
	if (Lines.Num() < 3 || Lines[0].TrimEnd() != TEXT("---"))
	{
		return false;
	}
	for (int32 Line = 1; Line < Lines.Num(); ++Line)
	{
		if (Lines[Line].TrimEnd() == TEXT("---"))
		{
			return !OutName.IsEmpty() && !OutDescription.IsEmpty();
		}
		FString Key;
		FString Value;
		if (!Lines[Line].Split(TEXT(":"), &Key, &Value) || Key.StartsWith(TEXT(" ")))
		{
			continue;
		}
		Value.TrimStartAndEndInline();
		if (Value.Len() >= 2 && (Value.StartsWith(TEXT("\"")) || Value.StartsWith(TEXT("'"))))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		if (Key == TEXT("name")) OutName = Value;
		else if (Key == TEXT("description")) OutDescription = Value;
		else if (Key == TEXT("disable-model-invocation")) bOutMenuOnly = Value == TEXT("true");
	}
	return false;
}

FString FolderHash(const FString& Folder)
{
	FSHA1 Sha;
	for (const FString& Relative : RelativeFiles(Folder))
	{
		const FTCHARToUTF8 Path(*Relative);
		Sha.Update(reinterpret_cast<const uint8*>(Path.Get()), Path.Length() + 1);
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *FPaths::Combine(Folder, Relative));
		Sha.Update(Bytes.GetData(), Bytes.Num());
		const uint8 Separator = 0;
		Sha.Update(&Separator, 1);
	}
	Sha.Final();
	uint8 Digest[FSHA1::DigestSize];
	Sha.GetHash(Digest);
	return BytesToHex(Digest, FSHA1::DigestSize);
}

TArray<FHyperAIStudioSkill> ListProjectSkills(const FString& ProjectRoot)
{
	TArray<FHyperAIStudioSkill> Skills;
	TSet<FString> Canonical;
	for (const FString& Folder : SkillFolders(CanonicalDirectory(ProjectRoot)))
	{
		FHyperAIStudioSkill Skill;
		if (!ReadSkill(Folder, TEXT("project"), Skill))
		{
			continue;
		}
		const FString Hash = FolderHash(Folder);
		Skill.bBuiltIn = ReadMarker(Folder, BuiltInMarker) == Hash;
		const FString Mirror = FPaths::Combine(ClaudeDirectory(ProjectRoot), Skill.Name);
		if (!FPaths::FileExists(FPaths::Combine(Mirror, TEXT("SKILL.md"))))
		{
			Skill.MirrorState = TEXT("missing");
		}
		else if (!FPaths::FileExists(FPaths::Combine(Mirror, MirrorMarker)))
		{
			Skill.MirrorState = TEXT("taken");
		}
		else
		{
			const FString MirrorHash = FolderHash(Mirror);
			Skill.MirrorState = MirrorHash != ReadMarker(Mirror, MirrorMarker) ? TEXT("edited") : MirrorHash == Hash ? TEXT("synced") : TEXT("stale");
		}
		Canonical.Add(Skill.Name);
		Skills.Add(MoveTemp(Skill));
	}
	// Claude-only skills: in .claude/skills, not written by HyperAIStudio.
	for (const FString& Folder : SkillFolders(ClaudeDirectory(ProjectRoot)))
	{
		FHyperAIStudioSkill Skill;
		if (!Canonical.Contains(FPaths::GetCleanFilename(Folder)) && !FPaths::FileExists(FPaths::Combine(Folder, MirrorMarker))
			&& ReadSkill(Folder, TEXT("claude"), Skill))
		{
			Skills.Add(MoveTemp(Skill));
		}
	}
	return Skills;
}

TArray<FHyperAIStudioSkill> ListPersonalSkills()
{
	TArray<FHyperAIStudioSkill> Skills;
	AddNestedSkills(PersonalClaudeDirectory(), TEXT("personal-claude"), Skills);
	for (const FString& Directory : PersonalAgentsDirectories())
	{
		AddNestedSkills(Directory, TEXT("personal-agents"), Skills);
	}
	return Skills;
}

FString HintFor(const FString& ProjectRoot, const FString& Message, TSet<FString>& Given)
{
	// Project memory's search: a skill fits with two of the message's words, or one that few skills hold.
	TArray<FString> Words;
	for (const FString& Word : Memory::SearchWords(Message))
	{
		if (Words.Num() >= 40)
		{
			break;
		}
		Words.AddUnique(Word);
	}
	const TArray<FHyperAIStudioSkill> Skills = ListProjectSkills(ProjectRoot).FilterByPredicate(
		[](const FHyperAIStudioSkill& Skill) { return !Skill.bMenuOnly; });
	if (Words.IsEmpty() || Skills.IsEmpty())
	{
		return FString();
	}
	TArray<TArray<FString>> Documents;
	for (const FHyperAIStudioSkill& Skill : Skills)
	{
		// A skill is mostly what its name says, so its name counts twice.
		const TArray<FString> Name = Memory::SearchWords(Skill.Name.Replace(TEXT("-"), TEXT(" ")));
		TArray<FString>& Document = Documents.Add_GetRef(Name);
		Document.Append(Name);
		Document.Append(Memory::SearchWords(Skill.Description));
	}
	const TArray<Memory::FRank> Ranks = Memory::Rank(Words, Documents);
	TArray<int32> Order;
	for (int32 Index = 0; Index < Ranks.Num(); ++Index)
	{
		Order.Add(Index);
	}
	Order.StableSort([&Ranks](const int32 A, const int32 B) { return Ranks[A].Score > Ranks[B].Score; });
	const int32 NeedWords = FMath::Min(2, Words.Num());
	TArray<FString> Lines;
	for (const int32 Index : Order)
	{
		const Memory::FRank& Found = Ranks[Index];
		const FHyperAIStudioSkill& Skill = Skills[Index];
		if (Lines.Num() >= 2 || Found.Matched == 0)
		{
			break;
		}
		if ((Found.Matched < NeedWords && Found.MatchedRare == 0) || Given.Contains(TEXT("skill:") + Skill.Name))
		{
			continue;
		}
		Given.Add(TEXT("skill:") + Skill.Name);
		FString Folder = Skill.Folder;
		FPaths::MakePathRelativeTo(Folder, *(ProjectRoot / TEXT("")));
		const FString Description = Skill.Description.Len() > 300 ? Skill.Description.Left(300) + TEXT("...") : Skill.Description;
		Lines.Add(FString::Printf(TEXT("- `%s`: %s Read `%s/SKILL.md` before you start."), *Skill.Name, *Description, *Folder));
	}
	return Lines.IsEmpty() ? FString()
		: TEXT("\n\n<skills>\nProject skills that fit this message. Use them without being asked.\n") + FString::Join(Lines, TEXT("\n")) + TEXT("\n</skills>");
}

FString InvocationPrefix(const FString& AgentName)
{
	if (AgentName.Equals(TEXT("Claude Code"), ESearchCase::IgnoreCase))
	{
		return TEXT("/");
	}
	return AgentName.Equals(TEXT("Codex"), ESearchCase::IgnoreCase) ? TEXT("$") : FString();
}

TArray<FHyperAIStudioSkill> ListForAgent(const FString& ProjectRoot, const FString& AgentName)
{
	const FString Prefix = InvocationPrefix(AgentName);
	TArray<FHyperAIStudioSkill> Skills;
	if (Prefix.IsEmpty())
	{
		return Skills;
	}
	const bool bClaude = Prefix == TEXT("/");
	for (FHyperAIStudioSkill& Skill : ListProjectSkills(ProjectRoot))
	{
		// Codex never reads .claude/skills.
		if (bClaude || Skill.Scope == TEXT("project"))
		{
			Skills.Add(MoveTemp(Skill));
		}
	}
	for (FHyperAIStudioSkill& Skill : ListPersonalSkills())
	{
		if (Skill.Scope == (bClaude ? TEXT("personal-claude") : TEXT("personal-agents")))
		{
			Skills.Add(MoveTemp(Skill));
		}
	}
	return Skills;
}

FSyncReport InstallBuiltIns(const FString& ProjectRoot, const FString& SourceDirectory)
{
	FSyncReport Report;
	for (const FString& Source : SkillFolders(SourceDirectory))
	{
		const FString Name = FPaths::GetCleanFilename(Source);
		const FString Target = FPaths::Combine(CanonicalDirectory(ProjectRoot), Name);
		const FString SourceHash = FolderHash(Source);
		if (!FPaths::DirectoryExists(Target))
		{
			if (CopyFolder(Source, Target) && WriteMarker(Target, BuiltInMarker, SourceHash))
			{
				++Report.Changed;
			}
			continue;
		}
		const FString Recorded = ReadMarker(Target, BuiltInMarker);
		if (Recorded.IsEmpty())
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: a project skill of that name already exists, so the built-in one was not installed."), *Name));
		}
		else if (FolderHash(Target) != Recorded)
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: edited in this project, so plugin updates to it are not applied."), *Name));
		}
		else if (Recorded != SourceHash && CopyFolder(Source, Target) && WriteMarker(Target, BuiltInMarker, SourceHash))
		{
			++Report.Changed;
		}
	}
	return Report;
}

FSyncReport SyncClaudeMirror(const FString& ProjectRoot)
{
	FSyncReport Report;
	TSet<FString> Sources;
	for (const FString& Source : SkillFolders(CanonicalDirectory(ProjectRoot)))
	{
		const FString Name = FPaths::GetCleanFilename(Source);
		Sources.Add(Name);
		const FString Mirror = FPaths::Combine(ClaudeDirectory(ProjectRoot), Name);
		const FString SourceHash = FolderHash(Source);
		if (!FPaths::DirectoryExists(Mirror))
		{
			if (CopyFolder(Source, Mirror) && WriteMarker(Mirror, MirrorMarker, SourceHash))
			{
				++Report.Changed;
			}
			continue;
		}
		const FString Recorded = ReadMarker(Mirror, MirrorMarker);
		if (Recorded.IsEmpty())
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: a Claude Code skill of that name already exists; Claude keeps using it."), *Name));
		}
		else if (FolderHash(Mirror) != Recorded)
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: its Claude Code copy was edited there. Promote it to keep those edits, or delete the copy."), *Name));
		}
		else if (Recorded != SourceHash && CopyFolder(Source, Mirror) && WriteMarker(Mirror, MirrorMarker, SourceHash))
		{
			++Report.Changed;
		}
	}
	// Copies of skills that no longer exist go too, unless someone edited them.
	for (const FString& Mirror : SkillFolders(ClaudeDirectory(ProjectRoot)))
	{
		const FString Recorded = ReadMarker(Mirror, MirrorMarker);
		if (!Recorded.IsEmpty() && !Sources.Contains(FPaths::GetCleanFilename(Mirror)) && FolderHash(Mirror) == Recorded
			&& IFileManager::Get().DeleteDirectory(*Mirror, /*RequireExists=*/false, /*Tree=*/true))
		{
			++Report.Changed;
		}
	}
	return Report;
}

bool Promote(const FString& ProjectRoot, const FString& Name, FString& OutError)
{
	const FString Mirror = FPaths::Combine(ClaudeDirectory(ProjectRoot), Name);
	if (!IsValidName(Name) || !FPaths::FileExists(FPaths::Combine(Mirror, TEXT("SKILL.md"))))
	{
		OutError = FString::Printf(TEXT("There is no Claude Code skill named '%s' in this project."), *Name);
		return false;
	}
	if (!CopyFolder(Mirror, FPaths::Combine(CanonicalDirectory(ProjectRoot), Name))
		|| !WriteMarker(Mirror, MirrorMarker, FolderHash(Mirror)))
	{
		OutError = TEXT("Could not copy the skill into .agents/skills.");
		return false;
	}
	return true;
}

bool SetMenuOnly(const FString& SkillFolder, const bool bMenuOnly, FString& OutError)
{
	const FString SkillPath = FPaths::Combine(SkillFolder, TEXT("SKILL.md"));
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *SkillPath))
	{
		OutError = TEXT("The skill has no SKILL.md.");
		return false;
	}
	TArray<FString> Lines;
	Text.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
	int32 Close = INDEX_NONE;
	for (int32 Line = 1; Line < Lines.Num(); ++Line)
	{
		if (Lines[Line].TrimEnd() == TEXT("---"))
		{
			Close = Line;
			break;
		}
	}
	if (Lines.IsEmpty() || Lines[0].TrimEnd() != TEXT("---") || Close == INDEX_NONE)
	{
		OutError = TEXT("SKILL.md has no frontmatter.");
		return false;
	}
	for (int32 Line = Close - 1; Line >= 1; --Line)
	{
		if (Lines[Line].StartsWith(TEXT("disable-model-invocation:")))
		{
			Lines.RemoveAt(Line);
			--Close;
		}
	}
	if (bMenuOnly)
	{
		Lines.Insert(TEXT("disable-model-invocation: true"), Close);
	}
	if (!FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")), *SkillPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("Could not write SKILL.md.");
		return false;
	}

	// Codex: agents/openai.yaml. Only a file HyperAIStudio wrote is removed; any other is edited in place.
	const FString PolicyPath = FPaths::Combine(SkillFolder, CodexPolicyFile);
	FString Policy;
	const bool bHasPolicy = FFileHelper::LoadFileToString(Policy, *PolicyPath);
	if (bMenuOnly)
	{
		if (!bHasPolicy)
		{
			Policy = MenuOnlyPolicy;
		}
		else if (Policy.Contains(TEXT("allow_implicit_invocation:")))
		{
			Policy.ReplaceInline(TEXT("allow_implicit_invocation: true"), TEXT("allow_implicit_invocation: false"));
		}
		else if (Policy.Contains(TEXT("policy:")))
		{
			Policy.ReplaceInline(TEXT("policy:"), TEXT("policy:\n  allow_implicit_invocation: false"));
		}
		else
		{
			Policy = Policy.TrimEnd() + TEXT("\n") + MenuOnlyPolicy;
		}
		return FFileHelper::SaveStringToFile(Policy, *PolicyPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	if (!bHasPolicy)
	{
		return true;
	}
	if (Policy.Replace(TEXT("\r"), TEXT("")) == MenuOnlyPolicy)
	{
		return IFileManager::Get().Delete(*PolicyPath);
	}
	Policy.ReplaceInline(TEXT("allow_implicit_invocation: false"), TEXT("allow_implicit_invocation: true"));
	return FFileHelper::SaveStringToFile(Policy, *PolicyPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

FString Import(const FString& ProjectRoot, const FString& Source, FString& OutError)
{
	FString Folder = Source;
	FString Scratch;
	if (Source.EndsWith(TEXT(".zip"), ESearchCase::IgnoreCase))
	{
		// Windows 10 and later ship bsdtar, which reads zip archives.
		Scratch = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("HyperAIStudio"), TEXT("SkillImport"), FGuid::NewGuid().ToString());
		IFileManager::Get().MakeDirectory(*Scratch, /*Tree=*/true);
		int32 ReturnCode = -1;
		FString Output;
		FString Errors;
		FPlatformProcess::ExecProcess(TEXT("tar"), *FString::Printf(TEXT("-xf \"%s\" -C \"%s\""), *Source, *Scratch), &ReturnCode, &Output, &Errors);
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Scratch, TEXT("SKILL.md"), /*Files=*/true, /*Directories=*/false);
		Folder = Found.IsEmpty() ? FString() : FPaths::GetPath(Found[0]);
		if (ReturnCode != 0 || Folder.IsEmpty())
		{
			OutError = TEXT("The zip does not hold a skill folder with a SKILL.md.");
			IFileManager::Get().DeleteDirectory(*Scratch, false, true);
			return FString();
		}
	}
	FHyperAIStudioSkill Skill;
	FString Name;
	if (!ReadSkill(Folder, TEXT("project"), Skill))
	{
		OutError = TEXT("That folder has no valid SKILL.md (frontmatter with name and description).");
	}
	else if (Name = Skill.Name; !IsValidName(Name))
	{
		OutError = FString::Printf(TEXT("'%s' is not a valid skill name: lowercase letters, digits and hyphens."), *Name);
		Name.Reset();
	}
	else if (FPaths::DirectoryExists(FPaths::Combine(CanonicalDirectory(ProjectRoot), Name)))
	{
		OutError = FString::Printf(TEXT("The project already has a skill named '%s'."), *Name);
		Name.Reset();
	}
	else if (!CopyFolder(Folder, FPaths::Combine(CanonicalDirectory(ProjectRoot), Name)))
	{
		OutError = TEXT("Could not copy the skill into .agents/skills.");
		Name.Reset();
	}
	if (!Scratch.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*Scratch, false, true);
	}
	return Name;
}

bool Export(const FString& SkillFolder, const FString& DestinationDirectory, FString& OutError)
{
	const FString Target = FPaths::Combine(DestinationDirectory, FPaths::GetCleanFilename(SkillFolder));
	if (FPaths::DirectoryExists(Target))
	{
		OutError = FString::Printf(TEXT("%s already exists; it was left alone."), *Target);
		return false;
	}
	if (!CopyFolder(SkillFolder, Target))
	{
		OutError = FString::Printf(TEXT("Could not copy the skill to %s."), *Target);
		return false;
	}
	return true;
}

bool Delete(const FString& ProjectRoot, const FString& Name, FString& OutError)
{
	const FString Folder = FPaths::Combine(CanonicalDirectory(ProjectRoot), Name);
	if (!IsValidName(Name) || !FPaths::DirectoryExists(Folder))
	{
		OutError = FString::Printf(TEXT("There is no project skill named '%s'."), *Name);
		return false;
	}
	if (!IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists=*/false, /*Tree=*/true))
	{
		OutError = TEXT("Could not delete the skill folder.");
		return false;
	}
	SyncClaudeMirror(ProjectRoot);
	return true;
}

int32 ImportEpicSkills(const FString& ProjectRoot, TArray<FString>& OutNotes)
{
	// Blueprint skills are classes only once loaded; native and Python ones already are.
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Blueprints;
	Registry.GetAssetsByClass(UBlueprint::StaticClass()->GetClassPathName(), Blueprints, /*bSearchSubClasses=*/true);
	for (const FAssetData& Asset : Blueprints)
	{
		FString Parent;
		if (Asset.GetTagValue(FBlueprintTags::NativeParentClassPath, Parent) && Parent.Contains(TEXT("AgentSkill")))
		{
			Asset.GetAsset();
		}
	}
	int32 Written = 0;
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		const FString ClassName = Class->GetName();
		if (!Class->IsChildOf(UAgentSkill::StaticClass()) || Class == UAgentSkill::StaticClass()
			|| Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
			|| ClassName.StartsWith(TEXT("SKEL_")) || ClassName.StartsWith(TEXT("REINST_")))
		{
			continue;
		}
		const UAgentSkill* Skill = Class->GetDefaultObject<UAgentSkill>();
		const FStrProperty* DescriptionProperty = FindFProperty<FStrProperty>(Class, TEXT("Description"));
		const FString Description = DescriptionProperty ? DescriptionProperty->GetPropertyValue_InContainer(Skill) : FString();
		if (!Skill || Description.IsEmpty())
		{
			continue;
		}
		FString Base = ClassName;
		Base.RemoveFromEnd(TEXT("_C"));
		Base.RemoveFromEnd(TEXT("Skill"));
		const FString Name = (TEXT("epic-") + KebabCase(Base)).Left(64);
		const FString Folder = FPaths::Combine(CanonicalDirectory(ProjectRoot), Name);
		if (!IsValidName(Name) || FPaths::DirectoryExists(Folder))
		{
			OutNotes.Add(FString::Printf(TEXT("%s: skipped, a project skill named %s exists."), *ClassName, *Name));
			continue;
		}
		const FString Text = FString::Printf(TEXT("---\nname: %s\ndescription: %s\n---\n\n%s\n\nConverted from Epic's AgentSkill %s.\n"),
			*Name, *Description.Replace(TEXT("\n"), TEXT(" ")), *Skill->GetDetails().Instructions, *Class->GetPathName());
		if (FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Folder, TEXT("SKILL.md")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			++Written;
		}
	}
	return Written;
}

TArray<FString> ToolSequence(const FString& TranscriptText)
{
	// Claude records calls as JSON and Codex as JavaScript inside JSON strings; unescaped, both read
	// toolset_name ... "X", tool_name ... "Y".
	const FString Text = TranscriptText.Replace(TEXT("\\\""), TEXT("\""));
	const FRegexPattern Pattern(TEXT("toolset_name\"?\\s*:\\s*\"([^\"]+)\"\\s*,\\s*\"?tool_name\"?\\s*:\\s*\"([^\"]+)\""));
	FRegexMatcher Matcher(Pattern, Text);
	TArray<FString> Sequence;
	while (Matcher.FindNext())
	{
		FString Toolset = Matcher.GetCaptureGroup(1);
		Toolset.Split(TEXT("."), nullptr, &Toolset, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		const FString Signature = Toolset + TEXT(".") + Matcher.GetCaptureGroup(2);
		if (Sequence.IsEmpty() || Sequence.Last() != Signature)
		{
			Sequence.Add(Signature);
		}
	}
	return Sequence;
}

TArray<FString> RepeatedWorkflows(const TArray<TArray<FString>>& Sessions, const int32 MinSessions)
{
	TMap<FString, int32> Counts;
	for (const TArray<FString>& Sequence : Sessions)
	{
		TSet<FString> Seen;
		for (int32 Index = 0; Index + 2 < Sequence.Num(); ++Index)
		{
			if (IsChange(Sequence[Index]) || IsChange(Sequence[Index + 1]) || IsChange(Sequence[Index + 2]))
			{
				Seen.Add(Sequence[Index] + TEXT(" -> ") + Sequence[Index + 1] + TEXT(" -> ") + Sequence[Index + 2]);
			}
		}
		for (const FString& Run : Seen)
		{
			++Counts.FindOrAdd(Run);
		}
	}
	TArray<TPair<FString, int32>> Found;
	for (const TPair<FString, int32>& Pair : Counts)
	{
		if (Pair.Value >= MinSessions)
		{
			Found.Add(Pair);
		}
	}
	Found.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B)
	{
		return A.Value != B.Value ? A.Value > B.Value : A.Key < B.Key;
	});
	TArray<FString> Runs;
	for (const TPair<FString, int32>& Pair : Found)
	{
		Runs.Add(Pair.Key);
	}
	return Runs;
}
}
