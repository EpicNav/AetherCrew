// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "HyperAIStudioMeshGenJob.h"
#include "HyperAIStudioSettings.h"

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
			{TEXT("weapon"), TEXT("P2"), false, 15000, TEXT("detailed"), 1.0f, 10.24f, false, TEXT("box"),
				TEXT("a held weapon is close to the camera but light: P2 at 15K with detailed textures")},
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

	bool Classify(const FHyperAIModelBrief& Brief, FHyperAIModelPlan& Out, FString& OutError)
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
		const bool bDetailWords = HasAny(Words, {TEXT("photoreal"), TEXT("highly detailed"), TEXT("intricate"), TEXT("ornate"), TEXT("cinematic"), TEXT("hero asset"),
			TEXT("scanned"), TEXT("realistic"), TEXT("hyper-detailed"), TEXT("ultra detailed")});
		const bool bLowWords = HasAny(Words, {TEXT("low poly"), TEXT("low-poly"), TEXT("lowpoly"), TEXT("mobile game"), TEXT("flat shaded"), TEXT("ps1")});
		const bool bQuadWords = HasAny(Words, {TEXT("quad"), TEXT("subdivision"), TEXT("subd"), TEXT("clean topology"), TEXT("edge loops"), TEXT("retopo")});
		const bool bPartsWords = HasAny(Words, {TEXT("separate parts"), TEXT("editable parts"), TEXT("modular parts"), TEXT("segmented"), TEXT("in parts")});
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
		if (bDetailWords && bH && Brief.FaceLimit <= 0)
		{
			Faces = FMath::Max(Faces, Out.bQuad ? 40000 : 500000);
			Why.Add(TEXT("faces raised for the detail the brief asks for"));
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
		Out.bNanite = Row.bNanite && !Out.bQuad && Use != TEXT("character") && Use != TEXT("creature");
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
			Why.Add(TEXT("H3.1 returns quads as FBX, which is imported as it is"));
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
		Out.RequestJson = HyperAIStudio::MeshGen::Private::JsonText(GenerationBody(Out, Brief, Placeholders));
		return true;
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
		Json->SetNumberField(TEXT("face_limit"), Plan.FaceLimit);
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

	TSharedRef<FJsonObject> RetopoBody(const FString& Input, const int32 FaceLimit, const bool bQuad, const bool bSmartUv)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		Json->SetStringField(TEXT("model"), RetopoModel);
		Json->SetNumberField(TEXT("face_limit"), FMath::Clamp(FaceLimit, MinFaces, bQuad ? RetopoMaxQuads : RetopoMaxTris));
		Json->SetBoolField(TEXT("quad"), bQuad);
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

	TSharedRef<FJsonObject> TextureBody(const FString& Input, const FString& Quality, const bool bPbr, const TArray<FString>& PartNames, const FString& Prompt)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		Json->SetStringField(TEXT("model"), TextureVersion);
		Json->SetStringField(TEXT("texture_quality"), Quality.IsEmpty() || Quality == TEXT("none") ? FString(TEXT("standard")) : Quality);
		Json->SetBoolField(TEXT("pbr"), bPbr);
		Json->SetBoolField(TEXT("delight"), true);
		Json->SetBoolField(TEXT("bake"), true);
		if (!Prompt.TrimStartAndEnd().IsEmpty())
		{
			const TSharedRef<FJsonObject> Guide = MakeShared<FJsonObject>();
			Guide->SetStringField(TEXT("text"), Prompt.TrimStartAndEnd().Left(MaxPromptCharacters));
			Json->SetObjectField(TEXT("texture_prompt"), Guide);
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

	TSharedRef<FJsonObject> RigBody(const FString& Input, const FString& Spec)
	{
		const TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("input"), Input);
		// The v3.0 rig model is the one that knows Unreal's skeleton convention.
		Json->SetStringField(TEXT("model"), RigModelV3);
		Json->SetStringField(TEXT("rig_type"), TEXT("biped"));
		Json->SetStringField(TEXT("spec"), Spec);
		Json->SetStringField(TEXT("out_format"), TEXT("glb"));
		return Json;
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
		if (Plan.bMakeViews)
		{
			Total += Line(TEXT("four views from the picture (image-to-multiview)"), Price(TEXT("image_to_multiview")));
		}
		if (!Plan.ViewEdits.IsEmpty())
		{
			Total += Line(FString::Printf(TEXT("view edits (edit-multiview, %d)"), Plan.ViewEdits.Num()), Price(TEXT("edit_multiview")));
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
		static const TCHAR* const Order[] = {TEXT("remove_bits"), TEXT("orient"), TEXT("fill_holes"), TEXT("normals"), TEXT("auto_uv"), TEXT("scale"), TEXT("pivot"),
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

		// Faces over budget or the wrong topology: retopology to the budget, unwrapping UVs in the same task at no extra price.
		const bool bFaces = Failing(Report, {TEXT("faces")});
		const bool bBroken = Failing(Report, {TEXT("non_manifold"), TEXT("holes"), TEXT("open_edges"), TEXT("flipped")});
		const bool bUv = Failing(Report, {TEXT("uv_overlap"), TEXT("uv_present"), TEXT("uv_range")});
		if ((bFaces || bBroken) && Tried(TEXT("retopo")) < 1)
		{
			const int32 Budget = Context.Plan.FaceLimit > 0 ? Context.Plan.FaceLimit : bQuad ? RetopoMaxQuads : RetopoMaxTris;
			FPaidFix Fix;
			Fix.Id = TEXT("retopo");
			Fix.What = FString::Printf(TEXT("Retopologise to %d %s with Tripo (v3.0), unwrapping UVs and baking the textures onto it"),
				FMath::Clamp(Budget, MinFaces, bQuad ? RetopoMaxQuads : RetopoMaxTris), bQuad ? TEXT("quads") : TEXT("triangles"));
			Fix.Endpoints = {TEXT("/mesh/decimate")};
			Fix.Bodies = {RetopoBody(Input, Budget, bQuad, /*bSmartUv=*/true)};
			Fix.PriceUSD = Price(bQuad ? TEXT("retopo_quad") : TEXT("retopo"));
			Fix.Clears = FailingIds(Report, {TEXT("faces"), TEXT("non_manifold"), TEXT("holes"), TEXT("open_edges"), TEXT("flipped"), TEXT("uv_overlap"), TEXT("uv_present"), TEXT("uv_range")});
			Add(MoveTemp(Fix));
		}
		// UVs that overlap with textures on them: Smart UV re-unwraps and re-bakes; it takes 80,000 triangles at most.
		if (bUv && Context.bHasTextures && Report.Triangles <= SmartUvMaxTris && Tried(TEXT("smart_uv")) < 1)
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
		// Missing or weak textures: texture again, a tier up after the first try.
		if (Failing(Report, {TEXT("textures"), TEXT("pbr"), TEXT("normal_map"), TEXT("roughness_metal")}) && Tried(TEXT("texture")) < 2)
		{
			FString Tier = Context.Plan.TextureQuality.IsEmpty() || Context.Plan.TextureQuality == TEXT("none") ? FString(TEXT("standard")) : Context.Plan.TextureQuality;
			if (Tried(TEXT("texture")) > 0 && Tier != TEXT("extreme"))
			{
				Tier = TierUp(Tier);
			}
			FPaidFix Fix;
			Fix.Id = TEXT("texture");
			Fix.What = FString::Printf(TEXT("Texture it again with Tripo (%s, PBR)"), *Tier);
			Fix.Endpoints = {TEXT("/models/texture")};
			Fix.Bodies = {TextureBody(Input, Tier, /*bPbr=*/true, {}, FString())};
			Fix.PriceUSD = Price(TexturePriceKey(Tier));
			Fix.Clears = FailingIds(Report, {TEXT("textures"), TEXT("pbr"), TEXT("normal_map"), TEXT("roughness_metal")});
			Add(MoveTemp(Fix));
		}
		// Holes and broken parts the free hole fill and retopology left: segment, then cap (quick) or complete (AI).
		if (bBroken && Tried(TEXT("retopo")) >= 1)
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
		if (Context.bSkeletal && Failing(Report, {TEXT("pivot"), TEXT("scale")}) && Tried(TEXT("convert")) < 1)
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
		if (Context.bCanRegenerate && Failing(Report, {TEXT("visual")}) && Tried(TEXT("regenerate")) < 2)
		{
			FPaidFix Fix;
			Fix.Id = TEXT("regenerate");
			Fix.What = TEXT("Make it again with Tripo: a new seed and a refined prompt, the shape being wrong");
			Fix.PriceUSD = GenerationPrice(Context.Plan);
			Fix.Clears = FailingIds(Report, {TEXT("visual")});
			Add(MoveTemp(Fix));
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
