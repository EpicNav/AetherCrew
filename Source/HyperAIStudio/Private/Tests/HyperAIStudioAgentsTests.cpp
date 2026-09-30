// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioAgents.h"
#include "HyperAIStudioSkills.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::AgentsTests
{
	struct FScratchRoot
	{
		FString Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("AgentsTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		~FScratchRoot() { IFileManager::Get().DeleteDirectory(*Path, /*RequireExists=*/false, /*Tree=*/true); }
	};

	FString Read(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	void Write(const FString& Path, const FString& Text)
	{
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FHyperAIStudioAgentDefinition DoorMaker()
	{
		FHyperAIStudioAgentDefinition Agent;
		Agent.Name = TEXT("door-maker");
		Agent.Description = TEXT("Makes doors: \"fast\" \\ safe");
		Agent.Tools = TEXT("read");
		Agent.ModelClaude = TEXT("sonnet");
		Agent.ModelCodex = TEXT("gpt-5.5");
		Agent.Skills = {TEXT("unreal-blueprint"), TEXT("hyperai-memory")};
		// Split so the hex escape stops at 01.
		Agent.Instructions = TEXT("Line one.\n\tTabbed \"quote\" \\ back\x01") TEXT("end");
		return Agent;
	}

	/** A copy: callers pass List()'s temporary result straight in. */
	TOptional<FHyperAIStudioAgentDefinition> Find(const TArray<FHyperAIStudioAgentDefinition>& Agents, const FString& Name)
	{
		const FHyperAIStudioAgentDefinition* Found = Agents.FindByPredicate([&Name](const FHyperAIStudioAgentDefinition& Agent) { return Agent.Name == Name; });
		return Found ? TOptional<FHyperAIStudioAgentDefinition>(*Found) : TOptional<FHyperAIStudioAgentDefinition>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentsFormatTest,
	"HyperAIStudio.Agents.Format",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentsFormatTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Agents;
	using namespace HyperAIStudio::AgentsTests;
	FHyperAIStudioAgentDefinition Agent;
	FString Error;
	TestTrue(TEXT("A definition with a quoted description, a YAML skill list and a marker parses"), Parse(TEXT(
		"---\nname: door-maker\ndescription: \"Makes doors: \\\"fast\\\"\"\ntools: read\nmodel_claude: opus\nskills:\n  - unreal-blueprint\n  - \"hyperai-memory\"\n---\n"
		"Body text.\n<!-- hyperai-builtin source=plugin hash=abc -->\n"), Agent, Error));
	TestEqual(TEXT("Name"), Agent.Name, FString(TEXT("door-maker")));
	TestEqual(TEXT("Escaped quotes and colons survive"), Agent.Description, FString(TEXT("Makes doors: \"fast\"")));
	TestEqual(TEXT("Tools"), Agent.Tools, FString(TEXT("read")));
	TestEqual(TEXT("Claude model"), Agent.ModelClaude, FString(TEXT("opus")));
	TestTrue(TEXT("Skill list"), Agent.Skills == TArray<FString>{TEXT("unreal-blueprint"), TEXT("hyperai-memory")});
	TestEqual(TEXT("Marker lines are not instructions"), Agent.Instructions, FString(TEXT("Body text.")));
	TestTrue(TEXT("Inline skill lists parse too"),
		Parse(TEXT("---\nname: a\ndescription: b\nskills: [one, two]\n---\nDo.\n"), Agent, Error) && Agent.Skills.Num() == 2 && Agent.Tools == TEXT("edit"));
	TestFalse(TEXT("Capitals are not a name"), Parse(TEXT("---\nname: Door\ndescription: b\n---\nDo.\n"), Agent, Error));
	TestFalse(TEXT("A description is required"), Parse(TEXT("---\nname: door\n---\nDo.\n"), Agent, Error));
	TestFalse(TEXT("tools is read or edit"), Parse(TEXT("---\nname: door\ndescription: b\ntools: write\n---\nDo.\n"), Agent, Error));
	TestFalse(TEXT("Instructions are required"), Parse(TEXT("---\nname: door\ndescription: b\n---\n\n"), Agent, Error));
	TestFalse(TEXT("A model name cannot carry shell characters"), Parse(TEXT("---\nname: door\ndescription: b\nmodel_codex: x;rm\n---\nDo.\n"), Agent, Error));

	const FHyperAIStudioAgentDefinition Door = DoorMaker();
	FHyperAIStudioAgentDefinition RoundTrip;
	TestTrue(TEXT("Format parses back"), Parse(Format(Door), RoundTrip, Error));
	TestTrue(TEXT("Format round-trips every field"), RoundTrip.Name == Door.Name && RoundTrip.Description == Door.Description
		&& RoundTrip.Tools == Door.Tools && RoundTrip.ModelClaude == Door.ModelClaude && RoundTrip.ModelCodex == Door.ModelCodex
		&& RoundTrip.Skills == Door.Skills && RoundTrip.Instructions == Door.Instructions);

	TestEqual(TEXT("Claude Code subagent file"), ToClaude(Door), FString(TEXT(
		"---\n"
		"name: door-maker\n"
		"description: \"Makes doors: \\\"fast\\\" \\\\ safe\"\n"
		"model: sonnet\n"
		"disallowedTools: Bash, Edit, NotebookEdit, Write\n"
		"skills:\n"
		"  - unreal-blueprint\n"
		"  - hyperai-memory\n"
		"---\n"
		"\n"
		"Line one.\n\tTabbed \"quote\" \\ back\x01") TEXT("end\n")));
	TestEqual(TEXT("Codex agent file, TOML-escaped"), ToCodex(Door), FString(TEXT(
		"name = \"door-maker\"\n"
		"description = \"Makes doors: \\\"fast\\\" \\\\ safe\"\n"
		"model = \"gpt-5.5\"\n"
		"sandbox_mode = \"read-only\"\n"
		"developer_instructions = \"Line one.\\n\\tTabbed \\\"quote\\\" \\\\ back\\u0001end\\n\\nSkills for this work: $unreal-blueprint, $hyperai-memory.\"\n")));
	FHyperAIStudioAgentDefinition Editor = Door;
	Editor.Tools = TEXT("edit");
	Editor.ModelClaude.Reset();
	Editor.ModelCodex.Reset();
	Editor.Skills.Reset();
	TestFalse(TEXT("An edit agent inherits Claude Code's tools"), ToClaude(Editor).Contains(TEXT("disallowedTools")));
	TestFalse(TEXT("An edit agent keeps the chat's Codex sandbox"), ToCodex(Editor).Contains(TEXT("sandbox_mode")));
	TestFalse(TEXT("No model line when none is set"), ToClaude(Editor).Contains(TEXT("model:")) || ToCodex(Editor).Contains(TEXT("model =")));

	const FString Marked = WithMarker(TEXT("a\nb"), TEXT("hyperai-generated"), TEXT("src.md"), TEXT("<!--"), TEXT(" -->"));
	TestTrue(TEXT("The marker is the last line"), Marked.StartsWith(TEXT("a\nb\n<!-- hyperai-generated source=src.md hash=")) && Marked.EndsWith(TEXT(" -->\n")));
	TestEqual(TEXT("An untouched file is clean"), MarkerState(Marked, TEXT("hyperai-generated")), FString(TEXT("clean")));
	TestEqual(TEXT("Git's CRLF checkout still reads as clean"), MarkerState(Marked.Replace(TEXT("\n"), TEXT("\r\n")), TEXT("hyperai-generated")), FString(TEXT("clean")));
	TestEqual(TEXT("A changed file is edited"), MarkerState(Marked.Replace(TEXT("b\n"), TEXT("c\n")), TEXT("hyperai-generated")), FString(TEXT("edited")));
	TestEqual(TEXT("A file without the marker is foreign"), MarkerState(TEXT("a\nb\n"), TEXT("hyperai-generated")), FString(TEXT("foreign")));
	TestEqual(TEXT("Another tag's marker is foreign"), MarkerState(Marked, TEXT("hyperai-builtin")), FString(TEXT("foreign")));
	TestEqual(TEXT("TOML control characters use \\u"), TomlString(TEXT("a\x7F")), FString(TEXT("\"a\\u007F\"")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentsSyncTest,
	"HyperAIStudio.Agents.Sync",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentsSyncTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Agents;
	using namespace HyperAIStudio::AgentsTests;
	const FScratchRoot Root;
	const FString ClaudeDoor = FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("door-maker.md"));
	const FString CodexDoor = FPaths::Combine(CodexDirectory(Root.Path), TEXT("door-maker.toml"));
	FString Error;

	FHyperAIStudioAgentDefinition Door = DoorMaker();
	TestTrue(TEXT("Save writes the definition"), Save(Root.Path, Door, FString(), Error));
	TestTrue(TEXT("and both agents' files"), FPaths::FileExists(ClaudeDoor) && FPaths::FileExists(CodexDoor));
	TestTrue(TEXT("Claude's file is the generated text plus a marker"), Read(ClaudeDoor).StartsWith(ToClaude(Door)));
	TOptional<FHyperAIStudioAgentDefinition> Listed = Find(List(Root.Path), TEXT("door-maker"));
	TestTrue(TEXT("It lists as synced for both"), Listed && Listed->Scope == TEXT("project") && Listed->ClaudeState == TEXT("synced") && Listed->CodexState == TEXT("synced"));
	TestEqual(TEXT("A second sync changes nothing"), Sync(Root.Path).Changed, 0);
	TestFalse(TEXT("Saving a second agent under a taken name fails"), Save(Root.Path, Door, FString(), Error));

	// Edited in place: never overwritten, while the other file still follows the definition.
	Write(ClaudeDoor, Read(ClaudeDoor) + TEXT("My own line.\n"));
	Door.Description = TEXT("Makes better doors");
	TestTrue(TEXT("Save succeeds"), Save(Root.Path, Door, Door.Name, Error));
	TestTrue(TEXT("The edited Claude file keeps its edit"), Read(ClaudeDoor).Contains(TEXT("My own line.")) && !Read(ClaudeDoor).Contains(TEXT("better doors")));
	TestTrue(TEXT("The Codex file follows the definition"), Read(CodexDoor).Contains(TEXT("Makes better doors")));
	Listed = Find(List(Root.Path), TEXT("door-maker"));
	TestTrue(TEXT("The edit shows"), Listed && Listed->ClaudeState == TEXT("edited") && Listed->CodexState == TEXT("synced"));
	TestTrue(TEXT("Sync explains why it left it"), Sync(Root.Path).Notes.ContainsByPredicate([](const FString& Note) { return Note.Contains(TEXT("edited there")); }));

	// A Codex agent HyperAIStudio did not write keeps its name.
	const FString ForeignCodex = FPaths::Combine(CodexDirectory(Root.Path), TEXT("gate-keeper.toml"));
	Write(ForeignCodex, TEXT("name = \"gate-keeper\"\ndescription = \"mine\"\ndeveloper_instructions = \"x\"\n"));
	FHyperAIStudioAgentDefinition Gate;
	Gate.Name = TEXT("gate-keeper");
	Gate.Description = TEXT("Guards gates");
	Gate.Instructions = TEXT("Guard.");
	TestTrue(TEXT("Saving a clashing agent succeeds"), Save(Root.Path, Gate, FString(), Error));
	TestEqual(TEXT("The foreign Codex file is untouched"), Read(ForeignCodex), FString(TEXT("name = \"gate-keeper\"\ndescription = \"mine\"\ndeveloper_instructions = \"x\"\n")));
	Listed = Find(List(Root.Path), TEXT("gate-keeper"));
	TestTrue(TEXT("and shows as taken"), Listed && Listed->CodexState == TEXT("taken") && Listed->ClaudeState == TEXT("synced"));

	// Renaming moves the definition; the old name's clean files go, foreign ones stay.
	FHyperAIStudioAgentDefinition Guard = Gate;
	Guard.Name = TEXT("gate-guard");
	TestTrue(TEXT("Rename"), Save(Root.Path, Guard, TEXT("gate-keeper"), Error));
	TestFalse(TEXT("The old definition is gone"), FPaths::FileExists(FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("gate-keeper.md"))));
	TestFalse(TEXT("with its generated Claude file"), FPaths::FileExists(FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("gate-keeper.md"))));
	TestTrue(TEXT("but not the foreign Codex file"), FPaths::FileExists(ForeignCodex));
	TestTrue(TEXT("The new name is written out"), FPaths::FileExists(FPaths::Combine(CodexDirectory(Root.Path), TEXT("gate-guard.toml"))));

	// Deleting removes clean generated files and keeps edited ones.
	TestTrue(TEXT("Delete"), Delete(Root.Path, TEXT("door-maker"), Error));
	TestFalse(TEXT("The clean Codex file goes"), FPaths::FileExists(CodexDoor));
	TestTrue(TEXT("The edited Claude file stays"), FPaths::FileExists(ClaudeDoor));
	TestFalse(TEXT("Deleting an unknown agent fails"), Delete(Root.Path, TEXT("nobody"), Error));

	// A Claude-only subagent can be shared with Codex.
	Write(FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("scout.md")),
		TEXT("---\nname: scout\ndescription: Looks around\ntools: Read, Grep, Glob\nmodel: haiku\n---\n\nLook, don't touch.\n"));
	Listed = Find(List(Root.Path), TEXT("scout"));
	TestTrue(TEXT("A Claude-only subagent lists as such"), Listed && Listed->Scope == TEXT("claude") && Listed->Description == TEXT("Looks around"));
	TestTrue(TEXT("Promote"), Promote(Root.Path, TEXT("scout"), Error));
	FHyperAIStudioAgentDefinition Scout;
	TestTrue(TEXT("It is a project definition now"), Parse(Read(FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("scout.md"))), Scout, Error));
	TestTrue(TEXT("An allow-list without editing tools stays read only, with its model"), Scout.Tools == TEXT("read") && Scout.ModelClaude == TEXT("haiku")
		&& Scout.Instructions == TEXT("Look, don't touch."));
	TestEqual(TEXT("Claude's file is now HyperAIStudio's"), MarkerState(Read(FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("scout.md"))), TEXT("hyperai-generated")), FString(TEXT("clean")));
	TestTrue(TEXT("Codex gets it read only"), Read(FPaths::Combine(CodexDirectory(Root.Path), TEXT("scout.toml"))).Contains(TEXT("sandbox_mode = \"read-only\"")));
	const FString Painter = FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("painter.md"));
	Write(Painter, TEXT("---\nname: painter\ndescription: Paints\ncolor: blue\n---\n\nPaint.\n"));
	TestFalse(TEXT("Fields Codex cannot carry stop Promote"), Promote(Root.Path, TEXT("painter"), Error));
	TestTrue(TEXT("and say which"), Error.Contains(TEXT("color")));
	TestTrue(TEXT("and leave the file alone"), Read(Painter).Contains(TEXT("color: blue")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentsTeamTest,
	"HyperAIStudio.Agents.Team",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentsTeamTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Agents;
	using namespace HyperAIStudio::AgentsTests;
	const FScratchRoot Root;
	TestFalse(TEXT("The plugin ships the team"), TeamDirectory().IsEmpty());
	TestEqual(TEXT("Update-only adds nothing to a project without the team"), InstallTeam(Root.Path, /*bUpdateOnly=*/true).Changed, 0);
	TestFalse(TEXT("No agents folder appears"), FPaths::DirectoryExists(CanonicalDirectory(Root.Path)));

	const HyperAIStudio::Skills::FSyncReport Report = InstallTeam(Root.Path, /*bUpdateOnly=*/false);
	TestTrue(TEXT("Installing writes files"), Report.Changed > 0);
	TestTrue(TEXT("without complaint"), Report.Notes.IsEmpty());
	const TArray<FHyperAIStudioAgentDefinition> Team = List(Root.Path);
	for (const TCHAR* Name : {TEXT("director"), TEXT("programmer"), TEXT("level-designer"), TEXT("lighting-artist"), TEXT("technical-artist"),
		TEXT("ui-designer"), TEXT("cinematographer"), TEXT("qa-tester")})
	{
		const TOptional<FHyperAIStudioAgentDefinition> Member = Find(Team, Name);
		TestTrue(FString::Printf(TEXT("%s is installed, built in and written for both agents"), Name),
			Member && Member->bBuiltIn && Member->ClaudeState == TEXT("synced") && Member->CodexState == TEXT("synced"));
		TestTrue(FString::Printf(TEXT("%s carries a quality bar or judges by one"), Name), Member && Member->Instructions.Contains(TEXT("quality bar")));
	}
	const TOptional<FHyperAIStudioAgentDefinition> Tester = Find(Team, TEXT("qa-tester"));
	TestTrue(TEXT("The QA tester only reads"), Tester && Tester->Tools == TEXT("read"));
	TestTrue(TEXT("The team is installed"), IsTeamInstalled(Root.Path));

	const FString Workflow = FPaths::Combine(WorkflowDirectory(Root.Path), TEXT("hyperai-team.js"));
	const FString WorkflowText = Read(Workflow);
	TestTrue(TEXT("The team workflow begins with its meta"), WorkflowText.StartsWith(TEXT("export const meta = {")));
	TestEqual(TEXT("and ends with HyperAIStudio's marker"), MarkerState(WorkflowText, TEXT("hyperai-generated")), FString(TEXT("clean")));
	TestTrue(TEXT("It runs the team's roles"), WorkflowText.Contains(TEXT("agentType: 'director'")) && WorkflowText.Contains(TEXT("agentType: 'qa-tester'"))
		&& WorkflowText.Contains(TEXT("'lighting-artist'")));
	FString SkillName;
	FString SkillDescription;
	bool bMenuOnly = false;
	TestTrue(TEXT("The team skill is installed for Codex"), HyperAIStudio::Skills::ParseSkill(
		Read(FPaths::Combine(HyperAIStudio::Skills::CanonicalDirectory(Root.Path), TEXT("team"), TEXT("SKILL.md"))), SkillName, SkillDescription, bMenuOnly));
	TestTrue(TEXT("and mirrored for Claude Code"), FPaths::FileExists(FPaths::Combine(HyperAIStudio::Skills::ClaudeDirectory(Root.Path), TEXT("team"), TEXT("SKILL.md"))));
	TestEqual(TEXT("Claude Code asks for the team with its skill"), TeamRequest(TEXT("Claude Code"), TEXT("a crypt")), FString(TEXT("/team a crypt")));
	TestEqual(TEXT("so does Codex"), TeamRequest(TEXT("Codex"), TEXT("a crypt")), FString(TEXT("$team a crypt")));
	TestTrue(TEXT("and another agent reads the skill's file"), TeamRequest(TEXT("Gemini"), TEXT("a crypt")).StartsWith(TEXT("Follow the team skill in .agents/skills/team/SKILL.md")));

	const FString VerifyText = Read(FPaths::Combine(WorkflowDirectory(Root.Path), TEXT("hyperai-verify.js")));
	TestTrue(TEXT("The verify workflow begins with its meta"), VerifyText.StartsWith(TEXT("export const meta = {")));
	TestTrue(TEXT("Its QA testers plan, write, run and judge cases"), VerifyText.Contains(TEXT("agentType: 'qa-tester'")) && VerifyText.Contains(TEXT("hyper_verify_run")));
	TestTrue(TEXT("The verify skill is installed for Codex"), HyperAIStudio::Skills::ParseSkill(
		Read(FPaths::Combine(HyperAIStudio::Skills::CanonicalDirectory(Root.Path), TEXT("verify"), TEXT("SKILL.md"))), SkillName, SkillDescription, bMenuOnly));
	const FString MatchText = Read(FPaths::Combine(WorkflowDirectory(Root.Path), TEXT("hyperai-match.js")));
	TestTrue(TEXT("The match workflow begins with its meta"), MatchText.StartsWith(TEXT("export const meta = {")));
	TestTrue(TEXT("Its builder loops on hyper_image_compare and the director judges"), MatchText.Contains(TEXT("hyper_image_compare"))
		&& MatchText.Contains(TEXT("agentType: 'director'")) && MatchText.Contains(TEXT("image-match skill")));

	// The scripts must parse the way the workflow runtime runs them: the body inside an async function.
	int32 NodeCode = -1;
	FString NodeOut;
	FString NodeErr;
	FPlatformProcess::ExecProcess(TEXT("node"), TEXT("--version"), &NodeCode, &NodeOut, &NodeErr);
	if (NodeCode != 0)
	{
		AddWarning(TEXT("node is not on PATH, so the workflows' JavaScript syntax was not checked."));
	}
	else
	{
		for (const FString& Script : {WorkflowText, VerifyText, MatchText})
		{
			const FString Wrapped = FPaths::Combine(Root.Path, TEXT("workflow-check.js"));
			Write(Wrapped, TEXT("(async () => {\n") + Script.Replace(TEXT("export const meta"), TEXT("const meta")) + TEXT("\n})\n"));
			FPlatformProcess::ExecProcess(TEXT("node"), *FString::Printf(TEXT("--check \"%s\""), *Wrapped), &NodeCode, &NodeOut, &NodeErr);
			TestEqual(TEXT("A workflow's JavaScript parses"), NodeCode, 0);
			if (NodeCode != 0)
			{
				AddError(NodeErr);
			}
		}
	}

	// An update keeps a team member the project edited, and the others unchanged.
	const FString Director = FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("director.md"));
	Write(Director, Read(Director).Replace(TEXT("you do not build."), TEXT("you do not build. Be brief.")));
	InstallTeam(Root.Path, /*bUpdateOnly=*/true);
	TestTrue(TEXT("The edited member keeps its edit"), Read(Director).Contains(TEXT("Be brief.")));
	const TOptional<FHyperAIStudioAgentDefinition> Edited = Find(List(Root.Path), TEXT("director"));
	TestTrue(TEXT("and is no longer marked built in, while its files follow the edit"), Edited && !Edited->bBuiltIn && Edited->ClaudeState == TEXT("synced"));
	TestTrue(TEXT("Claude's director carries the edit"), Read(FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("director.md"))).Contains(TEXT("Be brief.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioAgentsTeamMigrationTest,
	"HyperAIStudio.Agents.TeamMigration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioAgentsTeamMigrationTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Agents;
	using namespace HyperAIStudio::AgentsTests;
	const FScratchRoot Root;
	// A project with the old gameplay team, as the plugin installed it: one member untouched, one edited there.
	const auto OldMember = [](const TCHAR* Name)
	{
		return WithMarker(FString::Printf(TEXT("---\nname: %s\ndescription: The old team's %s.\ntools: read\n---\nYou did this job.\n"), Name, Name),
			TEXT("hyperai-builtin"), TEXT("plugin"), TEXT("<!--"), TEXT(" -->"));
	};
	const FString Untouched = FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("playtester.md"));
	const FString Edited = FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("reviewer.md"));
	Write(Untouched, OldMember(TEXT("playtester")));
	Write(Edited, OldMember(TEXT("reviewer")).Replace(TEXT("You did this job."), TEXT("You did this job, our way.")));
	Sync(Root.Path);
	const FString UntouchedCopy = FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("playtester.md"));
	TestTrue(TEXT("The old member was written for Claude Code"), FPaths::FileExists(UntouchedCopy));

	// The update-only refresh at startup moves it to the new team.
	const HyperAIStudio::Skills::FSyncReport Report = InstallTeam(Root.Path, /*bUpdateOnly=*/true);
	TestTrue(TEXT("A project with the old team gets the new one"), IsTeamInstalled(Root.Path));
	TestFalse(TEXT("An untouched old member goes"), FPaths::FileExists(Untouched));
	TestFalse(TEXT("and so do its generated copies"), FPaths::FileExists(UntouchedCopy));
	TestTrue(TEXT("An edited old member stays"), Read(Edited).Contains(TEXT("our way")));
	TestTrue(TEXT("and the report says why"), Report.Notes.ContainsByPredicate([](const FString& Note) { return Note.StartsWith(TEXT("reviewer:")); }));
	return true;
}

#endif
