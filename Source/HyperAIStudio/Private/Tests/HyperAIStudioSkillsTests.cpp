// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HyperAIStudioSkills.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::SkillsTests
{
	struct FScratchRoot
	{
		FString Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("SkillsTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		~FScratchRoot() { IFileManager::Get().DeleteDirectory(*Path, /*RequireExists=*/false, /*Tree=*/true); }
	};

	void WriteSkill(const FString& Folder, const FString& Name, const FString& Description, const FString& Body = TEXT("Do the thing."))
	{
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("---\nname: %s\ndescription: %s\n---\n\n%s\n"), *Name, *Description, *Body),
			*FPaths::Combine(Folder, TEXT("SKILL.md")));
	}

	FString Read(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSkillsFormatTest,
	"HyperAIStudio.Skills.Format",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSkillsFormatTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Skills;
	FString Name;
	FString Description;
	bool bMenuOnly = true;
	TestTrue(TEXT("A skill parses"), ParseSkill(TEXT("---\nname: door-maker\ndescription: \"Makes doors: fast\"\n---\n\nBody"), Name, Description, bMenuOnly));
	TestTrue(TEXT("Quoted values lose their quotes; colons in them survive"), Name == TEXT("door-maker") && Description == TEXT("Makes doors: fast") && !bMenuOnly);
	TestTrue(TEXT("disable-model-invocation reads as menu only"),
		ParseSkill(TEXT("---\nname: a\ndescription: b\ndisable-model-invocation: true\n---\n"), Name, Description, bMenuOnly) && bMenuOnly);
	TestFalse(TEXT("A description is required"), ParseSkill(TEXT("---\nname: a\n---\n"), Name, Description, bMenuOnly));
	TestTrue(TEXT("Lowercase words and hyphens are a valid name"), IsValidName(TEXT("unreal-blueprint2")));
	TestFalse(TEXT("Capitals are not"), IsValidName(TEXT("Blueprint")));
	TestFalse(TEXT("Path characters are not"), IsValidName(TEXT("../evil")));
	TestEqual(TEXT("Claude Code runs skills with a slash"), InvocationPrefix(TEXT("Claude Code")), FString(TEXT("/")));
	TestEqual(TEXT("Codex runs them with a dollar sign"), InvocationPrefix(TEXT("Codex")), FString(TEXT("$")));
	TestTrue(TEXT("Other agents have none"), InvocationPrefix(TEXT("Gemini")).IsEmpty());

	// Claude writes JSON; Codex writes JavaScript inside JSON strings.
	const FString Claude = TEXT("{\"type\":\"tool_use\",\"input\":{\"toolset_name\":\"editor_toolset.toolsets.blueprint.BlueprintTools\",\"tool_name\":\"create\"}}\n")
		TEXT("{\"input\":{\"toolset_name\":\"editor_toolset.toolsets.blueprint.BlueprintTools\",\"tool_name\":\"create\"}}\n")
		TEXT("{\"input\":{\"toolset_name\": \"editor_toolset.toolsets.asset.AssetTools\", \"tool_name\": \"save_assets\"}}");
	TestEqual(TEXT("Claude calls read in order, repeats collapsed"), FString::Join(ToolSequence(Claude), TEXT(" ")),
		FString(TEXT("BlueprintTools.create AssetTools.save_assets")));
	const FString Codex = TEXT("{\"payload\":{\"input\":\"const r=await tools.mcp__unreal_mcp__call_tool({toolset_name:\\\"HyperAIStudioAnimation.HyperAIStudioAnimationToolset\\\",tool_name:\\\"hyper_animation_inspect\\\",arguments:{}});\"}}");
	TestEqual(TEXT("Codex calls read too"), FString::Join(ToolSequence(Codex), TEXT(" ")),
		FString(TEXT("HyperAIStudioAnimationToolset.hyper_animation_inspect")));

	const TArray<FString> Build = {TEXT("BlueprintTools.create"), TEXT("ActorTools.add_component"), TEXT("AssetTools.save_assets")};
	const TArray<FString> Reads = {TEXT("SceneTools.get_current_level"), TEXT("SceneTools.find_actors"), TEXT("ObjectTools.get_properties")};
	TArray<FString> Mixed = Reads;
	Mixed.Append(Build);
	const TArray<FString> Runs = RepeatedWorkflows({Build, Mixed, Build, Reads, Reads, Reads});
	TestTrue(TEXT("A build run in three chats is found"), Runs.Contains(TEXT("BlueprintTools.create -> ActorTools.add_component -> AssetTools.save_assets")));
	TestFalse(TEXT("A read-only run is not a workflow worth a skill"), Runs.Contains(TEXT("SceneTools.get_current_level -> SceneTools.find_actors -> ObjectTools.get_properties")));
	TestTrue(TEXT("Two chats are not enough"), RepeatedWorkflows({Build, Build}).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSkillsMirrorTest,
	"HyperAIStudio.Skills.Mirror",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSkillsMirrorTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Skills;
	using namespace HyperAIStudio::SkillsTests;
	const FScratchRoot Root;
	const FString Source = FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("door-maker"));
	const FString Mirror = FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("door-maker"));
	WriteSkill(Source, TEXT("door-maker"), TEXT("Makes doors."));
	FFileHelper::SaveStringToFile(TEXT("reference"), *FPaths::Combine(Source, TEXT("references"), TEXT("notes.md")));

	TestEqual(TEXT("A new project skill is copied for Claude Code"), SyncClaudeMirror(Root.Path).Changed, 1);
	TestTrue(TEXT("Every file is copied"), Read(FPaths::Combine(Mirror, TEXT("references"), TEXT("notes.md"))) == TEXT("reference"));
	TestEqual(TEXT("The copy matches its source, markers aside"), FolderHash(Mirror), FolderHash(Source));
	TestEqual(TEXT("Nothing to do when nothing changed"), SyncClaudeMirror(Root.Path).Changed, 0);

	WriteSkill(Source, TEXT("door-maker"), TEXT("Makes doors, version 2."));
	TestEqual(TEXT("A changed source refreshes its unedited copy"), SyncClaudeMirror(Root.Path).Changed, 1);
	TestTrue(TEXT("The copy has the new text"), Read(FPaths::Combine(Mirror, TEXT("SKILL.md"))).Contains(TEXT("version 2")));

	WriteSkill(Mirror, TEXT("door-maker"), TEXT("Edited inside .claude/skills."));
	WriteSkill(Source, TEXT("door-maker"), TEXT("Makes doors, version 3."));
	const FSyncReport Edited = SyncClaudeMirror(Root.Path);
	TestTrue(TEXT("A copy edited in place is left alone and reported"), Edited.Changed == 0 && Edited.Notes.Num() == 1
		&& Read(FPaths::Combine(Mirror, TEXT("SKILL.md"))).Contains(TEXT("Edited inside")));
	const TArray<FHyperAIStudioSkill> Listed = ListProjectSkills(Root.Path);
	TestTrue(TEXT("It shows as edited"), Listed.Num() == 1 && Listed[0].MirrorState == TEXT("edited"));
	FString Error;
	TestTrue(TEXT("Promote keeps the edits for both agents"), Promote(Root.Path, TEXT("door-maker"), Error)
		&& Read(FPaths::Combine(Source, TEXT("SKILL.md"))).Contains(TEXT("Edited inside"))
		&& ListProjectSkills(Root.Path)[0].MirrorState == TEXT("synced"));

	// A Claude skill HyperAIStudio did not write is never touched.
	const FString Own = FPaths::Combine(ClaudeDirectory(Root.Path), TEXT("claude-own"));
	WriteSkill(Own, TEXT("claude-own"), TEXT("Mine."));
	WriteSkill(FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("claude-own")), TEXT("claude-own"), TEXT("Project version."));
	SyncClaudeMirror(Root.Path);
	TestTrue(TEXT("A same-named Claude skill wins"), Read(FPaths::Combine(Own, TEXT("SKILL.md"))).Contains(TEXT("Mine.")));

	// Deleting a project skill removes its unedited copy, and only that.
	TestTrue(TEXT("Delete removes the skill"), Delete(Root.Path, TEXT("door-maker"), Error) && !FPaths::DirectoryExists(Source));
	TestFalse(TEXT("And its copy"), FPaths::DirectoryExists(Mirror));
	TestTrue(TEXT("But never a Claude skill it did not write"), FPaths::DirectoryExists(Own));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSkillsBuiltInTest,
	"HyperAIStudio.Skills.BuiltInAndMenuOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSkillsBuiltInTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Skills;
	using namespace HyperAIStudio::SkillsTests;
	const FScratchRoot Root;
	const FString Plugin = FPaths::Combine(Root.Path, TEXT("PluginSkills"));
	const FString Installed = FPaths::Combine(CanonicalDirectory(Root.Path), TEXT("helper"));
	WriteSkill(FPaths::Combine(Plugin, TEXT("helper")), TEXT("helper"), TEXT("Helps. v1."));

	TestEqual(TEXT("A built-in skill installs"), InstallBuiltIns(Root.Path, Plugin).Changed, 1);
	TestTrue(TEXT("It lists as built in"), ListProjectSkills(Root.Path).ContainsByPredicate([](const FHyperAIStudioSkill& Skill) { return Skill.bBuiltIn; }));
	WriteSkill(FPaths::Combine(Plugin, TEXT("helper")), TEXT("helper"), TEXT("Helps. v2."));
	TestEqual(TEXT("An unedited built-in follows a plugin update"), InstallBuiltIns(Root.Path, Plugin).Changed, 1);
	TestTrue(TEXT("To the new text"), Read(FPaths::Combine(Installed, TEXT("SKILL.md"))).Contains(TEXT("v2")));
	WriteSkill(Installed, TEXT("helper"), TEXT("Helps, the project's way."));
	WriteSkill(FPaths::Combine(Plugin, TEXT("helper")), TEXT("helper"), TEXT("Helps. v3."));
	const FSyncReport Edited = InstallBuiltIns(Root.Path, Plugin);
	TestTrue(TEXT("An edited built-in keeps the project's text"), Edited.Changed == 0 && Edited.Notes.Num() == 1
		&& Read(FPaths::Combine(Installed, TEXT("SKILL.md"))).Contains(TEXT("project's way")));

	FString Error;
	TestTrue(TEXT("Menu only can be switched on"), SetMenuOnly(Installed, true, Error));
	TestTrue(TEXT("Claude Code gets disable-model-invocation"), Read(FPaths::Combine(Installed, TEXT("SKILL.md"))).Contains(TEXT("disable-model-invocation: true")));
	TestTrue(TEXT("Codex gets its policy file"), Read(FPaths::Combine(Installed, TEXT("agents"), TEXT("openai.yaml"))).Contains(TEXT("allow_implicit_invocation: false")));
	TestTrue(TEXT("It lists as menu only"), ListProjectSkills(Root.Path)[0].bMenuOnly);
	TestTrue(TEXT("And back"), SetMenuOnly(Installed, false, Error));
	TestFalse(TEXT("The frontmatter line goes"), Read(FPaths::Combine(Installed, TEXT("SKILL.md"))).Contains(TEXT("disable-model-invocation")));
	TestFalse(TEXT("A policy file HyperAIStudio wrote goes"), FPaths::FileExists(FPaths::Combine(Installed, TEXT("agents"), TEXT("openai.yaml"))));

	FFileHelper::SaveStringToFile(TEXT("interface:\n  display_name: \"Helper\"\n"), *FPaths::Combine(Installed, TEXT("agents"), TEXT("openai.yaml")));
	TestTrue(TEXT("A user's policy file is extended, not replaced"), SetMenuOnly(Installed, true, Error)
		&& Read(FPaths::Combine(Installed, TEXT("agents"), TEXT("openai.yaml"))).Contains(TEXT("display_name")));
	TestTrue(TEXT("Switching off keeps the user's file"), SetMenuOnly(Installed, false, Error)
		&& FPaths::FileExists(FPaths::Combine(Installed, TEXT("agents"), TEXT("openai.yaml"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioSkillsShippedTest,
	"HyperAIStudio.Skills.ShippedSkillsParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioSkillsShippedTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Skills;
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(BuiltInDirectory() / TEXT("*")), /*Files=*/false, /*Directories=*/true);
	TestTrue(TEXT("The plugin ships its skills, image-match among them"), Folders.Contains(TEXT("image-match")) && Folders.Contains(TEXT("unreal-playtest")));
	for (const FString& Folder : Folders)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *(BuiltInDirectory() / Folder / TEXT("SKILL.md")));
		FString Name;
		FString Description;
		bool bMenuOnly = false;
		TestTrue(FString::Printf(TEXT("%s parses, is named for its folder, and describes itself within 1024 characters"), *Folder),
			ParseSkill(Text, Name, Description, bMenuOnly) && Name == Folder && !Description.IsEmpty() && Description.Len() <= 1024);
	}
	return true;
}

#endif
