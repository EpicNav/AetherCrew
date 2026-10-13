// Games by Hyper 2026.

#include "HyperAIStudioSemanticSearch.h"

#include "HyperAIStudioProjectAssetIndex.h"
#include "Misc/Crc.h"

namespace HyperAIStudio::SemanticSearch
{
	namespace
	{
		/**
		 * The vocabulary: each line is one concept, its words written as people name and describe game assets. Words are
		 * stemmed as they are read, as queries and assets are, so a line need not list plurals. A word may sit in several.
		 */
		const TCHAR* const ConceptLines[] = {
			TEXT("stone rock boulder pebble cobble cobblestone granite marble slate rubble gravel flagstone masonry"),
			TEXT("brick bricks masonry mortar"),
			TEXT("moss mossy lichen overgrown ivy vine algae"),
			TEXT("wood wooden timber plank log lumber bark"),
			TEXT("metal metallic iron steel copper bronze brass aluminium aluminum chrome"),
			TEXT("rust rusty corroded corrosion oxidized oxidised"),
			TEXT("wall walls partition"),
			TEXT("floor ground flooring tile tiles pavement"),
			TEXT("roof roofing shingle"),
			TEXT("door gate doorway entrance"),
			TEXT("window glass pane"),
			TEXT("water river lake ocean sea pond stream waterfall wave liquid"),
			TEXT("tree oak pine birch spruce palm willow trunk"),
			TEXT("grass lawn turf meadow"),
			TEXT("foliage plant vegetation bush shrub fern leaf leaves flower"),
			TEXT("sand desert dune beach"),
			TEXT("snow ice frost frozen glacier"),
			TEXT("dirt mud soil earth dust"),
			TEXT("lava magma volcanic"),
			TEXT("fire flame burning ember torch campfire"),
			TEXT("smoke fog mist steam haze vapor vapour"),
			TEXT("light lamp lantern torch candle lighting bulb"),
			TEXT("sky skybox skysphere cloud atmosphere"),
			TEXT("character hero player npc creature mannequin humanoid"),
			TEXT("enemy monster foe"),
			TEXT("weapon sword axe spear bow gun rifle pistol blade dagger"),
			TEXT("armor armour helmet shield"),
			TEXT("vehicle car truck tank boat ship cart wagon"),
			TEXT("building house castle tower ruin temple hut cabin"),
			TEXT("bridge"),
			TEXT("road path street trail"),
			TEXT("rope chain cable"),
			TEXT("cloth fabric textile canvas cotton leather silk"),
			TEXT("bone skull skeleton"),
			TEXT("crate box barrel chest container"),
			TEXT("table chair bench desk stool furniture"),
			TEXT("material mat mtl shader surface"),
			TEXT("texture tex image bitmap"),
			TEXT("mesh model geometry staticmesh prop"),
			TEXT("skeletal skinned rig skeletalmesh"),
			TEXT("animation anim montage sequence clip"),
			TEXT("blueprint bp"),
			TEXT("sound audio sfx cue wav metasound"),
			TEXT("music soundtrack song"),
			TEXT("vfx fx particle niagara effect emitter"),
			TEXT("explosion blast explode detonation"),
			TEXT("ui widget hud menu interface button icon"),
			TEXT("level map world"),
			TEXT("dark black shadow gloomy"),
			TEXT("white bright pale"),
			TEXT("red crimson scarlet blood"),
			TEXT("green emerald verdant"),
			TEXT("blue azure cyan navy"),
			TEXT("yellow gold golden amber"),
			TEXT("brown tan beige"),
			TEXT("grey gray silver"),
			TEXT("orange"),
			TEXT("purple violet magenta"),
			TEXT("old ancient aged weathered worn damaged broken cracked ruined"),
			TEXT("new clean pristine polished shiny"),
			TEXT("wet damp puddle soaked"),
			TEXT("dry arid dusty"),
			TEXT("normal normalmap"),
			TEXT("roughness rough"),
			TEXT("diffuse albedo basecolor color colour"),
			TEXT("emissive glow glowing neon"),
			TEXT("decal"),
			TEXT("landscape terrain heightmap"),
			TEXT("cave cavern tunnel"),
			TEXT("mountain hill cliff peak"),
			TEXT("stair stairs staircase steps"),
			TEXT("pillar column post beam"),
			TEXT("fence railing barrier"),
			TEXT("dungeon crypt"),
			TEXT("forest woods jungle"),
			TEXT("scifi futuristic tech"),
			TEXT("medieval fantasy"),
			TEXT("jump leap"),
			TEXT("run sprint"),
			TEXT("walk"),
			TEXT("attack hit strike"),
			TEXT("death die dead"),
			TEXT("idle"),
			TEXT("footstep footsteps"),
			TEXT("coin treasure loot"),
			TEXT("key lock"),
			TEXT("health hp life"),
			TEXT("rain storm thunder lightning"),
			TEXT("wind breeze gust"),
			TEXT("night moon"),
			TEXT("day sun sunlight"),
			TEXT("postprocess pp"),
			TEXT("data datatable dt dataasset"),
		};

		/** Asset-name prefixes, as Epic's and most studios' naming conventions use them, and what they say. */
		struct FPrefix
		{
			const TCHAR* Prefix;
			const TCHAR* Words;
		};
		const FPrefix Prefixes[] = {
			{ TEXT("sm"), TEXT("mesh") },
			{ TEXT("sk"), TEXT("skeletal mesh") },
			{ TEXT("m"), TEXT("material") },
			{ TEXT("mi"), TEXT("material") },
			{ TEXT("mf"), TEXT("material") },
			{ TEXT("t"), TEXT("texture") },
			{ TEXT("tx"), TEXT("texture") },
			{ TEXT("bp"), TEXT("blueprint") },
			{ TEXT("abp"), TEXT("animation blueprint") },
			{ TEXT("a"), TEXT("animation") },
			{ TEXT("as"), TEXT("animation") },
			{ TEXT("am"), TEXT("animation") },
			{ TEXT("ns"), TEXT("vfx") },
			{ TEXT("ps"), TEXT("vfx") },
			{ TEXT("fx"), TEXT("vfx") },
			{ TEXT("s"), TEXT("sound") },
			{ TEXT("sc"), TEXT("sound") },
			{ TEXT("sw"), TEXT("sound") },
			{ TEXT("ms"), TEXT("sound") },
			{ TEXT("wbp"), TEXT("ui") },
			{ TEXT("wb"), TEXT("ui") },
			{ TEXT("l"), TEXT("level") },
			{ TEXT("lvl"), TEXT("level") },
			{ TEXT("dt"), TEXT("data") },
			{ TEXT("da"), TEXT("data") },
			{ TEXT("pp"), TEXT("postprocess") },
		};

		const TCHAR* const StopWords[] = {
			TEXT("a"), TEXT("an"), TEXT("the"), TEXT("of"), TEXT("with"), TEXT("and"), TEXT("or"), TEXT("for"), TEXT("in"),
			TEXT("on"), TEXT("to"), TEXT("by"), TEXT("from"), TEXT("that"), TEXT("this"), TEXT("some"), TEXT("any"), TEXT("my"),
			TEXT("our"), TEXT("its"), TEXT("is"), TEXT("are"), TEXT("has"), TEXT("have"), TEXT("which"), TEXT("one"), TEXT("find"),
			TEXT("show"), TEXT("me"), TEXT("all"), TEXT("asset"), TEXT("assets"), TEXT("like"), TEXT("looks"), TEXT("look"),
			TEXT("game"), TEXT("script"), TEXT("engine"),
		};

		/** Field weights: a name says most about an asset, a folder least. */
		float FieldWeight(const EField Field)
		{
			switch (Field)
			{
			case EField::Name: return 3.0f;
			case EField::Tag: return 2.0f;
			case EField::Class: return 1.5f;
			case EField::Parent: return 1.0f;
			default: return 1.0f;
			}
		}

		constexpr float ConceptShare = 0.8f;
		constexpr float QueryConceptWeight = 0.6f;
		constexpr float SpellingCredit = 0.35f;
		constexpr float NameBonus = 10.0f;
		constexpr float K1 = 1.2f;
		constexpr float B = 0.5f;

		bool IsVowel(const TCHAR C)
		{
			return C == TEXT('a') || C == TEXT('e') || C == TEXT('i') || C == TEXT('o') || C == TEXT('u');
		}

		struct FVocabulary
		{
			TMap<FString, TArray<int32>> ByStem;
			TMap<FString, TArray<int32>> ByPrefix;
			TSet<FString> Stops;
			int32 Count = 0;
		};

		const FVocabulary& Vocabulary()
		{
			static const FVocabulary Built = []
			{
				FVocabulary Out;
				for (const TCHAR* Line : ConceptLines)
				{
					const int32 Id = Out.Count++;
					TArray<FString> Words;
					FString(Line).ParseIntoArrayWS(Words);
					for (const FString& Word : Words)
					{
						Out.ByStem.FindOrAdd(Stem(Word)).AddUnique(Id);
					}
				}
				for (const FPrefix& Prefix : Prefixes)
				{
					TArray<FString> Words;
					FString(Prefix.Words).ParseIntoArrayWS(Words);
					TArray<int32>& Ids = Out.ByPrefix.FindOrAdd(Prefix.Prefix);
					for (const FString& Word : Words)
					{
						if (const TArray<int32>* Found = Out.ByStem.Find(Stem(Word)))
						{
							for (const int32 Id : *Found)
							{
								Ids.AddUnique(Id);
							}
						}
					}
				}
				for (const TCHAR* Stop : StopWords)
				{
					Out.Stops.Add(Stop);
				}
				return Out;
			}();
			return Built;
		}

		uint32 WordFeature(const FString& StemValue)
		{
			return MakeFeature(EFeature::Word, FCrc::StrCrc32(*StemValue));
		}

		bool IsAllDigits(const FString& Word)
		{
			for (const TCHAR C : Word)
			{
				if (!FChar::IsDigit(C))
				{
					return false;
				}
			}
			return true;
		}

		/** The words worth indexing from Text: no numbers, no single letters. */
		TArray<FString> IndexWords(const FStringView Text)
		{
			TArray<FString> Out;
			for (FString& Word : SplitWords(Text))
			{
				if (Word.Len() >= 2 && !IsAllDigits(Word))
				{
					Out.Add(MoveTemp(Word));
				}
			}
			return Out;
		}

		void AddWeight(TMap<uint32, float>& Features, const uint32 Feature, const float Weight)
		{
			float& Value = Features.FindOrAdd(Feature);
			// A word repeated in a name and its folder still counts, but never swamps the rest.
			Value = FMath::Min(Value + Weight, 6.0f);
		}

		float Bm25(const float Tf, const float DocLength, const float AverageLength, const float Idf)
		{
			const float Norm = 1.0f - B + B * (AverageLength > 0.0f ? DocLength / AverageLength : 1.0f);
			return Idf * (Tf * (K1 + 1.0f)) / (Tf + K1 * Norm);
		}

		float Idf(const int32 DocumentCount, const int32 DocumentFrequency)
		{
			return FMath::Loge(1.0f + (DocumentCount - DocumentFrequency + 0.5f) / (DocumentFrequency + 0.5f));
		}
	}

	uint32 MakeFeature(const EFeature Kind, const uint32 Hash)
	{
		return (static_cast<uint32>(Kind) << 30) | (Hash & 0x3FFFFFFFu);
	}

	EFeature KindOf(const uint32 Feature)
	{
		return static_cast<EFeature>(Feature >> 30);
	}

	TArray<FString> SplitWords(const FStringView Text)
	{
		TArray<FString> Out;
		FString Current;
		const auto Flush = [&Out, &Current]()
		{
			if (!Current.IsEmpty())
			{
				Out.Add(Current.ToLower());
				Current.Reset();
			}
		};
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR C = Text[Index];
			if (!FChar::IsAlnum(C))
			{
				Flush();
				continue;
			}
			if (!Current.IsEmpty())
			{
				const TCHAR Prev = Current[Current.Len() - 1];
				const bool bNextLower = Index + 1 < Text.Len() && FChar::IsLower(Text[Index + 1]);
				// stoneWall, Stone01, 01Stone, and ABCWord (an acronym before a word).
				if ((FChar::IsLower(Prev) && FChar::IsUpper(C))
					|| (FChar::IsDigit(Prev) != FChar::IsDigit(C))
					|| (FChar::IsUpper(Prev) && FChar::IsUpper(C) && bNextLower))
				{
					Flush();
				}
			}
			Current.AppendChar(C);
		}
		Flush();
		return Out;
	}

	FString Stem(const FString& Word)
	{
		FString Out = Word.ToLower();
		if (Out.Len() <= 3 || IsAllDigits(Out))
		{
			return Out;
		}
		const auto Chop = [&Out](const int32 Count) { Out.LeftChopInline(Count, EAllowShrinking::No); };
		if (Out.EndsWith(TEXT("ies")) && Out.Len() > 4)
		{
			Chop(3);
			Out.AppendChar(TEXT('y'));
		}
		else if (Out.EndsWith(TEXT("sses")) || Out.EndsWith(TEXT("shes")) || Out.EndsWith(TEXT("ches")) || Out.EndsWith(TEXT("xes")) || Out.EndsWith(TEXT("zes")))
		{
			Chop(2);
		}
		else if (Out.EndsWith(TEXT("s")) && !Out.EndsWith(TEXT("ss")) && !Out.EndsWith(TEXT("us")) && !Out.EndsWith(TEXT("is")))
		{
			Chop(1);
		}
		if (Out.Len() > 5 && Out.EndsWith(TEXT("ing")))
		{
			Chop(3);
		}
		else if (Out.Len() > 4 && Out.EndsWith(TEXT("ed")))
		{
			Chop(2);
		}
		else if (Out.Len() > 5 && Out.EndsWith(TEXT("en")))
		{
			Chop(2);
		}
		else if (Out.Len() > 5 && Out.EndsWith(TEXT("ish")))
		{
			Chop(3);
		}
		else if (Out.Len() >= 5 && Out.EndsWith(TEXT("y")) && !IsVowel(Out[Out.Len() - 2]))
		{
			Chop(1);
		}
		// fogg -> fog, redd -> red; moss, wall and buzz keep theirs.
		if (Out.Len() >= 4)
		{
			const TCHAR Last = Out[Out.Len() - 1];
			if (Last == Out[Out.Len() - 2] && !IsVowel(Last) && Last != TEXT('s') && Last != TEXT('l') && Last != TEXT('z'))
			{
				Chop(1);
			}
		}
		return Out;
	}

	bool IsStopWord(const FString& Word)
	{
		return Vocabulary().Stops.Contains(Word.ToLower());
	}

	void ConceptsOf(const FString& StemValue, const bool bPrefix, TArray<int32>& OutConcepts)
	{
		const FVocabulary& Vocab = Vocabulary();
		if (const TArray<int32>* Found = (bPrefix ? Vocab.ByPrefix : Vocab.ByStem).Find(StemValue))
		{
			for (const int32 Id : *Found)
			{
				OutConcepts.AddUnique(Id);
			}
		}
	}

	int32 ConceptCount()
	{
		return Vocabulary().Count;
	}

	void GramsOf(const FString& Word, TArray<uint32>& OutGrams)
	{
		if (Word.Len() < 3)
		{
			return;
		}
		const FString Padded = TEXT(" ") + Word + TEXT(" ");
		for (int32 Index = 0; Index + 3 <= Padded.Len(); ++Index)
		{
			const FString Gram = Padded.Mid(Index, 3);
			OutGrams.AddUnique(MakeFeature(EFeature::Gram, FCrc::StrCrc32(*Gram)));
		}
	}

	bool IsUsefulTagValue(const FString& Value)
	{
		if (Value.Len() < 3 || Value.Len() > 64 || IsAllDigits(Value))
		{
			return false;
		}
		bool bLetter = false;
		for (const TCHAR C : Value)
		{
			if (C == TEXT('/') || C == TEXT('\\') || C == TEXT('\'') || C == TEXT('"') || C == TEXT('=') || C == TEXT('(')
				|| C == TEXT('{') || C == TEXT('[') || C == TEXT(':'))
			{
				return false;
			}
			bLetter |= FChar::IsAlpha(C);
		}
		if (!bLetter || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase)
			|| Value.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		// Enum constants (PF_DXT1, TC_Default, EBlendMode::...) say how an asset is stored, not what it shows.
		int32 Underscore = INDEX_NONE;
		if (Value.FindChar(TEXT('_'), Underscore) && Underscore > 0 && Underscore <= 4)
		{
			bool bUpperPrefix = true;
			for (int32 Index = 0; Index < Underscore; ++Index)
			{
				bUpperPrefix &= FChar::IsUpper(Value[Index]) || FChar::IsDigit(Value[Index]);
			}
			if (bUpperPrefix)
			{
				return false;
			}
		}
		// A GUID or hash: long and hex only.
		if (Value.Len() >= 16)
		{
			bool bHex = true;
			for (const TCHAR C : Value)
			{
				bHex &= FChar::IsHexDigit(C) || C == TEXT('-');
			}
			if (bHex)
			{
				return false;
			}
		}
		return true;
	}

	FDoc BuildDoc(const FDocInput& Input)
	{
		TMap<uint32, float> Features;
		TSet<uint32> Grams;
		float Length = 0.0f;
		const auto AddWords = [&Features, &Grams, &Length](const TArray<FString>& Words, const EField Field)
		{
			const float Weight = FieldWeight(Field);
			for (const FString& Word : Words)
			{
				const FString StemValue = Stem(Word);
				AddWeight(Features, WordFeature(StemValue), Weight);
				Length += Weight;
				TArray<int32> Concepts;
				ConceptsOf(StemValue, false, Concepts);
				for (const int32 Concept : Concepts)
				{
					AddWeight(Features, MakeFeature(EFeature::Concept, static_cast<uint32>(Concept)), Weight * ConceptShare);
				}
				if (Field == EField::Name)
				{
					TArray<uint32> WordGrams;
					GramsOf(StemValue, WordGrams);
					Grams.Append(WordGrams);
				}
			}
		};

		// The name's first word may be a naming-convention prefix (SM_, MI_, T_...), which says what kind of asset it is.
		const TArray<FString> NameParts = SplitWords(Input.AssetName);
		if (NameParts.Num() > 1 && NameParts[0].Len() <= 3)
		{
			TArray<int32> Concepts;
			ConceptsOf(NameParts[0], true, Concepts);
			for (const int32 Concept : Concepts)
			{
				AddWeight(Features, MakeFeature(EFeature::Concept, static_cast<uint32>(Concept)), FieldWeight(EField::Class) * ConceptShare);
			}
		}
		AddWords(IndexWords(Input.AssetName), EField::Name);
		for (const FString& Tag : Input.Tags)
		{
			AddWords(IndexWords(Tag), EField::Tag);
		}
		FString ClassName = Input.ClassPath;
		int32 Dot = INDEX_NONE;
		if (ClassName.FindLastChar(TEXT('.'), Dot))
		{
			ClassName.RightChopInline(Dot + 1);
		}
		AddWords(IndexWords(ClassName), EField::Class);
		FString Parent = Input.ParentClass;
		if (Parent.FindLastChar(TEXT('.'), Dot))
		{
			Parent.RightChopInline(Dot + 1);
		}
		Parent.RemoveFromEnd(TEXT("_C"));
		AddWords(IndexWords(Parent), EField::Parent);
		TArray<FString> FolderWords;
		TArray<FString> Segments;
		Input.PackagePath.ParseIntoArray(Segments, TEXT("/"));
		// The mount point (Game, or a plugin's name) says nothing about the asset.
		for (int32 Index = 1; Index < Segments.Num(); ++Index)
		{
			FolderWords.Append(IndexWords(Segments[Index]));
		}
		AddWords(FolderWords, EField::Folder);

		FDoc Doc;
		Doc.Length = Length;
		Doc.GramCount = Grams.Num();
		Doc.Features.Reserve(Features.Num() + Grams.Num());
		for (const TPair<uint32, float>& Pair : Features)
		{
			Doc.Features.Add({Pair.Key, Pair.Value});
		}
		for (const uint32 Gram : Grams)
		{
			Doc.Features.Add({Gram, 1.0f});
		}
		Doc.Features.Sort([](const FFeatureWeight& Left, const FFeatureWeight& Right) { return Left.Feature < Right.Feature; });
		if (Doc.Features.Num() > MaxFeaturesPerAsset)
		{
			// Keep the strongest: an asset with a hundred tag words is not more relevant than one with ten.
			Doc.Features.Sort([](const FFeatureWeight& Left, const FFeatureWeight& Right)
			{
				return Left.Weight != Right.Weight ? Left.Weight > Right.Weight : Left.Feature < Right.Feature;
			});
			Doc.Features.SetNum(MaxFeaturesPerAsset);
			Doc.Features.Sort([](const FFeatureWeight& Left, const FFeatureWeight& Right) { return Left.Feature < Right.Feature; });
		}
		return Doc;
	}

	FQuery AnalyzeQuery(const FString& Text, const bool bConcepts)
	{
		FQuery Query;
		Query.bConcepts = bConcepts;
		for (const FString& Word : SplitWords(Text))
		{
			if (IsAllDigits(Word))
			{
				continue;
			}
			Query.Raw.AddUnique(Word);
			if (IsStopWord(Word) || Word.Len() < 2 || Query.Words.Num() >= MaxQueryWords)
			{
				continue;
			}
			const FString StemValue = Stem(Word);
			if (Query.Words.ContainsByPredicate([&StemValue](const FQueryWord& Existing) { return Existing.Stem == StemValue; }))
			{
				continue;
			}
			FQueryWord& Out = Query.Words.AddDefaulted_GetRef();
			Out.Stem = StemValue;
			Out.Word = WordFeature(StemValue);
			if (bConcepts)
			{
				TArray<int32> Concepts;
				ConceptsOf(StemValue, false, Concepts);
				for (const int32 Concept : Concepts)
				{
					Out.Concepts.Add(MakeFeature(EFeature::Concept, static_cast<uint32>(Concept)));
				}
				GramsOf(StemValue, Out.Grams);
			}
		}
		return Query;
	}

	TArray<FRanked> Rank(const FHyperAIProjectAssetIndexSnapshot& Assets, const FQuery& Query, const FRankLimits& Limits, bool& bOutTruncated)
	{
		bOutTruncated = false;
		TArray<FRanked> Out;
		if (Query.IsEmpty() || Assets.Folders.IsEmpty())
		{
			return Out;
		}
		const double Start = FPlatformTime::Seconds();
		int32 DocumentCount = 0;
		double TotalLength = 0.0;
		for (const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder : Assets.Folders)
		{
			DocumentCount += Folder->Rows.Num();
			TotalLength += Folder->TotalDocLength;
		}
		const float AverageLength = DocumentCount > 0 ? static_cast<float>(TotalLength / DocumentCount) : 1.0f;

		// Document frequencies first, so every folder is scored with the project's statistics.
		TMap<uint32, int32> Frequency;
		for (const FQueryWord& Word : Query.Words)
		{
			Frequency.Add(Word.Word, 0);
			for (const uint32 Concept : Word.Concepts)
			{
				Frequency.Add(Concept, 0);
			}
		}
		for (const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder : Assets.Folders)
		{
			for (TPair<uint32, int32>& Pair : Frequency)
			{
				if (const TArray<FHyperAIProjectAssetPosting>* Postings = Folder->Postings.Find(Pair.Key))
				{
					Pair.Value += Postings->Num();
				}
			}
		}

		struct FAccumulator
		{
			float Score = 0.0f;
			uint8 Matched = 0;
			uint8 Exact = 0;
			uint8 GramHits[MaxQueryWords] = {};
		};
		TMap<const FHyperAIProjectAssetRow*, FAccumulator> Scores;
		int32 FolderIndex = 0;
		for (const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder : Assets.Folders)
		{
			if ((++FolderIndex & 15) == 0 && FPlatformTime::Seconds() - Start > Limits.BudgetSeconds)
			{
				bOutTruncated = true;
				break;
			}
			for (int32 WordIndex = 0; WordIndex < Query.Words.Num(); ++WordIndex)
			{
				const FQueryWord& Word = Query.Words[WordIndex];
				const uint8 Bit = static_cast<uint8>(1u << WordIndex);
				const auto Score = [&](const uint32 Feature, const float QueryWeight, const bool bExact)
				{
					const TArray<FHyperAIProjectAssetPosting>* Postings = Folder->Postings.Find(Feature);
					if (!Postings)
					{
						return;
					}
					const float WordIdf = Idf(DocumentCount, Frequency.FindRef(Feature));
					for (const FHyperAIProjectAssetPosting& Posting : *Postings)
					{
						const FHyperAIProjectAssetRow& Row = Folder->Rows[Posting.Row];
						FAccumulator& Acc = Scores.FindOrAdd(&Row);
						Acc.Score += QueryWeight * Bm25(Posting.Weight, Row.DocLength, AverageLength, WordIdf);
						Acc.Matched |= Bit;
						Acc.Exact |= bExact ? Bit : 0;
					}
				};
				Score(Word.Word, 1.0f, true);
				for (const uint32 Concept : Word.Concepts)
				{
					Score(Concept, QueryConceptWeight, false);
				}
				for (const uint32 Gram : Word.Grams)
				{
					if (const TArray<FHyperAIProjectAssetPosting>* Postings = Folder->Postings.Find(Gram))
					{
						for (const FHyperAIProjectAssetPosting& Posting : *Postings)
						{
							FAccumulator& Acc = Scores.FindOrAdd(&Folder->Rows[Posting.Row]);
							Acc.GramHits[WordIndex] = static_cast<uint8>(FMath::Min(255, Acc.GramHits[WordIndex] + 1));
						}
					}
				}
			}
		}

		const int32 Needed = FMath::Max(1, FMath::CeilToInt(Limits.MinCoverage * Query.Words.Num()));
		for (TPair<const FHyperAIProjectAssetRow*, FAccumulator>& Pair : Scores)
		{
			FAccumulator& Acc = Pair.Value;
			uint8 Spelled = 0;
			for (int32 WordIndex = 0; WordIndex < Query.Words.Num(); ++WordIndex)
			{
				const uint8 Bit = static_cast<uint8>(1u << WordIndex);
				const int32 QueryGrams = Query.Words[WordIndex].Grams.Num();
				// A misspelt word: most of its trigrams are in one of the name's words.
				if (!(Acc.Matched & Bit) && QueryGrams >= 4 && Acc.GramHits[WordIndex] * 2 >= QueryGrams)
				{
					const float Similarity = static_cast<float>(Acc.GramHits[WordIndex])
						/ FMath::Sqrt(static_cast<float>(QueryGrams) * FMath::Max(1, static_cast<int32>(Pair.Key->GramCount)));
					Acc.Score += SpellingCredit * Similarity * 4.0f;
					Acc.Matched |= Bit;
					Spelled |= Bit;
				}
			}
			if (FMath::CountBits(Acc.Matched) < Needed)
			{
				continue;
			}
			FRanked& Ranked = Out.AddDefaulted_GetRef();
			Ranked.Row = Pair.Key;
			Ranked.Score = Acc.Score;
			bool bLiteral = !Query.Raw.IsEmpty();
			for (const FString& Raw : Query.Raw)
			{
				if (!IsStopWord(Raw) && !Pair.Key->SearchTextLower.Contains(Raw, ESearchCase::CaseSensitive))
				{
					bLiteral = false;
					break;
				}
			}
			if (bLiteral && FMath::CountBits(Acc.Exact) == Query.Words.Num())
			{
				Ranked.Score += NameBonus;
				Ranked.Match = TEXT("name");
			}
			else if (!Query.bConcepts)
			{
				Ranked.Match = TEXT("words");
			}
			else
			{
				Ranked.Match = Spelled == Acc.Matched ? TEXT("spelling") : TEXT("meaning");
			}
		}
		Out.Sort([](const FRanked& Left, const FRanked& Right)
		{
			if (Left.Score != Right.Score)
			{
				return Left.Score > Right.Score;
			}
			return Left.Row->ObjectPath.ToString() < Right.Row->ObjectPath.ToString();
		});
		if (Out.Num() > Limits.MaxResults)
		{
			Out.SetNum(Limits.MaxResults);
		}
		return Out;
	}
}
