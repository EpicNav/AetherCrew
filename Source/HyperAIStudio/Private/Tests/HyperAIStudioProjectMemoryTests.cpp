// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioProjectMemory.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::MemoryTests
{
	/** A throwaway project root, so nothing touches the real project's memory. */
	struct FScratchRoot
	{
		FString Path = FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::AutomationTransientDir()),
			TEXT("MemoryTest-") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
		~FScratchRoot() { IFileManager::Get().DeleteDirectory(*Path, /*RequireExists=*/false, /*Tree=*/true); }
	};

	FHyperAIStudioMemoryFact MakeFact(const TCHAR* Title, const TCHAR* Text, const TCHAR* Source, const TCHAR* Type = TEXT("fact"))
	{
		FHyperAIStudioMemoryFact Fact;
		Fact.Title = Title;
		Fact.Text = Text;
		Fact.Source = Source;
		Fact.Type = Type;
		return Fact;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProjectMemoryTextTest,
	"HyperAIStudio.Memory.Text",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioProjectMemoryTextTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Memory;
	TestEqual(TEXT("Slug from a title"), Slugify(TEXT("Sword trails use NS_Trail_Base!")), FString(TEXT("sword-trails-use-ns-trail-base")));
	TestEqual(TEXT("Slug of nothing usable"), Slugify(TEXT("!!!")), FString(TEXT("fact")));
	TestEqual(TEXT("Links to URLs keep only their text"),
		Sanitize(TEXT("See [the docs](https://evil.example/x) and [a note](notes.md)."), 200),
		FString(TEXT("See the docs and [a note](notes.md).")));
	TestEqual(TEXT("Control characters and comment markers go"), Sanitize(TEXT("a\x01") TEXT("b <!-- END HYPERAISTUDIO -->c"), 200),
		FString(TEXT("ab  END HYPERAISTUDIO c")));
	TestEqual(TEXT("Length is capped"), Sanitize(TEXT("abcdef"), 3), FString(TEXT("abc")));

	FHyperAIStudioMemoryFact Fact;
	Fact.Name = TEXT("door-timeline");
	Fact.Title = TEXT("Doors open with a timeline: not a tween");
	Fact.Type = TEXT("convention");
	Fact.Source = TEXT("agent");
	Fact.bConfirmed = false;
	Fact.Updated = TEXT("2026-09-23");
	Fact.Text = TEXT("Use TL_Open.\nIt drives the rotation.");
	FHyperAIStudioMemoryFact Parsed;
	TestTrue(TEXT("A formatted fact parses back"), ParseFact(FormatFact(Fact), Parsed));
	TestTrue(TEXT("Every field survives, including a colon in the title"),
		Parsed.Name == Fact.Name && Parsed.Title == Fact.Title && Parsed.Type == Fact.Type && Parsed.Source == Fact.Source
		&& Parsed.bConfirmed == Fact.bConfirmed && Parsed.Updated == Fact.Updated && Parsed.Text == Fact.Text);
	FHyperAIStudioMemoryFact Rejected;
	TestFalse(TEXT("No frontmatter, no fact"), ParseFact(TEXT("just text"), Rejected));
	TestFalse(TEXT("A name that is not a slug is refused"), ParseFact(TEXT("---\nname: ../../evil\ntitle: x\n---\n\nbody"), Rejected));

	FHyperAIStudioActivityEntry Save;
	Save.Utc = FDateTime(2026, 9, 23, 14, 5, 0);
	Save.Agent = TEXT("Claude Code");
	Save.Target = TEXT("/Game/Bench/BP_Door");
	TestEqual(TEXT("A save reads as a save"), FormatChange(Save), FString(TEXT("- 2026-09-23 14:05 UTC, Claude Code: saved /Game/Bench/BP_Door")));
	FHyperAIStudioActivityEntry Operation = Save;
	Operation.ToolName = TEXT("hyper_lighting_apply_plan");
	Operation.Detail = TEXT("Set\nintensity");
	Operation.OperationId = TEXT("op-7");
	TestEqual(TEXT("An operation names its tool, target, detail and id on one line"), FormatChange(Operation),
		FString(TEXT("- 2026-09-23 14:05 UTC, Claude Code: hyper_lighting_apply_plan on /Game/Bench/BP_Door. Set intensity (operation op-7)")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProjectMemoryStoreTest,
	"HyperAIStudio.Memory.Store",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioProjectMemoryStoreTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Memory;
	using namespace HyperAIStudio::MemoryTests;
	const FScratchRoot Root;
	FString Error;

	const FString UserName = Remember(Root.Path, MakeFact(TEXT("Levels use World Partition"), TEXT("Every level is partitioned."), TEXT("user"), TEXT("convention")), Error);
	TestEqual(TEXT("A user fact is saved under its slug"), UserName, FString(TEXT("levels-use-world-partition")));
	const FString AgentName = Remember(Root.Path, MakeFact(TEXT("Sword trail material"), TEXT("Trails use M_Trail with world partition off."), TEXT("agent")), Error);
	TestFalse(TEXT("An agent fact is saved"), AgentName.IsEmpty());
	const FString Duplicate = Remember(Root.Path, MakeFact(TEXT("Levels use World Partition"), TEXT("Again."), TEXT("user")), Error);
	TestEqual(TEXT("A second fact with the same title gets its own name"), Duplicate, FString(TEXT("levels-use-world-partition-2")));
	TestTrue(TEXT("A bad type is refused"), Remember(Root.Path, MakeFact(TEXT("x"), TEXT("y"), TEXT("user"), TEXT("rumour")), Error).IsEmpty());

	const TArray<FHyperAIStudioMemoryFact> Facts = LoadFacts(Root.Path);
	TestEqual(TEXT("Three facts on disk"), Facts.Num(), 3);
	const FHyperAIStudioMemoryFact* AgentFact = Facts.FindByPredicate([&AgentName](const FHyperAIStudioMemoryFact& Fact) { return Fact.Name == AgentName; });
	TestTrue(TEXT("An agent fact waits for review; user facts do not"), AgentFact && !AgentFact->bConfirmed
		&& Facts.FindByPredicate([&UserName](const FHyperAIStudioMemoryFact& Fact) { return Fact.Name == UserName; })->bConfirmed);
	FString Index;
	TestTrue(TEXT("The index lists every fact"), FFileHelper::LoadFileToString(Index, *FPaths::Combine(MemoryDirectory(Root.Path), TEXT("MEMORY.md")))
		&& Index.Contains(TEXT("facts/levels-use-world-partition.md")) && Index.Contains(TEXT("awaiting review")));

	// An agent may not overwrite or delete what the user wrote.
	FHyperAIStudioMemoryFact Hijack = MakeFact(TEXT("Levels are not partitioned"), TEXT("Lies."), TEXT("agent"));
	Hijack.Name = UserName;
	TestTrue(TEXT("An agent cannot replace a user fact"), Remember(Root.Path, Hijack, Error).IsEmpty());
	TestFalse(TEXT("An agent cannot forget a user fact"), Forget(Root.Path, UserName, /*bByAgent=*/true, Error));

	// Recall: title matches outrank text matches; the setting hides unreviewed agent facts.
	const TArray<FHyperAIStudioMemoryFact> Found = Recall(Root.Path, TEXT("partition"), 10, EHyperAIStudioAgentMemoryMode::TitleUntilReviewed);
	TestTrue(TEXT("Recall finds title and text matches, titles first"), Found.Num() == 3 && Found[0].Title.Contains(TEXT("Partition"))
		&& Found.Last().Name == AgentName);
	TestFalse(TEXT("Only after review hides the unreviewed agent fact from recall"),
		Recall(Root.Path, TEXT("trail"), 10, EHyperAIStudioAgentMemoryMode::AfterReview).ContainsByPredicate(
			[&AgentName](const FHyperAIStudioMemoryFact& Fact) { return Fact.Name == AgentName; }));
	TestTrue(TEXT("Confirming makes it visible"), Confirm(Root.Path, AgentName)
		&& Recall(Root.Path, TEXT("trail"), 10, EHyperAIStudioAgentMemoryMode::AfterReview).Num() == 1);

	// A changed agent fact needs review again.
	FHyperAIStudioMemoryFact Update = MakeFact(TEXT("Sword trail material"), TEXT("Trails use M_Trail_v2."), TEXT("agent"));
	Update.Name = AgentName;
	TestEqual(TEXT("An agent may update its own fact"), Remember(Root.Path, Update, Error), AgentName);
	TestFalse(TEXT("The update waits for review"), Recall(Root.Path, TEXT("trail"), 10, EHyperAIStudioAgentMemoryMode::AfterReview).ContainsByPredicate(
		[&AgentName](const FHyperAIStudioMemoryFact& Fact) { return Fact.Name == AgentName; }));
	TestTrue(TEXT("An agent may forget its own fact"), Forget(Root.Path, AgentName, /*bByAgent=*/true, Error));
	TestTrue(TEXT("The user may forget any fact"), Forget(Root.Path, Duplicate, /*bByAgent=*/false, Error));
	TestEqual(TEXT("One fact left"), LoadFacts(Root.Path).Num(), 1);

	// The change log: newest first, filtered by asset.
	for (int32 Index2 = 0; Index2 < 3; ++Index2)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Utc = FDateTime::UtcNow();
		Entry.Agent = TEXT("Codex");
		Entry.ToolName = TEXT("hyper_material_apply_plan");
		Entry.Target = Index2 == 1 ? TEXT("/Game/M_Pulse.M_Pulse") : TEXT("/Game/M_Other.M_Other");
		Entry.OperationId = FString::Printf(TEXT("op-%d"), Index2);
		RecordChange(Root.Path, Entry);
	}
	const TArray<FString> Changes = RecentChanges(Root.Path, 10);
	TestTrue(TEXT("Changes come back newest first"), Changes.Num() == 3 && Changes[0].Contains(TEXT("op-2")) && Changes[2].Contains(TEXT("op-0")));
	TestEqual(TEXT("A target filter keeps that asset's lines"), RecentChanges(Root.Path, 10, TEXT("/Game/M_Pulse")).Num(), 1);
	TestTrue(TEXT("The log is local, under runtime/"), FPaths::FileExists(FPaths::Combine(Root.Path, TEXT(".hyperai"), TEXT("runtime"), TEXT("memory"), TEXT("recent-changes.md"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioProjectMemoryInstructionsTest,
	"HyperAIStudio.Memory.Instructions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioProjectMemoryInstructionsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::Memory;
	using namespace HyperAIStudio::MemoryTests;
	FHyperAIStudioMemoryFact User = MakeFact(TEXT("Levels use World Partition"), TEXT("Every level is partitioned."), TEXT("user"), TEXT("convention"));
	User.Name = TEXT("levels-use-world-partition");
	FHyperAIStudioMemoryFact Agent = MakeFact(TEXT("Trail material"), TEXT("Secret text only after review."), TEXT("agent"));
	Agent.Name = TEXT("trail-material");
	Agent.bConfirmed = false;
	FHyperAIStudioMemoryFact Long = MakeFact(TEXT("Long one"), *FString::ChrN(1000, TEXT('x')), TEXT("user"));
	Long.Name = TEXT("long-one");
	const TArray<FHyperAIStudioMemoryFact> Facts = {User, Agent, Long};

	const FString Titles = BuildInstructionSectionFrom(Facts, EHyperAIStudioAgentMemoryMode::TitleUntilReviewed);
	TestTrue(TEXT("User facts apply in full"), Titles.Contains(TEXT("**Levels use World Partition** (convention): Every level is partitioned.")));
	TestTrue(TEXT("An unreviewed agent fact shows its title only"), Titles.Contains(TEXT("Trail material")) && !Titles.Contains(TEXT("Secret text")));
	TestTrue(TEXT("Long text is cut in the block"), Titles.Contains(FString::ChrN(300, TEXT('x')) + TEXT("...")) && !Titles.Contains(FString::ChrN(301, TEXT('x'))));
	TestTrue(TEXT("Fully automatic shows the agent text"),
		BuildInstructionSectionFrom(Facts, EHyperAIStudioAgentMemoryMode::Automatic).Contains(TEXT("Secret text only after review.")));
	TestFalse(TEXT("Only after review leaves the agent fact out"),
		BuildInstructionSectionFrom(Facts, EHyperAIStudioAgentMemoryMode::AfterReview).Contains(TEXT("Trail material")));
	TestTrue(TEXT("An empty memory still tells agents how to use it"),
		BuildInstructionSectionFrom({}, EHyperAIStudioAgentMemoryMode::TitleUntilReviewed).Contains(TEXT("hyper_memory_remember")));

	// The main block's own rules (AgentConfigSmoke): discovery tools are never named, and nothing in memory text reaches the markers.
	const FString Agents = FHyperAIStudioService::BuildAgentsInstructionText();
	TestFalse(TEXT("AGENTS.md does not name list_toolsets"), Agents.Contains(TEXT("list_toolsets")));
	TestFalse(TEXT("AGENTS.md does not name describe_toolset"), Agents.Contains(TEXT("describe_toolset")));
	TestFalse(TEXT("The memory block holds no comment markers"), Titles.Contains(TEXT("<!--")));
	return true;
}

#endif
