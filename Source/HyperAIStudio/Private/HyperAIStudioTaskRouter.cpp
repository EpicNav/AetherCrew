// Games by Hyper 2026.

#include "HyperAIStudioTaskRouter.h"

#include "HyperAIStudioAgents.h"
#include "HyperAIStudioCapabilityPackRegistry.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioProjectMemory.h"
#include "HyperAIStudioPromptPresets.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSkills.h"

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
			// "trees" is foliage too, so "Plant trees along the road" stays here and not with behavior trees.
			{TEXT("worldbuilding_navigation"), TEXT("landscape landscapes terrain heightmap sculpt sculpting paint painting layer layers road roads spline splines navmesh navigation pathfinding foliage grass trees hills world partition streaming hlod")},
			{TEXT("networking_game_framework"), TEXT("multiplayer network networking replication replicate replicated server client clients rpc online game mode gamemode state controller pawn framework session")},
			{TEXT("audio_metasound"), TEXT("audio metasound metasounds cue attenuation mixer submix music ambient ambience volume concurrency occlusion falloff procedural")},
			{TEXT("materials_dynamic_material"), TEXT("material materials shader shaders texture textures instance instances roughness metallic specular normal emissive parameter parameters master tint color colour translucent opacity dynamic designer mossy stone wood metal")},
			{TEXT("niagara_vfx"), TEXT("vfx niagara particle particles effect effects emitter emitters explosion explosions smoke fire flame flames spark sparks magic spell spells trail trails impact impacts muzzle flash weather rain snow dust swarm")},
			{TEXT("texture_graph"), TEXT("texture graph procedural tileable seamless pattern noise generate")},
			{TEXT("lighting_lookdev"), TEXT("light lights lighting lit sun sunlight sky skylight fog haze exposure shadow shadows lumen reflection reflections reflective glossy gloss wet mood dusk dawn sunset sunrise night day time atmosphere bloom grade grading lut look reference match relight torch flicker warmer cooler")},
			// Placing what is there, too: "spots", not "spot", which reads as "spotlight".
			{TEXT("level_design"), TEXT("level levels design blockout block greybox graybox whitebox layout room rooms corridor corridors arena cover stairs ramp ramps spawn sightline sightlines player scale playable place placing placed props drop dropped ground settle clear spots spatial")},
			{TEXT("mesh_terrain"), TEXT("terrain cliff cliffs rock rocks cave caves canyon mountain mountains island mesh")},
			{TEXT("anim_blueprint"), TEXT("animation blueprint anim abp state machine locomotion graph blend layered aim offset inertialization")},
			{TEXT("pcg"), TEXT("pcg procedural scatter scattering populate forest biome points generation generator rocks trees")},
			{TEXT("ui_slate_mvvm"), TEXT("ui umg widget widgets hud menu menus button buttons screen screens inventory healthbar bar dialog prompt slate mvvm interface title settings red")},
			{TEXT("cinematics"), TEXT("cinematic cinematics sequencer cutscene cutscenes camera cameras shot shots render movie trailer keyframe keyframes pans pan")},
			{TEXT("data_config_localization"), TEXT("data table tables datatable asset assets config ini setting settings localization localize translate translation language languages csv")},
			{TEXT("physics_chaos"), TEXT("physics chaos ragdoll collision collisions constraint constraints rigid body simulate gravity cloth")},
			{TEXT("destruction"), TEXT("destruction destructible destroy destroyed fracture fractured fracturing shatter shattered shattering shatters crumble crumbles collapse collapses breakable smash smashed debris voronoi geometry collection collections cracks chunks")},
			{TEXT("geometry_interchange"), TEXT("geometry import importing export fbx gltf obj usd interchange")},
			{TEXT("live_production"), TEXT("live link livelink virtual production mocap capture timecode genlock ndisplay")},
			{TEXT("gameplay_systems"), TEXT("gameplay system systems inventory quest quests interaction interact interactable pickup pickups save load checkpoint dialogue health damage respawn score")},
			{TEXT("character"), TEXT("character characters player movement walk run jump double crouch sprint pawn capsule camera third first person controller")},
			{TEXT("paper2d"), TEXT("2d sprite sprites paper2d flipbook flipbooks tilemap tileset tile pixel")},
			{TEXT("actor_modifier_property_animation"), TEXT("modifier modifiers property animate rotate rotating spin spinning bob motion design")},
			{TEXT("automation_profiling_build"), TEXT("test tests testing automation profile profiling performance fps frame stat stats build cook package packaging compile fail rate framerate low slow lag laggy hitch hitches stutter stuttering")},
			{TEXT("uaf_graph"), TEXT("uaf animation framework animnext graph")},
			{TEXT("image_generation"), TEXT("image images picture pictures generate draw concept art reference illustration")},
			// Rigging and animating as the generator's own words ("rigged", "autorig"), so "rig" and "animation" stay the
			// animation pack's.
			{TEXT("mesh_generation"), TEXT("3d model models generate generated textured photo picture realistic tripo meshy hi3d viggle paid rigged autorig retexture retextured")},
			{TEXT("audio_generation"), TEXT("elevenlabs stable stability generate generated paid sfx clip clips takes variations")},
			// No "props" or "placeholder": "place" reads as "placeholder", and props to place are level design's. PolyEdit and
			// PolyGroups are Modeling Mode's tools a hand-off points people at; "sculpt" stays the landscape's.
			// Its UV and bake tools' names lengthen its document, so "arch" is named outright to keep stone arches here.
			{TEXT("mesh_modeling"), TEXT("model models modeling modelling modeled mesh meshes kitbash lowpoly poly primitive primitives rock boulder boulders arch arches archway pillar pillars column columns crystal crystals mushroom mushrooms tower towers watchtower ruins ruined rubble monolith trunk stump crate crates bevel extrude inset boolean revolve lathe polyedit polygroup polygroups")},
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
			// A pack's own skill comes before a general one it shares, so a message is pointed at it first.
			{TEXT("gameplay_ai"), TEXT("unreal-ai")},
			{TEXT("enhanced_input"), TEXT("unreal-input")},
			{TEXT("networking_game_framework"), TEXT("unreal-networking")},
			{TEXT("data_config_localization"), TEXT("unreal-data")},
			{TEXT("lighting_lookdev"), TEXT("unreal-lighting")},
			{TEXT("pcg"), TEXT("unreal-pcg")},
			{TEXT("editor_lifecycle"), TEXT("unreal-cpp-build")},
			{TEXT("blueprint"), TEXT("unreal-blueprint")},
			{TEXT("blueprint_structure"), TEXT("unreal-blueprint")},
			{TEXT("gameplay_systems"), TEXT("unreal-blueprint")},
			{TEXT("enhanced_input"), TEXT("unreal-blueprint")},
			{TEXT("gas"), TEXT("unreal-gas")},
			{TEXT("niagara_vfx"), TEXT("unreal-niagara")},
			{TEXT("lighting_lookdev"), TEXT("image-match")},
			{TEXT("level_design"), TEXT("unreal-level-design")},
			{TEXT("worldbuilding_navigation"), TEXT("unreal-level-design")},
			{TEXT("worldbuilding_navigation"), TEXT("unreal-world-partition")},
			{TEXT("pcg"), TEXT("unreal-level-design")},
			{TEXT("mesh_terrain"), TEXT("unreal-level-design")},
			{TEXT("mesh_modeling"), TEXT("unreal-modeling")},
			{TEXT("mesh_generation"), TEXT("generated-characters")},
			{TEXT("animation_rigging"), TEXT("unreal-animation")},
			{TEXT("ui_slate_mvvm"), TEXT("unreal-umg")},
			{TEXT("cinematics"), TEXT("unreal-cinematics")},
			{TEXT("pie_runtime"), TEXT("unreal-playtest")},
			{TEXT("automation_profiling_build"), TEXT("performance")},
			{TEXT("destruction"), TEXT("unreal-destruction")},
			{TEXT("physics_chaos"), TEXT("unreal-destruction")},
			{TEXT("sound_design"), TEXT("sound-design")},
			{TEXT("audio_metasound"), TEXT("game-audio")},
			{TEXT("audio_generation"), TEXT("game-audio")},
		};

		/**
		 * The packs each of HyperAI's team roles works with. A role's skills serve it and add nothing: the UI designer's
		 * Blueprint skill wires its buttons and does not make it a programmer. The QA tester is found by what it is asked
		 * to do (LeadWordList); the playtest tools are not among its packs, since their "play" reads like "player".
		 */
		const TPair<const TCHAR*, const TCHAR*> RolePackList[] = {
			{TEXT("programmer"), TEXT("blueprint blueprint_structure gameplay_systems gas gameplay_ai enhanced_input networking_game_framework character physics_chaos data_config_localization scripting diagnostics")},
			{TEXT("level-designer"), TEXT("level_design worldbuilding_navigation pcg mesh_terrain geometry_interchange mesh_modeling")},
			{TEXT("lighting-artist"), TEXT("lighting_lookdev")},
			{TEXT("technical-artist"), TEXT("materials_dynamic_material texture_graph niagara_vfx animation_rigging anim_blueprint uaf_graph actor_modifier_property_animation destruction")},
			{TEXT("ui-designer"), TEXT("ui_slate_mvvm")},
			{TEXT("cinematographer"), TEXT("cinematics")},
			{TEXT("qa-tester"), TEXT("automation_profiling_build")},
		};

		/**
		 * A request whose first word asks to test or to plan is the QA tester's or the director's, whatever it tests or
		 * plans. The word must match exactly: "plant" is not "plan", nor "special" "spec".
		 */
		const TPair<const TCHAR*, const TCHAR*> LeadWordList[] = {
			{TEXT("director"), TEXT("plan plans planning planned architecture architect roadmap spec specs specification outline strategy approach proposal propose review reviews overview")},
			{TEXT("qa-tester"), TEXT("test tests testing tester playtest playtests playtesting qa verify verification check checks prove")},
		};

		/** First words that lead to a role only with one of its lead words later: "Play the level and check the doors", not "Play a sound". */
		const TPair<const TCHAR*, const TCHAR*> OpenerList[] = {
			{TEXT("qa-tester"), TEXT("play run")},
		};

		/** Words for where the work goes, which any role's requests use: "add rain to the level" is not level design. */
		const TCHAR* const PlaceWords[] = {TEXT("level"), TEXT("levels")};

		/** A project agent fits when its words and its packs' work score this much together. */
		constexpr double MinAgentScore = 5.0;
		/** Two builders' roles that fit this nearly as well as each other make a request the team's. */
		constexpr double TeamShare = 0.65;
		constexpr int32 MaxAgentDescriptionChars = 300;

		template <int32 Num>
		FString Lookup(const TPair<const TCHAR*, const TCHAR*> (&List)[Num], const FString& Key)
		{
			for (const TPair<const TCHAR*, const TCHAR*>& Entry : List)
			{
				if (Key == Entry.Key) return Entry.Value;
			}
			return FString();
		}

		/** Skills every role carries: they say nothing about which role a request is for. */
		bool IsCrossCuttingSkill(const FString& Skill)
		{
			return Skill == TEXT("image-match") || Skill == TEXT("hyperai-memory");
		}

		/** A job title several roles share counts once in an agent's name; what the role works on counts three times. */
		bool IsRoleTitle(const FString& Word)
		{
			return Word == TEXT("designer") || Word == TEXT("artist");
		}

		/** The packs whose work an agent does: its role's for one of HyperAI's team, else those its skills are made for. */
		TArray<FString> PacksOfAgent(const FHyperAIStudioAgentDefinition& Agent)
		{
			TArray<FString> Packs;
			const FString Role = Lookup(RolePackList, Agent.Name);
			if (!Role.IsEmpty())
			{
				Role.ParseIntoArrayWS(Packs);
				return Packs;
			}
			for (const FString& Skill : Agent.Skills)
			{
				if (IsCrossCuttingSkill(Skill)) continue;
				for (const TPair<const TCHAR*, const TCHAR*>& Entry : PackSkillList)
				{
					if (Skill == Entry.Value) Packs.AddUnique(Entry.Key);
				}
			}
			return Packs;
		}

		/** What an agent says it is and does. */
		TArray<FString> OwnWords(const FHyperAIStudioAgentDefinition& Agent)
		{
			TArray<FString> Words;
			for (const FString& Word : Memory::SearchWords(Agent.Name.Replace(TEXT("-"), TEXT(" "))))
			{
				for (int32 Times = IsRoleTitle(Word) ? 1 : 3; Times > 0; --Times)
				{
					Words.Add(Word);
				}
			}
			Words.Append(Memory::SearchWords(Agent.Description));
			return Words;
		}

		template <int32 Num>
		bool IsListed(const TPair<const TCHAR*, const TCHAR*> (&List)[Num], const FString& AgentName, const FString& Word)
		{
			TArray<FString> Words;
			Lookup(List, AgentName).ParseIntoArrayWS(Words);
			return Words.Contains(Word);
		}

		/** The role a request leads to by what it asks first, if any. */
		bool LeadsTo(const FString& AgentName, const TArray<FString>& Query)
		{
			if (IsListed(LeadWordList, AgentName, Query[0]))
			{
				return true;
			}
			return IsListed(OpenerList, AgentName, Query[0])
				&& Query.ContainsByPredicate([&AgentName](const FString& Word) { return IsListed(LeadWordList, AgentName, Word); });
		}

		bool IsPlaceWord(const FString& Word)
		{
			for (const TCHAR* Place : PlaceWords)
			{
				if (Word == Place) return true;
			}
			return false;
		}

		/** A builder makes a part of a request; the director plans and judges, and the QA tester proves. */
		bool IsBuilder(const FString& AgentName)
		{
			return Lookup(LeadWordList, AgentName).IsEmpty();
		}

		/** Agents::TeamRequest's message, in any agent's way. */
		bool IsTeamRequest(const FString& Message)
		{
			for (const TCHAR* Agent : {TEXT("Claude Code"), TEXT("Codex"), TEXT("")})
			{
				if (Message.StartsWith(Agents::TeamRequest(Agent, FString()), ESearchCase::CaseSensitive))
				{
					return true;
				}
			}
			return false;
		}

		/** The CLI can start the agent by name: its own copy of the definition is there, HyperAI's or edited there. */
		bool CanStartByName(const FHyperAIStudioAgentDefinition& Agent, const FString& ChatAgent)
		{
			const FString Prefix = Skills::InvocationPrefix(ChatAgent);
			const bool bClaude = Prefix == TEXT("/");
			if (Agent.Scope == TEXT("claude"))
			{
				return bClaude;
			}
			const FString& State = bClaude ? Agent.ClaudeState : Agent.CodexState;
			return !Prefix.IsEmpty() && (State == TEXT("synced") || State == TEXT("stale") || State == TEXT("edited"));
		}

		FString Shortened(const FString& Text, const int32 MaxChars)
		{
			const FString Line = Text.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
			return Line.Len() > MaxChars ? Line.Left(MaxChars - 3) + TEXT("...") : Line;
		}

		/** The pack every task uses (plans, operations, memory): it says nothing about which task a message is. */
		bool IsFoundation(const FString& PackId)
		{
			return PackId == TEXT("shared_foundation");
		}

		/** A pack any kind of task may use (the scene, diagnostics, playtests), as a preset may whatever its own packs. */
		bool IsForAnyTask(const FString& PackId)
		{
			for (const TCHAR* Pack : PromptPresets::FoundationPacks)
			{
				if (PackId == Pack) return true;
			}
			return false;
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

	TArray<FPackDocument> PackDocuments(const bool bToolDescriptions)
	{
		TMap<FString, FString> Descriptions;
		if (bToolDescriptions)
		{
			TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
			TArray<FString> Diagnostics;
			FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(Bindings, Diagnostics);
			for (const FHyperAIStudioRuntimeToolBinding& Binding : Bindings)
			{
				Descriptions.Add(Binding.ToolName, Binding.Description);
			}
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
		// A message whose best pack no route has, such as a paid 3D model, is that pack's work: only the routes' names count
		// then, or "textured" would send it to Materials. A pack any kind of task uses says nothing about which kind it is.
		int32 Top = INDEX_NONE;
		for (int32 Index = 0; Index < PackRanks.Num(); ++Index)
		{
			if (Top == INDEX_NONE || PackRanks[Index].Score > PackRanks[Top].Score)
			{
				Top = Index;
			}
		}
		const bool bUnrouted = Top != INDEX_NONE && PackRanks[Top].Score >= MinPackScore && !IsForAnyTask(Documents[Top].PackId)
			&& !Routes.ContainsByPredicate([&Top, &Documents](const FHyperAIStudioAgentTaskRoute& Route)
			{
				return !Route.Category.IsEmpty() && Route.Packs.Contains(Documents[Top].PackId);
			});
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
				const int32 Found = bUnrouted ? INDEX_NONE : Documents.IndexOfByPredicate([&Pack](const FPackDocument& Document) { return Document.PackId == Pack; });
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

	TOptional<FRouteDecision> DecideNamed(const FString& Category, const FString& Message, const TArray<FHyperAIStudioAgentTaskRoute>& Routes,
		const TArray<FString>& UsableAgents, const FString& PreferredAgent)
	{
		const int32 Index = Category.IsEmpty() ? INDEX_NONE : Routes.IndexOfByPredicate([&Category](const FHyperAIStudioAgentTaskRoute& Route)
		{
			return Route.Category.Equals(Category, ESearchCase::IgnoreCase);
		});
		if (Index == INDEX_NONE)
		{
			// Routes are the user's to rename and remove: then the words find the route, by its packs.
			return Decide(Message, Routes, UsableAgents, PreferredAgent);
		}
		const FHyperAIStudioAgentTaskRoute& Route = Routes[Index];
		FRouteDecision Decision;
		Decision.RouteIndex = Index;
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
		TSet<FString>& Given, const TArray<FString>& Named)
	{
		TArray<FString> Lines;
		auto List = [&Lines, &Given](const FHyperAIStudioRuntimeToolBinding& Tool)
		{
			Given.Add(TEXT("tool:") + Tool.ToolName);
			Lines.Add(FString::Printf(TEXT("- `%s` (toolset_name `%s`): %s"), *Tool.ToolName, *Tool.QualifiedToolset, *FirstSentence(Tool.Description)));
		};
		// The tools the message names come first, whatever their pack: its prompt calls them by name.
		TMap<FString, int32> NamedOfPack;
		for (const FString& Name : Named)
		{
			const FHyperAIStudioRuntimeToolBinding* Tool = Bindings.FindByPredicate([&Name](const FHyperAIStudioRuntimeToolBinding& Binding) { return Binding.ToolName == Name; });
			if (Tool && Tool->bToolEnabled && !Given.Contains(TEXT("tool:") + Name))
			{
				List(*Tool);
				if (const FString* Pack = PackOfTool.Find(Name))
				{
					++NamedOfPack.FindOrAdd(*Pack);
				}
			}
		}
		struct FPackTools
		{
			FString Key;
			/** Its enabled tools not listed yet: read, edit, check. */
			TArray<const FHyperAIStudioRuntimeToolBinding*> Tools;
			/** Its lines among the tools the message names, and how many more it lists. */
			int32 NumNamed = 0;
			int32 Take = 0;
		};
		TArray<FPackTools> Listing;
		for (const FPackFit& Fit : Packs)
		{
			const FString Key = TEXT("tools:") + Fit.PackId;
			if (Given.Contains(Key) || Listing.ContainsByPredicate([&Key](const FPackTools& Each) { return Each.Key == Key; }))
			{
				continue;
			}
			FPackTools Each;
			Each.Key = Key;
			Each.NumNamed = NamedOfPack.FindRef(Fit.PackId);
			for (const FHyperAIStudioRuntimeToolBinding& Binding : Bindings)
			{
				const FString* Pack = PackOfTool.Find(Binding.ToolName);
				if (Binding.bToolEnabled && Pack && *Pack == Fit.PackId && !Given.Contains(TEXT("tool:") + Binding.ToolName))
				{
					Each.Tools.Add(&Binding);
				}
			}
			if (Each.Tools.IsEmpty() && Each.NumNamed == 0)
			{
				continue;
			}
			Each.Tools.StableSort([](const FHyperAIStudioRuntimeToolBinding& A, const FHyperAIStudioRuntimeToolBinding& B) { return Role(A.ToolName) < Role(B.ToolName); });
			Listing.Add(MoveTemp(Each));
		}
		// The lines left are shared out, so the first packs do not starve the last; a share a pack cannot use goes to the next.
		int32 Left = MaxToolsListed - Lines.Num();
		const int32 Share = FMath::Max(1, Left / FMath::Max(1, Listing.Num()));
		for (const bool bShare : {true, false})
		{
			for (FPackTools& Each : Listing)
			{
				const int32 Most = FMath::Min(Each.Tools.Num(), FMath::Max(0, MaxToolsPerPack - Each.NumNamed));
				const int32 More = FMath::Clamp((bShare ? FMath::Min(Most, Share) : Most) - Each.Take, 0, FMath::Max(0, Left));
				Each.Take += More;
				Left -= More;
			}
		}
		for (const FPackTools& Each : Listing)
		{
			for (int32 Index = 0; Index < Each.Take; ++Index)
			{
				List(*Each.Tools[Index]);
			}
			// Pointed at a pack only with a line of it: one the lines ran out for can still be listed later.
			if (Each.NumNamed + Each.Take > 0)
			{
				Given.Add(Each.Key);
			}
		}
		return Lines.IsEmpty() ? FString()
			: TEXT("\n\n<tools>\nHyperAI tools for this task. Call each through call_tool with its toolset_name; .hyperai/docs/HyperAIStudio-ToolGuide.md lists their fields under that toolset's heading.\n")
				+ FString::Join(Lines, TEXT("\n")) + TEXT("\n</tools>");
	}

	FString ToolHintFor(const TArray<FPackFit>& Packs, TSet<FString>& Given, const TArray<FString>& Named)
	{
		if (Packs.IsEmpty() && Named.IsEmpty())
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
		return ToolHintFrom(Packs, Bindings, PackOfTool, Given, Named);
	}

	TOptional<FAgentPick> AgentFor(const FString& Message, const TArray<FHyperAIStudioAgentDefinition>& ProjectAgents, const bool bTeam,
		const TArray<FPackDocument>& Documents)
	{
		TArray<FString> Query = QueryWords(Message);
		if (Query.IsEmpty() || ProjectAgents.IsEmpty())
		{
			return {};
		}
		auto Pick = [](const FHyperAIStudioAgentDefinition& Agent)
		{
			FAgentPick Picked;
			Picked.Agent = Agent.Name;
			Picked.Description = Agent.Description;
			return Picked;
		};
		// Asked first to test or to plan: the QA tester's or the director's, whatever is tested or planned.
		for (const FHyperAIStudioAgentDefinition& Agent : ProjectAgents)
		{
			if (LeadsTo(Agent.Name, Query))
			{
				return Pick(Agent);
			}
		}
		Query.RemoveAll([](const FString& Word) { return IsPlaceWord(Word); });
		if (Query.IsEmpty())
		{
			return {};
		}
		struct FScored
		{
			int32 Index = INDEX_NONE;
			double Score = 0.0;
			TArray<FString> Packs;
			/** The best of its packs that fit the message clearly, and that pack's score. */
			FString PackId;
			double PackScore = 0.0;
			bool bClear = false;
		};
		TArray<TArray<FString>> Own;
		for (const FHyperAIStudioAgentDefinition& Agent : ProjectAgents)
		{
			Own.Add(OwnWords(Agent));
		}
		const TArray<Memory::FRank> OwnRanks = Memory::Rank(Query, Own);
		const TArray<Memory::FRank> PackRanks = Memory::Rank(Query, WordsOf(Documents));
		TArray<FScored> Scored;
		for (int32 Index = 0; Index < ProjectAgents.Num(); ++Index)
		{
			FScored& Each = Scored.AddDefaulted_GetRef();
			Each.Index = Index;
			Each.Packs = PacksOfAgent(ProjectAgents[Index]);
			for (const FString& Pack : Each.Packs)
			{
				const int32 Found = Documents.IndexOfByPredicate([&Pack](const FPackDocument& Document) { return Document.PackId == Pack; });
				if (Found != INDEX_NONE && PackRanks[Found].Score >= MinPackScore && PackRanks[Found].Score > Each.PackScore)
				{
					Each.PackId = Pack;
					Each.PackScore = PackRanks[Found].Score;
				}
			}
			Each.Score = OwnRanks[Index].Score + Each.PackScore;
			Each.bClear = IsClearFit(OwnRanks[Index], Query.Num()) || Each.PackScore > 0.0;
		}
		Scored.StableSort([](const FScored& A, const FScored& B) { return A.Score > B.Score; });
		const FScored& Top = Scored[0];
		if (!Top.bClear || Top.Score < MinAgentScore)
		{
			return {};
		}
		const FHyperAIStudioAgentDefinition& TopAgent = ProjectAgents[Top.Index];
		// Parts for two builders that fit about as well, neither able to do the other's: the team shares them out.
		TArray<const FScored*> Builders;
		for (const FScored& Each : Scored)
		{
			if (Each.PackScore > 0.0 && IsBuilder(ProjectAgents[Each.Index].Name))
			{
				Builders.Add(&Each);
			}
		}
		if (bTeam && IsBuilder(TopAgent.Name) && Builders.Num() >= 2 && Builders[1]->Score >= Builders[0]->Score * TeamShare
			&& !Builders[0]->Packs.Contains(Builders[1]->PackId) && !Builders[1]->Packs.Contains(Builders[0]->PackId))
		{
			FAgentPick Team;
			Team.bTeam = true;
			return Team;
		}
		return Pick(TopAgent);
	}

	FString AgentHintFrom(const FAgentPick& Pick, const TArray<FHyperAIStudioAgentDefinition>& ProjectAgents, const FString& ProjectRoot,
		const FString& ChatAgent, TSet<FString>& Given)
	{
		const FString Key = TEXT("agent:") + (Pick.bTeam ? FString(TEXT("team")) : Pick.Agent);
		if (Given.Contains(Key))
		{
			return FString();
		}
		Given.Add(Key);
		if (Pick.bTeam)
		{
			return TEXT("\n\n<agents>\nThis request has parts for several of the project's agents. Run it as HyperAI's team with the `team` skill: read `.agents/skills/team/SKILL.md` first. The director plans the parts, each part goes to its agent, and the QA tester proves it.\n</agents>");
		}
		const FHyperAIStudioAgentDefinition* Agent = ProjectAgents.FindByPredicate([&Pick](const FHyperAIStudioAgentDefinition& Each) { return Each.Name == Pick.Agent; });
		FString Lines = FString::Printf(TEXT("\n\n<agents>\nThis task is for the project's `%s` agent: %s\n"), *Pick.Agent, *Shortened(Pick.Description, MaxAgentDescriptionChars));
		if (Agent && CanStartByName(*Agent, ChatAgent))
		{
			Lines += Skills::InvocationPrefix(ChatAgent) == TEXT("/")
				? FString::Printf(TEXT("Hand it to that agent with your Agent tool, `%s` as the agent type."), *Pick.Agent)
				: FString::Printf(TEXT("Spawn the `%s` agent with your multi-agent tools and hand it the task."), *Pick.Agent);
			Lines += TEXT(" Give it the request as it was asked, with the skills and tools listed here, and report what it did.");
		}
		else
		{
			FString File = Agent && !Agent->File.IsEmpty() ? Agent->File : FPaths::Combine(Agents::CanonicalDirectory(ProjectRoot), Pick.Agent + TEXT(".md"));
			FPaths::MakePathRelativeTo(File, *(ProjectRoot / TEXT("")));
			Lines += FString::Printf(TEXT("Read `%s` first and do the task as that agent, to its instructions and quality bar."), *File);
		}
		return Lines + TEXT(" Answer yourself only a question you can answer without doing the work.\n</agents>");
	}

	FString AgentHintFor(const FString& Message, const FString& ProjectRoot, const FString& ChatAgent, const FString& StartedAs, TSet<FString>& Given)
	{
		// A chat started as a project agent is that agent's own; a request to the team is the team's to share out.
		if (!StartedAs.IsEmpty() || IsTeamRequest(Message))
		{
			return FString();
		}
		const TArray<FHyperAIStudioAgentDefinition> ProjectAgents = Agents::List(ProjectRoot);
		if (ProjectAgents.IsEmpty())
		{
			return FString();
		}
		const bool bTeam = ProjectAgents.ContainsByPredicate([](const FHyperAIStudioAgentDefinition& Agent) { return Agent.Name == TEXT("director"); })
			&& FPaths::FileExists(FPaths::Combine(Skills::CanonicalDirectory(ProjectRoot), TEXT("team"), TEXT("SKILL.md")));
		// How people ask for each pack's work and its tools' names: tool descriptions, written for agents, mislead about roles.
		const TOptional<FAgentPick> Pick = AgentFor(Message, ProjectAgents, bTeam, PackDocuments(/*bToolDescriptions=*/false));
		return Pick ? AgentHintFrom(*Pick, ProjectAgents, ProjectRoot, ChatAgent, Given) : FString();
	}

	FString AgentHintNamed(const FString& Agent, const FString& Message, const FString& ProjectRoot, const FString& ChatAgent, const FString& StartedAs,
		TSet<FString>& Given)
	{
		if (!StartedAs.IsEmpty() || IsTeamRequest(Message))
		{
			return FString();
		}
		const TArray<FHyperAIStudioAgentDefinition> ProjectAgents = Agents::List(ProjectRoot);
		FAgentPick Pick;
		if (Agent == TEXT("team"))
		{
			// The team runs only where it was added: the Team switch adds it, a hint never does.
			if (!Agents::IsTeamInstalled(ProjectRoot))
			{
				return FString();
			}
			Pick.bTeam = true;
			return AgentHintFrom(Pick, ProjectAgents, ProjectRoot, ChatAgent, Given);
		}
		const FHyperAIStudioAgentDefinition* Named = ProjectAgents.FindByPredicate([&Agent](const FHyperAIStudioAgentDefinition& Each) { return Each.Name == Agent; });
		if (!Named)
		{
			return AgentHintFor(Message, ProjectRoot, ChatAgent, StartedAs, Given);
		}
		Pick.Agent = Named->Name;
		Pick.Description = Named->Description;
		return AgentHintFrom(Pick, ProjectAgents, ProjectRoot, ChatAgent, Given);
	}
}
