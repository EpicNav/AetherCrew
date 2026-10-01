// Games by Hyper 2026.

#include "HyperAIStudioTaskRouter.h"

#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioProjectMemory.h"
#include "HyperAIStudioSettings.h"

namespace HyperAIStudio::TaskRouter
{
	namespace
	{
		constexpr int32 MaxQueryWords = 40;
		constexpr int32 MaxToolsPerPack = 6;
		constexpr int32 MaxToolsListed = 9;
		constexpr int32 MaxToolDescriptionChars = 140;
		/** Below this a pack only shares a word or two with the message by chance, such as "time" in "timeline". */
		constexpr double MinPackScore = 5.0;
		/** A second pack joins the first only when it fits about half as well or better. */
		constexpr double SecondPackShare = 0.5;
		constexpr double MinRouteScore = 6.0;
		/** A route is chosen only when the next one fits clearly less well: an even split is no answer. */
		constexpr double RouteMargin = 0.85;
		/** A route's name is matched against only a handful of names, so its score counts three times beside its packs'. */
		constexpr double NameWeight = 3.0;
		/** A request's first word says what is asked ("Plan a stealth system"), so a route named by it counts double. */
		constexpr double LeadWordBonus = 2.0;

		/**
		 * How people ask for each pack's kind of work, beyond its tools' own names and descriptions, which are written for
		 * agents rather than for the people asking. Tuned against sample requests; a pack missing here still has its tools' words.
		 */
		const TPair<const TCHAR*, const TCHAR*> PackWordList[] = {
			{TEXT("context_scene_project_search"), TEXT("scene actor actors selected selection outliner find search locate place move rotate scale transform spawn duplicate rename label folder attach parent")},
			{TEXT("blueprint"), TEXT("blueprint blueprints bp graph node nodes event events variable variables function functions compile pin pins wire logic script scripting visual door doors pickup pickups trigger triggers spawner timer projectile switch lever button opens open")},
			{TEXT("blueprint_structure"), TEXT("timeline timelines interface interfaces struct structs structure enum enums macro macros collapse collapsed rpc")},
			{TEXT("diagnostics"), TEXT("error errors warning warnings broken crash crashes bug bugs diagnose diagnosis audit log logs dependency dependencies reference references missing failing fails problem problems")},
			{TEXT("pie_runtime"), TEXT("play playtest playtesting pie simulate bot verify works working test gameplay check game")},
			{TEXT("enhanced_input"), TEXT("input inputs key keys keybind keybinding binding bindings bind mapping mappings controller gamepad keyboard mouse action actions press pressed shift")},
			{TEXT("gameplay_ai"), TEXT("ai enemy enemies npc npcs behavior behaviour tree trees blackboard state perception patrol chase attack guard guards wander see sight")},
			{TEXT("gas"), TEXT("gas ability abilities attribute attributes effect effects cooldown cooldowns cost buff buffs debuff debuffs damage heal healing health mana stamina tag tags cue cues dodge dash")},
			{TEXT("animation_rigging"), TEXT("animation animations anim montage montages sequence sequences blend space blendspace notify notifies root motion retarget retargeting retargeter ik rig rigging skeleton pose matching mocap")},
			{TEXT("worldbuilding_navigation"), TEXT("landscape landscapes terrain heightmap sculpt sculpting paint painting layer layers road roads spline splines navmesh navigation pathfinding foliage grass hills world partition streaming hlod")},
			{TEXT("networking_game_framework"), TEXT("multiplayer network networking replication replicate replicated server client clients rpc online game mode gamemode state controller pawn framework session")},
			{TEXT("audio_metasound"), TEXT("audio metasound metasounds cue attenuation mixer submix music ambient ambience volume")},
			{TEXT("materials_dynamic_material"), TEXT("material materials shader shaders texture textures instance instances roughness metallic specular normal emissive parameter parameters master tint color colour translucent opacity dynamic designer mossy stone wood metal")},
			{TEXT("niagara_vfx"), TEXT("vfx niagara particle particles effect effects emitter emitters explosion explosions smoke fire flame flames spark sparks magic spell spells trail trails impact impacts muzzle flash weather rain snow dust swarm")},
			{TEXT("texture_graph"), TEXT("texture graph procedural tileable seamless pattern noise generate")},
			{TEXT("lighting_lookdev"), TEXT("light lights lighting lit sun sunlight sky skylight fog haze exposure shadow shadows lumen reflection reflections reflective glossy gloss wet mood dusk dawn sunset sunrise night day time atmosphere bloom grade grading lut look reference match relight torch flicker warmer cooler")},
			{TEXT("level_design"), TEXT("level levels design blockout block greybox graybox whitebox layout room rooms corridor corridors arena cover stairs ramp ramps spawn sightline sightlines player scale playable")},
			{TEXT("mesh_terrain"), TEXT("terrain cliff cliffs rock rocks cave caves canyon mountain mountains island mesh")},
			{TEXT("anim_blueprint"), TEXT("animation blueprint anim abp state machine locomotion graph blend layered aim offset inertialization")},
			{TEXT("pcg"), TEXT("pcg procedural scatter scattering populate forest biome points generation generator rocks trees")},
			{TEXT("ui_slate_mvvm"), TEXT("ui umg widget widgets hud menu menus button buttons screen screens inventory healthbar bar dialog prompt slate mvvm interface title settings red")},
			{TEXT("cinematics"), TEXT("cinematic cinematics sequencer cutscene cutscenes camera cameras shot shots render movie trailer keyframe keyframes pans pan")},
			{TEXT("data_config_localization"), TEXT("data table tables datatable asset assets config ini setting settings localization localize translate translation language languages csv")},
			{TEXT("physics_chaos"), TEXT("physics chaos ragdoll collision collisions destruction destructible fracture constraint constraints rigid body simulate gravity")},
			{TEXT("geometry_interchange"), TEXT("geometry import importing export fbx gltf obj usd interchange modeling boolean")},
			{TEXT("live_production"), TEXT("live link livelink virtual production mocap capture timecode genlock ndisplay")},
			{TEXT("gameplay_systems"), TEXT("gameplay system systems inventory quest quests interaction interact interactable pickup pickups save load checkpoint dialogue health damage respawn score")},
			{TEXT("character"), TEXT("character characters player movement walk run jump double crouch sprint pawn capsule camera third first person controller")},
			{TEXT("paper2d"), TEXT("2d sprite sprites paper2d flipbook flipbooks tilemap tileset tile pixel")},
			{TEXT("actor_modifier_property_animation"), TEXT("modifier modifiers property animate rotate rotating spin spinning bob motion design")},
			{TEXT("automation_profiling_build"), TEXT("test tests testing automation profile profiling performance fps frame stat stats build cook package packaging compile fail")},
			{TEXT("uaf_graph"), TEXT("uaf animation framework animnext graph")},
			{TEXT("image_generation"), TEXT("image images picture pictures generate draw concept art reference illustration")},
			{TEXT("mesh_generation"), TEXT("3d model models generate generated mesh prop props asset text")},
			{TEXT("sound_design"), TEXT("sound sounds sfx effect footstep footsteps whoosh impact designer decibel synth layer vocal vocals creature ambiance loop music gravel")},
			{TEXT("scripting"), TEXT("python script scripting batch automate editor utility")},
			{TEXT("editor_lifecycle"), TEXT("restart editor rebuild relaunch reopen")},
			{TEXT("source_control"), TEXT("source control git perforce checkout submit commit lock locked")},
		};

		/** Words for the bundled routes beyond their names and packs: planning has no pack of its own. */
		const TPair<const TCHAR*, const TCHAR*> CategoryWordList[] = {
			{TEXT("plan a feature"), TEXT("plan planning plans design designing architecture architect roadmap spec specification outline strategy approach proposal")},
		};

		/** The bundled skill made for each pack's kind of work. */
		const TPair<const TCHAR*, const TCHAR*> PackSkillList[] = {
			{TEXT("blueprint"), TEXT("unreal-blueprint")},
			{TEXT("blueprint_structure"), TEXT("unreal-blueprint")},
			{TEXT("gameplay_systems"), TEXT("unreal-blueprint")},
			{TEXT("enhanced_input"), TEXT("unreal-blueprint")},
			{TEXT("gas"), TEXT("unreal-gas")},
			{TEXT("niagara_vfx"), TEXT("unreal-niagara")},
			{TEXT("lighting_lookdev"), TEXT("image-match")},
			{TEXT("level_design"), TEXT("unreal-level-design")},
			{TEXT("worldbuilding_navigation"), TEXT("unreal-level-design")},
			{TEXT("pcg"), TEXT("unreal-level-design")},
			{TEXT("mesh_terrain"), TEXT("unreal-level-design")},
			{TEXT("animation_rigging"), TEXT("unreal-animation")},
			{TEXT("ui_slate_mvvm"), TEXT("unreal-umg")},
			{TEXT("cinematics"), TEXT("unreal-cinematics")},
			{TEXT("pie_runtime"), TEXT("unreal-playtest")},
			{TEXT("sound_design"), TEXT("sound-design")},
			{TEXT("audio_metasound"), TEXT("sound-design")},
		};

		/** The pack every task uses (plans, operations, memory): it says nothing about which task a message is. */
		bool IsFoundation(const FString& PackId)
		{
			return PackId == TEXT("shared_foundation");
		}

		TArray<FString> QueryWords(const FString& Message)
		{
			TArray<FString> Words;
			for (const FString& Word : Memory::SearchWords(Message))
			{
				if (Words.Num() >= MaxQueryWords)
				{
					break;
				}
				Words.AddUnique(Word);
			}
			return Words;
		}

		/** A clear fit, as memory's search judges one: two of the message's words, or one that few documents hold. */
		bool IsClearFit(const Memory::FRank& Rank, const int32 QueryWords)
		{
			return Rank.Score > 0.0 && (Rank.Matched >= FMath::Min(2, QueryWords) || Rank.MatchedRare > 0);
		}

		FString CategoryWords(const FString& Category)
		{
			for (const TPair<const TCHAR*, const TCHAR*>& Entry : CategoryWordList)
			{
				if (Category.Equals(Entry.Key, ESearchCase::IgnoreCase)) return Entry.Value;
			}
			return FString();
		}

		TArray<FString> ToolNameWords(const FString& ToolName)
		{
			FString Name = ToolName;
			Name.RemoveFromStart(TEXT("hyper_"));
			return Memory::SearchWords(Name.Replace(TEXT("_"), TEXT(" ")));
		}

		TArray<TArray<FString>> WordsOf(const TArray<FPackDocument>& Documents)
		{
			TArray<TArray<FString>> Words;
			for (const FPackDocument& Document : Documents)
			{
				Words.Add(Document.Words);
			}
			return Words;
		}

		/** Read first, then the edit, then its check: the order an agent calls a pack's tools in. */
		int32 Role(const FString& ToolName)
		{
			if (ToolName.Contains(TEXT("inspect")) || ToolName.Contains(TEXT("snapshot")) || ToolName.Contains(TEXT("status")) || ToolName.Contains(TEXT("report")))
			{
				return 0;
			}
			return ToolName.Contains(TEXT("validate")) || ToolName.Contains(TEXT("compare")) || ToolName.Contains(TEXT("verify")) || ToolName.Contains(TEXT("diff")) ? 2 : 1;
		}

		FString FirstSentence(const FString& Text)
		{
			FString Line = Text.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
			const int32 End = Line.Find(TEXT(". "));
			if (End != INDEX_NONE)
			{
				Line = Line.Left(End + 1);
			}
			return Line.Len() > MaxToolDescriptionChars ? Line.Left(MaxToolDescriptionChars - 3) + TEXT("...") : Line;
		}
	}

	FString PackWords(const FString& PackId)
	{
		for (const TPair<const TCHAR*, const TCHAR*>& Entry : PackWordList)
		{
			if (PackId == Entry.Key) return Entry.Value;
		}
		return FString();
	}

	TArray<FPackDocument> PackDocuments()
	{
		TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
		TArray<FString> Diagnostics;
		FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(Bindings, Diagnostics);
		TMap<FString, FString> Descriptions;
		for (const FHyperAIStudioRuntimeToolBinding& Binding : Bindings)
		{
			Descriptions.Add(Binding.ToolName, Binding.Description);
		}
		TArray<FPackDocument> Documents;
		for (const FHyperAIStudioCapabilityPackDefinition& Pack : FHyperAIStudioCapabilityPackRegistry::GetCatalog().Packs)
		{
			if (IsFoundation(Pack.Id))
			{
				continue;
			}
			FPackDocument& Document = Documents.AddDefaulted_GetRef();
			Document.PackId = Pack.Id;
			// How people ask for the work counts twice beside the tools' own words.
			const TArray<FString> Asked = Memory::SearchWords(PackWords(Pack.Id));
			Document.Words.Append(Asked);
			Document.Words.Append(Asked);
			Document.Words.Append(Memory::SearchWords(Pack.Id.Replace(TEXT("_"), TEXT(" "))));
			for (const FString& Tool : Pack.ToolNames)
			{
				Document.Words.Append(ToolNameWords(Tool));
				if (const FString* Description = Descriptions.Find(Tool))
				{
					Document.Words.Append(Memory::SearchWords(*Description));
				}
			}
		}
		return Documents;
	}

	TArray<FPackFit> RankPacks(const FString& Message, const TArray<FPackDocument>& Documents, const int32 Limit)
	{
		TArray<FPackFit> Fits;
		const TArray<FString> Query = QueryWords(Message);
		if (Query.IsEmpty() || Documents.IsEmpty() || Limit <= 0)
		{
			return Fits;
		}
		const TArray<Memory::FRank> Ranks = Memory::Rank(Query, WordsOf(Documents));
		TArray<int32> Order;
		for (int32 Index = 0; Index < Ranks.Num(); ++Index)
		{
			Order.Add(Index);
		}
		Order.StableSort([&Ranks](const int32 A, const int32 B) { return Ranks[A].Score > Ranks[B].Score; });
		for (const int32 Index : Order)
		{
			const Memory::FRank& Rank = Ranks[Index];
			if (Fits.Num() >= Limit || !IsClearFit(Rank, Query.Num()) || Rank.Score < MinPackScore
				|| (!Fits.IsEmpty() && Rank.Score < Fits[0].Score * SecondPackShare))
			{
				break;
			}
			Fits.Add({Documents[Index].PackId, Rank.Score});
		}
		return Fits;
	}

	int32 RankRoutes(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes, const TArray<FPackDocument>& Documents)
	{
		const TArray<FString> Query = QueryWords(Message);
		if (Query.IsEmpty() || Routes.IsEmpty())
		{
			return INDEX_NONE;
		}
		// Each pack is scored once among all packs, so a route of many packs does not read as less like each of them.
		const TArray<Memory::FRank> PackRanks = Memory::Rank(Query, WordsOf(Documents));
		TArray<TArray<FString>> Names;
		for (const FHyperAIStudioAgentTaskRoute& Route : Routes)
		{
			TArray<FString>& Name = Names.AddDefaulted_GetRef();
			const TArray<FString> Own = Memory::SearchWords(Route.Category);
			for (int32 Times = 0; Times < 3; ++Times)
			{
				Name.Append(Own);
			}
			Name.Append(Memory::SearchWords(CategoryWords(Route.Category)));
		}
		const TArray<Memory::FRank> NameRanks = Memory::Rank(Query, Names);
		const TArray<Memory::FRank> LeadRanks = Memory::Rank({Query[0]}, Names);
		int32 Best = INDEX_NONE;
		double BestScore = 0.0;
		double SecondScore = 0.0;
		bool bBestClear = false;
		for (int32 Index = 0; Index < Routes.Num(); ++Index)
		{
			if (Routes[Index].Category.IsEmpty())
			{
				continue;
			}
			double Score = NameRanks[Index].Score * NameWeight * (LeadRanks[Index].Matched > 0 ? LeadWordBonus : 1.0);
			bool bClear = IsClearFit(NameRanks[Index], Query.Num());
			double BestPack = 0.0;
			for (const FString& Pack : Routes[Index].Packs)
			{
				const int32 Found = Documents.IndexOfByPredicate([&Pack](const FPackDocument& Document) { return Document.PackId == Pack; });
				if (Found != INDEX_NONE && PackRanks[Found].Score >= MinPackScore)
				{
					BestPack = FMath::Max(BestPack, PackRanks[Found].Score);
					bClear |= IsClearFit(PackRanks[Found], Query.Num());
				}
			}
			Score += BestPack;
			if (Score > BestScore)
			{
				SecondScore = BestScore;
				BestScore = Score;
				Best = Index;
				bBestClear = bClear;
			}
			else
			{
				SecondScore = FMath::Max(SecondScore, Score);
			}
		}
		return Best != INDEX_NONE && bBestClear && BestScore >= MinRouteScore && SecondScore < BestScore * RouteMargin ? Best : INDEX_NONE;
	}

	TArray<FPackFit> PacksFor(const FString& Message, const int32 Limit)
	{
		return RankPacks(Message, PackDocuments(), Limit);
	}

	int32 RouteFor(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes)
	{
		return RankRoutes(Message, Routes, PackDocuments());
	}

	TOptional<FRouteDecision> Decide(const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes, const TArray<FString>& UsableAgents,
		const FString& PreferredAgent)
	{
		const int32 Index = RouteFor(Message, Routes);
		if (Index == INDEX_NONE)
		{
			return {};
		}
		const FHyperAIStudioAgentTaskRoute& Route = Routes[Index];
		FRouteDecision Decision;
		Decision.RouteIndex = Index;
		// As the new-tab menu picks: the route's own agent, else the best record on its packs, else the preferred agent.
		Decision.Choice = FHyperAIStudioAgentScoreboard::ChooseAgent(Route.AgentName, Route.ModelId, Route.Packs, UsableAgents, PreferredAgent);
		if (Decision.Choice.Agent.IsEmpty())
		{
			return {};
		}
		return Decision;
	}

	TArray<FString> SkillsForPacks(const TArray<FPackFit>& Packs)
	{
		TArray<FString> Skills;
		for (const FPackFit& Fit : Packs)
		{
			for (const TPair<const TCHAR*, const TCHAR*>& Entry : PackSkillList)
			{
				if (Fit.PackId == Entry.Key)
				{
					Skills.AddUnique(Entry.Value);
				}
			}
		}
		return Skills;
	}

	FString ToolHintFrom(const TArray<FPackFit>& Packs, const TArray<FHyperAIStudioRuntimeToolBinding>& Bindings, const TMap<FString, FString>& PackOfTool,
		TSet<FString>& Given)
	{
		TArray<FString> Lines;
		for (const FPackFit& Fit : Packs)
		{
			const FString Key = TEXT("tools:") + Fit.PackId;
			if (Given.Contains(Key))
			{
				continue;
			}
			TArray<const FHyperAIStudioRuntimeToolBinding*> Tools;
			for (const FHyperAIStudioRuntimeToolBinding& Binding : Bindings)
			{
				const FString* Pack = PackOfTool.Find(Binding.ToolName);
				if (Binding.bToolEnabled && Pack && *Pack == Fit.PackId)
				{
					Tools.Add(&Binding);
				}
			}
			if (Tools.IsEmpty())
			{
				continue;
			}
			Tools.StableSort([](const FHyperAIStudioRuntimeToolBinding& A, const FHyperAIStudioRuntimeToolBinding& B) { return Role(A.ToolName) < Role(B.ToolName); });
			Given.Add(Key);
			for (int32 Index = 0; Index < Tools.Num() && Index < MaxToolsPerPack && Lines.Num() < MaxToolsListed; ++Index)
			{
				Lines.Add(FString::Printf(TEXT("- `%s` (toolset_name `%s`): %s"), *Tools[Index]->ToolName, *Tools[Index]->QualifiedToolset,
					*FirstSentence(Tools[Index]->Description)));
			}
		}
		return Lines.IsEmpty() ? FString()
			: TEXT("\n\n<tools>\nHyperAI tools for this task. Call each through call_tool with its toolset_name; .hyperai/docs/HyperAIStudio-ToolGuide.md lists their fields under that toolset's heading.\n")
				+ FString::Join(Lines, TEXT("\n")) + TEXT("\n</tools>");
	}

	FString ToolHintFor(const TArray<FPackFit>& Packs, TSet<FString>& Given)
	{
		if (Packs.IsEmpty())
		{
			return FString();
		}
		TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
		TArray<FString> Diagnostics;
		FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(Bindings, Diagnostics);
		TMap<FString, FString> PackOfTool;
		for (const FHyperAIStudioCapabilityToolDefinition& Tool : FHyperAIStudioCapabilityPackRegistry::GetCatalog().Tools)
		{
			PackOfTool.Add(Tool.Name, Tool.PackId);
		}
		return ToolHintFrom(Packs, Bindings, PackOfTool, Given);
	}
}
