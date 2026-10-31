// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "Engine/StaticMesh.h"
#include "HyperAIStudioMeshGenJob.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioTripoSettings.h"
#include "PhysicsEngine/BodySetup.h"

/**
 * The pure parts of the model creator: the brief classified into a plan, Tripo's v3 request bodies, prices, and the
 * ladder of paid fixes. Each request is shaped as Tripo's v3 reference documents it (developers.tripo3d.ai/en/docs,
 * read 30 October 2026); nothing here sends anything.
 */
namespace HyperAIStudio::MeshGen::Creator
{
	namespace
	{
		/** What each use defaults to. Faces are triangles, or quads when bQuad. */
		struct FUseDefaults
		{
			const TCHAR* Use;
			const TCHAR* Family;
			bool bQuad;
			int32 Faces;
			const TCHAR* Texture;
			float SizeMeters;
			float TexelDensity;
			bool bNanite;
			const TCHAR* Collision;
			const TCHAR* Why;
		};

		const FUseDefaults UseTable[] = {
			{TEXT("hero_prop"), TEXT("H3.1"), false, 300000, TEXT("detailed"), 1.0f, 10.24f, true, TEXT("box"),
				TEXT("a hero prop is seen up close: H3.1's detail, rendered with Nanite, at 1024 px/m")},
			{TEXT("prop"), TEXT("P2"), false, 20000, TEXT("standard"), 0.8f, 5.12f, false, TEXT("box"),
				TEXT("a game prop wants clean, light topology: P2 Smart Poly at 20K triangles")},
			{TEXT("environment"), TEXT("H3.1"), false, 200000, TEXT("detailed"), 3.0f, 5.12f, true, TEXT("box"),
				TEXT("organic environment pieces (rock, cliff, trunk) carry their shape in detail: H3.1 with Nanite")},
			{TEXT("kit"), TEXT("P2"), false, 30000, TEXT("standard"), 4.0f, 5.12f, false, TEXT("complex"),
				TEXT("modular kit pieces are hard-surface and repeat: P2's clean topology, complex collision to walk on")},
			{TEXT("background"), TEXT("P2"), false, 5000, TEXT("standard"), 5.0f, 2.56f, false, TEXT("none"),
				TEXT("background pieces are seen far away: P2 at 5K triangles, no collision")},
			{TEXT("character"), TEXT("P2"), true, 20000, TEXT("detailed"), 1.8f, 10.24f, false, TEXT("none"),
				TEXT("a character deforms when animated: P2 quads at 20K keep edge loops a rig bends well")},
			{TEXT("creature"), TEXT("P2"), true, 25000, TEXT("detailed"), 1.5f, 10.24f, false, TEXT("none"),
				TEXT("a creature deforms when animated: P2 quads at 25K")},
			{TEXT("vehicle"), TEXT("H3.1"), false, 150000, TEXT("detailed"), 4.5f, 5.12f, true, TEXT("box"),
				TEXT("a vehicle is a large hard-surface hero: H3.1 at 150K triangles with Nanite")},
			{TEXT("weapon"), TEXT("H3.1"), false, 20000, TEXT("detailed"), 1.0f, 10.24f, false, TEXT("box"),
				TEXT("a held weapon is close to the camera but light: H3.1's detail generated at several times the budget, then finished in the editor to 20K, Nanite off")},
			{TEXT("foliage"), TEXT("P2"), false, 8000, TEXT("standard"), 2.0f, 5.12f, true, TEXT("none"),
				TEXT("foliage is placed by the thousand: P2 at 8K with Nanite (Tripo makes solid geometry, not alpha cards)")},
			{TEXT("miniature"), TEXT("P2"), false, 2000, TEXT("standard"), 0.3f, 5.12f, false, TEXT("box"),
				TEXT("a small pickup or icon-sized object: P2 at 2K")},
		};

		const FUseDefaults& Defaults(const FString& Use)
		{
			for (const FUseDefaults& Row : UseTable)
			{
				if (Use == Row.Use)
				{
					return Row;
				}
			}
			return UseTable[1];
		}

		bool HasAny(const FString& Text, std::initializer_list<const TCHAR*> Words)
		{
			for (const TCHAR* Word : Words)
			{
				if (Text.Contains(Word))
				{
					return true;
				}
			}
			return false;
		}

		/** Words as whole words: "car" is not in "carved". */
		bool HasWord(const FString& Text, std::initializer_list<const TCHAR*> Words)
		{
			TArray<FString> Tokens;
			FString Clean;
			for (const TCHAR Char : Text)
			{
				Clean.AppendChar(FChar::IsAlnum(Char) ? FChar::ToLower(Char) : TEXT(' '));
			}
			Clean.ParseIntoArrayWS(Tokens);
			for (const TCHAR* Word : Words)
			{
				for (const FString& Token : Tokens)
				{
					// A plural or a possessive still counts: soldiers, cars.
					if (Token == Word || Token == FString(Word) + TEXT("s") || Token == FString(Word) + TEXT("es"))
					{
						return true;
					}
				}
			}
			return false;
		}

		/** auto, on or off as a choice: unset, true or false. */
		TOptional<bool> Switch(const FString& Value)
		{
			const FString Text = Value.TrimStartAndEnd().ToLower();
			if (Text == TEXT("on") || Text == TEXT("true") || Text == TEXT("yes") || Text == TEXT("1"))
			{
				return true;
			}
			if (Text == TEXT("off") || Text == TEXT("false") || Text == TEXT("no") || Text == TEXT("0"))
			{
				return false;
			}
			return {};
		}

		FString TierUp(const FString& Tier)
		{
			return Tier == TEXT("none") ? FString(TEXT("standard")) : Tier == TEXT("fast") || Tier == TEXT("standard") ? FString(TEXT("detailed")) : FString(TEXT("extreme"));
		}

		/** A price key's tier suffix: none -> base, fast and standard -> _standard. */
		FString TierSuffix(const FString& Tier)
		{
			return Tier == TEXT("none") ? FString() : Tier == TEXT("detailed") ? FString(TEXT("_detailed")) : Tier == TEXT("extreme") ? FString(TEXT("_extreme"))
				: FString(TEXT("_standard"));
		}

		void SetStrings(const TSharedRef<FJsonObject>& Json, const TCHAR* Name, const TArray<FString>& Values)
		{
			TArray<TSharedPtr<FJsonValue>> List;
			for (const FString& Value : Values)
			{
				List.Add(MakeShared<FJsonValueString>(Value));
			}
			Json->SetArrayField(Name, List);
		}

		const TCHAR* const DefaultNegative = TEXT("blurry, low quality, broken mesh, floating parts, holes, baked lighting, ground plane, text, watermark");

		// Words that move a choice, shared by the provider choice and Tripo's classification.
		bool AsksDetail(const FString& Words)
		{
			return HasAny(Words, {TEXT("photoreal"), TEXT("highly detailed"), TEXT("intricate"), TEXT("ornate"), TEXT("cinematic"), TEXT("hero asset"),
				TEXT("scanned"), TEXT("realistic"), TEXT("hyper-detailed"), TEXT("ultra detailed")});
		}

		bool AsksLowPoly(const FString& Words)
		{
			return HasAny(Words, {TEXT("low poly"), TEXT("low-poly"), TEXT("lowpoly"), TEXT("mobile game"), TEXT("flat shaded"), TEXT("ps1")});
		}

		bool AsksQuads(const FString& Words)
		{
			return HasAny(Words, {TEXT("quad"), TEXT("subdivision"), TEXT("subd"), TEXT("clean topology"), TEXT("edge loops"), TEXT("retopo")});
		}

		bool AsksParts(const FString& Words)
		{
			return HasAny(Words, {TEXT("separate parts"), TEXT("editable parts"), TEXT("modular parts"), TEXT("segmented"), TEXT("in parts")});
		}

		/** What Hi3D is chosen for: the most faithful, densest shape. */
		bool AsksFidelity(const FString& Words)
		{
			return HasAny(Words, {TEXT("photoreal"), TEXT("intricate"), TEXT("max detail"), TEXT("maximum detail"), TEXT("highest detail"), TEXT("sculpt"),
				TEXT("scanned"), TEXT("photogrammetry")});
		}

		bool AsksPortrait(const FString& Words)
		{
			return HasWord(Words, {TEXT("portrait"), TEXT("bust"), TEXT("headshot")}) || HasAny(Words, {TEXT("head sculpt"), TEXT("face scan")});
		}

		bool IsAuto(const FString& Value)
		{
			const FString Text = Value.TrimStartAndEnd().ToLower();
			return Text.IsEmpty() || Text == TEXT("auto");
		}

		/** Tripo's image models: the chat_image family's quality ladder, and the families that take none. */
		const TArray<FString>& ImageQualities()
		{
			static const TArray<FString> Values = {TEXT("low"), TEXT("medium"), TEXT("high"), TEXT("xhigh"), TEXT("max")};
			return Values;
		}

		/** Levenshtein distance, for a tier word misheard or misspelt: sunburn, sunbust, flair. */
		int32 EditDistance(const FString& A, const FString& B)
		{
			TArray<int32> Row;
			for (int32 J = 0; J <= B.Len(); ++J)
			{
				Row.Add(J);
			}
			for (int32 I = 1; I <= A.Len(); ++I)
			{
				int32 Diagonal = Row[0];
				Row[0] = I;
				for (int32 J = 1; J <= B.Len(); ++J)
				{
					const int32 Above = Row[J];
					Row[J] = FMath::Min3(Row[J] + 1, Row[J - 1] + 1, Diagonal + (FChar::ToLower(A[I - 1]) == FChar::ToLower(B[J - 1]) ? 0 : 1));
					Diagonal = Above;
				}
			}
			return Row[B.Len()];
		}

		/** The picture's template for a use: props are extracted as assets, characters stood in a T-pose, creatures left free. */
		FString TemplateForUse(const FString& Use, const FString& Words)
		{
			if (HasWord(Words, {TEXT("options"), TEXT("variants"), TEXT("variations")}))
			{
				return TEXT("variants");
			}
			return Use == TEXT("character") ? FString(TEXT("t_pose")) : Use == TEXT("creature") || Use == TEXT("foliage") || Use == TEXT("background") ? FString()
				: FString(TEXT("asset_extraction"));
		}
	}

	FString UseFromWords(const FString& Words)
	{
		const FString Text = Words.ToLower();
		if (HasWord(Text, {TEXT("character"), TEXT("hero"), TEXT("soldier"), TEXT("knight"), TEXT("warrior"), TEXT("man"), TEXT("woman"), TEXT("girl"), TEXT("boy"),
			TEXT("person"), TEXT("npc"), TEXT("humanoid"), TEXT("robot"), TEXT("wizard"), TEXT("villager"), TEXT("mannequin"), TEXT("astronaut")})
			&& !HasWord(Text, {TEXT("statue"), TEXT("figurine")}))
		{
			return TEXT("character");
		}
		if (HasWord(Text, {TEXT("creature"), TEXT("monster"), TEXT("dragon"), TEXT("wolf"), TEXT("dog"), TEXT("cat"), TEXT("horse"), TEXT("beast"), TEXT("animal"),
			TEXT("spider"), TEXT("bird"), TEXT("fish")}))
		{
			return TEXT("creature");
		}
		if (HasWord(Text, {TEXT("car"), TEXT("truck"), TEXT("tank"), TEXT("vehicle"), TEXT("spaceship"), TEXT("ship"), TEXT("boat"), TEXT("aircraft"), TEXT("plane"),
			TEXT("motorcycle"), TEXT("bike"), TEXT("mech"), TEXT("wagon"), TEXT("cart")}))
		{
			return TEXT("vehicle");
		}
		if (HasWord(Text, {TEXT("sword"), TEXT("axe"), TEXT("rifle"), TEXT("gun"), TEXT("pistol"), TEXT("bow"), TEXT("spear"), TEXT("dagger"), TEXT("hammer"),
			TEXT("shield"), TEXT("staff"), TEXT("weapon"), TEXT("blade")}))
		{
			return TEXT("weapon");
		}
		if (HasWord(Text, {TEXT("tree"), TEXT("bush"), TEXT("shrub"), TEXT("plant"), TEXT("flower"), TEXT("grass"), TEXT("fern"), TEXT("mushroom"), TEXT("foliage")}))
		{
			return TEXT("foliage");
		}
		if (HasWord(Text, {TEXT("wall"), TEXT("floor"), TEXT("doorway"), TEXT("pillar"), TEXT("column"), TEXT("stairs"), TEXT("staircase"), TEXT("arch"),
			TEXT("modular"), TEXT("kit"), TEXT("corridor"), TEXT("roof"), TEXT("tile")}))
		{
			return TEXT("kit");
		}
		if (HasWord(Text, {TEXT("rock"), TEXT("boulder"), TEXT("cliff"), TEXT("stump"), TEXT("log"), TEXT("ruin"), TEXT("ruins"), TEXT("stone"), TEXT("terrain")}))
		{
			return TEXT("environment");
		}
		if (HasWord(Text, {TEXT("coin"), TEXT("gem"), TEXT("key"), TEXT("potion"), TEXT("pickup"), TEXT("icon"), TEXT("ring"), TEXT("bottle")}))
		{
			return TEXT("miniature");
		}
		if (HasWord(Text, {TEXT("background"), TEXT("distant"), TEXT("skyline"), TEXT("mountain"), TEXT("mountains")}))
		{
			return TEXT("background");
		}
		if (HasWord(Text, {TEXT("statue"), TEXT("throne"), TEXT("altar"), TEXT("artifact"), TEXT("relic"), TEXT("centerpiece"), TEXT("centrepiece")}))
		{
			return TEXT("hero_prop");
		}
		return FString();
	}

	FString ChooseProvider(const FHyperAIModelBrief& Brief, const bool bRig, FString& OutReason, FString& OutError)
	{
		const FString Asked = Brief.Provider.TrimStartAndEnd().ToLower();
		const FString Model = Brief.Model.TrimStartAndEnd();
		const bool bHi3DModel = Hi3DIsModel(Model);
		const bool bTripoModel = !Model.IsEmpty() && !bHi3DModel;
		if (!IsAuto(Asked) && Asked != TEXT("tripo") && Asked != TEXT("hi3d") && Asked != TEXT("hitem3d"))
		{
			OutError = TEXT("provider is auto, tripo or hi3d.");
			return FString();
		}
		if (Asked == TEXT("tripo") || Asked == TEXT("hi3d") || Asked == TEXT("hitem3d"))
		{
			const bool bHi = Asked != TEXT("tripo");
			if (bHi && bTripoModel)
			{
				OutError = FString::Printf(TEXT("model %s is Tripo's; Hi3D's are %s."), *Model, *FString::Join(Hi3DModels(), TEXT(", ")));
				return FString();
			}
			if (!bHi && (bHi3DModel || !Brief.Resolution.TrimStartAndEnd().IsEmpty()))
			{
				OutError = TEXT("Hi3D's model ids and resolution are for provider hi3d; with Tripo the model is H3.1 or P2.");
				return FString();
			}
			OutReason = FString::Printf(TEXT("provider: %s, as given"), bHi ? TEXT("hi3d") : TEXT("tripo"));
			return bHi ? TEXT("hi3d") : TEXT("tripo");
		}
		if (bHi3DModel || !Brief.Resolution.TrimStartAndEnd().IsEmpty())
		{
			OutReason = bHi3DModel ? FString::Printf(TEXT("provider: hi3d, because model %s is Hi3D's"), *Model) : FString(TEXT("provider: hi3d, because resolution is Hi3D's"));
			return TEXT("hi3d");
		}
		if (bTripoModel)
		{
			OutReason = FString::Printf(TEXT("provider: tripo, because model %s is Tripo's"), *Model);
			return TEXT("tripo");
		}
		// Auto. Tripo for what only it does, or what Hi3D's 100,000-face floor does not suit.
		const FString Words = Brief.Prompt.ToLower();
		FString Use = Brief.Use.TrimStartAndEnd().ToLower();
		Use = Use.IsEmpty() ? UseFromWords(Brief.Prompt) : Use == TEXT("hero") ? FString(TEXT("hero_prop")) : Use;
		const bool bPicture = !Brief.ImagePath.TrimStartAndEnd().IsEmpty() || Brief.MultiviewImages.ContainsByPredicate([](const FString& View) { return !View.TrimStartAndEnd().IsEmpty(); });
		const FString Platform = Brief.Platform.TrimStartAndEnd().ToLower();
		auto Tripo = [&OutReason](const TCHAR* Why) { OutReason = FString(TEXT("provider: tripo, ")) + Why; return FString(TEXT("tripo")); };
		if (!bPicture)
		{
			return Tripo(TEXT("for a brief in words (Hi3D works from pictures; ask for provider hi3d to have a reference picture made first)"));
		}
		if (!Brief.ViewEdits.IsEmpty() || !Brief.MultiviewTaskId.TrimStartAndEnd().IsEmpty())
		{
			return Tripo(TEXT("because view edits and multiview tasks are Tripo's"));
		}
		if (Brief.Topology.TrimStartAndEnd().ToLower().StartsWith(TEXT("quad")) || AsksQuads(Words) || Use == TEXT("character") || Use == TEXT("creature"))
		{
			return Tripo(TEXT("because the model wants quads (or deforms, as a character or creature does); Hi3D makes triangles"));
		}
		if (Switch(Brief.Parts).Get(false) || AsksParts(Words) || Switch(Brief.SmartLowPoly).Get(false) || AsksLowPoly(Words) || !Brief.GeometryQuality.TrimStartAndEnd().IsEmpty())
		{
			return Tripo(TEXT("because parts, low poly and geometry quality are Tripo's"));
		}
		if (bRig)
		{
			return Tripo(TEXT("because the model will be rigged, which Tripo does"));
		}
		if (Platform == TEXT("mobile") || Platform == TEXT("vr"))
		{
			return Tripo(TEXT("because mobile and VR want light models, and Hi3D starts at 100,000 faces"));
		}
		if (Brief.FaceLimit > 0 && Brief.FaceLimit < Hi3DMinFaces)
		{
			return Tripo(TEXT("because an exact budget under 100,000 faces is Tripo's to make (Hi3D starts at 100,000)"));
		}
		if (AsksPortrait(Words))
		{
			OutReason = TEXT("provider: hi3d, a portrait or bust from a picture, which Hi3D's portrait models make");
			return TEXT("hi3d");
		}
		if (Use == TEXT("hero_prop") || Use == TEXT("environment") || Use == TEXT("vehicle"))
		{
			OutReason = FString::Printf(TEXT("provider: hi3d, a picture of a %s wants the most faithful, densest shape, which Nanite renders as it is"), *Use);
			return TEXT("hi3d");
		}
		if (AsksFidelity(Words))
		{
			OutReason = TEXT("provider: hi3d, because the brief asks for maximum fidelity (photoreal, intricate, max detail, sculpted)");
			return TEXT("hi3d");
		}
		return Tripo(TEXT("the default for a picture of a game piece of this kind"));
	}

	/**
	 * A weapon: the project's convention (or the reference the brief matches) sets its budget, Nanite and finishing; the
	 * generation asks for three times the budget (four for a detailed brief), so the detail survives in the normal map,
	 * and the free finishing reduces it to the budget.
	 */
	static bool PlanWeapon(const FHyperAIModelBrief& Brief, const bool bDetailWords, FHyperAIModelPlan& Out, TArray<FString>& Why, FString& OutError)
	{
		FWeaponFinish Finish;
		const FString Match = Brief.MatchAsset.TrimStartAndEnd();
		if (!Match.IsEmpty())
		{
			TArray<FString> Lines;
			if (!MatchReference(Match, Finish, Lines, OutError, /*bKeep=*/false))
			{
				return false;
			}
			Why.Append(Lines);
			Out.MatchAsset = Match;
			if (Brief.SizeMeters <= 0.f && Finish.LengthCm > 0.f)
			{
				Out.SizeMeters = Finish.LengthCm / 100.f;
				Why.Add(FString::Printf(TEXT("size: %.2f m, the reference's length"), Out.SizeMeters));
			}
		}
		else
		{
			Finish = WeaponFinishFor(Out.SizeMeters, 0);
			const FHyperAIStudioWeaponConvention& Convention = GetDefault<UHyperAIStudioTripoSettings>()->WeaponConvention;
			Why.Add(Convention.Source.IsEmpty() ? FString(TEXT("weapon: HyperAI's defaults (length on Y, head toward -Y, pivot in the grip, blade a fifth as thick as wide, grip 3.4 cm, guard 3 cm deep); give match_asset to follow one of the project's weapons"))
				: FString::Printf(TEXT("weapon: the project's convention, %s"), *Convention.Summary));
		}
		if (Brief.FaceLimit <= 0)
		{
			const int32 Budget = FMath::Clamp(Finish.TargetTris, Out.MinFaces, Out.MaxFaces);
			if (Budget != Out.FaceLimit)
			{
				Out.FaceLimit = Budget;
				Why.Add(FString::Printf(TEXT("budget: %d triangles, from %s"), Budget, *Finish.Source));
			}
			Out.bNanite = Finish.bNanite;
		}
		if (Out.Collision == TEXT("box") && !Match.IsEmpty())
		{
			const UStaticMesh* Reference = LoadObject<UStaticMesh>(nullptr, *Match, nullptr, LOAD_NoWarn | LOAD_Quiet);
			const UBodySetup* Body = Reference ? Reference->GetBodySetup() : nullptr;
			if (Body && Body->AggGeom.GetElementCount() == 0 && Body->CollisionTraceFlag != CTF_UseComplexAsSimple)
			{
				Out.Collision = TEXT("none");
				Why.Add(TEXT("collision: none, as the reference has"));
			}
		}
		Out.ProviderFaces = FMath::Clamp(Out.FaceLimit * (bDetailWords ? 4 : 3), Out.MinFaces, Out.MaxFaces);
		Out.bLocalReduce = Out.ProviderFaces > Out.FaceLimit;
		Why.Add(FString::Printf(TEXT("generated at %d triangles, then finished in the editor for free: turned and placed, thinned to real proportions, reduced to %d, Nanite %s"),
			Out.ProviderFaces, Out.FaceLimit, Out.bNanite ? TEXT("on") : TEXT("off")));
		return true;
	}

	static bool ClassifyTripo(const FHyperAIModelBrief& Brief, FHyperAIModelPlan& Out, FString& OutError)
	{
		Out = FHyperAIModelPlan();
		const FString Prompt = Brief.Prompt.TrimStartAndEnd();
		const FString Words = Prompt.ToLower();
		TArray<FString>& Why = Out.Reasons;

		// The use: given, or read from the words, or a plain prop.
		FString Use = Brief.Use.TrimStartAndEnd().ToLower().Replace(TEXT(" "), TEXT("_")).Replace(TEXT("-"), TEXT("_"));
		Use = Use == TEXT("environment_kit") || Use == TEXT("modular") ? FString(TEXT("kit")) : Use == TEXT("hero") ? FString(TEXT("hero_prop")) : Use;
		bool bKnownUse = false;
		for (const FUseDefaults& Row : UseTable)
		{
			bKnownUse |= Use == Row.Use;
		}
		if (!Use.IsEmpty() && !bKnownUse)
		{
			OutError = TEXT("use is hero_prop, prop, environment, kit, background, character, creature, vehicle, weapon, foliage or miniature.");
			return false;
		}
		if (Use.IsEmpty())
		{
			Use = UseFromWords(Prompt);
			Why.Add(Use.IsEmpty() ? FString(TEXT("use: prop (nothing in the brief named another use)")) : FString::Printf(TEXT("use: %s, read from the brief's words"), *Use));
			Use = Use.IsEmpty() ? FString(TEXT("prop")) : Use;
		}
		else
		{
			Why.Add(FString::Printf(TEXT("use: %s, as given"), *Use));
		}
		const FUseDefaults& Row = Defaults(Use);
		Out.Use = Use;
		Why.Add(Row.Why);

		// Words that move the choice: detail up, poly down, quads, parts.
		const bool bDetailWords = AsksDetail(Words);
		const bool bLowWords = AsksLowPoly(Words);
		const bool bQuadWords = AsksQuads(Words);
		const bool bPartsWords = AsksParts(Words);
		const bool b8kWords = HasAny(Words, {TEXT("8k"), TEXT("extreme detail texture")});

		// The family.
		FString Family = Row.Family;
		const FString ModelOverride = Brief.Model.TrimStartAndEnd();
		if (!ModelOverride.IsEmpty())
		{
			const FString Lower = ModelOverride.ToLower();
			if (Lower == TEXT("h3.1") || Lower == TEXT("h3") || Lower == TEXT("high") || Lower == TEXT("high_detail") || ModelOverride == H31Model)
			{
				Family = TEXT("H3.1");
			}
			else if (Lower == TEXT("p2") || Lower == TEXT("smart_poly") || Lower == TEXT("smartpoly") || ModelOverride == P2Model)
			{
				Family = TEXT("P2");
			}
			else
			{
				OutError = FString::Printf(TEXT("model is H3.1 (%s) or P2 (%s)."), H31Model, P2Model);
				return false;
			}
			Why.Add(FString::Printf(TEXT("family: %s, as given"), *Family));
		}
		else if (bDetailWords && Family == TEXT("P2") && Use != TEXT("background") && Use != TEXT("miniature"))
		{
			Family = TEXT("H3.1");
			Why.Add(TEXT("family: H3.1, because the brief asks for fine detail (photoreal, intricate, cinematic...)"));
		}
		else if (bLowWords && Family == TEXT("H3.1"))
		{
			Family = TEXT("P2");
			Why.Add(TEXT("family: P2, because the brief asks for low poly"));
		}
		else if (Switch(Brief.Parts).Get(false) || (bPartsWords && !Switch(Brief.Parts).IsSet()))
		{
			if (Family != TEXT("H3.1"))
			{
				Family = TEXT("H3.1");
				Why.Add(TEXT("family: H3.1, the only one that makes editable parts"));
			}
		}
		const bool bH = Family == TEXT("H3.1");
		Out.Family = Family;
		Out.Model = bH ? H31Model : P2Model;

		// Topology.
		const FString Topology = Brief.Topology.TrimStartAndEnd().ToLower();
		if (!Topology.IsEmpty() && Topology != TEXT("tris") && Topology != TEXT("triangles") && Topology != TEXT("quads") && Topology != TEXT("quad"))
		{
			OutError = TEXT("topology is tris or quads.");
			return false;
		}
		Out.bQuad = Topology.IsEmpty() ? (Row.bQuad || bQuadWords) : Topology.StartsWith(TEXT("quad"));
		Why.Add(!Topology.IsEmpty() ? FString::Printf(TEXT("topology: %s, as given"), Out.bQuad ? TEXT("quads") : TEXT("triangles"))
			: Out.bQuad ? (Row.bQuad ? FString(TEXT("topology: quads, for clean deformation")) : FString(TEXT("topology: quads, because the brief asks for clean topology")))
			: FString(TEXT("topology: triangles, what a game engine renders")));

		// Parts and smart low poly, H3.1's own.
		const TOptional<bool> PartsChoice = Switch(Brief.Parts);
		const TOptional<bool> LowPolyChoice = Switch(Brief.SmartLowPoly);
		if (!bH && (PartsChoice.Get(false) || LowPolyChoice.Get(false)))
		{
			OutError = TEXT("parts and smart_low_poly are H3.1 options; P2 already makes clean low-poly topology. Drop them or choose model H3.1.");
			return false;
		}
		const FString Geometry = Brief.GeometryQuality.TrimStartAndEnd().ToLower();
		if (!bH && Geometry == TEXT("detailed"))
		{
			OutError = TEXT("geometry_quality detailed is an H3.1 option; P2 has no Ultra mode.");
			return false;
		}
		Out.bParts = bH && (PartsChoice.IsSet() ? PartsChoice.GetValue() : bPartsWords);
		Out.bSmartLowPoly = bH && !Out.bParts && (LowPolyChoice.IsSet() ? LowPolyChoice.GetValue() : bLowWords);
		if (Out.bParts)
		{
			Why.Add(PartsChoice.IsSet() ? FString(TEXT("parts: on, as given")) : FString(TEXT("parts: on, because the brief asks for separate parts")));
			if (Out.bQuad)
			{
				Out.bQuad = false;
				Why.Add(TEXT("topology: triangles after all; Tripo returns parts as triangle meshes and ignores quad"));
			}
		}
		if (Out.bSmartLowPoly)
		{
			Why.Add(LowPolyChoice.IsSet() ? FString(TEXT("smart low poly: on, as given")) : FString(TEXT("smart low poly: on, because the brief asks for low poly on H3.1")));
		}

		// The face budget, then its range.
		const FString Platform = Brief.Platform.TrimStartAndEnd().ToLower();
		Out.Platform = Platform.IsEmpty() ? FString(TEXT("pc")) : Platform;
		if (Out.Platform != TEXT("pc") && Out.Platform != TEXT("console") && Out.Platform != TEXT("mobile") && Out.Platform != TEXT("vr"))
		{
			OutError = TEXT("platform is pc, console, mobile or vr.");
			return false;
		}
		if (Out.bSmartLowPoly)
		{
			Out.MinFaces = MinFaces;
			Out.MaxFaces = Out.bQuad ? SmartLowPolyMaxQuads : SmartLowPolyMaxTris;
		}
		else
		{
			Out.MinFaces = MinFaces;
			Out.MaxFaces = bH ? (Out.bQuad ? H31MaxQuads : H31MaxTris) : (Out.bQuad ? P2MaxQuads : P2MaxTris);
		}
		int32 Faces = Row.Faces;
		if (Row.bQuad != Out.bQuad)
		{
			// The use's budget is in its own topology: a quad is two triangles.
			Faces = Out.bQuad ? Faces / 2 : Faces * 2;
		}
		if (bDetailWords && bH && Brief.FaceLimit <= 0 && Use != TEXT("weapon"))
		{
			// Four times the use's budget, not a flat figure: a sword asked for in detail is not a cliff.
			const int32 Raised = FMath::Clamp(Faces * 4, Out.bQuad ? 20000 : 40000, Out.bQuad ? 50000 : 300000);
			if (Raised > Faces)
			{
				Faces = Raised;
				Why.Add(FString::Printf(TEXT("faces raised to %d (four times a %s's budget) for the detail the brief asks for"), Faces, *Use));
			}
		}
		if (Out.Platform == TEXT("mobile"))
		{
			Faces /= 4;
			Why.Add(TEXT("faces quartered for mobile"));
		}
		else if (Out.Platform == TEXT("vr"))
		{
			Faces /= 2;
			Why.Add(TEXT("faces halved for VR's frame rate"));
		}
		if (Brief.FaceLimit > 0)
		{
			if (Brief.FaceLimit < Out.MinFaces || Brief.FaceLimit > Out.MaxFaces)
			{
				OutError = FString::Printf(TEXT("face_limit %d is outside %s%s's range, %d to %d %s."), Brief.FaceLimit, *Family, Out.bSmartLowPoly ? TEXT(" smart low poly") : TEXT(""),
					Out.MinFaces, Out.MaxFaces, Out.bQuad ? TEXT("quads") : TEXT("triangles"));
				return false;
			}
			Faces = Brief.FaceLimit;
			Why.Add(FString::Printf(TEXT("faces: %d, as given"), Faces));
		}
		const int32 Clamped = FMath::Clamp(Faces, Out.MinFaces, Out.MaxFaces);
		if (Clamped != Faces)
		{
			Why.Add(FString::Printf(TEXT("faces: %d, held to %s's range %d-%d"), Clamped, *Family, Out.MinFaces, Out.MaxFaces));
		}
		Out.FaceLimit = Clamped;
		Why.Add(FString::Printf(TEXT("budget: %d %s"), Out.FaceLimit, Out.bQuad ? TEXT("quads") : TEXT("triangles")));

		Out.GeometryQuality = TEXT("standard");
		if (bH)
		{
			const bool bNeedsUltra = !Out.bQuad && Out.FaceLimit > H31StandardMaxTris;
			if (Geometry == TEXT("standard") && bNeedsUltra)
			{
				OutError = TEXT("H3.1 makes more than 1,500,000 triangles only with geometry_quality detailed.");
				return false;
			}
			Out.GeometryQuality = Geometry == TEXT("detailed") || bNeedsUltra || (Geometry.IsEmpty() && bDetailWords && Use == TEXT("hero_prop")) ? TEXT("detailed") : TEXT("standard");
			if (Out.GeometryQuality == TEXT("detailed"))
			{
				Why.Add(bNeedsUltra ? FString(TEXT("geometry: detailed (Ultra), needed above 1,500,000 triangles"))
					: Geometry == TEXT("detailed") ? FString(TEXT("geometry: detailed, as given")) : FString(TEXT("geometry: detailed (Ultra), for a hero piece the brief wants finely detailed")));
			}
		}

		// Texture tier and PBR.
		FString Tier = Brief.TextureQuality.TrimStartAndEnd().ToLower();
		if (!Tier.IsEmpty() && Tier != TEXT("none") && Tier != TEXT("fast") && Tier != TEXT("standard") && Tier != TEXT("detailed") && Tier != TEXT("extreme"))
		{
			OutError = TEXT("texture_quality is none, fast, standard, detailed or extreme.");
			return false;
		}
		if (Tier.IsEmpty())
		{
			Tier = Row.Texture;
			if (b8kWords)
			{
				Tier = TEXT("extreme");
				Why.Add(TEXT("textures: extreme (8K), because the brief asks for it"));
			}
			else if (bDetailWords && Tier != TEXT("extreme"))
			{
				Tier = TierUp(Tier);
				Why.Add(FString::Printf(TEXT("textures: %s, a tier up for the detail the brief asks for"), *Tier));
			}
			if (Out.Platform == TEXT("mobile") && (Tier == TEXT("detailed") || Tier == TEXT("extreme")))
			{
				Tier = TEXT("standard");
				Why.Add(TEXT("textures: standard, the most a mobile budget wants"));
			}
			Why.Add(FString::Printf(TEXT("textures: %s"), *Tier));
		}
		else
		{
			Why.Add(FString::Printf(TEXT("textures: %s, as given"), *Tier));
		}
		Out.TextureQuality = Tier;
		const TOptional<bool> PbrChoice = Switch(Brief.Pbr);
		Out.bPbr = Tier != TEXT("none") && PbrChoice.Get(true);
		if (Tier == TEXT("none") && PbrChoice.Get(false))
		{
			OutError = TEXT("pbr needs textures: Tripo forces textures on when pbr is on. Choose a texture_quality other than none.");
			return false;
		}
		Why.Add(Out.bPbr ? FString(TEXT("PBR: base colour, metallic, roughness and normal maps, into an ORM material")) : FString(TEXT("PBR: off")));
		Out.TextureVersion = Tier == TEXT("none") ? FString() : FString(TextureVersion);
		if (Out.bParts && Tier != TEXT("none"))
		{
			Why.Add(TEXT("parts are made untextured (Tripo refuses texture with parts), then textured by a /models/texture task, priced below"));
		}

		// Size, density, Nanite, collision.
		Out.SizeMeters = Brief.SizeMeters > 0.f ? Brief.SizeMeters : Row.SizeMeters;
		Why.Add(Brief.SizeMeters > 0.f ? FString::Printf(TEXT("size: %.2f m, as given"), Out.SizeMeters)
			: FString::Printf(TEXT("size: %.2f m, a typical %s (give size_meters to set it)"), Out.SizeMeters, *Use));
		Out.TexelDensity = Out.Platform == TEXT("mobile") ? Row.TexelDensity / 2.f : Row.TexelDensity;
		const bool bDeforms = Use == TEXT("character") || Use == TEXT("creature");
		Out.bNanite = Row.bNanite && !Out.bQuad && !bDeforms;
		if (!Out.bNanite && !Out.bQuad && !bDeforms && Out.FaceLimit > 100000)
		{
			// Dense triangles without Nanite cost every frame; a static mesh this dense should be Nanite.
			Out.bNanite = true;
			Why.Add(FString::Printf(TEXT("Nanite on: %d triangles is too dense to draw without it"), Out.FaceLimit));
		}
		Out.Collision = Row.Collision;

		// Input.
		const bool bEdits = !Brief.ViewEdits.IsEmpty();
		const bool bViews = Brief.MultiviewImages.Num() > 0 || !Brief.MultiviewTaskId.TrimStartAndEnd().IsEmpty();
		const bool bImage = !Brief.ImagePath.TrimStartAndEnd().IsEmpty();
		if (bViews && bImage)
		{
			OutError = TEXT("Give image_path or multiview views, not both.");
			return false;
		}
		// Edited views: Tripo's edit-multiview works on four views it made (image-to-multiview) or an earlier such task.
		if (bEdits && !bImage && Brief.MultiviewTaskId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("view_edits need image_path (four views are made from it first) or multiview_task_id.");
			return false;
		}
		TSet<FString> EditedViews;
		for (const FString& Edit : Brief.ViewEdits)
		{
			FString View, What;
			if (!Edit.Split(TEXT(":"), &View, &What) || What.TrimStartAndEnd().IsEmpty() || What.Len() > 1024)
			{
				OutError = TEXT("Each view edit is \"view: what to change\" (up to 1024 characters), such as \"back: add a quiver\".");
				return false;
			}
			View = View.TrimStartAndEnd().ToLower();
			if ((View != TEXT("front") && View != TEXT("left") && View != TEXT("back") && View != TEXT("right")) || EditedViews.Contains(View))
			{
				OutError = TEXT("A view edit names front, left, back or right, each at most once.");
				return false;
			}
			EditedViews.Add(View);
			Out.ViewEdits.Add(View + TEXT(": ") + What.TrimStartAndEnd());
		}
		if (Brief.ViewEdits.Num() > 4)
		{
			OutError = TEXT("Four view edits at most, one a view.");
			return false;
		}
		Out.bMakeViews = bEdits && bImage;
		const bool bMultiview = bViews || bEdits;
		if (!bMultiview && !bImage && Prompt.IsEmpty())
		{
			OutError = TEXT("Say what to make in prompt, or give image_path or multiview views.");
			return false;
		}
		if (Prompt.Len() > MaxPromptCharacters)
		{
			OutError = TEXT("prompt is 1024 characters at most.");
			return false;
		}
		if (Brief.MultiviewImages.Num() > 0)
		{
			int32 Views = 0;
			for (const FString& View : Brief.MultiviewImages)
			{
				Views += !View.TrimStartAndEnd().IsEmpty();
			}
			if (Brief.MultiviewImages.Num() > 4 || Brief.MultiviewImages[0].TrimStartAndEnd().IsEmpty() || Views < 2)
			{
				OutError = TEXT("multiview_images is front, left, back, right (\"\" skips one): the front view and at least one more.");
				return false;
			}
		}
		Out.Input = bMultiview ? TEXT("multiview") : bImage ? TEXT("image") : TEXT("text");
		if (bEdits)
		{
			Why.Add(FString::Printf(TEXT("views: %s, edited (%s), then made into the model from all four"),
				Out.bMakeViews ? TEXT("four made from the picture first") : TEXT("the given multiview task's"), *FString::Join(Out.ViewEdits, TEXT("; "))));
		}
		Out.Endpoint = bMultiview ? TEXT("/generation/multiview-to-model") : bImage ? TEXT("/generation/image-to-model") : TEXT("/generation/text-to-model");
		if (Out.bQuad && bH)
		{
			Why.Add(TEXT("H3.1 returns quads as FBX, which is imported as it is; quads held to your range, 500-50,000 (Tripo's own cap is 150,000)"));
		}

		const FString Negative = Brief.NegativePrompt.TrimStartAndEnd();
		if (Negative.Len() > MaxNegativeCharacters)
		{
			OutError = TEXT("negative_prompt is 255 characters at most.");
			return false;
		}
		Out.NegativePrompt = Negative.IsEmpty() && Out.Input == TEXT("text") ? FString(DefaultNegative) : Negative;

		Out.EstimatedCost = GenerationPrice(Out, &Out.PriceLines);
		Out.Credits = Credits(Out.EstimatedCost);
		TArray<FString> Placeholders;
		if (bImage && !bMultiview)
		{
			Placeholders.Add(TEXT("<file_token of image>"));
		}
		for (int32 Index = 0; Index < Brief.MultiviewImages.Num(); ++Index)
		{
			Placeholders.Add(Brief.MultiviewImages[Index].TrimStartAndEnd().IsEmpty() ? FString() : FString::Printf(TEXT("<file_token of view %d>"), Index));
		}
		if (Out.Use == TEXT("weapon") && !Out.bQuad && !PlanWeapon(Brief, bDetailWords, Out, Why, OutError))
		{
			return false;
		}
		Out.RequestJson = HyperAIStudio::MeshGen::Private::JsonText(GenerationBody(Out, Brief, Placeholders));
		return true;
	}

	// ---------------------------------------------------------------- the reference picture

	TArray<FString> TripoImageModels()
	{
		return {TEXT("chat_image_2.5_sunburst"), TEXT("chat_image_2.5_flare"), TEXT("chat_image_2"), TEXT("chat_image_1.5"), TEXT("banana2"), TEXT("banana_pro"),
			TEXT("banana"), TEXT("seedream_v5"), TEXT("seedream_v4")};
	}

	bool ImageModelFromName(const FString& In, FString& OutId, FString& OutNote)
	{
		FString Name = In.TrimStartAndEnd().ToLower().Replace(TEXT(" "), TEXT("_")).Replace(TEXT("-"), TEXT("_"));
		while (Name.Contains(TEXT("__")))
		{
			Name = Name.Replace(TEXT("__"), TEXT("_"));
		}
		auto Refuse = [&OutNote, &In](const FString& Why)
		{
			OutNote = Why.IsEmpty() ? FString::Printf(TEXT("image_model %s is not one of Tripo's: %s."), *In, *FString::Join(TripoImageModels(), TEXT(", "))) : Why;
			return false;
		};
		// What people call OpenAI's models is Tripo's chat_image family.
		for (const TCHAR* Prefix : {TEXT("gpt_image"), TEXT("chatgpt_image"), TEXT("openai_image"), TEXT("image_gen"), TEXT("chat_image")})
		{
			if (Name.StartsWith(Prefix))
			{
				Name = TEXT("chat_image") + Name.Mid(FCString::Strlen(Prefix));
				break;
			}
		}
		Name = Name.Replace(TEXT("nano_banana_pro"), TEXT("banana_pro")).Replace(TEXT("nano_banana_2"), TEXT("banana2")).Replace(TEXT("nano_banana2"), TEXT("banana2"))
			.Replace(TEXT("nano_banana"), TEXT("banana")).Replace(TEXT("banana_2"), TEXT("banana2"));
		if (Name == TEXT("seedream_5") || Name == TEXT("seedream5") || Name == TEXT("seedream"))
		{
			Name = TEXT("seedream_v5");
		}
		else if (Name == TEXT("seedream_4") || Name == TEXT("seedream4"))
		{
			Name = TEXT("seedream_v4");
		}
		if (Name.StartsWith(TEXT("chat_image")))
		{
			TArray<FString> Tokens;
			Name.Mid(10).ParseIntoArray(Tokens, TEXT("_"));
			FString Version = Tokens.IsEmpty() ? FString(TEXT("2.5")) : Tokens[0];
			FString Tier = Tokens.Num() > 1 ? Tokens[1] : FString();
			if (Version.Len() > 0 && !FChar::IsDigit(Version[0]))
			{
				// chat_image_sunburst: the tier with no version, the newest version.
				Tier = Version;
				Version = TEXT("2.5");
			}
			if (Version == TEXT("1") || Version == TEXT("1.0"))
			{
				return Refuse(TEXT("chat_image_1 was retired on 23 October 2026: use chat_image_2.5_flare or chat_image_2.5_sunburst."));
			}
			if (Version == TEXT("2.5"))
			{
				if (Tier.IsEmpty())
				{
					Tier = TEXT("flare");
				}
				else if (EditDistance(Tier, TEXT("sunburst")) <= 2)
				{
					Tier = TEXT("sunburst");
				}
				else if (EditDistance(Tier, TEXT("flare")) <= 2)
				{
					Tier = TEXT("flare");
				}
				else
				{
					return Refuse(FString::Printf(TEXT("chat_image_2.5 comes as flare (quick) or sunburst (most faithful), not %s."), *Tier));
				}
				Name = TEXT("chat_image_2.5_") + Tier;
			}
			else
			{
				Name = TEXT("chat_image_") + Version;
			}
			if (Version == TEXT("1.5"))
			{
				OutNote = TEXT("(chat_image_1.5 retires on 1 December 2026; chat_image_2.5 replaces it)");
			}
		}
		if (!TripoImageModels().Contains(Name))
		{
			return Refuse(FString());
		}
		OutId = Name;
		const FString Given = In.TrimStartAndEnd();
		if (!Given.Equals(Name, ESearchCase::IgnoreCase))
		{
			OutNote = FString::Printf(TEXT("(read from '%s')%s%s"), *Given, OutNote.IsEmpty() ? TEXT("") : TEXT(" "), *OutNote);
		}
		return true;
	}

	bool ImageModelTakesQuality(const FString& Model, const FString& Quality)
	{
		if (Model == TEXT("chat_image_2"))
		{
			return Quality == TEXT("low") || Quality == TEXT("medium") || Quality == TEXT("high");
		}
		return Model.StartsWith(TEXT("chat_image_2.5")) && ImageQualities().Contains(Quality);
	}

	bool ImageModelTakesBackground(const FString& Model)
	{
		return Model.StartsWith(TEXT("chat_image_2.5"));
	}

	bool ImageSizeValid(const FString& Model, const FString& Size, FString& OutError)
	{
		if (Size.IsEmpty())
		{
			return true;
		}
		int32 Width = 0, Height = 0;
		FString W, H;
		const bool bPixels = Size.ToLower().Split(TEXT("x"), &W, &H) && W.IsNumeric() && H.IsNumeric();
		if (bPixels)
		{
			Width = FCString::Atoi(*W);
			Height = FCString::Atoi(*H);
		}
		if (Model.StartsWith(TEXT("chat_image")))
		{
			static const TArray<FString> Presets = {TEXT("1024x1024"), TEXT("1536x1024"), TEXT("1024x1536"), TEXT("2048x2048"), TEXT("2048x1152"), TEXT("3840x2160"),
				TEXT("2160x3840"), TEXT("auto")};
			if (Presets.Contains(Size.ToLower()))
			{
				return true;
			}
			const int64 Pixels = int64(Width) * Height;
			if (!bPixels || Width <= 0 || Height <= 0 || Width > 3840 || Height > 3840 || Width % 16 || Height % 16 || FMath::Max(Width, Height) > 3 * FMath::Min(Width, Height)
				|| Pixels < 655360 || Pixels > 8294400)
			{
				OutError = TEXT("image_size for chat_image is a preset (1024x1024, 1536x1024, 1024x1536, 2048x2048, 2048x1152, 3840x2160, 2160x3840, auto) or WxH: ")
					TEXT("edges up to 3840 in multiples of 16, the long edge at most three times the short, 655,360 to 8,294,400 pixels.");
				return false;
			}
			return true;
		}
		if (Model == TEXT("seedream_v5"))
		{
			if (Size == TEXT("2K") || Size == TEXT("3K"))
			{
				return true;
			}
			OutError = TEXT("image_size for seedream_v5 is 2K or 3K.");
			return false;
		}
		if (Model == TEXT("seedream_v4"))
		{
			const double Mega = double(Width) * Height / 1e6;
			if (Size == TEXT("2K") || Size == TEXT("4K") || (bPixels && Mega >= 3.69 && Mega <= 16.78))
			{
				return true;
			}
			OutError = TEXT("image_size for seedream_v4 is 2K, 4K, or WxH of 3.69 to 16.78 megapixels.");
			return false;
		}
		OutError = FString::Printf(TEXT("%s takes no image_size."), *Model);
		return false;
	}

	FString ImageSizeTier(const FString& Size)
	{
		FString W, H;
		if (Size.ToLower() == TEXT("auto") || Size == TEXT("2K"))
		{
			return TEXT("2k");
		}
		if (Size == TEXT("3K") || Size == TEXT("4K"))
		{
			return TEXT("4k");
		}
		if (Size.ToLower().Split(TEXT("x"), &W, &H))
		{
			const int32 Long = FMath::Max(FCString::Atoi(*W), FCString::Atoi(*H));
			return Long <= 1024 ? TEXT("1k") : Long <= 2048 ? TEXT("2k") : TEXT("4k");
		}
		return TEXT("1k");
	}

	float ImagePrice(const FString& Model, const FString& Quality, const FString& Size)
	{
		const FString Key = TEXT("img_") + Model + (Quality.IsEmpty() ? FString() : TEXT("_") + Quality) + TEXT("_") + ImageSizeTier(Size);
		return FMath::Max(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("tripo"), Key), 0.f);
	}

	TSharedRef<FJsonObject> TextToImageBody(const FHyperAIModelPlan& Plan)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("model"), Plan.ImageModel);
		Json->SetStringField(TEXT("prompt"), Plan.ImagePrompt);
		if (!Plan.ImageSize.IsEmpty())
		{
			Json->SetStringField(TEXT("size"), Plan.ImageSize);
		}
		// quality only where the model takes it: any other model refuses the field.
		if (!Plan.ImageQuality.IsEmpty() && ImageModelTakesQuality(Plan.ImageModel, Plan.ImageQuality))
		{
			Json->SetStringField(TEXT("quality"), Plan.ImageQuality);
		}
		if (!Plan.ImageBackground.IsEmpty() && ImageModelTakesBackground(Plan.ImageModel))
		{
			Json->SetStringField(TEXT("background"), Plan.ImageBackground);
		}
		Json->SetStringField(TEXT("output_format"), TEXT("png"));
		if (!Plan.ImageTemplate.IsEmpty())
		{
			Json->SetStringField(TEXT("template"), Plan.ImageTemplate);
		}
		return Json;
	}

	/** The picture made first, when the brief is words alone: its model, quality, size, background, template and words. */
	static bool PlanReference(const FHyperAIModelBrief& Brief, FHyperAIModelPlan& Out, FString& OutError)
	{
		TArray<FString>& Why = Out.Reasons;
		const FString Choice = Brief.ReferenceImage.TrimStartAndEnd().ToLower();
		const TOptional<bool> Asked = Switch(Choice);
		if (!IsAuto(Choice) && !Asked.IsSet())
		{
			OutError = TEXT("reference_image is auto, on or off.");
			return false;
		}
		const bool bHi = Out.Provider == TEXT("hi3d");
		if (Out.Input != TEXT("text"))
		{
			if (Asked.Get(false))
			{
				OutError = TEXT("reference_image on makes a picture from the words; this brief already gives a picture or views.");
				return false;
			}
			return true;
		}
		bool bOn = true;
		if (bHi)
		{
			if (Asked.IsSet() && !Asked.GetValue())
			{
				OutError = TEXT("Hi3D works from a picture: from words alone it needs reference_image on (or auto), or choose provider tripo.");
				return false;
			}
			Why.Add(TEXT("reference picture: made first from the words, as Hi3D works from pictures"));
		}
		else if (Asked.IsSet())
		{
			bOn = Asked.GetValue();
			Why.Add(bOn ? FString(TEXT("reference picture: on, as given")) : FString(TEXT("reference picture: off, as given; text-to-model")));
		}
		else
		{
			bOn = Out.Family == TEXT("P2") || (Out.Use != TEXT("background") && Out.Use != TEXT("miniature"));
			Why.Add(bOn ? FString(TEXT("reference picture: made first, so the shape is pinned before the 3D is paid for and the visual check has a reference"))
				: FString(TEXT("reference picture: off for a background or miniature piece; text-to-model")));
		}
		if (!bOn)
		{
			return true;
		}
		Out.bReferenceFirst = true;
		Out.ReferenceSource = TEXT("tripo");
		FString Model = TEXT("chat_image_2.5_flare");
		FString Note;
		if (!Brief.ImageModel.TrimStartAndEnd().IsEmpty() && !ImageModelFromName(Brief.ImageModel, Model, Note))
		{
			OutError = Note;
			return false;
		}
		Out.ImageModel = Model;
		Why.Add(Brief.ImageModel.TrimStartAndEnd().IsEmpty() ? FString::Printf(TEXT("image model: %s, the quick chat_image tier (chat_image_2.5_sunburst is the most faithful)"), *Model)
			: FString::Printf(TEXT("image model: %s%s%s"), *Model, Note.IsEmpty() ? TEXT(", as given") : TEXT(" "), *Note));
		const bool bBig = Out.Use == TEXT("hero_prop") || Out.Use == TEXT("vehicle") || Out.TextureQuality == TEXT("extreme");
		FString Quality = Brief.ImageQuality.TrimStartAndEnd().ToLower();
		if (!Quality.IsEmpty())
		{
			if (!ImageModelTakesQuality(Model, Quality))
			{
				OutError = Model.StartsWith(TEXT("chat_image_2.5")) ? FString(TEXT("image_quality is low, medium, high, xhigh or max."))
					: Model == TEXT("chat_image_2") ? FString(TEXT("chat_image_2 takes image_quality low, medium or high."))
					: FString::Printf(TEXT("%s takes no image_quality; only the chat_image models do."), *Model);
				return false;
			}
		}
		else if (ImageModelTakesQuality(Model, TEXT("high")))
		{
			Quality = Out.Platform == TEXT("mobile") ? TEXT("medium") : TEXT("high");
		}
		FString Size = Brief.ImageSize.TrimStartAndEnd();
		if (Size.IsEmpty())
		{
			Size = Model.StartsWith(TEXT("chat_image")) ? FString(bBig ? TEXT("2048x2048") : TEXT("1024x1024")) : Model.StartsWith(TEXT("seedream")) ? FString(TEXT("2K")) : FString();
		}
		if (!ImageSizeValid(Model, Size, OutError))
		{
			return false;
		}
		FString Background = Brief.ImageBackground.TrimStartAndEnd().ToLower();
		if (!Background.IsEmpty() && Background != TEXT("auto") && Background != TEXT("opaque") && Background != TEXT("transparent"))
		{
			OutError = TEXT("image_background is auto, opaque or transparent.");
			return false;
		}
		if (ImageModelTakesBackground(Model))
		{
			Background = Background.IsEmpty() ? FString(TEXT("transparent")) : Background;
		}
		else if (Background == TEXT("transparent"))
		{
			OutError = FString::Printf(TEXT("A transparent background is the chat_image_2.5 models'; %s draws one."), *Model);
			return false;
		}
		else
		{
			Background.Reset();
		}
		FString Template = Brief.ImageTemplate.TrimStartAndEnd().ToLower();
		if (Template == TEXT("none"))
		{
			Template.Reset();
		}
		else if (Template.IsEmpty())
		{
			Template = TemplateForUse(Out.Use, Brief.Prompt.ToLower());
		}
		else if (Template != TEXT("asset_extraction") && Template != TEXT("character_completion") && Template != TEXT("t_pose") && Template != TEXT("variants") && Template != TEXT("figure"))
		{
			OutError = TEXT("image_template is asset_extraction, character_completion, t_pose, variants, figure or none.");
			return false;
		}
		Out.ImageQuality = Quality;
		Out.ImageSize = Size;
		Out.ImageBackground = Background;
		Out.ImageTemplate = Template;
		const FString Negative = Out.NegativePrompt.IsEmpty() ? FString(DefaultNegative) : Out.NegativePrompt;
		const FString View = Out.Use == TEXT("creature") ? FString(TEXT(", full body, side three-quarter view"))
			: FString(TEXT(", single isolated object, entire object in frame, three-quarter front view, centred"));
		Out.ImagePrompt = Brief.Prompt.TrimStartAndEnd() + View + TEXT(", even neutral lighting, no cast shadows, no ground plane, no text")
			+ (Background.IsEmpty() ? TEXT(", plain light-grey background") : TEXT("")) + TEXT(" --no ") + Negative;
		Why.Add(FString::Printf(TEXT("picture: %s%s%s, %s%s%s, PNG"), *Model, Quality.IsEmpty() ? TEXT("") : TEXT(" "), *Quality, Size.IsEmpty() ? TEXT("the model's own size") : *Size,
			Background.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", %s background"), *Background), Template.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", template %s"), *Template)));
		const TOptional<bool> Review = Switch(Brief.ReviewReference);
		if (!IsAuto(Brief.ReviewReference) && !Review.IsSet())
		{
			OutError = TEXT("review_reference is auto, on or off.");
			return false;
		}
		Out.bReviewReference = Review.IsSet() ? Review.GetValue()
			: Out.Use == TEXT("hero_prop") || Out.Use == TEXT("character") || Out.Use == TEXT("vehicle") || Out.Use == TEXT("weapon");
		Why.Add(Out.bReviewReference ? FString(TEXT("review: the picture is approved and paid for first; the 3D generation is asked for once you have seen it"))
			: FString(TEXT("review: one approval for the picture and the 3D generation")));
		Out.Input = TEXT("image");
		if (!bHi)
		{
			Out.Endpoint = TEXT("/generation/image-to-model");
			Why.Add(TEXT("the model is made from the picture's task (image-to-model, the picture's colours kept)"));
		}
		return true;
	}

	/** Hi3D's plan: the shared choices (use, platform, size, textures, Nanite) as Tripo's H3.1 would make them, then Hi3D's own. */
	static bool ClassifyHi3D(const FHyperAIModelBrief& Brief, const FString& ProviderWhy, FHyperAIModelPlan& Out, FString& OutError)
	{
		if (Brief.Topology.TrimStartAndEnd().ToLower().StartsWith(TEXT("quad")))
		{
			OutError = TEXT("Hi3D makes triangles; quads are Tripo's (provider tripo).");
			return false;
		}
		if (Switch(Brief.Parts).Get(false) || Switch(Brief.SmartLowPoly).Get(false) || !Brief.GeometryQuality.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("parts, smart_low_poly and geometry_quality are Tripo's options (provider tripo).");
			return false;
		}
		if (!Brief.ViewEdits.IsEmpty() || !Brief.MultiviewTaskId.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("view_edits and multiview_task_id are Tripo's; Hi3D takes views as multiview_images.");
			return false;
		}
		FHyperAIModelBrief Base = Brief;
		Base.Provider = TEXT("tripo");
		Base.Model = TEXT("H3.1");
		Base.FaceLimit = 0;
		Base.Topology = TEXT("tris");
		Base.Parts = TEXT("off");
		Base.SmartLowPoly = TEXT("off");
		Base.Resolution.Reset();
		FHyperAIModelPlan Shared;
		if (!ClassifyTripo(Base, Shared, OutError))
		{
			return false;
		}
		Out = FHyperAIModelPlan();
		TArray<FString>& Why = Out.Reasons;
		Why.Add(ProviderWhy);
		for (const FString& Line : Shared.Reasons)
		{
			if (Line.StartsWith(TEXT("use:")) || Line.StartsWith(TEXT("size:")) || Line.StartsWith(TEXT("faces quartered")) || Line.StartsWith(TEXT("faces halved")))
			{
				Why.Add(Line);
			}
		}
		Out.Provider = TEXT("hi3d");
		Out.Family = TEXT("Hi3D");
		Out.Use = Shared.Use;
		Out.Platform = Shared.Platform;
		Out.SizeMeters = Shared.SizeMeters;
		Out.TexelDensity = Shared.TexelDensity;
		Out.Collision = Shared.Collision;
		Out.bNanite = Shared.bNanite;
		Out.Input = Shared.Input;
		Out.NegativePrompt = Shared.NegativePrompt;
		Out.Endpoint = Hi3DSubmitPath;
		const FString Words = Brief.Prompt.ToLower();

		// Model and resolution.
		const FString GivenModel = Brief.Model.TrimStartAndEnd().ToLower();
		if (!GivenModel.IsEmpty() && !Hi3DIsModel(GivenModel))
		{
			OutError = FString::Printf(TEXT("model is one of Hi3D's: %s."), *FString::Join(Hi3DModels(), TEXT(", ")));
			return false;
		}
		Out.Model = !GivenModel.IsEmpty() ? GivenModel : AsksPortrait(Words) ? FString(Hi3DDefaultPortrait) : FString(Hi3DDefaultModel);
		Why.Add(!GivenModel.IsEmpty() ? FString::Printf(TEXT("model: %s, as given"), *Out.Model)
			: Hi3DIsPortrait(Out.Model) ? FString::Printf(TEXT("model: %s, Hi3D's portrait model for a head or bust"), *Out.Model)
			: FString::Printf(TEXT("model: %s (Ultra3D), Hi3D's most detailed"), *Out.Model));
		const TArray<FString> Resolutions = Hi3DResolutions(Out.Model);
		const FString GivenResolution = Brief.Resolution.TrimStartAndEnd().ToLower();
		if (!GivenResolution.IsEmpty())
		{
			if (!Resolutions.Contains(GivenResolution))
			{
				OutError = FString::Printf(TEXT("%s takes resolution %s."), *Out.Model, *FString::Join(Resolutions, TEXT(" or ")));
				return false;
			}
			Out.Resolution = GivenResolution;
			Why.Add(FString::Printf(TEXT("resolution: %s, as given"), *Out.Resolution));
		}
		else if (Out.Model == Hi3DDefaultModel)
		{
			const bool bMaster = HasWord(Words, {TEXT("extreme"), TEXT("master")}) || Brief.TextureQuality.TrimStartAndEnd().ToLower() == TEXT("extreme");
			Out.Resolution = bMaster ? TEXT("2048master") : TEXT("2048quality");
			Why.Add(bMaster ? FString(TEXT("resolution: 2048master, for an extreme or master brief (up to 5,000,000 faces)")) : FString(TEXT("resolution: 2048quality, v3.0's default")));
		}
		else
		{
			Out.Resolution = Resolutions.Last();
			Why.Add(FString::Printf(TEXT("resolution: %s, the model's finest"), *Out.Resolution));
		}

		// Textures and PBR.
		const FString Tier = Brief.TextureQuality.TrimStartAndEnd().ToLower();
		Out.TextureQuality = Tier == TEXT("none") ? FString(TEXT("none")) : Shared.TextureQuality;
		const TOptional<bool> PbrChoice = Switch(Brief.Pbr);
		if (PbrChoice.Get(false) && !Hi3DSupportsPbr(Out.Model))
		{
			OutError = FString::Printf(TEXT("%s has no PBR maps; v2.0, v2.1 and v3.0 do."), *Out.Model);
			return false;
		}
		Out.bPbr = Out.TextureQuality != TEXT("none") && Hi3DSupportsPbr(Out.Model) && PbrChoice.Get(true);
		Why.Add(Out.TextureQuality == TEXT("none") ? FString(TEXT("textures: none, geometry only (request type 1)"))
			: FString::Printf(TEXT("textures: Hi3D's own, made with the geometry%s"), Out.bPbr ? TEXT(", with PBR maps into an ORM material") : TEXT(", no PBR")));

		// Faces: Hi3D makes 100,000 at least.
		const int32 Most = Hi3DMaxFacesFor(Out.Resolution);
		if (Brief.FaceLimit > 0 && (Brief.FaceLimit < MinFaces || Brief.FaceLimit > Most))
		{
			OutError = FString::Printf(TEXT("face_limit %d is outside Hi3D's reach at %s: up to %d (and 100,000 at least, smaller budgets reduced in the editor)."),
				Brief.FaceLimit, *Out.Resolution, Most);
			return false;
		}
		const int32 Budget = Brief.FaceLimit > 0 ? Brief.FaceLimit : Shared.FaceLimit;
		Out.MinFaces = MinFaces;
		Out.MaxFaces = Most;
		if (Out.bNanite && Brief.FaceLimit <= 0)
		{
			Out.ProviderFaces = FMath::Min(Hi3DRecommendedFaces(Out.Resolution), Most);
			Out.FaceLimit = Out.ProviderFaces;
			Why.Add(FString::Printf(TEXT("faces: %d, Hi3D's own count at %s; a Nanite piece keeps the dense mesh"), Out.ProviderFaces, *Out.Resolution));
		}
		else if (Budget < Hi3DMinFaces)
		{
			Out.ProviderFaces = Hi3DMinFaces;
			Out.FaceLimit = Budget;
			Out.bLocalReduce = true;
			Why.Add(FString::Printf(TEXT("faces: made at 100,000 (Hi3D's least), then reduced in the editor for free to the budget of %d triangles"), Budget));
		}
		else
		{
			Out.ProviderFaces = FMath::Min(Budget, Most);
			Out.FaceLimit = Out.ProviderFaces;
			Why.Add(FString::Printf(TEXT("faces: %d%s"), Out.FaceLimit, Brief.FaceLimit > 0 ? TEXT(", as given") : TEXT(", the use's budget")));
		}
		Why.Add(Out.bNanite ? FString(TEXT("Nanite: on")) : FString(TEXT("Nanite: off for this use")));
		if (!PlanReference(Brief, Out, OutError))
		{
			return false;
		}
		Out.EstimatedCost = GenerationPrice(Out, &Out.PriceLines);
		Out.Credits = Hi3DCredits(Hi3DPrice(Out.Model, Out.Resolution, Out.TextureQuality == TEXT("none") ? 1 : 3, Out.bPbr));

		// The form as it will go, pictures as placeholders.
		TArray<FString> Images;
		const bool bViews = Brief.MultiviewImages.Num() > 0;
		if (bViews)
		{
			for (const FString& View : Hi3DViewOrder(Brief.MultiviewImages))
			{
				Images.Add(View.IsEmpty() ? FString() : TEXT("<view>"));
			}
		}
		else
		{
			Images.Add(Out.bReferenceFirst ? TEXT("<reference picture>") : TEXT("<image>"));
		}
		TArray<TPair<FString, FString>> Text, Files;
		Hi3DFields(Hi3DRequestFromPlan(Out, Images, bViews), Text, Files);
		const TSharedRef<FJsonObject> Form = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Text)
		{
			Form->SetStringField(Pair.Key, Pair.Value);
		}
		for (const TPair<FString, FString>& Pair : Files)
		{
			Form->SetStringField(Pair.Key, Pair.Value);
		}
		if (Out.bReferenceFirst)
		{
			const TSharedRef<FJsonObject> Both = MakeShared<FJsonObject>();
			Both->SetObjectField(TEXT("text_to_image"), TextToImageBody(Out));
			Both->SetObjectField(TEXT("submit_task"), Form);
			Out.RequestJson = HyperAIStudio::MeshGen::Private::JsonText(Both);
		}
		else
		{
			Out.RequestJson = HyperAIStudio::MeshGen::Private::JsonText(Form);
		}
		return true;
	}

	bool Classify(const FHyperAIModelBrief& Brief, FHyperAIModelPlan& Out, FString& OutError, const bool bRig)
	{
		FString ProviderWhy;
		const FString Provider = ChooseProvider(Brief, bRig, ProviderWhy, OutError);
		if (Provider.IsEmpty())
		{
			return false;
		}
		if (Provider == TEXT("hi3d"))
		{
			return ClassifyHi3D(Brief, ProviderWhy, Out, OutError);
		}
		if (!ClassifyTripo(Brief, Out, OutError))
		{
			return false;
		}
		Out.Provider = TEXT("tripo");
		// A weapon is generated denser than its budget and finished down to it; anything else is asked for at its budget.
		Out.ProviderFaces = Out.ProviderFaces > 0 ? Out.ProviderFaces : Out.FaceLimit;
		Out.Reasons.Insert(ProviderWhy, 0);
		if (!PlanReference(Brief, Out, OutError))
		{
			return false;
		}
		if (Out.bReferenceFirst)
		{
			Out.PriceLines.Reset();
			Out.EstimatedCost = GenerationPrice(Out, &Out.PriceLines);
			Out.Credits = Credits(Out.EstimatedCost);
			const TSharedRef<FJsonObject> Both = MakeShared<FJsonObject>();
			Both->SetObjectField(TEXT("text_to_image"), TextToImageBody(Out));
			Both->SetObjectField(TEXT("image_to_model"), GenerationBody(Out, Brief, {TEXT("<task_id of the reference picture>")}));
			Out.RequestJson = HyperAIStudio::MeshGen::Private::JsonText(Both);
		}
		return true;
	}

	void UseImageGenReference(FHyperAIModelPlan& Plan)
	{
		FHyperAIStudioImageModelSettings Model;
		if (!Plan.bReferenceFirst || !ReferenceImageModel(Model))
		{
			return;
		}
		Plan.ReferenceSource = TEXT("imagegen");
		Plan.ImageModel = Model.Id;
		Plan.ImageQuality.Reset();
		Plan.ImageSize.Reset();
		Plan.ImageBackground.Reset();
		Plan.ImageTemplate.Reset();
		Plan.bReviewReference = false;
		Plan.Reasons.Add(FString::Printf(TEXT("picture: no Tripo key, so HyperAI's image generation (%s) makes it, in the same approval as the 3D"), *Model.Label));
		Plan.PriceLines.Reset();
		Plan.EstimatedCost = GenerationPrice(Plan, &Plan.PriceLines);
	}

	FCheckTargets TargetsForUse(const FString& Use, const FString& Platform, const float SizeMeters, const int32 FaceLimit, const FString& Topology)
	{
		FHyperAIModelBrief Brief;
		Brief.Use = Use;
		Brief.Platform = Platform;
		Brief.SizeMeters = SizeMeters;
		Brief.Topology = Topology;
		Brief.Prompt = TEXT("model");
		FHyperAIModelPlan Plan;
		FString Error;
		FCheckTargets Targets;
		Targets.Use = Use;
		if (!Classify(Brief, Plan, Error))
		{
			Targets.FaceLimit = FaceLimit;
			Targets.SizeMeters = SizeMeters;
			return Targets;
		}
		Targets = TargetsFromPlan(Plan);
		// With no use known there is no budget or size to hold it to, only what any game model needs.
		if (Use.IsEmpty())
		{
			Targets.FaceLimit = 0;
			Targets.MinFaces = 0;
			Targets.SizeMeters = SizeMeters;
			Targets.bNanite = false;
		}
		if (FaceLimit > 0)
		{
			Targets.FaceLimit = FaceLimit;
		}
		return Targets;
	}

	// ---------------------------------------------------------------- requests

	TSharedRef<FJsonObject> GenerationBody(const FHyperAIModelPlan& Plan, const FHyperAIModelBrief& Brief, const TArray<FString>& Inputs)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		const bool bH = Plan.Family == TEXT("H3.1");
		Json->SetStringField(TEXT("model"), Plan.Model);
		if (Plan.Input == TEXT("text"))
		{
			Json->SetStringField(TEXT("prompt"), Brief.Prompt.TrimStartAndEnd());
			if (!Plan.NegativePrompt.IsEmpty())
			{
				Json->SetStringField(TEXT("negative_prompt"), Plan.NegativePrompt);
			}
		}
		else if (Plan.Input == TEXT("image"))
		{
			Json->SetStringField(TEXT("input"), Inputs.IsEmpty() ? FString() : Inputs[0]);
			// The picture's colours first; the input is used as it is.
			Json->SetStringField(TEXT("texture_alignment"), TEXT("original_image"));
		}
		else
		{
			TArray<TSharedPtr<FJsonValue>> Views;
			if (!Brief.MultiviewTaskId.TrimStartAndEnd().IsEmpty())
			{
				const TSharedRef<FJsonObject> Task = MakeShared<FJsonObject>();
				Task->SetStringField(TEXT("task_id"), Brief.MultiviewTaskId.TrimStartAndEnd());
				Views.Add(MakeShared<FJsonValueObject>(Task));
			}
			else
			{
				// The view-key form Tripo recommends: {front: token}, {left: token}...; the server orders them.
				const TCHAR* const Names[] = {TEXT("front"), TEXT("left"), TEXT("back"), TEXT("right")};
				for (int32 Index = 0; Index < Inputs.Num() && Index < 4; ++Index)
				{
					if (!Inputs[Index].IsEmpty())
					{
						const TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
						View->SetStringField(Names[Index], Inputs[Index]);
						Views.Add(MakeShared<FJsonValueObject>(View));
					}
				}
			}
			Json->SetArrayField(TEXT("inputs"), Views);
			Json->SetStringField(TEXT("texture_alignment"), TEXT("original_image"));
		}
		Json->SetNumberField(TEXT("face_limit"), Plan.ProviderFaces > 0 ? Plan.ProviderFaces : Plan.FaceLimit);
		const bool bTextured = Plan.TextureQuality != TEXT("none") && !Plan.bParts;
		Json->SetBoolField(TEXT("texture"), bTextured);
		Json->SetBoolField(TEXT("pbr"), bTextured && Plan.bPbr);
		if (bTextured)
		{
			Json->SetStringField(TEXT("texture_quality"), Plan.TextureQuality);
			Json->SetStringField(TEXT("texture_version"), Plan.TextureVersion);
			Json->SetBoolField(TEXT("delight"), true);
		}
		if (Plan.bQuad)
		{
			Json->SetBoolField(TEXT("quad"), true);
		}
		if (bH)
		{
			Json->SetStringField(TEXT("geometry_quality"), Plan.GeometryQuality);
			if (Plan.bSmartLowPoly)
			{
				Json->SetBoolField(TEXT("smart_low_poly"), true);
			}
			if (Plan.bParts)
			{
				// Tripo refuses parts with texture or pbr (1004), and ignores quad: both are off above.
				Json->SetBoolField(TEXT("generate_parts"), true);
			}
		}
		// Metres, so the in-editor scale fix starts from a real size; export_uv keeps a UV layout for the textures.
		Json->SetBoolField(TEXT("auto_size"), true);
		Json->SetBoolField(TEXT("export_uv"), true);
		if (Brief.Seed > 0)
		{
			Json->SetNumberField(TEXT("model_seed"), Brief.Seed);
		}
		return Json;
	}

	TSharedRef<FJsonObject> RetopoBody(const FString& Input, const int32 FaceLimit, const bool bQuad, const bool bSmartUv, const bool bV1)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		Json->SetStringField(TEXT("model"), bV1 ? RetopoModelV1 : RetopoModel);
		Json->SetNumberField(TEXT("face_limit"), FMath::Clamp(FaceLimit, MinFaces, bV1 ? (bQuad ? RetopoV1MaxQuads : RetopoV1MaxTris) : bQuad ? RetopoMaxQuads : RetopoMaxTris));
		Json->SetBoolField(TEXT("quad"), bQuad);
		if (bV1)
		{
			// v1.0 takes neither smart_uv nor bake: the model keeps the UVs it has.
			return Json;
		}
		// smart_uv is valid only on v3.0 and does not change the price.
		Json->SetBoolField(TEXT("smart_uv"), bSmartUv);
		Json->SetBoolField(TEXT("bake"), true);
		return Json;
	}

	TSharedRef<FJsonObject> SmartUvBody(const FString& Input)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		Json->SetStringField(TEXT("model"), TEXT("default"));
		Json->SetBoolField(TEXT("bake"), true);
		return Json;
	}

	TSharedRef<FJsonObject> TextureBody(const FString& Input, const FString& Quality, const bool bPbr, const TArray<FString>& PartNames, const FString& Prompt,
		const FString& Image, const int32 Seed, const FString& Alignment)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		Json->SetStringField(TEXT("model"), TextureVersion);
		Json->SetStringField(TEXT("texture_quality"), Quality.IsEmpty() || Quality == TEXT("none") ? FString(TEXT("standard")) : Quality);
		Json->SetBoolField(TEXT("pbr"), bPbr);
		Json->SetBoolField(TEXT("delight"), true);
		Json->SetBoolField(TEXT("bake"), true);
		// texture_prompt takes one of text, image or images: the reference picture wins, as Tripo recommends for a task input.
		if (!Image.IsEmpty())
		{
			const TSharedRef<FJsonObject> Guide = MakeShared<FJsonObject>();
			Guide->SetStringField(TEXT("image"), Image);
			Json->SetObjectField(TEXT("texture_prompt"), Guide);
		}
		else if (!Prompt.TrimStartAndEnd().IsEmpty())
		{
			const TSharedRef<FJsonObject> Guide = MakeShared<FJsonObject>();
			Guide->SetStringField(TEXT("text"), Prompt.TrimStartAndEnd().Left(MaxPromptCharacters));
			Json->SetObjectField(TEXT("texture_prompt"), Guide);
		}
		if (Seed > 0)
		{
			Json->SetNumberField(TEXT("texture_seed"), Seed);
		}
		if (!Alignment.IsEmpty())
		{
			Json->SetStringField(TEXT("texture_alignment"), Alignment);
		}
		if (!PartNames.IsEmpty())
		{
			SetStrings(Json, TEXT("part_names"), PartNames);
		}
		return Json;
	}

	TSharedRef<FJsonObject> ConvertBody(const FString& Input, const FConvertOptions& Options)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		// quad forces FBX, which Unreal imports as it is.
		Json->SetStringField(TEXT("format"), Options.bQuad ? FString(TEXT("FBX")) : Options.Format);
		if (Options.bQuad)
		{
			Json->SetBoolField(TEXT("quad"), true);
		}
		if (Options.FaceLimit > 0)
		{
			Json->SetNumberField(TEXT("face_limit"), Options.FaceLimit);
		}
		if (Options.bFlattenBottom)
		{
			Json->SetBoolField(TEXT("flatten_bottom"), true);
		}
		if (Options.bPivotToCenterBottom)
		{
			Json->SetBoolField(TEXT("pivot_to_center_bottom"), true);
		}
		if (!FMath::IsNearlyEqual(Options.ScaleFactor, 1.f))
		{
			Json->SetNumberField(TEXT("scale_factor"), Options.ScaleFactor);
		}
		if (Options.TextureSize > 0)
		{
			Json->SetNumberField(TEXT("texture_size"), Options.TextureSize);
		}
		if (!Options.TextureFormat.IsEmpty())
		{
			Json->SetStringField(TEXT("texture_format"), Options.TextureFormat);
		}
		if (Options.bPackUv)
		{
			Json->SetBoolField(TEXT("pack_uv"), true);
		}
		if (Json->HasField(TEXT("format")) && Json->GetStringField(TEXT("format")) == TEXT("FBX"))
		{
			// Scale baked into the geometry rather than left on a node, which Unreal would otherwise apply twice.
			Json->SetStringField(TEXT("fbx_preset"), TEXT("bake_scale"));
		}
		Json->SetBoolField(TEXT("bake"), true);
		return Json;
	}

	TSharedRef<FJsonObject> SegmentBody(const FString& Input)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		Json->SetStringField(TEXT("model"), SegmentModel);
		Json->SetStringField(TEXT("segmentation_granularity"), TEXT("balanced"));
		Json->SetBoolField(TEXT("split_by_connectivity"), true);
		return Json;
	}

	TSharedRef<FJsonObject> CompleteBody(const FString& SegmentTaskId, const TArray<FString>& PartNames, const bool bQuickCap)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), SegmentTaskId);
		Json->SetStringField(TEXT("completion_mode"), bQuickCap ? TEXT("quick_cap") : TEXT("ai_completion"));
		if (!PartNames.IsEmpty())
		{
			SetStrings(Json, TEXT("part_names"), PartNames);
		}
		return Json;
	}

	TSharedRef<FJsonObject> ImageToMultiviewBody(const FString& Input)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		return Json;
	}

	TSharedRef<FJsonObject> EditMultiviewBody(const FString& MultiviewTaskId, const TArray<TPair<FString, FString>>& Edits)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), MultiviewTaskId);
		TArray<TSharedPtr<FJsonValue>> Prompts;
		for (const TPair<FString, FString>& Edit : Edits)
		{
			const TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("prompt"), Edit.Value.Left(1024));
			Item->SetStringField(TEXT("view"), Edit.Key);
			Prompts.Add(MakeShared<FJsonValueObject>(Item));
		}
		Json->SetArrayField(TEXT("prompts"), Prompts);
		return Json;
	}

	TSharedRef<FJsonObject> RigBody(const FString& Input, const FString& RigModel, const FString& RigType, const FString& Spec)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		Json->SetStringField(TEXT("model"), RigModel);
		Json->SetStringField(TEXT("rig_type"), RigType.IsEmpty() ? FString(TEXT("biped")) : RigType);
		// spec (unreal: the UE Mannequin's bone names) is a biped's only.
		if (!Spec.IsEmpty() && (RigType.IsEmpty() || RigType == TEXT("biped")))
		{
			Json->SetStringField(TEXT("spec"), Spec);
		}
		Json->SetStringField(TEXT("out_format"), TEXT("glb"));
		return Json;
	}

	TSharedRef<FJsonObject> RigCheckBody(const FString& Input)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		return Json;
	}

	void RigChoice(const FString& RigType, FString& OutModel, FString& OutSpec)
	{
		// The v3.0 rig model knows Unreal's humanoid skeleton; v2.5 rigs every other body Tripo recognises.
		const bool bBiped = RigType.IsEmpty() || RigType == TEXT("biped");
		OutModel = bBiped ? RigModelV3 : RigModelV25;
		OutSpec = bBiped ? TEXT("unreal") : TEXT("");
	}

	// ---------------------------------------------------------------- prices

	float Price(const TCHAR* Task)
	{
		return FMath::Max(UHyperAIStudioSettings::FindMeshTaskPrice(TEXT("tripo"), Task), 0.f);
	}

	int32 Credits(const float USD)
	{
		return FMath::RoundToInt(USD * 100.f);
	}

	const TCHAR* TexturePriceKey(const FString& Quality)
	{
		return Quality == TEXT("extreme") ? TEXT("texture_8k") : Quality == TEXT("detailed") ? TEXT("texture_hd") : TEXT("texture");
	}

	float GenerationPrice(const FHyperAIModelPlan& Plan, TArray<FString>* OutLines)
	{
		auto Line = [OutLines](const FString& What, const float USD)
		{
			if (OutLines)
			{
				OutLines->Add(FString::Printf(TEXT("%s: %d credits ($%.2f)"), *What, Credits(USD), USD));
			}
			return USD;
		};
		// Parts come untextured; their texture is a task of its own.
		const FString Tier = Plan.bParts ? FString(TEXT("none")) : Plan.TextureQuality;
		float Total = 0.f;
		if (Plan.bReferenceFirst)
		{
			if (Plan.ReferenceSource == TEXT("imagegen"))
			{
				const float USD = ReferenceImagePrice(Plan.ImageModel);
				Total += USD;
				if (OutLines)
				{
					OutLines->Add(FString::Printf(TEXT("reference picture (HyperAI image generation, %s): $%.2f"), *Plan.ImageModel, USD));
				}
			}
			else
			{
				const FString Tier_ = ImageSizeTier(Plan.ImageSize);
				Total += Line(FString::Printf(TEXT("reference picture (%s%s%s, %s)"), *Plan.ImageModel, Plan.ImageQuality.IsEmpty() ? TEXT("") : TEXT(" "), *Plan.ImageQuality,
					Tier_ == TEXT("1k") ? TEXT("up to 1K") : Tier_ == TEXT("2k") ? TEXT("2K") : TEXT("4K")), ImagePrice(Plan.ImageModel, Plan.ImageQuality, Plan.ImageSize));
			}
		}
		if (Plan.Provider == TEXT("hi3d"))
		{
			const int32 Type = Plan.TextureQuality == TEXT("none") ? 1 : 3;
			const float USD = Hi3DPrice(Plan.Model, Plan.Resolution, Type, Plan.bPbr);
			if (OutLines)
			{
				OutLines->Add(FString::Printf(TEXT("Hi3D %s at %s, %s: %d Hi3D credits ($%.2f)"), *Plan.Model, *Plan.Resolution,
					Type == 1 ? TEXT("geometry only") : Plan.bPbr ? TEXT("textured with PBR") : TEXT("textured"), Hi3DCredits(USD), USD));
			}
			return Total + USD;
		}
		if (Plan.bMakeViews)
		{
			Total += Line(TEXT("four views from the picture (image-to-multiview)"), Price(TEXT("image_to_multiview")));
		}
		if (!Plan.ViewEdits.IsEmpty())
		{
			Total += Line(FString::Printf(TEXT("view edits (edit-multiview, %d at 5 credits an edited view)"), Plan.ViewEdits.Num()), Price(TEXT("edit_multiview")) * Plan.ViewEdits.Num());
		}
		if (Plan.Family == TEXT("P2"))
		{
			const FString Key = TEXT("p2") + TierSuffix(Tier);
			Total += Line(FString::Printf(TEXT("P2 generation (%s textures)"), *Tier), Price(*Key));
		}
		else
		{
			const FString Key = (Plan.Input == TEXT("text") ? FString(TEXT("h3_text")) : FString(TEXT("h3_image"))) + TierSuffix(Tier);
			Total += Line(FString::Printf(TEXT("H3.1 %s generation (%s textures)"), Plan.Input == TEXT("text") ? TEXT("text") : TEXT("image/multiview"), *Tier), Price(*Key));
			if (Plan.GeometryQuality == TEXT("detailed"))
			{
				Total += Line(TEXT("detailed geometry"), Price(TEXT("addon_detailed_geometry")));
			}
			if (Plan.bQuad)
			{
				Total += Line(TEXT("quad mesh"), Price(TEXT("addon_quad")));
			}
			if (Plan.bSmartLowPoly)
			{
				Total += Line(TEXT("smart low poly"), Price(TEXT("addon_smart_low_poly")));
			}
			if (Plan.bParts)
			{
				Total += Line(TEXT("parts"), Price(TEXT("addon_parts")));
				if (Plan.TextureQuality != TEXT("none"))
				{
					Total += Line(FString::Printf(TEXT("texture task for the parts (%s)"), *Plan.TextureQuality), Price(TexturePriceKey(Plan.TextureQuality)));
				}
			}
		}
		return Total;
	}

	// ---------------------------------------------------------------- the ladder

	namespace
	{
		bool Failing(const FHyperAIModelCheckReport& Report, std::initializer_list<const TCHAR*> Ids)
		{
			for (const FHyperAIModelCheck& Check : Report.Checks)
			{
				if (Check.Result == TEXT("fail"))
				{
					for (const TCHAR* Id : Ids)
					{
						if (Check.Id == Id)
						{
							return true;
						}
					}
				}
			}
			return false;
		}

		TArray<FString> FailingIds(const FHyperAIModelCheckReport& Report, std::initializer_list<const TCHAR*> Ids)
		{
			TArray<FString> Out;
			for (const FHyperAIModelCheck& Check : Report.Checks)
			{
				for (const TCHAR* Id : Ids)
				{
					if (Check.Result == TEXT("fail") && Check.Id == Id)
					{
						Out.AddUnique(Check.Id);
					}
				}
			}
			return Out;
		}
	}

	TArray<FString> LocalFixesFor(const FHyperAIModelCheckReport& Report)
	{
		// The order matters: bits go before holes are filled, orientation before normals, geometry before pivot and scale.
		static const TCHAR* const Order[] = {TEXT("remove_bits"), TEXT("weapon_finish"), TEXT("simplify"), TEXT("orient"), TEXT("fill_holes"), TEXT("normals"), TEXT("auto_uv"), TEXT("scale"), TEXT("pivot"),
			TEXT("collision"), TEXT("lightmap_uv"), TEXT("nanite"), TEXT("materials")};
		TSet<FString> Wanted;
		for (const FHyperAIModelCheck& Check : Report.Checks)
		{
			if ((Check.Result == TEXT("fail") || Check.Result == TEXT("warn")) && !Check.LocalFix.IsEmpty())
			{
				TArray<FString> Fixes;
				Check.LocalFix.ParseIntoArray(Fixes, TEXT(","));
				for (const FString& Fix : Fixes)
				{
					Wanted.Add(Fix.TrimStartAndEnd());
				}
			}
		}
		TArray<FString> Out;
		for (const TCHAR* Fix : Order)
		{
			if (Wanted.Contains(Fix))
			{
				Out.Add(Fix);
			}
		}
		return Out;
	}

	TArray<FPaidFix> PaidLadder(const FHyperAIModelCheckReport& Report, const FFixContext& Context, TArray<FString>* OutOverBudget)
	{
		TArray<FPaidFix> Candidates;
		const FString Input = !Context.InputTaskId.IsEmpty() ? Context.InputTaskId : Context.InputToken;
		if (Input.IsEmpty())
		{
			return Candidates;
		}
		const bool bQuad = Context.Plan.Family.IsEmpty() ? false : Context.Plan.bQuad;
		auto Tried = [&Context](const TCHAR* Id) { return Context.Tried.FindRef(Id); };
		auto Add = [&Candidates](FPaidFix&& Fix) { Candidates.Add(MoveTemp(Fix)); };
		const bool bTripo = Context.bTripo;

		// Faces over budget or the wrong topology: retopology to the budget, unwrapping UVs in the same task at no extra price.
		const bool bFaces = Failing(Report, {TEXT("faces")});
		const bool bBroken = Failing(Report, {TEXT("non_manifold"), TEXT("holes"), TEXT("open_edges"), TEXT("flipped")});
		const bool bUv = Failing(Report, {TEXT("uv_overlap"), TEXT("uv_present"), TEXT("uv_range")});
		// The target is the budget (with none known, the faces the model has): never one far under it, which would throw away
		// the detail the plan paid for. Above v3.0's 20,000, decimation v1.0 takes the budget whole (no bake, so the UVs stay).
		const int32 FacesNow = bQuad ? Report.Quads : Report.Triangles;
		const int32 Budget = Context.Plan.FaceLimit > 0 ? Context.Plan.FaceLimit : FacesNow;
		const bool bV1 = Budget > (bQuad ? RetopoMaxQuads : RetopoMaxTris);
		const int32 Target = FMath::Clamp(Budget, MinFaces, bV1 ? (bQuad ? RetopoV1MaxQuads : RetopoV1MaxTris) : bQuad ? RetopoMaxQuads : RetopoMaxTris);
		const bool bKeepsDetail = bFaces || Target * 5 >= Budget * 4;
		if (bTripo && (bFaces || bBroken) && bKeepsDetail && Budget > 0 && Tried(TEXT("retopo")) < 1)
		{
			FPaidFix Fix;
			Fix.Id = TEXT("retopo");
			Fix.What = bV1 ? FString::Printf(TEXT("Reduce it to %d %s with Tripo's decimation (v1.0), keeping its UVs (no bake)"), Target, bQuad ? TEXT("quads") : TEXT("triangles"))
				: FString::Printf(TEXT("Retopologise to %d %s with Tripo (v3.0), unwrapping UVs and baking the textures onto it"), Target, bQuad ? TEXT("quads") : TEXT("triangles"));
			Fix.Endpoints = {TEXT("/mesh/decimate")};
			Fix.Bodies = {RetopoBody(Input, Target, bQuad, /*bSmartUv=*/!bV1, bV1)};
			Fix.PriceUSD = Price(bV1 ? TEXT("retopo_v1") : bQuad ? TEXT("retopo_quad") : TEXT("retopo"));
			Fix.Clears = bV1 ? FailingIds(Report, {TEXT("faces"), TEXT("non_manifold"), TEXT("holes"), TEXT("open_edges"), TEXT("flipped")})
				: FailingIds(Report, {TEXT("faces"), TEXT("non_manifold"), TEXT("holes"), TEXT("open_edges"), TEXT("flipped"), TEXT("uv_overlap"), TEXT("uv_present"), TEXT("uv_range")});
			Add(MoveTemp(Fix));
		}
		// UVs that overlap with textures on them: Smart UV re-unwraps and re-bakes; it takes 80,000 triangles at most.
		if (bTripo && bUv && Context.bHasTextures && Report.Triangles <= SmartUvMaxTris && Tried(TEXT("smart_uv")) < 1)
		{
			FPaidFix Fix;
			Fix.Id = TEXT("smart_uv");
			Fix.What = TEXT("Unwrap the UVs again with Tripo's Smart UV, baking the textures onto the new layout");
			Fix.Endpoints = {TEXT("/mesh/uv")};
			Fix.Bodies = {SmartUvBody(Input)};
			Fix.PriceUSD = Price(TEXT("smart_uv"));
			Fix.Clears = FailingIds(Report, {TEXT("uv_overlap"), TEXT("uv_present"), TEXT("uv_range")});
			Add(MoveTemp(Fix));
		}
		// Missing or weak textures: texture again; the first retry changes only the seed (same tier, same price), the next goes a tier up.
		if (bTripo && Failing(Report, {TEXT("textures"), TEXT("pbr"), TEXT("normal_map"), TEXT("roughness_metal")}) && Tried(TEXT("texture")) < 3)
		{
			FString Tier = Context.Plan.TextureQuality.IsEmpty() || Context.Plan.TextureQuality == TEXT("none") ? FString(TEXT("standard")) : Context.Plan.TextureQuality;
			if (Tried(TEXT("texture")) > 1 && Tier != TEXT("extreme"))
			{
				Tier = TierUp(Tier);
			}
			const int32 Seed = Tried(TEXT("texture")) > 0 ? 1 + (FMath::Rand() % 100000) : 0;
			const bool bImage = !Context.ReferenceInput.IsEmpty();
			FPaidFix Fix;
			Fix.Id = TEXT("texture");
			Fix.What = FString::Printf(TEXT("Texture it again with Tripo (%s, PBR%s%s)"), *Tier, bImage ? TEXT(", from the reference picture") : TEXT(""),
				Seed > 0 ? TEXT(", a new texture seed") : TEXT(""));
			Fix.Endpoints = {TEXT("/models/texture")};
			Fix.Bodies = {TextureBody(Input, Tier, /*bPbr=*/true, {}, FString(), Context.ReferenceInput, Seed,
				bImage ? FString(TEXT("original_image")) : FString(TEXT("geometry")))};
			Fix.PriceUSD = Price(TexturePriceKey(Tier));
			Fix.Clears = FailingIds(Report, {TEXT("textures"), TEXT("pbr"), TEXT("normal_map"), TEXT("roughness_metal")});
			Add(MoveTemp(Fix));
		}
		// Holes and broken parts the free hole fill and retopology left: segment, then cap (quick) or complete (AI).
		if (bTripo && bBroken && Tried(TEXT("retopo")) >= 1)
		{
			const bool bQuick = Tried(TEXT("quick_cap")) < 1;
			if (bQuick || Tried(TEXT("complete")) < 1)
			{
				FPaidFix Fix;
				Fix.Id = bQuick ? TEXT("quick_cap") : TEXT("complete");
				Fix.What = bQuick ? FString(TEXT("Segment it with Tripo, then seal its open boundaries (quick cap)"))
					: FString(TEXT("Segment it with Tripo, then regenerate the broken parts (AI completion)"));
				Fix.Endpoints = {TEXT("/mesh/segment"), TEXT("/mesh/complete")};
				// The completion's input is the segmentation's task, filled in when that finishes.
				Fix.Bodies = {SegmentBody(Input), CompleteBody(TEXT("{segment_task}"), {}, bQuick)};
				Fix.PriceUSD = Price(TEXT("segment")) + Price(bQuick ? TEXT("complete_quick") : TEXT("complete"));
				Fix.Clears = FailingIds(Report, {TEXT("non_manifold"), TEXT("holes"), TEXT("open_edges"), TEXT("flipped")});
				Add(MoveTemp(Fix));
			}
		}
		// A skeletal mesh is not reshaped in the editor: Tripo's convert moves its pivot and scale.
		if (bTripo && Context.bSkeletal && Failing(Report, {TEXT("pivot"), TEXT("scale")}) && Tried(TEXT("convert")) < 1)
		{
			FConvertOptions Options;
			Options.Format = TEXT("FBX");
			Options.bPivotToCenterBottom = true;
			for (const FHyperAIModelCheck& Check : Report.Checks)
			{
				if (Check.Id == TEXT("scale") && Check.Result == TEXT("fail"))
				{
					const float Have = FCString::Atof(*Check.Value);
					const float Want = FCString::Atof(*Check.Expected);
					Options.ScaleFactor = Have > 0.f && Want > 0.f ? Want / Have : 1.f;
				}
			}
			FPaidFix Fix;
			Fix.Id = TEXT("convert");
			Fix.What = TEXT("Convert it with Tripo: pivot to the bottom centre, scaled to size");
			Fix.Endpoints = {TEXT("/models/convert")};
			Fix.Bodies = {ConvertBody(Input, Options)};
			Fix.PriceUSD = Price(TEXT("convert_advanced"));
			Fix.Clears = FailingIds(Report, {TEXT("pivot"), TEXT("scale")});
			Add(MoveTemp(Fix));
		}
		// The shape is wrong: only a new generation fixes that. It is the dearest, so it comes last.
		if (bTripo && Context.Provider == TEXT("tripo") && Context.bCanRegenerate && Failing(Report, {TEXT("visual")}) && Tried(TEXT("regenerate")) < 2)
		{
			FPaidFix Fix;
			Fix.Id = TEXT("regenerate");
			Fix.What = TEXT("Make it again with Tripo: a new seed and a refined prompt, the shape being wrong");
			Fix.PriceUSD = GenerationPrice(Context.Plan);
			Fix.Clears = FailingIds(Report, {TEXT("visual")});
			Add(MoveTemp(Fix));
		}

		// A Hi3D model: Hi3D textures the current mesh again (texture only, v3.0 or v1.5), or makes it again a resolution up.
		if (Context.Provider == TEXT("hi3d") && !Context.Hi3DImages.IsEmpty())
		{
			const FString PlanModel = Context.Plan.Model.IsEmpty() ? FString(Hi3DDefaultModel) : Context.Plan.Model;
			if (Failing(Report, {TEXT("textures"), TEXT("pbr"), TEXT("normal_map"), TEXT("roughness_metal")}) && Tried(TEXT("hi3d_texture")) < 2)
			{
				FPaidFix Fix;
				Fix.Id = TEXT("hi3d_texture");
				Fix.Provider = TEXT("hi3d");
				Fix.Hi3D.RequestType = 2;
				Fix.Hi3D.Model = Hi3DCanTextureOnly(PlanModel) ? PlanModel : FString(Hi3DDefaultModel);
				Fix.Hi3D.Resolution = Fix.Hi3D.Model == PlanModel ? Context.Plan.Resolution : Hi3DResolutions(Fix.Hi3D.Model)[0];
				Fix.Hi3D.bPbr = Hi3DSupportsPbr(Fix.Hi3D.Model);
				Fix.Hi3D.Images = Context.Hi3DImages;
				Fix.Hi3D.bMultiview = Context.bHi3DMultiview;
				Fix.What = FString::Printf(TEXT("Texture it again with Hi3D (%s, texture only on the current mesh%s)"), *Fix.Hi3D.Model, Fix.Hi3D.bPbr ? TEXT(", PBR") : TEXT(""));
				Fix.Endpoints = {Hi3DSubmitPath};
				Fix.PriceUSD = Hi3DTexturePrice(Fix.Hi3D.bPbr);
				Fix.Clears = FailingIds(Report, {TEXT("textures"), TEXT("pbr"), TEXT("normal_map"), TEXT("roughness_metal")});
				Add(MoveTemp(Fix));
			}
			if (Context.bCanRegenerate && Failing(Report, {TEXT("visual")}) && Tried(TEXT("hi3d_regenerate")) < 2)
			{
				const FString Up = Hi3DResolutionUp(PlanModel, Context.Plan.Resolution);
				FPaidFix Fix;
				Fix.Id = TEXT("hi3d_regenerate");
				Fix.Provider = TEXT("hi3d");
				Fix.Hi3D = Hi3DRequestFromPlan(Context.Plan, Context.Hi3DImages, Context.bHi3DMultiview);
				Fix.Hi3D.Model = PlanModel;
				Fix.Hi3D.Resolution = Up.IsEmpty() ? Context.Plan.Resolution : Up;
				Fix.Hi3D.Faces = FMath::Clamp(Context.Plan.ProviderFaces, Hi3DMinFaces, Hi3DMaxFacesFor(Fix.Hi3D.Resolution));
				Fix.What = FString::Printf(TEXT("Make it again with Hi3D %s at %s%s, the shape being wrong"), *PlanModel, *Fix.Hi3D.Resolution,
					Up.IsEmpty() ? (Context.bHi3DMultiview ? TEXT(" from the views") : TEXT(" (already its finest)")) : TEXT(", a resolution up"));
				Fix.Endpoints = {Hi3DSubmitPath};
				Fix.PriceUSD = Hi3DPrice(PlanModel, Fix.Hi3D.Resolution, Fix.Hi3D.RequestType, Fix.Hi3D.bPbr);
				Fix.Clears = FailingIds(Report, {TEXT("visual")});
				Add(MoveTemp(Fix));
			}
		}

		// Cheapest first; among equal prices, the one that clears most.
		Candidates.StableSort([](const FPaidFix& A, const FPaidFix& B)
		{
			return !FMath::IsNearlyEqual(A.PriceUSD, B.PriceUSD) ? A.PriceUSD < B.PriceUSD : A.Clears.Num() > B.Clears.Num();
		});
		// Retopology clears faces; nothing cheaper does, so a model over budget goes there first whatever else fails.
		if (bFaces)
		{
			const int32 Retopo = Candidates.IndexOfByPredicate([](const FPaidFix& Fix) { return Fix.Id == TEXT("retopo"); });
			if (Retopo > 0)
			{
				FPaidFix Moved = Candidates[Retopo];
				Candidates.RemoveAt(Retopo);
				Candidates.Insert(MoveTemp(Moved), 0);
			}
		}
		TArray<FPaidFix> Affordable;
		for (FPaidFix& Fix : Candidates)
		{
			if (Fix.PriceUSD <= Context.RemainingUSD + KINDA_SMALL_NUMBER)
			{
				Affordable.Add(MoveTemp(Fix));
			}
			else if (OutOverBudget)
			{
				OutOverBudget->Add(FString::Printf(TEXT("%s ($%.2f, over the $%.2f left)"), *Fix.What, Fix.PriceUSD, Context.RemainingUSD));
			}
		}
		return Affordable;
	}
}
