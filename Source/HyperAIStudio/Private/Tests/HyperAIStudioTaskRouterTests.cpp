// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioAgents.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSkills.h"
#include "HyperAIStudioTaskRouter.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::TaskRouterTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString TopPack(const FString& Message, const TArray<TaskRouter::FPackDocument>& Documents)
	{
		const TArray<TaskRouter::FPackFit> Fits = TaskRouter::RankPacks(Message, Documents, 2);
		return Fits.IsEmpty() ? FString(TEXT("none")) : Fits[0].PackId;
	}

	FString RouteName(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes, const TArray<TaskRouter::FPackDocument>& Documents)
	{
		const int32 Index = TaskRouter::RankRoutes(Message, Routes, Documents);
		return Index == INDEX_NONE ? FString(TEXT("none")) : Routes[Index].Category;
	}

	TArray<FHyperAIStudioAgentTaskRoute> DefaultRoutes()
	{
		UHyperAIStudioSettings* Settings = NewObject<UHyperAIStudioSettings>();
		Settings->AgentTaskRoutes.Reset();
		Settings->EnsureDefaultTaskRoutes();
		return Settings->AgentTaskRoutes;
	}

	FHyperAIStudioRuntimeToolBinding Binding(const TCHAR* Tool, const TCHAR* Toolset, const TCHAR* Description, const bool bEnabled = true)
	{
		FHyperAIStudioRuntimeToolBinding Result;
		Result.ToolName = Tool;
		Result.QualifiedToolset = Toolset;
		Result.Description = Description;
		Result.bToolEnabled = bEnabled;
		return Result;
	}

	/** HyperAI's team as the plugin ships it, by name. */
	TArray<FHyperAIStudioAgentDefinition> TeamAgents()
	{
		const FString Folder = FPaths::Combine(Agents::TeamDirectory(), TEXT("agents"));
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(Folder, TEXT("*.md")), /*Files=*/true, /*Directories=*/false);
		TArray<FHyperAIStudioAgentDefinition> Team;
		for (const FString& File : Files)
		{
			FString Text;
			FString Error;
			FHyperAIStudioAgentDefinition Agent;
			if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(Folder, File)) && Agents::Parse(Text, Agent, Error))
			{
				Team.Add(MoveTemp(Agent));
			}
		}
		Team.Sort([](const FHyperAIStudioAgentDefinition& A, const FHyperAIStudioAgentDefinition& B) { return A.Name < B.Name; });
		return Team;
	}

	FString PickName(const FString& Message, const TArray<FHyperAIStudioAgentDefinition>& ProjectAgents, const bool bTeam,
		const TArray<TaskRouter::FPackDocument>& Documents)
	{
		const TOptional<TaskRouter::FAgentPick> Pick = TaskRouter::AgentFor(Message, ProjectAgents, bTeam, Documents);
		return !Pick ? FString(TEXT("none")) : Pick->bTeam ? FString(TEXT("team")) : Pick->Agent;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTaskRouterPacksTest,
	"HyperAIStudio.TaskRouter.Packs",
	HyperAIStudio::TaskRouterTests::Flags)

bool FHyperAIStudioTaskRouterPacksTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::TaskRouterTests;
	const TArray<TaskRouter::FPackDocument> Documents = TaskRouter::PackDocuments();
	TestTrue(TEXT("Every pack but the shared foundation has words"), Documents.Num() > 30 && !Documents.ContainsByPredicate(
		[](const TaskRouter::FPackDocument& Document) { return Document.PackId == TEXT("shared_foundation") || Document.Words.IsEmpty(); }));
	// What people ask for, and the pack whose tools do it.
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("Make the explosion bigger with more sparks"), TEXT("niagara_vfx")},
		{TEXT("The floor should look wet and glossy under the dusk sun"), TEXT("lighting_lookdev")},
		{TEXT("Create a dodge ability with a stamina cost and a cooldown"), TEXT("gas")},
		{TEXT("Block out a small arena with cover and two ramps"), TEXT("level_design")},
		{TEXT("Give the main menu a settings screen"), TEXT("ui_slate_mvvm")},
		{TEXT("Lower the roughness on the floor material"), TEXT("materials_dynamic_material")},
		{TEXT("Make enemies patrol and chase the player when they see them"), TEXT("gameplay_ai")},
		{TEXT("Bind the dash to the left shift key"), TEXT("enhanced_input")},
		{TEXT("Make a footstep sound for gravel"), TEXT("sound_design")},
		{TEXT("Set up the locomotion state machine in the animation blueprint"), TEXT("anim_blueprint")},
		{TEXT("Add a cutscene where the camera pans over the castle"), TEXT("cinematics")},
		// Free modelling with Geometry Script, not the paid generator or the import pack.
		{TEXT("Make a placeholder stone arch"), TEXT("mesh_modeling")},
		{TEXT("Model a crystal cluster prop"), TEXT("mesh_modeling")},
		{TEXT("Make a low poly rock mesh"), TEXT("mesh_modeling")},
		{TEXT("Generate a textured 3D model of a car"), TEXT("mesh_generation")},
		{TEXT("Import the FBX of the sword"), TEXT("geometry_interchange")},
		{TEXT("Model a crate prop"), TEXT("mesh_modeling")},
		// Props that are there already are placed with the level design pack's spatial tools, not modelled again.
		{TEXT("Place some props around the camp"), TEXT("level_design")},
		{TEXT("Drop the rocks to the ground"), TEXT("level_design")},
		{TEXT("What time is it?"), TEXT("none")},
		{TEXT("Thanks!"), TEXT("none")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("'%s' needs"), Case.Key), TopPack(Case.Key, Documents), FString(Case.Value));
	}
	const TArray<TaskRouter::FPackFit> Both = TaskRouter::RankPacks(TEXT("Make the fire VFX glow brighter and light the room"), Documents, 2);
	TestTrue(TEXT("A task that is two kinds of work needs both packs"), Both.Num() == 2
		&& Both.ContainsByPredicate([](const TaskRouter::FPackFit& Fit) { return Fit.PackId == TEXT("niagara_vfx"); })
		&& Both.ContainsByPredicate([](const TaskRouter::FPackFit& Fit) { return Fit.PackId == TEXT("lighting_lookdev"); }));
	TestEqual(TEXT("The skills made for a pack's work"), FString::Join(TaskRouter::SkillsForPacks({{TEXT("niagara_vfx"), 1.0}, {TEXT("gas"), 1.0}}), TEXT(",")),
		FString(TEXT("unreal-niagara,unreal-gas")));
	TestTrue(TEXT("and none for a pack without one"), TaskRouter::SkillsForPacks({{TEXT("source_control"), 1.0}}).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTaskRouterRoutesTest,
	"HyperAIStudio.TaskRouter.Routes",
	HyperAIStudio::TaskRouterTests::Flags)

bool FHyperAIStudioTaskRouterRoutesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::TaskRouterTests;
	const TArray<TaskRouter::FPackDocument> Documents = TaskRouter::PackDocuments();
	const TArray<FHyperAIStudioAgentTaskRoute> Routes = DefaultRoutes();
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("Make the explosion bigger with more sparks"), TEXT("VFX")},
		{TEXT("Light the castle courtyard at night with torches"), TEXT("Lighting")},
		{TEXT("Create a material for a mossy stone wall"), TEXT("Materials")},
		{TEXT("Scatter rocks and trees over the landscape"), TEXT("Level Design")},
		{TEXT("Retarget the Manny animations onto my new character"), TEXT("Animation")},
		{TEXT("Make enemies patrol and chase the player when they see them"), TEXT("Gameplay")},
		// A request's first word says what is asked for.
		{TEXT("Plan a stealth system for the guards"), TEXT("Plan a feature")},
		{TEXT("Outline the approach for a crafting feature"), TEXT("Plan a feature")},
		{TEXT("Design a new level with a castle courtyard"), TEXT("Level Design")},
		// Free modelling is the level designer's.
		{TEXT("Model a ruined watchtower"), TEXT("Level Design")},
		{TEXT("Boolean modeling for the castle wall"), TEXT("Level Design")},
		// No route is made for these, or the message is two tasks at once: the chat keeps its agent. A paid 3D model is
		// not modelling, nor a material for its "textured".
		{TEXT("Make a footstep sound for gravel"), TEXT("none")},
		{TEXT("Generate a textured 3D model of a car"), TEXT("none")},
		{TEXT("Make the fire VFX glow brighter and light the room"), TEXT("none")},
		{TEXT("What time is it?"), TEXT("none")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("'%s' goes to"), Case.Key), RouteName(Case.Key, Routes, Documents), FString(Case.Value));
	}
	// A route the user named is matched by its name and its packs alike.
	TArray<FHyperAIStudioAgentTaskRoute> Custom = Routes;
	FHyperAIStudioAgentTaskRoute& Sound = Custom.AddDefaulted_GetRef();
	Sound.Category = TEXT("Audio");
	Sound.Packs = {TEXT("sound_design"), TEXT("audio_metasound")};
	TestEqual(TEXT("A route of the user's own takes its work"), RouteName(TEXT("Make a footstep sound for gravel"), Custom, Documents), FString(TEXT("Audio")));

	// Routes saved before free modelling joined Level Design get it once; taken out again, it stays out.
	UHyperAIStudioSettings* Saved = NewObject<UHyperAIStudioSettings>();
	Saved->AgentTaskRoutes = Routes;
	for (FHyperAIStudioAgentTaskRoute& Route : Saved->AgentTaskRoutes)
	{
		Route.Packs.Remove(TEXT("mesh_modeling"));
	}
	Saved->TaskRoutesVersion = 0;
	FHyperAIStudioAgentTaskRoute* SavedLevels = Saved->AgentTaskRoutes.FindByPredicate([](const FHyperAIStudioAgentTaskRoute& Route) { return Route.Category == TEXT("Level Design"); });
	TestTrue(TEXT("Saved routes are brought up to date"), Saved->EnsureDefaultTaskRoutes() && Saved->TaskRoutesVersion == UHyperAIStudioSettings::CurrentTaskRoutesVersion);
	if (TestNotNull(TEXT("The saved routes have Level Design"), SavedLevels))
	{
		TestTrue(TEXT("and it gets the modelling pack"), SavedLevels->Packs.Contains(TEXT("mesh_modeling")));
		SavedLevels->Packs.Remove(TEXT("mesh_modeling"));
		TestFalse(TEXT("once: the user's later edit stands"), Saved->EnsureDefaultTaskRoutes() || SavedLevels->Packs.Contains(TEXT("mesh_modeling")));
	}
	UHyperAIStudioSettings* Seeded = NewObject<UHyperAIStudioSettings>();
	Seeded->AgentTaskRoutes.Reset();
	Seeded->TaskRoutesVersion = 0;
	TestTrue(TEXT("Seeded routes are up to date"), Seeded->EnsureDefaultTaskRoutes() && Seeded->TaskRoutesVersion == UHyperAIStudioSettings::CurrentTaskRoutesVersion);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTaskRouterDecideTest,
	"HyperAIStudio.TaskRouter.Decide",
	HyperAIStudio::TaskRouterTests::Flags)

bool FHyperAIStudioTaskRouterDecideTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::TaskRouterTests;
	const FString Scratch = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tests/TaskRouterScoreboard.json"));
	FHyperAIStudioAgentScoreboard::SetStoragePathForTests(Scratch);
	FHyperAIStudioAgentScoreboard::Reset();
	const TArray<FString> Usable = {TEXT("Claude Code"), TEXT("Codex")};
	TArray<FHyperAIStudioAgentTaskRoute> Routes = DefaultRoutes();

	TestFalse(TEXT("A message no route fits keeps the chat's agent"), TaskRouter::Decide(TEXT("Thanks!"), Routes, Usable, TEXT("Claude Code")).IsSet());
	TOptional<TaskRouter::FRouteDecision> Decision = TaskRouter::Decide(TEXT("Make the explosion bigger with more sparks"), Routes, Usable, TEXT("Claude Code"));
	TestTrue(TEXT("With no record yet, a task goes to the preferred agent"), Decision.IsSet() && Routes[Decision->RouteIndex].Category == TEXT("VFX")
		&& Decision->Choice.Agent == TEXT("Claude Code"));
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = EHyperAIStudioActivityKind::Completed;
		Entry.Agent = TEXT("Codex");
		Entry.PackId = TEXT("niagara_vfx");
		Entry.OperationId = FString::Printf(TEXT("router-%d"), Index);
		FHyperAIStudioAgentScoreboard::Tally(Entry);
	}
	Decision = TaskRouter::Decide(TEXT("Make the explosion bigger with more sparks"), Routes, Usable, TEXT("Claude Code"));
	TestTrue(TEXT("Once an agent has a record on the task's packs, it takes the task: ") + (Decision.IsSet() ? Decision->Choice.Reason : FString()),
		Decision.IsSet() && Decision->Choice.Agent == TEXT("Codex") && Decision->Choice.Reason.Contains(TEXT("4 of 4")));
	Decision = TaskRouter::Decide(TEXT("Plan a stealth system for the guards"), Routes, Usable, TEXT("Codex"));
	TestTrue(TEXT("A route that names its agent and model starts them"), Decision.IsSet() && Decision->Choice.Agent == TEXT("Claude Code")
		&& Decision->Choice.Model == UHyperAIStudioSettings::SmartModelId);
	TestFalse(TEXT("With no agent set up, nothing is routed"), TaskRouter::Decide(TEXT("Make the explosion bigger with more sparks"), Routes, {}, FString()).IsSet());

	// A preset names its route by its category; a route the user renamed is found by the words instead.
	Decision = TaskRouter::DecideNamed(TEXT("Level Design"), TEXT("Make the explosion bigger with more sparks"), Routes, Usable, TEXT("Claude Code"));
	TestTrue(TEXT("A named route is taken whatever the words say"), Decision.IsSet() && Routes[Decision->RouteIndex].Category == TEXT("Level Design"));
	TArray<FHyperAIStudioAgentTaskRoute> Renamed = Routes;
	for (FHyperAIStudioAgentTaskRoute& Route : Renamed)
	{
		if (Route.Category == TEXT("Level Design")) Route.Category = TEXT("Levels");
	}
	Decision = TaskRouter::DecideNamed(TEXT("Level Design"), TEXT("Scatter rocks and trees over the landscape"), Renamed, Usable, TEXT("Claude Code"));
	TestTrue(TEXT("A route the user renamed is found by its packs"), Decision.IsSet() && Renamed[Decision->RouteIndex].Category == TEXT("Levels"));
	TestFalse(TEXT("A named route with no agent set up starts nothing"), TaskRouter::DecideNamed(TEXT("Level Design"), TEXT("Scatter rocks and trees over the landscape"),
		Routes, {}, FString()).IsSet());

	FHyperAIStudioAgentScoreboard::SetStoragePathForTests(FString());
	IFileManager::Get().Delete(*Scratch);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTaskRouterToolsTest,
	"HyperAIStudio.TaskRouter.ToolsAndSkills",
	HyperAIStudio::TaskRouterTests::Flags)

bool FHyperAIStudioTaskRouterToolsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::TaskRouterTests;
	const TCHAR* Niagara = TEXT("HyperAIStudioNiagara.HyperAIStudioNiagaraToolset");
	const TArray<FHyperAIStudioRuntimeToolBinding> Bindings = {
		Binding(TEXT("hyper_niagara_validate"), Niagara, TEXT("Proves a system compiles and spawns. Then capture it.")),
		Binding(TEXT("hyper_niagara_apply_plan"), Niagara, TEXT("Closed Niagara edits as one undo step.")),
		Binding(TEXT("hyper_niagara_inspect"), Niagara, TEXT("A system's emitters, modules and renderers.")),
		Binding(TEXT("hyper_gas_inspect"), TEXT("HyperAIStudioGAS.HyperAIStudioGASToolset"), TEXT("Effects and abilities."), /*bEnabled=*/false),
	};
	const TMap<FString, FString> PackOfTool = {{TEXT("hyper_niagara_validate"), TEXT("niagara_vfx")}, {TEXT("hyper_niagara_apply_plan"), TEXT("niagara_vfx")},
		{TEXT("hyper_niagara_inspect"), TEXT("niagara_vfx")}, {TEXT("hyper_gas_inspect"), TEXT("gas")}};
	TSet<FString> Given;
	const FString Hint = TaskRouter::ToolHintFrom({{TEXT("niagara_vfx"), 9.0}, {TEXT("gas"), 6.0}}, Bindings, PackOfTool, Given);
	TestTrue(TEXT("The pack's tools are named with the toolset_name call_tool takes"), Hint.StartsWith(TEXT("\n\n<tools>"))
		&& Hint.Contains(TEXT("`hyper_niagara_apply_plan` (toolset_name `HyperAIStudioNiagara.HyperAIStudioNiagaraToolset`): Closed Niagara edits as one undo step.")));
	const int32 Inspect = Hint.Find(TEXT("hyper_niagara_inspect"));
	const int32 Apply = Hint.Find(TEXT("hyper_niagara_apply_plan"));
	const int32 Validate = Hint.Find(TEXT("hyper_niagara_validate"));
	TestTrue(TEXT("in the order an agent calls them: read, edit, check"), Inspect != INDEX_NONE && Inspect < Apply && Apply < Validate);
	TestTrue(TEXT("each with its first sentence"), Hint.Contains(TEXT("compiles and spawns.")) && !Hint.Contains(TEXT("Then capture it")));
	TestFalse(TEXT("A tool that is not enabled is left out"), Hint.Contains(TEXT("hyper_gas_inspect")));
	TestTrue(TEXT("A chat is pointed at a pack's tools once"), TaskRouter::ToolHintFrom({{TEXT("niagara_vfx"), 9.0}}, Bindings, PackOfTool, Given).IsEmpty());
	TSet<FString> Fresh;
	TestTrue(TEXT("No pack, no tools"), TaskRouter::ToolHintFrom({}, Bindings, PackOfTool, Fresh).IsEmpty());

	// Three packs of eight tools each, as a preset names them, and a pack of tools the preset's prompt calls by name.
	TArray<FHyperAIStudioRuntimeToolBinding> Many;
	TMap<FString, FString> PackOfMany;
	for (const TCHAR* Pack : {TEXT("a"), TEXT("b"), TEXT("c"), TEXT("f")})
	{
		for (int32 Index = 1; Index <= 8; ++Index)
		{
			const FString Tool = FString::Printf(TEXT("hyper_%s_tool_%d"), Pack, Index);
			Many.Add(Binding(*Tool, TEXT("HyperAIStudioTest.HyperAIStudioTestToolset"), TEXT("Does one thing.")));
			PackOfMany.Add(Tool, FString::Printf(TEXT("pack_%s"), Pack));
		}
	}
	const TArray<TaskRouter::FPackFit> Three = {{TEXT("pack_a"), 100.0}, {TEXT("pack_b"), 100.0}, {TEXT("pack_c"), 100.0}};
	TSet<FString> Shared;
	const FString Split = TaskRouter::ToolHintFrom(Three, Many, PackOfMany, Shared);
	TestTrue(TEXT("Three packs share the lines, so the third gets its tools too: ") + Split, Split.Contains(TEXT("hyper_a_tool_3")) && !Split.Contains(TEXT("hyper_a_tool_4"))
		&& Split.Contains(TEXT("hyper_c_tool_3")) && Shared.Contains(TEXT("tools:pack_c")));
	TArray<FString> Named;
	for (int32 Index = 1; Index <= 8; ++Index)
	{
		Named.Add(FString::Printf(TEXT("hyper_f_tool_%d"), Index));
	}
	TSet<FString> Crowded;
	const FString First = TaskRouter::ToolHintFrom(Three, Many, PackOfMany, Crowded, Named);
	TestTrue(TEXT("The tools the message names come first: ") + First, First.Contains(TEXT("hyper_f_tool_1"))
		&& First.Find(TEXT("hyper_f_tool_8")) < First.Find(TEXT("hyper_a_tool_1")));
	TestTrue(TEXT("A pack the lines ran out for is not marked as sent"), Crowded.Contains(TEXT("tools:pack_a")) && !Crowded.Contains(TEXT("tools:pack_b"))
		&& !Crowded.Contains(TEXT("tools:pack_c")));
	const FString Later = TaskRouter::ToolHintFrom(Three, Many, PackOfMany, Crowded, Named);
	TestTrue(TEXT("so a later message lists it, and nothing twice: ") + Later, Later.Contains(TEXT("hyper_b_tool_1")) && Later.Contains(TEXT("hyper_c_tool_1"))
		&& !Later.Contains(TEXT("hyper_a_tool_")) && !Later.Contains(TEXT("hyper_f_tool_")) && Crowded.Contains(TEXT("tools:pack_c")));
	TSet<FString> Inside;
	const FString Own = TaskRouter::ToolHintFrom(Three, Many, PackOfMany, Inside, {TEXT("hyper_b_tool_7")});
	TestTrue(TEXT("A named tool of a listed pack comes first, once: ") + Own, Own.Find(TEXT("hyper_b_tool_7")) < Own.Find(TEXT("hyper_a_tool_1"))
		&& Own.Find(TEXT("hyper_b_tool_7")) == Own.Find(TEXT("hyper_b_tool_7"), ESearchCase::CaseSensitive, ESearchDir::FromEnd));

	// The skill made for the task's work comes first, even when the message shares no word with it.
	const FString Root = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("RouterTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	auto WriteSkill = [&Root](const TCHAR* Name, const TCHAR* Description)
	{
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("---\nname: %s\ndescription: %s\n---\n\nDo the thing.\n"), Name, Description),
			*FPaths::Combine(Skills::CanonicalDirectory(Root), Name, TEXT("SKILL.md")));
	};
	WriteSkill(TEXT("unreal-niagara"), TEXT("Build or change a Niagara system with HyperAI's tools."));
	WriteSkill(TEXT("image-match"), TEXT("Make a scene look like a reference image and prove it."));
	TSet<FString> SkillsGiven;
	const FString Skill = Skills::HintFor(Root, TEXT("Make the explosion bigger with more sparks"), SkillsGiven, {TEXT("unreal-niagara"), TEXT("missing-skill")});
	TestTrue(TEXT("The skill for the task's work is named: ") + Skill, Skill.Contains(TEXT("`unreal-niagara`")) && !Skill.Contains(TEXT("missing-skill")));
	TestTrue(TEXT("once per chat"), Skills::HintFor(Root, TEXT("Make the explosion bigger"), SkillsGiven, {TEXT("unreal-niagara")}).IsEmpty());
	IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);

	// What a message carries, and the one line that says so.
	TSet<FString> ChatGiven;
	FString Summary;
	const FString Context = FHyperAIStudioNativeChat::ContextFor(Root, TEXT("Make the explosion bigger with more sparks"), TEXT("Claude Code"), FString(),
		ChatGiven, Summary);
	TestTrue(TEXT("A message carries the tools for its task"), Context.Contains(TEXT("<tools>")) && Context.Contains(TEXT("hyper_niagara_apply_plan")));
	TestTrue(TEXT("and says so in a line: ") + Summary, Summary.Contains(TEXT("the niagara_vfx tools")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioTaskRouterAgentsTest,
	"HyperAIStudio.TaskRouter.Agents",
	HyperAIStudio::TaskRouterTests::Flags)

bool FHyperAIStudioTaskRouterAgentsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::TaskRouterTests;
	const TArray<TaskRouter::FPackDocument> Documents = TaskRouter::PackDocuments(/*bToolDescriptions=*/false);
	const TArray<FHyperAIStudioAgentDefinition> Team = TeamAgents();
	TestEqual(TEXT("The plugin ships the team's eight agents"), Team.Num(), 8);
	// What people ask for, and the member of the team made for it.
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{TEXT("Playtest the level and check the doors open"), TEXT("qa-tester")},
		{TEXT("Verify the inventory saves and loads"), TEXT("qa-tester")},
		{TEXT("Can you test whether the AI guards see the player through walls?"), TEXT("qa-tester")},
		{TEXT("Profile the frame time in the forest level"), TEXT("qa-tester")},
		{TEXT("Why is the frame rate so low?"), TEXT("qa-tester")},
		{TEXT("Play the level and check the doors open"), TEXT("qa-tester")},
		{TEXT("Light the castle courtyard at night with torches"), TEXT("lighting-artist")},
		{TEXT("Make the floor look wet and glossy at dusk"), TEXT("lighting-artist")},
		{TEXT("Match the lighting to this reference image"), TEXT("lighting-artist")},
		{TEXT("Block out a small arena with cover and two ramps"), TEXT("level-designer")},
		{TEXT("Scatter rocks and trees over the landscape"), TEXT("level-designer")},
		{TEXT("Plant trees along the road"), TEXT("level-designer")},
		{TEXT("Model a stone pillar with a plinth"), TEXT("level-designer")},
		{TEXT("Make the explosion bigger with more sparks"), TEXT("technical-artist")},
		{TEXT("Create a material for a mossy stone wall"), TEXT("technical-artist")},
		{TEXT("Retarget the Manny animations onto my new character"), TEXT("technical-artist")},
		{TEXT("Create a dodge ability with a stamina cost and a cooldown"), TEXT("programmer")},
		{TEXT("Make enemies patrol and chase the player when they see them"), TEXT("programmer")},
		{TEXT("Make the door open with a timeline"), TEXT("programmer")},
		{TEXT("Fix the crash when the player respawns"), TEXT("programmer")},
		{TEXT("Make the player jump higher"), TEXT("programmer")},
		{TEXT("Let the player open doors with the E key"), TEXT("programmer")},
		{TEXT("Play a sound when the door opens"), TEXT("programmer")},
		{TEXT("Give the main menu a settings screen"), TEXT("ui-designer")},
		{TEXT("Make the health bar red when health is low"), TEXT("ui-designer")},
		{TEXT("Add a cutscene where the camera pans over the castle"), TEXT("cinematographer")},
		{TEXT("Render a trailer of the boss fight"), TEXT("cinematographer")},
		// Asked first to plan or to test: the director's or the QA tester's, whatever is planned or tested.
		{TEXT("Plan a stealth system for the guards"), TEXT("director")},
		{TEXT("Review what the team built against the request"), TEXT("director")},
		{TEXT("Check that the lights in the hall are not too bright"), TEXT("qa-tester")},
		// Parts for two builders, neither able to do the other's: the team shares them out.
		{TEXT("Build a small arena, light it at dusk and add a health bar"), TEXT("team")},
		{TEXT("Make a fire VFX and light the room with it"), TEXT("team")},
		{TEXT("Add a health system and a health bar"), TEXT("team")},
		// Work no member is made for, a place rather than a kind of work, or no task at all: the chat's agent keeps it.
		{TEXT("Make a footstep sound for gravel"), TEXT("none")},
		{TEXT("Add rain to the level"), TEXT("none")},
		{TEXT("Special effects for the boss fight"), TEXT("none")},
		{TEXT("What time is it?"), TEXT("none")},
		{TEXT("Thanks!"), TEXT("none")},
	};
	for (const TPair<const TCHAR*, const TCHAR*>& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("'%s' is for"), Case.Key), PickName(Case.Key, Team, /*bTeam=*/true, Documents), FString(Case.Value));
	}
	TestEqual(TEXT("Without the team, the role that fits best takes it all"),
		PickName(TEXT("Build a small arena, light it at dusk and add a health bar"), Team, /*bTeam=*/false, Documents), FString(TEXT("lighting-artist")));
	// An agent of the project's own is matched by its name, what it says it does, and its skills' work.
	TArray<FHyperAIStudioAgentDefinition> Custom = Team;
	FHyperAIStudioAgentDefinition& Vfx = Custom.AddDefaulted_GetRef();
	Vfx.Name = TEXT("vfx-artist");
	Vfx.Description = TEXT("Makes the game's visual effects: fire, smoke, sparks, magic and impacts.");
	Vfx.Skills = {TEXT("unreal-niagara")};
	TestEqual(TEXT("A custom agent takes its own work"), PickName(TEXT("Make the explosion bigger with more sparks"), Custom, true, Documents), FString(TEXT("vfx-artist")));
	TestEqual(TEXT("No agents, no pick"), PickName(TEXT("Playtest the level"), {}, true, Documents), FString(TEXT("none")));

	// The hand-off, in each chat agent's own way.
	TArray<FHyperAIStudioAgentDefinition> Synced = Team;
	for (FHyperAIStudioAgentDefinition& Agent : Synced)
	{
		Agent.Scope = TEXT("project");
		Agent.ClaudeState = TEXT("synced");
		Agent.CodexState = TEXT("missing");
	}
	const FString Root = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()), TEXT("AgentRouterTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TaskRouter::FAgentPick Tester;
	Tester.Agent = TEXT("qa-tester");
	Tester.Description = TEXT("Tests what the team built.");
	TSet<FString> Given;
	const FString Claude = TaskRouter::AgentHintFrom(Tester, Synced, Root, TEXT("Claude Code"), Given);
	TestTrue(TEXT("Claude Code hands the task to the agent with its Agent tool: ") + Claude, Claude.StartsWith(TEXT("\n\n<agents>\nThis task is for the project's `qa-tester` agent: Tests what the team built."))
		&& Claude.Contains(TEXT("with your Agent tool, `qa-tester` as the agent type")) && Claude.EndsWith(TEXT("</agents>")));
	TestTrue(TEXT("A chat is pointed at an agent once"), TaskRouter::AgentHintFrom(Tester, Synced, Root, TEXT("Claude Code"), Given).IsEmpty());
	TSet<FString> CodexGiven;
	const FString Unsynced = TaskRouter::AgentHintFrom(Tester, Synced, Root, TEXT("Codex"), CodexGiven);
	TestTrue(TEXT("Codex without its copy of the agent reads the role's file: ") + Unsynced, Unsynced.Contains(TEXT("Read `.hyperai/agents/qa-tester.md` first")));
	for (FHyperAIStudioAgentDefinition& Agent : Synced)
	{
		Agent.CodexState = TEXT("synced");
	}
	CodexGiven.Reset();
	TestTrue(TEXT("Codex with it spawns the agent"), TaskRouter::AgentHintFrom(Tester, Synced, Root, TEXT("Codex"), CodexGiven)
		.Contains(TEXT("Spawn the `qa-tester` agent with your multi-agent tools")));
	TSet<FString> GeminiGiven;
	TestTrue(TEXT("Any other agent reads the role's file"), TaskRouter::AgentHintFrom(Tester, Synced, Root, TEXT("Gemini"), GeminiGiven)
		.Contains(TEXT("Read `.hyperai/agents/qa-tester.md` first")));
	TaskRouter::FAgentPick Whole;
	Whole.bTeam = true;
	TSet<FString> TeamGiven;
	TestTrue(TEXT("The team runs a request with parts for several roles"), TaskRouter::AgentHintFrom(Whole, Synced, Root, TEXT("Codex"), TeamGiven)
		.Contains(TEXT("`team` skill")) && TeamGiven.Contains(TEXT("agent:team")));

	// From the project's own agents on disk, as a chat sends it.
	Agents::InstallTeam(Root, /*bUpdateOnly=*/false);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bWasHanding = Settings->bHandTasksToProjectAgents;
	Settings->bHandTasksToProjectAgents = true;
	TSet<FString> ChatGiven;
	FString Summary;
	const FString Context = FHyperAIStudioNativeChat::ContextFor(Root, TEXT("Playtest the level and check the doors open"), TEXT("Claude Code"), FString(),
		ChatGiven, Summary);
	TestTrue(TEXT("A message carries its agent: ") + Context, Context.Contains(TEXT("<agents>")) && Context.Contains(TEXT("`qa-tester` as the agent type")));
	TestTrue(TEXT("and says so in a line: ") + Summary, Summary.StartsWith(TEXT("a hand-off to the qa-tester agent")));
	TSet<FString> AsTester;
	TestTrue(TEXT("A chat started as a project agent keeps its tasks"), TaskRouter::AgentHintFor(TEXT("Light the hall at dusk"), Root, TEXT("Claude Code"),
		TEXT("qa-tester"), AsTester).IsEmpty());
	TSet<FString> TeamChat;
	TestTrue(TEXT("A request with parts for several roles goes to the team"), TaskRouter::AgentHintFor(
		TEXT("Build a small arena, light it at dusk and add a health bar"), Root, TEXT("Codex"), FString(), TeamChat).Contains(TEXT("`team` skill")));
	TSet<FString> ToTeam;
	TestTrue(TEXT("A request sent to the team is the team's"), TaskRouter::AgentHintFor(Agents::TeamRequest(TEXT("Codex"), TEXT("Playtest the level")), Root,
		TEXT("Codex"), FString(), ToTeam).IsEmpty());
	Settings->bHandTasksToProjectAgents = false;
	TSet<FString> Off;
	FString OffSummary;
	TestFalse(TEXT("Turned off, nothing is handed off"), FHyperAIStudioNativeChat::ContextFor(Root, TEXT("Playtest the level and check the doors open"),
		TEXT("Claude Code"), FString(), Off, OffSummary).Contains(TEXT("<agents>")));
	Settings->bHandTasksToProjectAgents = bWasHanding;
	IFileManager::Get().DeleteDirectory(*Root, /*RequireExists=*/false, /*Tree=*/true);
	return true;
}

#endif
