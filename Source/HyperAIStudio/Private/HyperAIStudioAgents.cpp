// Games by Hyper 2026.

#include "HyperAIStudioAgents.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace HyperAIStudio::Agents
{
namespace
{
	const TCHAR* GeneratedTag = TEXT("hyperai-generated");
	const TCHAR* BuiltInTag = TEXT("hyperai-builtin");
	/**
	 * What the team once installed under names it no longer ships: the gameplay team's members became the director
	 * (gameplay-architect and reviewer), the programmer, the technical artist, the level designer and the QA tester.
	 */
	const TCHAR* const RetiredAgents[] = {TEXT("gameplay-architect"), TEXT("blueprint-builder"), TEXT("asset-builder"), TEXT("level-builder"),
		TEXT("playtester"), TEXT("reviewer")};
	const TCHAR* const RetiredWorkflows[] = {TEXT("hyperai-build-system.js")};
	const TCHAR* const RetiredSkills[] = {TEXT("build-system")};
	/** A read agent may inspect files and call Unreal MCP tools, but not write files or run commands. */
	const TCHAR* ReadOnlyDenied = TEXT("Bash, Edit, NotebookEdit, Write");

	using FFields = TArray<TPair<FString, FString>>;

	FString Normalize(const FString& Text)
	{
		return Text.Replace(TEXT("\r\n"), TEXT("\n"));
	}

	FString Sha1(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
		return BytesToHex(Digest, FSHA1::DigestSize).ToLower();
	}

	bool Load(const FString& Path, FString& Out)
	{
		return FFileHelper::LoadFileToString(Out, *Path);
	}

	bool WriteFile(const FString& Path, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** Files ending in Extension directly in Directory, sorted. */
	TArray<FString> FilesIn(const FString& Directory, const TCHAR* Extension)
	{
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Directory, FString(TEXT("*")) + Extension), /*Files=*/true, /*Directories=*/false);
		Names.Sort();
		TArray<FString> Paths;
		for (const FString& Name : Names)
		{
			Paths.Add(FPaths::Combine(Directory, Name));
		}
		return Paths;
	}

	FString SourceOf(const FString& Name)
	{
		return FString::Printf(TEXT(".hyperai/agents/%s.md"), *Name);
	}

	FString Unquote(FString Value)
	{
		Value.TrimStartAndEndInline();
		if (Value.Len() >= 2 && Value.StartsWith(TEXT("'")) && Value.EndsWith(TEXT("'")))
		{
			return Value.Mid(1, Value.Len() - 2).Replace(TEXT("''"), TEXT("'"));
		}
		if (Value.Len() < 2 || !Value.StartsWith(TEXT("\"")) || !Value.EndsWith(TEXT("\"")))
		{
			return Value;
		}
		FString Out;
		for (int32 Index = 1; Index < Value.Len() - 1; ++Index)
		{
			if (Value[Index] == TEXT('\\') && Index + 1 < Value.Len() - 1)
			{
				const TCHAR Next = Value[++Index];
				Out.AppendChar(Next == TEXT('n') ? TEXT('\n') : Next == TEXT('t') ? TEXT('\t') : Next);
			}
			else
			{
				Out.AppendChar(Value[Index]);
			}
		}
		return Out;
	}

	/** Always double-quoted, so a description with a colon or a leading symbol stays one YAML string. */
	FString YamlString(const FString& Value)
	{
		FString Out = TEXT("\"");
		for (const TCHAR C : Value)
		{
			if (C == TEXT('\\') || C == TEXT('"'))
			{
				Out.AppendChar(TEXT('\\'));
				Out.AppendChar(C);
			}
			else if (C == TEXT('\n') || C == TEXT('\r') || C == TEXT('\t'))
			{
				Out.AppendChar(TEXT(' '));
			}
			else if (C >= 0x20)
			{
				Out.AppendChar(C);
			}
		}
		return Out + TEXT("\"");
	}

	TArray<FString> SplitList(FString Value)
	{
		Value.TrimStartAndEndInline();
		if (Value.StartsWith(TEXT("[")) && Value.EndsWith(TEXT("]")))
		{
			Value = Value.Mid(1, Value.Len() - 2);
		}
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/true);
		TArray<FString> Items;
		for (const FString& Part : Parts)
		{
			const FString Item = Unquote(Part);
			if (!Item.IsEmpty())
			{
				Items.Add(Item);
			}
		}
		return Items;
	}

	/** key: value lines between the first two --- lines, and the text after them. An indented "- item" list joins its key's value with commas. */
	bool ReadFrontmatter(const FString& Text, FFields& OutFields, FString& OutBody)
	{
		TArray<FString> Lines;
		Normalize(Text).ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
		if (Lines.Num() < 2 || Lines[0].TrimEnd() != TEXT("---"))
		{
			return false;
		}
		for (int32 Index = 1; Index < Lines.Num(); ++Index)
		{
			const FString& Line = Lines[Index];
			if (Line.TrimEnd() == TEXT("---"))
			{
				OutBody = FString::Join(TArrayView<const FString>(Lines).Slice(Index + 1, Lines.Num() - Index - 1), TEXT("\n"));
				return true;
			}
			const FString Trimmed = Line.TrimStart();
			const bool bIndented = Line.StartsWith(TEXT(" ")) || Line.StartsWith(TEXT("\t"));
			if (bIndented && Trimmed.StartsWith(TEXT("- ")) && !OutFields.IsEmpty())
			{
				FString& Value = OutFields.Last().Value;
				Value += (Value.IsEmpty() ? FString() : FString(TEXT(", "))) + Unquote(Trimmed.RightChop(2));
				continue;
			}
			FString Key;
			FString Value;
			if (!bIndented && !Trimmed.StartsWith(TEXT("#")) && Line.Split(TEXT(":"), &Key, &Value))
			{
				OutFields.Emplace(Key.TrimStartAndEnd(), Unquote(Value));
			}
		}
		return false;
	}

	FString Field(const FFields& Fields, const TCHAR* Key)
	{
		const TPair<FString, FString>* Found = Fields.FindByPredicate([Key](const TPair<FString, FString>& Pair) { return Pair.Key == Key; });
		return Found ? Found->Value : FString();
	}

	bool IsModelName(const FString& Model)
	{
		for (const TCHAR C : Model)
		{
			if (!(FChar::IsAlnum(C) && C < 128) && C != TEXT('.') && C != TEXT('-') && C != TEXT('_') && C != TEXT('[') && C != TEXT(']'))
			{
				return false;
			}
		}
		return Model.Len() <= 80;
	}

	bool Validate(const FHyperAIStudioAgentDefinition& Agent, FString& OutError)
	{
		if (!Skills::IsValidName(Agent.Name))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid agent name: use lowercase letters, digits and hyphens."), *Agent.Name);
			return false;
		}
		if (Agent.Description.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("It needs a description; agents are chosen by it.");
			return false;
		}
		if (Agent.Tools != TEXT("read") && Agent.Tools != TEXT("edit"))
		{
			OutError = TEXT("tools must be read or edit.");
			return false;
		}
		if (!IsModelName(Agent.ModelClaude) || !IsModelName(Agent.ModelCodex))
		{
			OutError = TEXT("A model name may use only letters, digits, dots, hyphens, underscores and brackets.");
			return false;
		}
		for (const FString& Skill : Agent.Skills)
		{
			if (!Skills::IsValidName(Skill))
			{
				OutError = FString::Printf(TEXT("'%s' is not a skill name."), *Skill);
				return false;
			}
		}
		if (Agent.Instructions.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("It needs instructions below the frontmatter.");
			return false;
		}
		return true;
	}

	/** Writes a generated file unless someone else's file has its name, or it was edited since it was written. */
	void WriteGenerated(const FString& Path, const FString& Text, const FString& Source, const TCHAR* CommentStart, const TCHAR* CommentEnd,
		const TCHAR* Owner, Skills::FSyncReport& Report)
	{
		const FString Wanted = WithMarker(Text, GeneratedTag, Source, CommentStart, CommentEnd);
		FString Existing;
		if (Load(Path, Existing))
		{
			const FString State = MarkerState(Existing, GeneratedTag);
			const FString Name = FPaths::GetBaseFilename(Path);
			if (State == TEXT("foreign"))
			{
				Report.Notes.Add(FString::Printf(TEXT("%s: %s already has a file of that name that HyperAIStudio did not write; it is left alone."), *Name, Owner));
				return;
			}
			if (State == TEXT("edited"))
			{
				Report.Notes.Add(FString::Printf(TEXT("%s: its %s file was edited there, so it is not regenerated. Edit %s instead, or delete that file."),
					*Name, Owner, *Source));
				return;
			}
			if (Normalize(Existing) == Wanted)
			{
				return;
			}
		}
		if (WriteFile(Path, Wanted))
		{
			++Report.Changed;
		}
	}

	FString GeneratedState(const FString& Path, const FString& Text, const FString& Source, const TCHAR* CommentStart, const TCHAR* CommentEnd)
	{
		FString Existing;
		if (!Load(Path, Existing))
		{
			return TEXT("missing");
		}
		const FString State = MarkerState(Existing, GeneratedTag);
		if (State != TEXT("clean"))
		{
			return State == TEXT("foreign") ? TEXT("taken") : TEXT("edited");
		}
		return Normalize(Existing) == WithMarker(Text, GeneratedTag, Source, CommentStart, CommentEnd) ? TEXT("synced") : TEXT("stale");
	}

	void Merge(Skills::FSyncReport& Into, const Skills::FSyncReport& From)
	{
		Into.Changed += From.Changed;
		Into.Notes.Append(From.Notes);
	}
}

FString CanonicalDirectory(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".hyperai"), TEXT("agents"));
}

FString ClaudeDirectory(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".claude"), TEXT("agents"));
}

FString CodexDirectory(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".codex"), TEXT("agents"));
}

FString WorkflowDirectory(const FString& ProjectRoot)
{
	return FPaths::Combine(ProjectRoot, TEXT(".claude"), TEXT("workflows"));
}

FString TeamDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	return Plugin.IsValid() ? FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("AgentTeam")) : FString();
}

bool Parse(const FString& Text, FHyperAIStudioAgentDefinition& OutAgent, FString& OutError)
{
	OutAgent = FHyperAIStudioAgentDefinition();
	FFields Fields;
	FString Body;
	if (!ReadFrontmatter(Text, Fields, Body))
	{
		OutError = TEXT("It needs a frontmatter block between --- lines.");
		return false;
	}
	OutAgent.Name = Field(Fields, TEXT("name"));
	OutAgent.Description = Field(Fields, TEXT("description"));
	OutAgent.Tools = Field(Fields, TEXT("tools")).ToLower();
	OutAgent.ModelClaude = Field(Fields, TEXT("model_claude"));
	OutAgent.ModelCodex = Field(Fields, TEXT("model_codex"));
	OutAgent.Skills = SplitList(Field(Fields, TEXT("skills")));
	if (OutAgent.Tools.IsEmpty())
	{
		OutAgent.Tools = TEXT("edit");
	}
	// Marker lines are HyperAIStudio's bookkeeping, not instructions.
	TArray<FString> Lines;
	Body.ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
	Lines.RemoveAll([](const FString& Line) { return Line.TrimStart().StartsWith(TEXT("<!-- hyperai-")); });
	OutAgent.Instructions = FString::Join(Lines, TEXT("\n")).TrimStartAndEnd();
	return Validate(OutAgent, OutError);
}

FString Format(const FHyperAIStudioAgentDefinition& Agent)
{
	FString Out = TEXT("---\n");
	Out += TEXT("name: ") + Agent.Name + TEXT("\n");
	Out += TEXT("description: ") + YamlString(Agent.Description) + TEXT("\n");
	Out += TEXT("tools: ") + Agent.Tools + TEXT("\n");
	if (!Agent.ModelClaude.IsEmpty())
	{
		Out += TEXT("model_claude: ") + Agent.ModelClaude + TEXT("\n");
	}
	if (!Agent.ModelCodex.IsEmpty())
	{
		Out += TEXT("model_codex: ") + Agent.ModelCodex + TEXT("\n");
	}
	if (!Agent.Skills.IsEmpty())
	{
		Out += TEXT("skills: ") + FString::Join(Agent.Skills, TEXT(", ")) + TEXT("\n");
	}
	return Out + TEXT("---\n") + Normalize(Agent.Instructions).TrimStartAndEnd() + TEXT("\n");
}

FString ToClaude(const FHyperAIStudioAgentDefinition& Agent)
{
	FString Out = TEXT("---\n");
	Out += TEXT("name: ") + Agent.Name + TEXT("\n");
	Out += TEXT("description: ") + YamlString(Agent.Description) + TEXT("\n");
	if (!Agent.ModelClaude.IsEmpty())
	{
		Out += TEXT("model: ") + Agent.ModelClaude + TEXT("\n");
	}
	if (Agent.Tools == TEXT("read"))
	{
		Out += TEXT("disallowedTools: ") + FString(ReadOnlyDenied) + TEXT("\n");
	}
	if (!Agent.Skills.IsEmpty())
	{
		Out += TEXT("skills:\n");
		for (const FString& Skill : Agent.Skills)
		{
			Out += TEXT("  - ") + Skill + TEXT("\n");
		}
	}
	return Out + TEXT("---\n\n") + Normalize(Agent.Instructions).TrimStartAndEnd() + TEXT("\n");
}

FString ToCodex(const FHyperAIStudioAgentDefinition& Agent)
{
	FString Instructions = Normalize(Agent.Instructions).TrimStartAndEnd();
	if (!Agent.Skills.IsEmpty())
	{
		TArray<FString> Invocations;
		for (const FString& Skill : Agent.Skills)
		{
			Invocations.Add(TEXT("$") + Skill);
		}
		Instructions += TEXT("\n\nSkills for this work: ") + FString::Join(Invocations, TEXT(", ")) + TEXT(".");
	}
	FString Out = TEXT("name = ") + TomlString(Agent.Name) + TEXT("\n");
	Out += TEXT("description = ") + TomlString(Agent.Description) + TEXT("\n");
	if (!Agent.ModelCodex.IsEmpty())
	{
		Out += TEXT("model = ") + TomlString(Agent.ModelCodex) + TEXT("\n");
	}
	if (Agent.Tools == TEXT("read"))
	{
		Out += TEXT("sandbox_mode = \"read-only\"\n");
	}
	return Out + TEXT("developer_instructions = ") + TomlString(Instructions) + TEXT("\n");
}

FString TomlString(const FString& Value)
{
	FString Out = TEXT("\"");
	for (const TCHAR C : Normalize(Value))
	{
		switch (C)
		{
		case TEXT('\\'): Out += TEXT("\\\\"); break;
		case TEXT('"'): Out += TEXT("\\\""); break;
		case TEXT('\n'): Out += TEXT("\\n"); break;
		case TEXT('\t'): Out += TEXT("\\t"); break;
		case TEXT('\r'): Out += TEXT("\\r"); break;
		default:
			if (C < 0x20 || C == 0x7F)
			{
				Out += FString::Printf(TEXT("\\u%04X"), static_cast<uint32>(C));
			}
			else
			{
				Out.AppendChar(C);
			}
		}
	}
	return Out + TEXT("\"");
}

FString WithMarker(const FString& Text, const FString& Tag, const FString& Source, const TCHAR* CommentStart, const TCHAR* CommentEnd)
{
	FString Body = Normalize(Text);
	if (!Body.EndsWith(TEXT("\n")))
	{
		Body += TEXT("\n");
	}
	return Body + FString::Printf(TEXT("%s %s source=%s hash=%s%s\n"), CommentStart, *Tag, *Source, *Sha1(Body), CommentEnd);
}

FString MarkerState(const FString& Text, const FString& Tag)
{
	const FString Body = Normalize(Text);
	const int32 At = Body.Find(TEXT(" ") + Tag + TEXT(" source="), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (At == INDEX_NONE)
	{
		return TEXT("foreign");
	}
	int32 LineStart = At;
	while (LineStart > 0 && Body[LineStart - 1] != TEXT('\n'))
	{
		--LineStart;
	}
	const int32 Newline = Body.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At);
	const int32 LineEnd = Newline == INDEX_NONE ? Body.Len() : Newline + 1;
	const FString Line = Body.Mid(LineStart, LineEnd - LineStart);
	const int32 HashAt = Line.Find(TEXT(" hash="));
	if (HashAt == INDEX_NONE)
	{
		return TEXT("foreign");
	}
	FString Recorded = Line.Mid(HashAt + 6);
	int32 End = 0;
	while (End < Recorded.Len() && FChar::IsHexDigit(Recorded[End]))
	{
		++End;
	}
	Recorded.LeftInline(End);
	// Everything but the marker line: text added after it counts as an edit too.
	return Sha1(Body.Left(LineStart) + Body.Mid(LineEnd)) == Recorded ? TEXT("clean") : TEXT("edited");
}

TArray<FHyperAIStudioAgentDefinition> List(const FString& ProjectRoot)
{
	TArray<FHyperAIStudioAgentDefinition> Agents;
	TSet<FString> Names;
	for (const FString& File : FilesIn(CanonicalDirectory(ProjectRoot), TEXT(".md")))
	{
		FString Text;
		FString Error;
		FHyperAIStudioAgentDefinition Agent;
		if (!Load(File, Text) || !Parse(Text, Agent, Error))
		{
			continue;
		}
		const FString Source = SourceOf(Agent.Name);
		Agent.File = FPaths::ConvertRelativePathToFull(File);
		Agent.Scope = TEXT("project");
		Agent.bBuiltIn = MarkerState(Text, BuiltInTag) == TEXT("clean");
		Agent.ClaudeState = GeneratedState(FPaths::Combine(ClaudeDirectory(ProjectRoot), Agent.Name + TEXT(".md")), ToClaude(Agent), Source, TEXT("<!--"), TEXT(" -->"));
		Agent.CodexState = GeneratedState(FPaths::Combine(CodexDirectory(ProjectRoot), Agent.Name + TEXT(".toml")), ToCodex(Agent), Source, TEXT("#"), TEXT(""));
		Names.Add(Agent.Name);
		Agents.Add(MoveTemp(Agent));
	}
	// Claude-only agents: in .claude/agents, not written by HyperAIStudio.
	for (const FString& File : FilesIn(ClaudeDirectory(ProjectRoot), TEXT(".md")))
	{
		FString Text;
		FFields Fields;
		FString Body;
		if (Names.Contains(FPaths::GetBaseFilename(File)) || !Load(File, Text) || MarkerState(Text, GeneratedTag) != TEXT("foreign")
			|| !ReadFrontmatter(Text, Fields, Body))
		{
			continue;
		}
		FHyperAIStudioAgentDefinition Agent;
		Agent.Name = FPaths::GetBaseFilename(File);
		Agent.Description = Field(Fields, TEXT("description"));
		Agent.File = FPaths::ConvertRelativePathToFull(File);
		Agent.Scope = TEXT("claude");
		Agents.Add(MoveTemp(Agent));
	}
	return Agents;
}

Skills::FSyncReport Sync(const FString& ProjectRoot)
{
	Skills::FSyncReport Report;
	TSet<FString> Names;
	for (const FString& File : FilesIn(CanonicalDirectory(ProjectRoot), TEXT(".md")))
	{
		FString Text;
		FString Error;
		FHyperAIStudioAgentDefinition Agent;
		if (!Load(File, Text) || !Parse(Text, Agent, Error))
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: %s"), *FPaths::GetCleanFilename(File), *Error));
			continue;
		}
		if (Agent.Name != FPaths::GetBaseFilename(File))
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: the file name must be its name, %s.md."), *FPaths::GetCleanFilename(File), *Agent.Name));
			continue;
		}
		Names.Add(Agent.Name);
		const FString Source = SourceOf(Agent.Name);
		WriteGenerated(FPaths::Combine(ClaudeDirectory(ProjectRoot), Agent.Name + TEXT(".md")), ToClaude(Agent), Source,
			TEXT("<!--"), TEXT(" -->"), TEXT("Claude Code"), Report);
		WriteGenerated(FPaths::Combine(CodexDirectory(ProjectRoot), Agent.Name + TEXT(".toml")), ToCodex(Agent), Source,
			TEXT("#"), TEXT(""), TEXT("Codex"), Report);
	}
	// Generated files whose definition is gone go too, unless someone edited them.
	auto Prune = [&Names, &Report](const FString& Directory, const TCHAR* Extension)
	{
		for (const FString& File : FilesIn(Directory, Extension))
		{
			FString Text;
			if (!Names.Contains(FPaths::GetBaseFilename(File)) && Load(File, Text) && Text.Contains(TEXT("source=.hyperai/agents/"))
				&& MarkerState(Text, GeneratedTag) == TEXT("clean") && IFileManager::Get().Delete(*File))
			{
				++Report.Changed;
			}
		}
	};
	Prune(ClaudeDirectory(ProjectRoot), TEXT(".md"));
	Prune(CodexDirectory(ProjectRoot), TEXT(".toml"));
	return Report;
}

Skills::FSyncReport InstallTeam(const FString& ProjectRoot, const bool bUpdateOnly)
{
	Skills::FSyncReport Report;
	const FString Team = TeamDirectory();
	const TArray<FString> Sources = FilesIn(FPaths::Combine(Team, TEXT("agents")), TEXT(".md"));
	// A project with the old team is a project with the team: it moves to the new one, even on an update.
	bool bMigrating = false;
	for (const TCHAR* Name : RetiredAgents)
	{
		bMigrating |= FPaths::FileExists(FPaths::Combine(CanonicalDirectory(ProjectRoot), FString(Name) + TEXT(".md")));
	}
	const bool bInstalled = bMigrating || Sources.ContainsByPredicate([&ProjectRoot](const FString& Source)
	{
		return FPaths::FileExists(FPaths::Combine(CanonicalDirectory(ProjectRoot), FPaths::GetCleanFilename(Source)));
	});
	if (Sources.IsEmpty() || (bUpdateOnly && !bInstalled))
	{
		return Report;
	}
	for (const FString& Source : Sources)
	{
		FString Text;
		if (!Load(Source, Text))
		{
			continue;
		}
		const FString Name = FPaths::GetBaseFilename(Source);
		const FString Target = FPaths::Combine(CanonicalDirectory(ProjectRoot), FPaths::GetCleanFilename(Source));
		const FString Wanted = WithMarker(Text, BuiltInTag, TEXT("plugin"), TEXT("<!--"), TEXT(" -->"));
		FString Existing;
		if (!Load(Target, Existing))
		{
			if ((!bUpdateOnly || bMigrating) && WriteFile(Target, Wanted))
			{
				++Report.Changed;
			}
			continue;
		}
		const FString State = MarkerState(Existing, BuiltInTag);
		if (State == TEXT("foreign"))
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: a project agent of that name already exists, so the team's was not installed."), *Name));
		}
		else if (State == TEXT("edited"))
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: edited in this project, so plugin updates to it are not applied."), *Name));
		}
		else if (Normalize(Existing) != Wanted && WriteFile(Target, Wanted))
		{
			++Report.Changed;
		}
	}
	// Members the team no longer has go, unless the project edited them; the roles they became are installed above.
	for (const TCHAR* Name : RetiredAgents)
	{
		const FString File = FPaths::Combine(CanonicalDirectory(ProjectRoot), FString(Name) + TEXT(".md"));
		FString Text;
		if (!Load(File, Text))
		{
			continue;
		}
		if (MarkerState(Text, BuiltInTag) != TEXT("clean"))
		{
			Report.Notes.Add(FString::Printf(TEXT("%s: no longer in the team, but it is this project's own or was edited here, so it stays."), Name));
		}
		else if (IFileManager::Get().Delete(*File))
		{
			++Report.Changed;
		}
	}
	for (const TCHAR* Name : RetiredWorkflows)
	{
		const FString File = FPaths::Combine(WorkflowDirectory(ProjectRoot), Name);
		FString Text;
		if (Load(File, Text) && MarkerState(Text, GeneratedTag) == TEXT("clean") && IFileManager::Get().Delete(*File))
		{
			++Report.Changed;
		}
	}
	for (const FHyperAIStudioSkill& Skill : Skills::ListProjectSkills(ProjectRoot))
	{
		FString Error;
		for (const TCHAR* Name : RetiredSkills)
		{
			if (Skill.Name == Name && Skill.bBuiltIn && Skills::Delete(ProjectRoot, Skill.Name, Error))
			{
				++Report.Changed;
			}
		}
	}
	for (const FString& Workflow : FilesIn(FPaths::Combine(Team, TEXT("workflows")), TEXT(".js")))
	{
		FString Text;
		if (Load(Workflow, Text))
		{
			// The marker goes last: a workflow script must begin with its meta.
			WriteGenerated(FPaths::Combine(WorkflowDirectory(ProjectRoot), FPaths::GetCleanFilename(Workflow)), Text, TEXT("plugin"),
				TEXT("//"), TEXT(""), TEXT("Claude Code"), Report);
		}
	}
	Merge(Report, Skills::InstallBuiltIns(ProjectRoot, FPaths::Combine(Team, TEXT("skills"))));
	Merge(Report, Skills::SyncClaudeMirror(ProjectRoot));
	Merge(Report, Sync(ProjectRoot));
	return Report;
}

bool IsTeamInstalled(const FString& ProjectRoot)
{
	for (const FString& Source : FilesIn(FPaths::Combine(TeamDirectory(), TEXT("agents")), TEXT(".md")))
	{
		if (!FPaths::FileExists(FPaths::Combine(CanonicalDirectory(ProjectRoot), FPaths::GetCleanFilename(Source))))
		{
			return false;
		}
	}
	return FPaths::FileExists(FPaths::Combine(Skills::CanonicalDirectory(ProjectRoot), TEXT("team"), TEXT("SKILL.md")));
}

FString TeamRequest(const FString& AgentName, const FString& Message)
{
	const FString Prefix = Skills::InvocationPrefix(AgentName);
	return Prefix.IsEmpty() ? TEXT("Follow the team skill in .agents/skills/team/SKILL.md for this request:\n\n") + Message
		: Prefix + TEXT("team ") + Message;
}

bool Save(const FString& ProjectRoot, const FHyperAIStudioAgentDefinition& Agent, const FString& PreviousName, FString& OutError)
{
	if (!Validate(Agent, OutError))
	{
		return false;
	}
	const FString Target = FPaths::Combine(CanonicalDirectory(ProjectRoot), Agent.Name + TEXT(".md"));
	if (Agent.Name != PreviousName && FPaths::FileExists(Target))
	{
		OutError = FString::Printf(TEXT("An agent named %s already exists."), *Agent.Name);
		return false;
	}
	if (!WriteFile(Target, Format(Agent)))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *Target);
		return false;
	}
	if (!PreviousName.IsEmpty() && PreviousName != Agent.Name)
	{
		IFileManager::Get().Delete(*FPaths::Combine(CanonicalDirectory(ProjectRoot), PreviousName + TEXT(".md")));
	}
	Sync(ProjectRoot);
	return true;
}

bool Delete(const FString& ProjectRoot, const FString& Name, FString& OutError)
{
	const FString Target = FPaths::Combine(CanonicalDirectory(ProjectRoot), Name + TEXT(".md"));
	if (!Skills::IsValidName(Name) || !FPaths::FileExists(Target))
	{
		OutError = FString::Printf(TEXT("There is no project agent named '%s'."), *Name);
		return false;
	}
	if (!IFileManager::Get().Delete(*Target))
	{
		OutError = FString::Printf(TEXT("Could not delete %s."), *Target);
		return false;
	}
	Sync(ProjectRoot);
	return true;
}

bool Promote(const FString& ProjectRoot, const FString& Name, FString& OutError)
{
	const FString File = FPaths::Combine(ClaudeDirectory(ProjectRoot), Name + TEXT(".md"));
	FString Text;
	FFields Fields;
	FString Body;
	if (!Skills::IsValidName(Name) || !Load(File, Text) || MarkerState(Text, GeneratedTag) != TEXT("foreign") || !ReadFrontmatter(Text, Fields, Body))
	{
		OutError = FString::Printf(TEXT("There is no Claude Code-only agent named '%s' in this project."), *Name);
		return false;
	}
	static const TSet<FString> Carried = {TEXT("name"), TEXT("description"), TEXT("model"), TEXT("skills"), TEXT("tools"), TEXT("disallowedTools")};
	TArray<FString> Dropped;
	for (const TPair<FString, FString>& Pair : Fields)
	{
		if (!Carried.Contains(Pair.Key))
		{
			Dropped.Add(Pair.Key);
		}
	}
	if (!Dropped.IsEmpty())
	{
		OutError = FString::Printf(TEXT("%s uses %s, which HyperAIStudio cannot carry to Codex. Remove those fields first, or keep this agent in Claude Code only."),
			*Name, *FString::Join(Dropped, TEXT(", ")));
		return false;
	}
	FHyperAIStudioAgentDefinition Agent;
	Agent.Name = Name;
	Agent.Description = Field(Fields, TEXT("description"));
	Agent.ModelClaude = Field(Fields, TEXT("model"));
	Agent.Skills = SplitList(Field(Fields, TEXT("skills")));
	Agent.Instructions = Body.TrimStartAndEnd();
	// An allow-list without editing tools, or a deny-list with them, keeps the agent read-only.
	const FString Allowed = Field(Fields, TEXT("tools"));
	const FString Denied = Field(Fields, TEXT("disallowedTools"));
	const bool bAllowsEdits = Allowed.IsEmpty() || Allowed.Contains(TEXT("Edit")) || Allowed.Contains(TEXT("Write")) || Allowed.Contains(TEXT("Bash"));
	Agent.Tools = bAllowsEdits && !Denied.Contains(TEXT("Edit")) && !Denied.Contains(TEXT("Write")) ? TEXT("edit") : TEXT("read");
	const FString Target = FPaths::Combine(CanonicalDirectory(ProjectRoot), Name + TEXT(".md"));
	if (FPaths::FileExists(Target))
	{
		OutError = FString::Printf(TEXT("A project agent named %s already exists."), *Name);
		return false;
	}
	if (!Validate(Agent, OutError))
	{
		return false;
	}
	if (!WriteFile(Target, Format(Agent)))
	{
		OutError = FString::Printf(TEXT("Could not write %s."), *Target);
		return false;
	}
	// Sync writes HyperAIStudio's version in its place, for both agents.
	IFileManager::Get().Delete(*File);
	Sync(ProjectRoot);
	return true;
}
}
