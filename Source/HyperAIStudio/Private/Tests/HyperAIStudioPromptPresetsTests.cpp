// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Algo/Find.h"
#include "ContentBrowserItemPath.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgents.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioChatHistoryPrefs.h"
#include "HyperAIStudioLocalization.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioPromptPresets.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSkills.h"
#include "HyperAIStudioTaskRouter.h"
#include "HyperAIStudioWidgetShot.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/TextLocalizationManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "SHyperAIStudioNativeChatView.h"
#include "SHyperAIStudioPromptPicker.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Text/STextBlock.h"

namespace HyperAIStudio::PromptPresetsTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;
	using namespace HyperAIStudio::PromptPresets;

	/** A folder of its own under the automation scratch space, gone when the test ends. */
	struct FScratch
	{
		FString Dir = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("PromptPresets-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		~FScratch()
		{
			SetBuiltInDirectoryForTests(FString());
			IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists=*/false, /*Tree=*/true);
		}
	};

	const TCHAR* const FixtureCategories = TEXT(R"JSON({"schema": "hyperai.prompt-categories.v1", "categories": [
		{"id": "pcg-foliage", "title": "PCG and foliage", "order": 30, "icon": "HyperAIStudio.Icon.Tools", "min": 2, "route": "Level Design"},
		{"id": "vfx-niagara", "title": "VFX with Niagara", "order": 90, "icon": "HyperAIStudio.Icon.Tools", "min": 1, "route": "VFX"},
		{"id": "team-builds", "title": "Team builds", "order": 310, "icon": "HyperAIStudio.Icon.Agents", "min": 1}]})JSON");

	const TCHAR* const FixturePcg = TEXT(R"JSON({"schema": "hyperai.prompt-presets.v1", "category": "pcg-foliage", "presets": [
		{"id": "pcg-foliage.forest-ecosystem", "title": "Forest ecosystem on a paint layer",
		 "summary": "Grow trees, shrubs and rocks on a landscape layer with PCG, then prove density and clearances.",
		 "packs": ["pcg", "worldbuilding_navigation"], "skills": ["unreal-level-design"], "agent": "level-designer",
		 "tools": ["hyper_pcg_scatter_inspect", "hyper_pcg_scatter_apply_plan", "hyper_capture_view"], "ops": ["pcg.scatter"],
		 "difficulty": "standard", "tags": ["trees", "biome", "scatter"],
		 "context": {"selection": "actors", "classes": ["Landscape"], "min": 1},
		 "placeholders": [{"name": "layer", "label": "Paint layer", "default": "Grass", "hint": "A layer the landscape already has."},
			{"name": "biome", "label": "Biome", "default": "temperate forest"}],
		 "prompt": "On {{selected_actor}} in {{level}}, grow a {{biome}} ecosystem on the `{{layer}}` paint layer.\n1. hyper_pcg_scatter_inspect: confirm the layer exists.\n2. One hyper_pcg_scatter_apply_plan under {{content_folder}}/PCG.\n3. Capture two views with hyper_capture_view and report a PASS/FAIL table."},
		{"id": "pcg-foliage.rock-scatter", "title": "Scatter the selected rock meshes",
		 "summary": "Scatter the selected static meshes over the level's ground with PCG.",
		 "packs": ["pcg"], "tools": ["hyper_pcg_scatter_apply_plan"], "difficulty": "starter", "tags": ["rocks"],
		 "context": {"selection": "assets", "classes": ["StaticMesh"], "min": 1},
		 "prompt": "Scatter {{selected_assets}} over the ground of {{level}} with one hyper_pcg_scatter_apply_plan, 30 to 60 cm apart, then report the count."}]})JSON");

	const TCHAR* const FixtureVfx = TEXT(R"JSON({"schema": "hyperai.prompt-presets.v1", "category": "vfx-niagara", "presets": [
		{"id": "vfx-niagara.campfire", "title": "Campfire with sparks and smoke",
		 "summary": "Build a looping Niagara campfire with embers, sparks and smoke, then prove it spawns.",
		 "packs": ["niagara_vfx"], "skills": ["unreal-niagara"], "tools": ["hyper_niagara_apply_plan", "hyper_niagara_validate"],
		 "difficulty": "standard", "tags": ["niagara", "fire", "particles"],
		 "prompt": "Build a looping campfire Niagara system under {{content_folder}}/VFX with hyper_niagara_apply_plan: flames, embers and smoke. Then run hyper_niagara_validate and report a PASS/FAIL table."}]})JSON");

	const TCHAR* const FixtureTeam = TEXT(R"JSON({"schema": "hyperai.prompt-presets.v1", "category": "team-builds", "presets": [
		{"id": "team-builds.arena", "title": "Arena with lighting and a HUD",
		 "summary": "The team blocks out an arena, lights it at dusk and adds a health bar.",
		 "packs": ["level_design"], "agent": "team", "difficulty": "advanced",
		 "prompt": "Build a small arena in {{level}}: block it out with cover and two ramps, light it at dusk, and add a health bar to the HUD. Prove each part."}]})JSON");

	bool Write(const FString& Path, const FString& Text)
	{
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** The fixture library's folder: three categories and four presets. */
	FString WriteFixture(const FString& Dir)
	{
		const FString Folder = FPaths::Combine(Dir, TEXT("BuiltIn"));
		Write(FPaths::Combine(Folder, TEXT("categories.json")), FixtureCategories);
		Write(FPaths::Combine(Folder, TEXT("pcg-foliage.json")), FixturePcg);
		Write(FPaths::Combine(Folder, TEXT("vfx-niagara.json")), FixtureVfx);
		Write(FPaths::Combine(Folder, TEXT("team-builds.json")), FixtureTeam);
		return Folder;
	}

	/** One Landscape selected in a test level. */
	FPromptContext LandscapeContext()
	{
		FPromptContext Context;
		Context.ActorLabels = {TEXT("Landscape")};
		Context.ActorPaths = {TEXT("/Game/Maps/Test.Test:PersistentLevel.Landscape_0")};
		Context.ActorClasses = {TEXT("Landscape,LandscapeProxy,PartitionActor,Actor")};
		Context.AssetPaths = {TEXT("/Game/Rocks/SM_Rock_A.SM_Rock_A"), TEXT("/Game/Rocks/SM_Rock_B.SM_Rock_B")};
		Context.AssetClasses = {TEXT("StaticMesh,StreamableRenderAsset"), TEXT("StaticMesh,StreamableRenderAsset")};
		Context.Level = TEXT("/Game/Maps/Test");
		Context.Project = TEXT("PromptTests");
		return Context;
	}

	/** What Validate is checked against for the fixtures. */
	TArray<FString> ValidateFixture(const FLibrary& Library)
	{
		const TSet<FString> Packs = {TEXT("pcg"), TEXT("worldbuilding_navigation"), TEXT("niagara_vfx"), TEXT("level_design"), TEXT("shared_foundation")};
		const TMap<FString, FString> PackOfTool = {{TEXT("hyper_pcg_scatter_inspect"), TEXT("pcg")}, {TEXT("hyper_pcg_scatter_apply_plan"), TEXT("pcg")},
			{TEXT("hyper_capture_view"), TEXT("shared_foundation")}, {TEXT("hyper_niagara_apply_plan"), TEXT("niagara_vfx")},
			{TEXT("hyper_niagara_validate"), TEXT("niagara_vfx")}, {TEXT("hyper_material_apply_plan"), TEXT("materials_dynamic_material")}};
		return Validate(Library, Packs, PackOfTool, {TEXT("unreal-level-design"), TEXT("unreal-niagara")}, {TEXT("level-designer"), TEXT("qa-tester"), TEXT("team")},
			{TEXT("Level Design"), TEXT("VFX"), TEXT("Gameplay")});
	}

	/** A chat with no agent behind it: what it would send is kept in Sent. */
	class FFakeChat final : public FHyperAIStudioNativeChat
	{
	public:
		explicit FFakeChat(const FString& Root)
			: FHyperAIStudioNativeChat(TEXT("Codex"), Root, FString(), FString())
		{
		}

		void MakeReady() { Ready(TEXT("fake-session")); }
		void FinishTurn() { TurnFinished(); }
		TArray<FString> Sent;

		/** The latest "Sent with it" line. */
		FString LastSummary() const
		{
			for (int32 Index = GetItems().Num() - 1; Index >= 0; --Index)
			{
				if (GetItems()[Index]->Kind == FHyperAIStudioChatItem::EKind::Notice && GetItems()[Index]->Text.StartsWith(TEXT("Sent with it")))
				{
					return GetItems()[Index]->Text;
				}
			}
			return FString();
		}

	protected:
		virtual FString CommandLine() const override { return FString(); }
		virtual bool WritesJsonRpcVersion() const override { return false; }
		virtual void Handshake() override {}
		virtual void SendNow(const FString& Text, const TArray<FString>& Images) override { Sent.Add(Text); }
		virtual void InterruptNow() override {}
		virtual void HandleMessage(const FString& Method, const TSharedPtr<FJsonObject>& Params, const TSharedPtr<FJsonValue>& RequestId) override {}
		virtual TSharedRef<FJsonObject> AnswerPayload(const FHyperAIStudioChatItem& Item, int32 Choice) override { return MakeShared<FJsonObject>(); }
	};

	/** Every widget of Type under Root, depth first. */
	void FindWidgets(const TSharedRef<SWidget>& Root, const FName Type, TArray<TSharedRef<SWidget>>& Out)
	{
		if (Root->GetType() == Type)
		{
			Out.Add(Root);
		}
		FChildren* Children = Root->GetChildren();
		for (int32 Index = 0; Children && Index < Children->Num(); ++Index)
		{
			FindWidgets(Children->GetChildAt(Index), Type, Out);
		}
	}

	TSharedPtr<SWidget> FindWidget(const TSharedRef<SWidget>& Root, const FName Type)
	{
		TArray<TSharedRef<SWidget>> Found;
		FindWidgets(Root, Type, Found);
		return Found.IsEmpty() ? TSharedPtr<SWidget>() : TSharedPtr<SWidget>(Found[0]);
	}

	/** The pcg pack's tools are on in this editor, so a tool block can name them. */
	bool PcgToolsEnabled()
	{
		TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
		TArray<FString> Diagnostics;
		FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(Bindings, Diagnostics);
		return EnabledPacks(Bindings).Contains(TEXT("pcg"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsShippedParseTest,
	"HyperAIStudio.PromptPresets.ShippedParse",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsShippedParseTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	const FLibrary Library = Load(BuiltInDirectory(), FString());
	for (const FString& Error : Library.Errors)
	{
		AddError(TEXT("The shipped library has an error: ") + Error);
	}
	TestTrue(FString::Printf(TEXT("The plugin ships at least 300 presets (%d)"), Library.Presets.Num()), Library.Presets.Num() >= 300);
	for (const FHyperAIStudioPromptCategory& Category : Library.Categories)
	{
		const int32 Count = Library.Presets.FilterByPredicate([&Category](const FHyperAIStudioPromptPreset& Each) { return Each.Category == Category.Id; }).Num();
		TestTrue(FString::Printf(TEXT("%s has at least its %d presets (%d)"), *Category.Id, Category.Min, Count), Count >= Category.Min);
	}
	TSet<FString> Ids;
	for (const FHyperAIStudioPromptPreset& Preset : Library.Presets)
	{
		const FString At = Preset.SourceFile + TEXT(" ") + Preset.Id + TEXT(": ");
		bool bTwice = false;
		Ids.Add(Preset.Id, &bTwice);
		TestFalse(At + TEXT("the id is unique"), bTwice);
		FString Category;
		FString Name;
		TestTrue(At + TEXT("the id is <category>.<kebab> with its file's category"), Preset.Id.Split(TEXT("."), &Category, &Name) && Category == Preset.Category
			&& !Name.IsEmpty() && !Name.Contains(TEXT(".")));
		TestTrue(At + TEXT("its category exists"), Library.FindCategory(Preset.Category) != nullptr);
		TestTrue(At + TEXT("the title is 1-60 characters"), !Preset.Title.IsEmpty() && Preset.Title.Len() <= 60);
		TestTrue(At + TEXT("the summary is 1-160 characters on one line"), !Preset.Summary.IsEmpty() && Preset.Summary.Len() <= 160 && !Preset.Summary.Contains(TEXT("\n")));
		TestTrue(At + TEXT("the prompt is at most 4000 characters"), !Preset.Prompt.IsEmpty() && Preset.Prompt.Len() <= 4000);
		TestTrue(At + TEXT("the difficulty is starter, standard or advanced"),
			Preset.Difficulty == TEXT("starter") || Preset.Difficulty == TEXT("standard") || Preset.Difficulty == TEXT("advanced"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsReferencesTest,
	"HyperAIStudio.PromptPresets.References",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsReferencesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	// Against the catalog compiled into this editor, the plugin's skills and team, and the default task routes.
	const FLibrary Library = Load(BuiltInDirectory(), FString());
	const TArray<FString> Errors = ValidateInstalled(Library);
	for (const FString& Error : Errors)
	{
		AddError(Error);
	}
	TestEqual(TEXT("Every pack, tool, skill, agent and route the shipped presets name exists"), Errors.Num(), 0);

	// The checks themselves, on a library made to break them.
	FScratch Scratch;
	FLibrary Broken = Load(WriteFixture(Scratch.Dir), FString());
	TestEqual(TEXT("The fixture library is valid"), FString::Join(ValidateFixture(Broken), TEXT("\n")), FString());
	FHyperAIStudioPromptPreset& Forest = Broken.Presets[0];
	Forest.Packs.Add(TEXT("not_a_pack"));
	Forest.Skills.Add(TEXT("not-a-skill"));
	Forest.Agent = TEXT("stranger");
	Forest.Tools.Add(TEXT("hyper_material_apply_plan"));
	Forest.Tools.Add(TEXT("hyper_not_a_tool"));
	Broken.Categories[0].Route = TEXT("Nowhere");
	const FString Found = FString::Join(ValidateFixture(Broken), TEXT("\n"));
	TestTrue(TEXT("An unknown pack is caught"), Found.Contains(TEXT("pack not_a_pack is not in the catalog")));
	TestTrue(TEXT("an unknown skill"), Found.Contains(TEXT("skill not-a-skill")));
	TestTrue(TEXT("an unknown agent"), Found.Contains(TEXT("agent stranger")));
	TestTrue(TEXT("a tool of a pack the preset does not name"), Found.Contains(TEXT("tool hyper_material_apply_plan belongs to pack materials_dynamic_material")));
	TestTrue(TEXT("a tool the catalog does not have"), Found.Contains(TEXT("tool hyper_not_a_tool is not in the catalog")));
	TestTrue(TEXT("a tool the prompt never names"), Found.Contains(TEXT("tool hyper_not_a_tool is listed but the prompt does not name it")));
	TestTrue(TEXT("and a route that is not a task route"), Found.Contains(TEXT("route 'Nowhere'")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsPlaceholdersTest,
	"HyperAIStudio.PromptPresets.Placeholders",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsPlaceholdersTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	const FLibrary Library = Load(BuiltInDirectory(), FString());
	for (const FHyperAIStudioPromptPreset& Preset : Library.Presets)
	{
		TSet<FString> Declared;
		for (const FHyperAIStudioPromptPlaceholder& Placeholder : Preset.Placeholders)
		{
			bool bTwice = false;
			Declared.Add(Placeholder.Name, &bTwice);
			TestFalse(FString::Printf(TEXT("%s: placeholder %s is declared once"), *Preset.Id, *Placeholder.Name), bTwice);
			TestTrue(FString::Printf(TEXT("%s: placeholder %s is used"), *Preset.Id, *Placeholder.Name),
				Preset.Prompt.Contains(TEXT("{{") + Placeholder.Name + TEXT("}}"), ESearchCase::CaseSensitive));
		}
		for (const FString& Used : UsedNames(Preset.Prompt))
		{
			const bool bToken = Algo::FindByPredicate(ContextTokens, [&Used](const TCHAR* Token) { return Used == Token; }) != nullptr;
			TestTrue(FString::Printf(TEXT("%s: {{%s}} is a context token or a declared placeholder"), *Preset.Id, *Used), bToken || Declared.Contains(Used));
		}
	}
	// Every shipped prompt passes the full placeholder rules too.
	for (const FString& Error : ValidateInstalled(Library))
	{
		if (Error.Contains(TEXT("{{")) || Error.Contains(TEXT("placeholder")))
		{
			AddError(Error);
		}
	}

	// The rules, each broken once.
	FScratch Scratch;
	FLibrary Fixture = Load(WriteFixture(Scratch.Dir), FString());
	FHyperAIStudioPromptPreset& Forest = Fixture.Presets[0];
	Forest.Prompt += TEXT(" Then {{mystery}} and {{biome} and {{\"X\": 1}}.");
	Forest.Placeholders.Add({TEXT("unused"), TEXT("Unused"), TEXT("x"), FString()});
	Forest.Placeholders.Add({TEXT("layer"), TEXT("Layer again"), FString(), FString()});
	Forest.Placeholders.Add({TEXT("level"), TEXT("Level"), FString(), FString()});
	const FString Found = FString::Join(ValidateFixture(Fixture), TEXT("\n"));
	TestTrue(TEXT("An undeclared name is caught: ") + Found, Found.Contains(TEXT("{{mystery}} is neither a context token nor a declared placeholder")));
	TestTrue(TEXT("so is braced text that is not a name"), Found.Contains(TEXT("{{\"X\": 1}} is neither")));
	TestTrue(TEXT("a name left open"), Found.Contains(TEXT("{{biome has no closing }}")));
	TestTrue(TEXT("a placeholder the prompt does not use"), Found.Contains(TEXT("placeholder unused is declared but the prompt does not use it")));
	TestTrue(TEXT("a name declared twice"), Found.Contains(TEXT("placeholder 'layer' must be a new lower-case name")));
	TestTrue(TEXT("and a placeholder named like a context token"), Found.Contains(TEXT("placeholder 'level' must be a new lower-case name")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsFillTest,
	"HyperAIStudio.PromptPresets.Fill",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsFillTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	FHyperAIStudioPromptPreset Preset;
	Preset.Id = TEXT("pcg-foliage.fill-test");
	Preset.bBuiltIn = false;
	Preset.Placeholders = {{TEXT("biome"), TEXT("Biome"), TEXT("temperate forest"), FString()}, {TEXT("layer"), TEXT("Paint layer"), TEXT("Grass"), FString()}};
	Preset.Prompt = TEXT("On {{selected_actor}} in {{level}}, grow a {{biome}} on `{{layer}}` under {{content_folder}}/PCG for {{project}}.\n")
		TEXT("Assets:\n{{selected_assets}}\nPlan: {\"Kind\": \"landscape.create\"} and {{\"X\":1}} stay; so does {{unknown}}.");
	const FPromptContext Context = LandscapeContext();

	TArray<FString> Blanks;
	const FString Filled = Fill(Preset, Context, {{TEXT("layer"), TEXT("Moss")}}, Blanks);
	TestEqual(TEXT("Tokens fill from the selection, fields from what was typed, the rest from defaults"), Filled,
		FString(TEXT("On Landscape (/Game/Maps/Test.Test:PersistentLevel.Landscape_0) in /Game/Maps/Test, grow a temperate forest on `Moss` under /Game/HyperAI/PCG for PromptTests.\n")
			TEXT("Assets:\n/Game/Rocks/SM_Rock_A.SM_Rock_A\n/Game/Rocks/SM_Rock_B.SM_Rock_B\nPlan: {\"Kind\": \"landscape.create\"} and {{\"X\":1}} stay; so does {{unknown}}.")));
	TestTrue(TEXT("and nothing is left blank"), Blanks.IsEmpty());

	TestTrue(TEXT("What was typed comes before the selection"), Fill(Preset, Context, {{TEXT("selected_actor"), TEXT("MyLand")}}, Blanks).StartsWith(TEXT("On MyLand in")));
	TestTrue(TEXT("and before a default"), Fill(Preset, Context, {{TEXT("biome"), TEXT("desert")}}, Blanks).Contains(TEXT("grow a desert on")));
	TestTrue(TEXT("Blank typing does not count"), Fill(Preset, Context, {{TEXT("biome"), TEXT("  ")}}, Blanks).Contains(TEXT("grow a temperate forest")));

	const FString Empty = Fill(Preset, FPromptContext(), {}, Blanks);
	TestTrue(TEXT("A missing selection becomes [Label]: ") + Empty, Empty.StartsWith(TEXT("On [Selected actor] in [Level], grow a temperate forest")));
	TestTrue(TEXT("and is listed as a blank"), Blanks.Contains(TEXT("Selected actor")) && Blanks.Contains(TEXT("Level")) && Blanks.Contains(TEXT("Selected assets")));
	TestTrue(TEXT("The content folder falls back to /Game/HyperAI"), Empty.Contains(TEXT("under /Game/HyperAI/PCG")));
	const FString NoDefaults = Fill(Preset, Context, {}, Blanks, /*bUseDefaults=*/false);
	TestTrue(TEXT("Without defaults, an untyped field is a blank"), NoDefaults.Contains(TEXT("grow a [Biome] on `[Paint layer]`")) && Blanks == TArray<FString>{TEXT("Biome"), TEXT("Paint layer")});
	TestEqual(TEXT("The names a prompt uses, once each"), FString::Join(UsedNames(Preset.Prompt), TEXT(",")),
		FString(TEXT("selected_actor,level,biome,layer,content_folder,project,selected_assets,unknown")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsContentFolderTest,
	"HyperAIStudio.PromptPresets.ContentFolder",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsContentFolderTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresets;
	// Opening the picker while the Content Browser showed /All asserted in the engine's GetInternalPathString.
	TestEqual(TEXT("A Content Browser with no folder gives none"), ContentFolderOf(FContentBrowserItemPath()), FString());
	const FContentBrowserItemPath All(TEXT("/All"), EContentBrowserPathType::Virtual);
	TestFalse(TEXT("/All is a virtual folder with no content folder behind it"), All.HasInternalPath());
	TestEqual(TEXT("so it gives none, without asserting"), ContentFolderOf(All), FString());
	TestEqual(TEXT("A content folder comes through as it is"),
		ContentFolderOf(FContentBrowserItemPath(TEXT("/Game/Props"), EContentBrowserPathType::Internal)), FString(TEXT("/Game/Props")));
	const FPromptContext Context = CaptureContext();
	TestFalse(TEXT("The captured context always has a folder to write to"), Context.ContentFolder.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsProjectOverrideTest,
	"HyperAIStudio.PromptPresets.ProjectOverride",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsProjectOverrideTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	FScratch Scratch;
	const FString BuiltIn = WriteFixture(Scratch.Dir);
	const FString Root = FPaths::Combine(Scratch.Dir, TEXT("Project"));
	const FString Team = FPaths::Combine(ProjectDirectory(Root), TEXT("team.json"));
	auto TeamFile = [](const TCHAR* Title)
	{
		return FString::Printf(TEXT(R"JSON({"schema": "hyperai.prompt-presets.v1", "category": "team", "title": "Our team", "presets": [
			{"id": "pcg-foliage.forest-ecosystem", "title": "%s", "summary": "Our studio's forest.", "packs": ["pcg"], "difficulty": "standard",
			 "prompt": "Grow our studio's forest on {{selected_actor}} with hyper_pcg_scatter_apply_plan and report a PASS/FAIL table, as we always do."},
			{"id": "pcg-foliage.rock-scatter", "hidden": true},
			{"id": "team.door-check", "title": "Check every door", "summary": "Play the level and open every door.", "packs": ["pie_runtime"],
			 "difficulty": "starter", "flavour": "ours", "prompt": "Play {{level}} and walk through every door; report which ones do not open in a PASS/FAIL table."}]})JSON"), Title);
	};
	Write(Team, TeamFile(TEXT("Our forest")));
	Write(FPaths::Combine(ProjectDirectory(Root), TEXT("broken.json")), TEXT("{ \"schema\": \"hyperai.prompt-presets.v1\", "));

	const FLibrary Library = Load(BuiltIn, ProjectDirectory(Root));
	const FHyperAIStudioPromptPreset* Forest = Library.Find(TEXT("pcg-foliage.forest-ecosystem"));
	TestTrue(TEXT("A project preset replaces the built-in with its id, in its place"), Forest && Forest->Title == TEXT("Our forest")
		&& Forest->Category == TEXT("pcg-foliage") && !Forest->bBuiltIn);
	TestTrue(TEXT("and shows as written, untranslated"), Forest && Forest->DisplayTitle().ToString() == TEXT("Our forest"));
	TestNull(TEXT("A hidden one leaves the library"), Library.Find(TEXT("pcg-foliage.rock-scatter")));
	const FHyperAIStudioPromptPreset* Door = Library.Find(TEXT("team.door-check"));
	const FHyperAIStudioPromptCategory* Ours = Library.FindCategory(TEXT("team"));
	TestTrue(TEXT("A new one joins under the project's own category"), Door && Door->Category == TEXT("team") && Ours && Ours->Title == TEXT("Our team")
		&& Library.Categories.Last().Id == TEXT("team"));
	TestTrue(TEXT("A field HyperAI does not know is a warning in a project file"),
		Library.Warnings.ContainsByPredicate([](const FString& Warning) { return Warning.Contains(TEXT("team.door-check: flavour: unknown field")); }));
	TestTrue(TEXT("A broken file is an error that names it"),
		Library.Errors.Num() == 1 && Library.Errors[0].Contains(TEXT("broken.json")));
	TestNotNull(TEXT("and the built-ins still load"), Library.Find(TEXT("vfx-niagara.campfire")));
	FLibrary Listed;
	ParseCategories(TEXT(R"JSON({"schema": "hyperai.prompt-categories.v1", "categories": ["oops", {"id": "ours", "title": "Ours"}]})JSON"), TEXT("categories.json"),
		/*bBuiltIn=*/false, Listed);
	TestTrue(TEXT("A category entry that is not an object is its file's error, and the others still load"), Listed.Errors.Num() == 1
		&& Listed.Errors[0].StartsWith(TEXT("categories.json[0]")) && Listed.FindCategory(TEXT("ours")) != nullptr);

	// Kept until a file changes.
	SetBuiltInDirectoryForTests(BuiltIn);
	const FHyperAIStudioPromptPreset* Cached = Get(Root).Find(TEXT("pcg-foliage.forest-ecosystem"));
	TestTrue(TEXT("The library loads for the project"), Cached && Cached->Title == TEXT("Our forest"));
	Write(Team, TeamFile(TEXT("Our better forest")));
	IFileManager::Get().SetTimeStamp(*Team, FDateTime::UtcNow() + FTimespan::FromSeconds(10.0));
	Cached = Get(Root).Find(TEXT("pcg-foliage.forest-ecosystem"));
	TestTrue(TEXT("and loads again when a file's time stamp changes"), Cached && Cached->Title == TEXT("Our better forest"));
	IFileManager::Get().Delete(*FPaths::Combine(ProjectDirectory(Root), TEXT("broken.json")));
	TestEqual(TEXT("or a file goes"), Get(Root).Errors.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsSearchTest,
	"HyperAIStudio.PromptPresets.Search",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsSearchTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	FScratch Scratch;
	const FLibrary Library = Load(WriteFixture(Scratch.Dir), FString());
	TestEqual(TEXT("The fixture loads whole"), FString::Join(Library.Errors, TEXT("\n")), FString());
	auto IdsOf = [&Library](const TArray<int32>& Found)
	{
		TArray<FString> Ids;
		for (const int32 Index : Found)
		{
			Ids.Add(Library.Presets[Index].Id);
		}
		return Ids;
	};
	TestTrue(TEXT("'forest' finds the forest ecosystem in the top three"),
		IdsOf(Search(Library, TEXT("forest"), FString(), nullptr, 3)).Contains(TEXT("pcg-foliage.forest-ecosystem")));
	TestEqual(TEXT("'sparks and smoke' finds the campfire first"), FString::Join(IdsOf(Search(Library, TEXT("sparks and smoke"), FString(), nullptr, 1)), TEXT(",")),
		FString(TEXT("vfx-niagara.campfire")));
	TestTrue(TEXT("Packs and tags are searched too"), IdsOf(Search(Library, TEXT("niagara"), FString(), nullptr, 0)).Contains(TEXT("vfx-niagara.campfire")));
	TestTrue(TEXT("A category keeps the search inside it"), Search(Library, TEXT("forest"), TEXT("vfx-niagara"), nullptr, 0).IsEmpty());
	TestEqual(TEXT("With no words, a category lists all of its presets"), FString::Join(IdsOf(Search(Library, FString(), TEXT("pcg-foliage"), nullptr, 0)), TEXT(",")),
		FString(TEXT("pcg-foliage.forest-ecosystem,pcg-foliage.rock-scatter")));
	TestEqual(TEXT("The limit holds"), Search(Library, FString(), FString(), nullptr, 2).Num(), 2);
	TestTrue(TEXT("Nonsense finds nothing"), Search(Library, TEXT("qqzzxxvv"), FString(), nullptr, 0).IsEmpty());

	const FPromptContext Landscape = LandscapeContext();
	FPromptContext Nothing;
	const FHyperAIStudioPromptPreset& Forest = *Library.Find(TEXT("pcg-foliage.forest-ecosystem"));
	const FHyperAIStudioPromptPreset& Rocks = *Library.Find(TEXT("pcg-foliage.rock-scatter"));
	const FHyperAIStudioPromptPreset& Campfire = *Library.Find(TEXT("vfx-niagara.campfire"));
	TestTrue(TEXT("A preset that asks for a Landscape fits one selected"), FitsSelection(Forest, Landscape));
	TestFalse(TEXT("but not an empty selection"), FitsSelection(Forest, Nothing));
	FPromptContext Rock = Landscape;
	Rock.ActorClasses = {TEXT("StaticMeshActor,Actor")};
	TestFalse(TEXT("or another actor"), FitsSelection(Forest, Rock));
	TestTrue(TEXT("A preset that asks for static meshes fits the selected assets"), FitsSelection(Rocks, Landscape));
	Rock.AssetPaths.Reset();
	Rock.AssetClasses.Reset();
	TestFalse(TEXT("and not a selection of actors alone"), FitsSelection(Rocks, Rock));
	TestFalse(TEXT("A preset that works on no selection never claims one"), FitsSelection(Campfire, Landscape));
	FHyperAIStudioPromptPreset Two = Forest;
	Two.SelectionMin = 2;
	TestFalse(TEXT("A preset that needs two Landscapes does not fit one"), FitsSelection(Two, Landscape));

	// A tie on the words goes to what fits the selection.
	FLibrary Twins = Library;
	FHyperAIStudioPromptPreset Twin = Forest;
	// The same words as the forest, so only the selection tells them apart.
	Twin.Id = TEXT("pcg-foliage.forest-ecosystem-b");
	Twin.SelectionKind = TEXT("none");
	Twins.Presets.Insert(Twin, 0);
	auto FirstOf = [&Twins](const TArray<int32>& Found) { return Found.IsEmpty() ? FString() : Twins.Presets[Found[0]].Id; };
	TestEqual(TEXT("Without a selection the first listed wins a tie"), FirstOf(Search(Twins, TEXT("forest ecosystem"), FString(), nullptr, 1)),
		FString(TEXT("pcg-foliage.forest-ecosystem-b")));
	TestEqual(TEXT("A preset that fits the selection comes first among equals"), FirstOf(Search(Twins, TEXT("forest ecosystem"), FString(), &Landscape, 1)),
		FString(TEXT("pcg-foliage.forest-ecosystem")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsHintsTest,
	"HyperAIStudio.PromptPresets.Hints",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsHintsTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::PromptPresetsTests;
	FScratch Scratch;
	const FString Root = FPaths::Combine(Scratch.Dir, TEXT("Project"));
	Skills::InstallBuiltIns(Root, Skills::BuiltInDirectory());
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bWasHanding = Settings->bHandTasksToProjectAgents;
	Settings->bHandTasksToProjectAgents = true;
	ON_SCOPE_EXIT
	{
		Settings->bHandTasksToProjectAgents = bWasHanding;
	};

	TestTrue(TEXT("The words alone name no pack"), TaskRouter::PacksFor(TEXT("do the thing")).IsEmpty());
	FHyperAIStudioMessageHints Hints;
	Hints.PresetId = TEXT("pcg-foliage.forest-ecosystem");
	Hints.Packs = {TEXT("pcg")};
	Hints.Tools = {TEXT("hyper_pcg_validate")};
	Hints.Skills = {TEXT("unreal-level-design")};
	TSet<FString> Given;
	FString Summary;
	const FString Context = FHyperAIStudioNativeChat::ContextFor(Root, TEXT("do the thing"), Hints, TEXT("Claude Code"), FString(), Given, Summary);
	TestTrue(TEXT("The preset's skill goes with it"), Given.Contains(TEXT("skill:unreal-level-design")) && Context.Contains(TEXT("`unreal-level-design`")));
	if (PcgToolsEnabled())
	{
		TestTrue(TEXT("and so do its pack's tools: ") + Context, Given.Contains(TEXT("tools:pcg")) && Context.Contains(TEXT("<tools>")) && Context.Contains(TEXT("hyper_pcg")));
		// Read, edit, check is the order otherwise: the tool the prompt names goes before the pack's read.
		const int32 NamedLine = Context.Find(TEXT("`hyper_pcg_validate` (toolset_name"));
		TestTrue(TEXT("the one its prompt names first"), NamedLine != INDEX_NONE && NamedLine < Context.Find(TEXT("`hyper_pcg_inspect`")));
		TestFalse(TEXT("and the line under it counts no tool as a note: ") + Summary, Summary.Contains(TEXT("project memory")));
	}
	else
	{
		AddWarning(TEXT("The pcg pack is not loaded in this editor, so no pcg tool block can be checked."));
	}
	TestTrue(TEXT("The line under the message names the preset: ") + Summary, Summary.StartsWith(TEXT("preset:pcg-foliage.forest-ecosystem")));
	TSet<FString> Plain;
	FString PlainSummary;
	FHyperAIStudioNativeChat::ContextFor(Root, TEXT("do the thing"), TEXT("Claude Code"), FString(), Plain, PlainSummary);
	TestFalse(TEXT("Without hints the words decide, and they name nothing"), Plain.Contains(TEXT("tools:pcg")) || Plain.Contains(TEXT("skill:unreal-level-design")));

	// The agent a preset names, when the project has it.
	Agents::InstallTeam(Root, /*bUpdateOnly=*/false);
	FHyperAIStudioMessageHints Tester;
	Tester.Agent = TEXT("qa-tester");
	TSet<FString> TesterGiven;
	const FString HandOff = FHyperAIStudioNativeChat::ContextFor(Root, TEXT("Light the hall at dusk"), Tester, TEXT("Claude Code"), FString(), TesterGiven, Summary);
	TestTrue(TEXT("A preset's agent takes the task whatever the words say: ") + HandOff, TesterGiven.Contains(TEXT("agent:qa-tester")) && HandOff.Contains(TEXT("`qa-tester`")));
	TSet<FString> AsAgent;
	FHyperAIStudioNativeChat::ContextFor(Root, TEXT("Light the hall at dusk"), Tester, TEXT("Claude Code"), TEXT("lighting-artist"), AsAgent, Summary);
	TestFalse(TEXT("A chat started as a project agent keeps the task"), AsAgent.Contains(TEXT("agent:qa-tester")));
	FHyperAIStudioMessageHints Team;
	Team.Agent = TEXT("team");
	TSet<FString> TeamGiven;
	TestTrue(TEXT("The team takes a team preset where it is installed"), FHyperAIStudioNativeChat::ContextFor(Root, TEXT("Build the arena"), Team,
		TEXT("Codex"), FString(), TeamGiven, Summary).Contains(TEXT("`team` skill")) && TeamGiven.Contains(TEXT("agent:team")));
	TSet<FString> ToTeam;
	FHyperAIStudioNativeChat::ContextFor(Root, Agents::TeamRequest(TEXT("Codex"), TEXT("Build the arena")), Team, TEXT("Codex"), FString(), ToTeam, Summary);
	TestFalse(TEXT("but not a request already sent to the team"), ToTeam.Contains(TEXT("agent:team")));
	FHyperAIStudioMessageHints Stranger;
	Stranger.Agent = TEXT("stranger");
	TSet<FString> Fallback;
	FHyperAIStudioNativeChat::ContextFor(Root, TEXT("Playtest the level and check the doors open"), Stranger, TEXT("Claude Code"), FString(), Fallback, Summary);
	TestTrue(TEXT("An agent the project lacks falls back to the pick by the words"), Fallback.Contains(TEXT("agent:qa-tester")));

	const FString Bare = FPaths::Combine(Scratch.Dir, TEXT("Bare"));
	TSet<FString> NoTeam;
	FHyperAIStudioNativeChat::ContextFor(Bare, TEXT("Build the arena"), Team, TEXT("Codex"), FString(), NoTeam, Summary);
	TestFalse(TEXT("Without the team in the project, a team preset hands nothing off"), NoTeam.Contains(TEXT("agent:team")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsQueuedTest,
	"HyperAIStudio.PromptPresets.QueuedKeepsHints",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsQueuedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::PromptPresetsTests;
	FScratch Scratch;
	const FString Root = FPaths::Combine(Scratch.Dir, TEXT("Project"));
	Skills::InstallBuiltIns(Root, Skills::BuiltInDirectory());
	FHyperAIStudioNativeChat::SetContextInTestsForTests(true);
	ON_SCOPE_EXIT
	{
		FHyperAIStudioNativeChat::SetContextInTestsForTests(false);
	};
	const TSharedRef<FFakeChat> Chat = MakeShared<FFakeChat>(Root);

	FHyperAIStudioMessageHints Forest;
	Forest.PresetId = TEXT("pcg-foliage.forest-ecosystem");
	Forest.Packs = {TEXT("pcg")};
	Forest.Skills = {TEXT("unreal-level-design")};
	Chat->Send(TEXT("do the thing"), {}, Forest);
	TestEqual(TEXT("A message before the agent is ready waits"), Chat->NumQueued(), 1);
	Chat->MakeReady();
	TestTrue(TEXT("Once ready it goes, with its preset's skill"), Chat->Sent.Num() == 1 && Chat->Sent[0].StartsWith(TEXT("do the thing"))
		&& Chat->Sent[0].Contains(TEXT("`unreal-level-design`")));
	TestTrue(TEXT("and the line naming its preset: ") + Chat->LastSummary(), Chat->LastSummary().Contains(TEXT("preset:pcg-foliage.forest-ecosystem")));

	FHyperAIStudioMessageHints Fire;
	Fire.PresetId = TEXT("vfx-niagara.campfire");
	Fire.Packs = {TEXT("niagara_vfx")};
	Fire.Skills = {TEXT("unreal-niagara")};
	Chat->Send(TEXT("and the other thing"), {}, Fire);
	TestEqual(TEXT("A message during a turn waits for it"), Chat->NumQueued(), 1);
	Chat->FinishTurn();
	TestTrue(TEXT("When the turn ends it goes with its own preset's skill"), Chat->Sent.Num() == 2 && Chat->Sent[1].StartsWith(TEXT("and the other thing"))
		&& Chat->Sent[1].Contains(TEXT("`unreal-niagara`")) && !Chat->Sent[1].Contains(TEXT("`unreal-level-design`")));
	TestTrue(TEXT("and its line: ") + Chat->LastSummary(), Chat->LastSummary().Contains(TEXT("preset:vfx-niagara.campfire")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsLocalizedTest,
	"HyperAIStudio.PromptPresets.Localized",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsLocalizedTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	namespace L10n = HyperAIStudio::Localization;
	FScratch Scratch;
	const FLibrary Fixture = Load(WriteFixture(Scratch.Dir), FString());
	const FHyperAIStudioPromptPreset& Forest = *Fixture.Find(TEXT("pcg-foliage.forest-ecosystem"));
	const FString Folder = FPaths::Combine(Scratch.Dir, TEXT("Localization"));
	Write(FPaths::Combine(Folder, TEXT("zz.json")), TEXT(R"JSON({"culture": "zz", "entries": [
		{"namespace": "HyperAIStudioPrompts", "key": "pcg-foliage.forest-ecosystem.title", "source": "Forest ecosystem on a paint layer", "text": "Foret sur un calque"},
		{"namespace": "HyperAIStudioPrompts", "key": "pcg-foliage.forest-ecosystem.ph.layer.label", "source": "Paint layer", "text": "Calque"},
		{"namespace": "HyperAIStudioPrompts", "key": "category.pcg-foliage", "source": "PCG and foliage", "text": "PCG et feuillage"}]})JSON"));
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const FString Saved = Settings->UILanguage;
	L10n::SetDirectoryForTests(Folder);
	Settings->UILanguage = TEXT("zz");
	L10n::Refresh();
	FTextLocalizationManager::Get().WaitForAsyncTasks();
	TestEqual(TEXT("A built-in preset's title shows in HyperAI's language"), Forest.DisplayTitle().ToString(), FString(TEXT("Foret sur un calque")));
	TestEqual(TEXT("so does a field's label"), Forest.DisplayLabel(Forest.Placeholders[0]).ToString(), FString(TEXT("Calque")));
	TestEqual(TEXT("and a category's title"), Fixture.FindCategory(TEXT("pcg-foliage"))->DisplayTitle().ToString(), FString(TEXT("PCG et feuillage")));
	TestEqual(TEXT("An untranslated summary stays English"), Forest.DisplaySummary().ToString(), Forest.Summary);
	TArray<FString> Blanks;
	TestTrue(TEXT("A blank reads in the shown language too"), Fill(Forest, FPromptContext(), {}, Blanks, /*bUseDefaults=*/false).Contains(TEXT("[Calque]")));
	Settings->UILanguage = Saved;
	L10n::SetDirectoryForTests(FString());
	L10n::Refresh();
	FTextLocalizationManager::Get().WaitForAsyncTasks();
	TestEqual(TEXT("and English comes back"), Forest.DisplayTitle().ToString(), Forest.Title);

	// The shipped translation covers the whole shipped library.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	TArray<L10n::FEntry> Entries;
	FString Error;
	if (!TestTrue(TEXT("The shipped zh-Hans translation loads"), Plugin.IsValid()
		&& L10n::LoadEntries(FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Localization"), TEXT("zh-Hans.json")), Entries, Error)))
	{
		return false;
	}
	TMap<FString, L10n::FEntry> Translated;
	for (const L10n::FEntry& Entry : Entries)
	{
		if (Entry.Namespace == TEXT("HyperAIStudioPrompts"))
		{
			Translated.Add(Entry.Key, Entry);
		}
	}
	// An entry for today's English, with text: an empty one would show the English in a Chinese picker.
	auto Expect = [this, &Translated](const FString& Key, const FString& Source)
	{
		const L10n::FEntry* Found = Translated.Find(Key);
		TestTrue(FString::Printf(TEXT("zh-Hans translates %s"), *Key), Found && Found->Source == Source && !Found->Text.TrimStartAndEnd().IsEmpty());
	};
	const FLibrary Shipped = Load(BuiltInDirectory(), FString());
	for (const FHyperAIStudioPromptCategory& Category : Shipped.Categories)
	{
		Expect(TEXT("category.") + Category.Id, Category.Title);
	}
	for (const FHyperAIStudioPromptPreset& Preset : Shipped.Presets)
	{
		Expect(Preset.Id + TEXT(".title"), Preset.Title);
		Expect(Preset.Id + TEXT(".summary"), Preset.Summary);
		for (const FHyperAIStudioPromptPlaceholder& Placeholder : Preset.Placeholders)
		{
			Expect(FString::Printf(TEXT("%s.ph.%s.label"), *Preset.Id, *Placeholder.Name), Placeholder.Label);
			if (!Placeholder.Hint.IsEmpty())
			{
				Expect(FString::Printf(TEXT("%s.ph.%s.hint"), *Preset.Id, *Placeholder.Name), Placeholder.Hint);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsRouterAgreementTest,
	"HyperAIStudio.PromptPresets.RouterAgreement",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsRouterAgreementTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::PromptPresetsTests;
	// Informational: the router's guess from a filled prompt's words against the packs the preset names, counted as the
	// router's tuning is: agreement, no pick at all, a foundation pack (the playtest and capture tools every prompt's
	// proof steps name), and a wrong pick. The mismatches are a held-out set for tuning the router's words. A preset's
	// message never depends on this guess: it names its packs.
	const FLibrary Library = Load(BuiltInDirectory(), FString());
	int32 Agree = 0;
	int32 None = 0;
	TArray<FString> Foundation;
	TArray<FString> Wrong;
	for (const FHyperAIStudioPromptPreset& Preset : Library.Presets)
	{
		TArray<FString> Blanks;
		const TArray<TaskRouter::FPackFit> Packs = TaskRouter::PacksFor(Fill(Preset, FPromptContext(), {}, Blanks));
		const FString Line = Packs.IsEmpty() ? FString()
			: FString::Printf(TEXT("%s: router %s, preset %s"), *Preset.Id, *Packs[0].PackId, *FString::Join(Preset.Packs, TEXT(",")));
		if (Packs.IsEmpty())
		{
			++None;
		}
		else if (Preset.Packs.Contains(Packs[0].PackId))
		{
			++Agree;
		}
		else if (Algo::FindByPredicate(FoundationPacks, [&Packs](const TCHAR* Pack) { return Packs[0].PackId == Pack; }))
		{
			Foundation.Add(Line);
		}
		else
		{
			Wrong.Add(Line);
		}
	}
	const int32 Total = Library.Presets.Num();
	AddInfo(FString::Printf(TEXT("Router agreement: %d agree, %d no pick, %d foundation pack, %d wrong of %d presets."), Agree, None, Foundation.Num(),
		Wrong.Num(), Total));
	for (const FString& Line : Foundation)
	{
		AddInfo(TEXT("Foundation pick: ") + Line);
	}
	for (const FString& Line : Wrong)
	{
		AddInfo(TEXT("Wrong pick: ") + Line);
	}
	// The aim is at most one wrong pick in ten; the floor only catches the router getting worse.
	if (Wrong.Num() * 10 > Total)
	{
		AddWarning(FString::Printf(TEXT("The router picks another pack than the preset's for %d of %d presets, above the 10%% aim; the lines above are its tuning set."),
			Wrong.Num(), Total));
	}
	TestTrue(FString::Printf(TEXT("Wrong picks stay at or below 20%% (%d of %d)"), Wrong.Num(), Total), Wrong.Num() * 5 <= Total);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsPickerTest,
	"HyperAIStudio.PromptPresets.Picker",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsPickerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio::PromptPresetsTests;
	// It builds with whatever the plugin ships.
	const TSharedRef<SHyperAIStudioPromptPicker> Shipped = SNew(SHyperAIStudioPromptPicker).Context(FPromptContext());
	TestEqual(TEXT("The shipped library's presets all show"), Shipped->NumShown(), Load(BuiltInDirectory(), FString()).Presets.Num());

	FScratch Scratch;
	SetBuiltInDirectoryForTests(WriteFixture(Scratch.Dir));
	const FString Root = FPaths::Combine(Scratch.Dir, TEXT("Project"));
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	const TArray<FString> Recent = Prefs->RecentPromptPresets;
	ON_SCOPE_EXIT
	{
		Prefs->RecentPromptPresets = Recent;
		Prefs->SaveConfig();
	};
	Prefs->RecentPromptPresets = {TEXT("vfx-niagara.campfire")};

	const FHyperAIStudioPromptPreset* Chosen = nullptr;
	FString ChosenText;
	TArray<FString> ChosenBlanks;
	FHyperAIStudioPromptPreset ChosenCopy;
	const TSharedRef<SHyperAIStudioPromptPicker> Picker = SNew(SHyperAIStudioPromptPicker)
		.ProjectRoot(Root)
		.Context(LandscapeContext())
		.OnPresetChosen_Lambda([&](const FHyperAIStudioPromptPreset& Preset, const FString& Text, const TArray<FString>& Blanks)
		{
			ChosenCopy = Preset;
			Chosen = &ChosenCopy;
			ChosenText = Text;
			ChosenBlanks = Blanks;
		});
	TestTrue(TEXT("With a Landscape and meshes selected, it opens on what fits them"), Picker->NumShown() == 2 && Picker->GetShown(0)->Id == TEXT("pcg-foliage.forest-ecosystem")
		&& Picker->GetShown(1)->Id == TEXT("pcg-foliage.rock-scatter"));
	HyperAIStudio::Tests::SaveWidgetShot(Picker, FIntPoint(720, 460), TEXT("prompt-picker"));
	Picker->SetCategory(SHyperAIStudioPromptPicker::RecentCategory);
	TestTrue(TEXT("Recent lists what was used last"), Picker->NumShown() == 1 && Picker->GetShown(0)->Id == TEXT("vfx-niagara.campfire"));
	// A hand-edited, merged or copied ini can list a preset twice. Listed twice, Slate's list view asserts once it builds
	// its rows, which the drawing below does.
	Prefs->RecentPromptPresets = {TEXT("vfx-niagara.campfire"), TEXT("VFX-Niagara.Campfire"), TEXT("vfx-niagara.campfire")};
	TestEqual(TEXT("Recent reads a preset listed twice once"), HyperAIStudio::PromptPresets::Recent().Num(), 1);
	Picker->SetCategory(FString());
	Picker->SetCategory(SHyperAIStudioPromptPicker::RecentCategory);
	TestEqual(TEXT("and the Recent list shows it once"), Picker->NumShown(), 1);
	HyperAIStudio::Tests::SaveWidgetShot(Picker, FIntPoint(720, 460), TEXT("prompt-picker-recent"));
	Prefs->RecentPromptPresets = {TEXT("vfx-niagara.campfire")};
	Picker->SetCategory(FString());
	TestEqual(TEXT("All shows every preset"), Picker->NumShown(), 4);
	Picker->SetSearchText(TEXT("niagara"));
	TestTrue(TEXT("Typing 'niagara' narrows the list to the VFX presets"), Picker->NumShown() >= 1 && Picker->GetShown(0)->Category == TEXT("vfx-niagara"));
	for (int32 Index = 0; Index < Picker->NumShown(); ++Index)
	{
		TestEqual(TEXT("Only VFX presets are left"), Picker->GetShown(Index)->Category, FString(TEXT("vfx-niagara")));
	}
	TestTrue(TEXT("Enter picks the selected preset"), Picker->Choose());
	TestTrue(TEXT("with its prompt filled in"), Chosen && Chosen->Id == TEXT("vfx-niagara.campfire") && ChosenText.StartsWith(TEXT("Build a looping campfire Niagara system under /Game/HyperAI/VFX")));

	Picker->SetSearchText(TEXT("forest"));
	Picker->Choose(/*bUseDefaults=*/false);
	TestTrue(TEXT("Insert and edit blanks leaves untyped fields as blanks"), ChosenBlanks == TArray<FString>{TEXT("Biome"), TEXT("Paint layer")}
		&& ChosenText.Contains(TEXT("grow a [Biome] ecosystem")) && ChosenText.StartsWith(TEXT("On Landscape (")));

	// Into the composer: the text, the first blank, the line saying what is left, and the hints for the send.
	const TSharedRef<FFakeChat> Chat = MakeShared<FFakeChat>(Root);
	const TSharedRef<SHyperAIStudioNativeChatView> View = SNew(SHyperAIStudioNativeChatView).Chat(Chat);
	View->InsertPreset(ChosenCopy, ChosenText, ChosenBlanks);
	// The composer's lines come back with the platform's line ends.
	auto Composed = [&View]() { return View->GetComposerText().Replace(TEXT("\r\n"), TEXT("\n")); };
	TestEqual(TEXT("The composer holds the prompt"), Composed(), ChosenText);
	TestEqual(TEXT("The line under it lists the blanks"), View->GetNotice(), FString(TEXT("Fill in: [Biome], [Paint layer]")));
	const FHyperAIStudioMessageHints Hints = View->GetPendingHints();
	TestTrue(TEXT("The next message goes with the preset's packs, tools, skills, agent and route"), Hints.PresetId == TEXT("pcg-foliage.forest-ecosystem")
		&& Hints.Packs == ChosenCopy.Packs && Hints.Tools == ChosenCopy.Tools && Hints.Skills == ChosenCopy.Skills && Hints.Agent == TEXT("level-designer")
		&& Hints.Route == TEXT("Level Design"));
	const TArray<FString> Used = HyperAIStudio::PromptPresets::Recent();
	TestTrue(TEXT("and it is first in Recent"), Used.Num() > 0 && Used[0] == TEXT("pcg-foliage.forest-ecosystem"));
	View->InsertPreset(ChosenCopy, TEXT("Second."));
	TestEqual(TEXT("A second prompt follows the first after a blank line"), Composed(), ChosenText + TEXT("\n\nSecond."));
	TestFalse(TEXT("Team stays off for a preset that is not the team's"), View->IsTeam());
	const FLibrary Fixture = Load(BuiltInDirectory(), FString());
	if (!TestNotNull(TEXT("The fixture has the team's preset"), Fixture.Find(TEXT("team-builds.arena"))))
	{
		return false;
	}
	View->InsertPreset(*Fixture.Find(TEXT("team-builds.arena")), TEXT("Build the arena."));
	TestTrue(TEXT("A team preset turns Team on, and says so"), View->IsTeam() && View->GetNotice().Contains(TEXT("Team is on")));
	HyperAIStudio::Tests::SaveWidgetShot(View, FIntPoint(900, 420), TEXT("prompt-inserted"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioPromptPresetsComposerTest,
	"HyperAIStudio.PromptPresets.Composer",
	HyperAIStudio::PromptPresetsTests::Flags)

bool FHyperAIStudioPromptPresetsComposerTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using namespace HyperAIStudio;
	using namespace HyperAIStudio::PromptPresetsTests;
	FScratch Scratch;
	SetBuiltInDirectoryForTests(WriteFixture(Scratch.Dir));
	const FString Root = FPaths::Combine(Scratch.Dir, TEXT("Project"));
	Skills::InstallBuiltIns(Root, Skills::BuiltInDirectory());
	Agents::InstallTeam(Root, /*bUpdateOnly=*/false);
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bWasHanding = Settings->bHandTasksToProjectAgents;
	Settings->bHandTasksToProjectAgents = true;
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	const TArray<FString> Recent = Prefs->RecentPromptPresets;
	FHyperAIStudioNativeChat::SetContextInTestsForTests(true);
	ON_SCOPE_EXIT
	{
		Settings->bHandTasksToProjectAgents = bWasHanding;
		Prefs->RecentPromptPresets = Recent;
		Prefs->SaveConfig();
		FHyperAIStudioNativeChat::SetContextInTestsForTests(false);
	};
	const FLibrary Fixture = Load(BuiltInDirectory(), FString());
	const FHyperAIStudioPromptPreset* Forest = Fixture.Find(TEXT("pcg-foliage.forest-ecosystem"));
	const FHyperAIStudioPromptPreset* Arena = Fixture.Find(TEXT("team-builds.arena"));
	if (!TestTrue(TEXT("The fixture has its presets"), Forest && Arena))
	{
		return false;
	}

	// The composer as the user drives it: Enter on its text, and its Team switch.
	const TSharedRef<FFakeChat> Chat = MakeShared<FFakeChat>(Root);
	Chat->MakeReady();
	const TSharedRef<SHyperAIStudioNativeChatView> View = SNew(SHyperAIStudioNativeChatView).Chat(Chat);
	const TSharedPtr<SWidget> Box = FindWidget(View, TEXT("SMultiLineEditableTextBox"));
	const TSharedPtr<SWidget> Typing = Box.IsValid() ? FindWidget(Box.ToSharedRef(), TEXT("SMultiLineEditableText")) : nullptr;
	if (!TestTrue(TEXT("The view has its composer"), Typing.IsValid()))
	{
		return false;
	}
	auto Enter = [&Typing]()
	{
		Typing->OnKeyDown(FGeometry(), FKeyEvent(EKeys::Enter, FModifierKeysState(), 0, /*bIsRepeat=*/false, 0, 0));
	};
	auto Checks = [&View]()
	{
		TArray<TSharedRef<SWidget>> Found;
		FindWidgets(View, TEXT("SCheckBox"), Found);
		TArray<TSharedRef<SCheckBox>> Boxes;
		for (const TSharedRef<SWidget>& Each : Found)
		{
			Boxes.Add(StaticCastSharedRef<SCheckBox>(Each));
		}
		return Boxes;
	};

	View->InsertPreset(*Forest, TEXT("Grow a [Biome] forest on the [Paint layer] layer."), {TEXT("Biome"), TEXT("Paint layer")});
	Enter();
	TestTrue(TEXT("The first send with blanks left only says which: ") + View->GetNotice(), Chat->Sent.IsEmpty()
		&& View->GetNotice().Contains(TEXT("[Biome], [Paint layer]")));
	Enter();
	TestTrue(TEXT("The second sends it as it is, with the preset's skill"), Chat->Sent.Num() == 1
		&& Chat->Sent[0].StartsWith(TEXT("Grow a [Biome] forest")) && Chat->Sent[0].Contains(TEXT("`unreal-level-design`")));
	TestTrue(TEXT("and the composer forgets the preset"), View->GetComposerText().IsEmpty() && View->GetPendingHints().IsEmpty() && View->GetNotice().IsEmpty());
	Chat->FinishTurn();

	// A team preset turns Team on; typed away, it takes Team and its line with it.
	View->InsertPreset(*Arena, TEXT("Do the thing."));
	TestTrue(TEXT("A team preset turns Team on"), View->IsTeam());
	StaticCastSharedPtr<SMultiLineEditableTextBox>(Box)->SetText(FText::GetEmpty());
	TestTrue(TEXT("Typing the composer empty forgets the preset, the Team it turned on and its line"), !View->IsTeam() && View->GetNotice().IsEmpty()
		&& View->GetPendingHints().IsEmpty());

	// Team switched off by hand: the message stays with this agent, with no hand-off to the team.
	const TArray<TSharedRef<SCheckBox>> Before = Checks();
	TArray<bool> WasChecked;
	for (const TSharedRef<SCheckBox>& Each : Before)
	{
		WasChecked.Add(Each->IsChecked());
	}
	View->InsertPreset(*Arena, TEXT("Do the thing."));
	TSharedPtr<SCheckBox> TeamSwitch;
	for (int32 Index = 0; Index < Before.Num(); ++Index)
	{
		TeamSwitch = !WasChecked[Index] && Before[Index]->IsChecked() ? TSharedPtr<SCheckBox>(Before[Index]) : TeamSwitch;
	}
	if (!TestTrue(TEXT("The Team switch shows it on"), TeamSwitch.IsValid()))
	{
		return false;
	}
	TeamSwitch->ToggleCheckedState();
	TestFalse(TEXT("and the user turns it off"), View->IsTeam());
	Enter();
	TestTrue(TEXT("The message goes to this agent"), Chat->Sent.Num() == 2 && Chat->Sent[1].StartsWith(TEXT("Do the thing.")));
	TestFalse(TEXT("without the team's hand-off: ") + Chat->LastSummary(), Chat->Sent.Num() == 2 && (Chat->Sent[1].Contains(TEXT("`team` skill"))
		|| Chat->LastSummary().Contains(TEXT("HyperAI's team"))));
	TestTrue(TEXT("but still named for its preset"), Chat->LastSummary().Contains(TEXT("preset:team-builds.arena")));
	Chat->FinishTurn();

	// A prompt file that cannot be read is said under the list, naming the file.
	Write(FPaths::Combine(ProjectDirectory(Root), TEXT("broken.json")), TEXT("{ \"schema\": "));
	const TSharedRef<SHyperAIStudioPromptPicker> Picker = SNew(SHyperAIStudioPromptPicker).ProjectRoot(Root).Context(FPromptContext());
	TArray<TSharedRef<SWidget>> Lines;
	FindWidgets(Picker, TEXT("STextBlock"), Lines);
	TestTrue(TEXT("The picker says which prompt file it could not read"), Lines.ContainsByPredicate([](const TSharedRef<SWidget>& Each)
	{
		const TSharedRef<STextBlock> Line = StaticCastSharedRef<STextBlock>(Each);
		return Line->GetVisibility().IsVisible() && Line->GetText().ToString().Contains(TEXT("broken.json"));
	}));
	return true;
}

#endif
