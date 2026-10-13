// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"

struct FHyperAIProjectAssetRow;
struct FHyperAIProjectAssetIndexSnapshot;

/**
 * Project search by meaning, on this machine only. Every asset becomes a sparse vector of weighted features: the stems of
 * the words in its name, tags, class and folders, the concepts those words belong to (a built-in vocabulary, so "moss"
 * meets "lichen" and "overgrown", "rock" meets "boulder" and "stone"), and the character trigrams of its name words, so a
 * misspelt word still lands. A query is analysed the same way and scored against the index's per-folder postings with
 * BM25. No model is downloaded and nothing is sent anywhere: the vocabulary ships in this file.
 *
 * Epic's SemanticSearch plugin (Experimental in 5.8) was the first choice, and was checked in the engine source: its only
 * embedding provider is an OpenAI-compatible HTTP API (api.openai.com by default) that captions thumbnails with a vision
 * model, so it would send project data off the machine unless a local server is set up for both captions and embeddings.
 */
namespace HyperAIStudio::SemanticSearch
{
	/** Feature kinds, in a feature id's top two bits. */
	enum class EFeature : uint32
	{
		Word = 0u,
		Concept = 1u,
		Gram = 2u
	};

	/** Where a word came from, which sets its weight: a name says most about an asset, a folder least. */
	enum class EField : uint8
	{
		Name,
		Tag,
		Class,
		Parent,
		Folder
	};

	/** What an asset is made of for the index; the registry gives all of it without loading the asset. */
	struct FDocInput
	{
		FString AssetName;
		/** /Script/Engine.MaterialInstanceConstant */
		FString ClassPath;
		/** /Game/Env/Walls */
		FString PackagePath;
		FString ParentClass;
		/** Short word-like Asset Registry tag values (see IsUsefulTagValue). */
		TArray<FString> Tags;
	};

	struct FFeatureWeight
	{
		uint32 Feature = 0;
		float Weight = 0.0f;
	};

	/** One asset's features, merged (one entry per feature) and sorted by feature id. */
	struct FDoc
	{
		TArray<FFeatureWeight> Features;
		/** Sum of word weights, for BM25's length normalisation. */
		float Length = 0.0f;
		/** Distinct trigrams of the name's words. */
		int32 GramCount = 0;
	};

	/** One query word, with the features that stand for it. */
	struct FQueryWord
	{
		FString Stem;
		uint32 Word = 0;
		TArray<uint32> Concepts;
		TArray<uint32> Grams;
	};

	struct FQuery
	{
		TArray<FQueryWord> Words;
		/** The words as typed (lowercased, stop words kept), for the literal name bonus. */
		TArray<FString> Raw;
		bool bConcepts = true;
		bool IsEmpty() const { return Words.IsEmpty(); }
	};

	/** Query words past this are ignored; a match mask has a bit per word. */
	constexpr int32 MaxQueryWords = 8;
	constexpr int32 MaxTagsPerAsset = 12;
	constexpr int32 MaxFeaturesPerAsset = 96;

	uint32 MakeFeature(EFeature Kind, uint32 Hash);
	EFeature KindOf(uint32 Feature);

	/** Lowercase words: CamelCase, snake_case, digits and punctuation all split ("SM_StoneWall01" -> sm, stone, wall, 01). */
	TArray<FString> SplitWords(FStringView Text);
	/** A light English stemmer: mossy -> moss, walls -> wall, wooden -> wood, rusted -> rust. */
	FString Stem(const FString& Word);
	bool IsStopWord(const FString& Word);
	/** The vocabulary's concept ids for a stem, or for an asset-name prefix when bPrefix (SM_, MI_, T_, BP_...). */
	void ConceptsOf(const FString& Stem, bool bPrefix, TArray<int32>& OutConcepts);
	/** How many concept groups the built-in vocabulary has. */
	int32 ConceptCount();
	/** Padded trigrams of one word, as features. */
	void GramsOf(const FString& Word, TArray<uint32>& OutGrams);
	/** Tag values worth indexing: short, word-like, not a number, path, enum constant or GUID. */
	bool IsUsefulTagValue(const FString& Value);

	FDoc BuildDoc(const FDocInput& Input);
	/** bConcepts false is the fallback: words only, no vocabulary and no trigrams (plain BM25 over the index's words). */
	FQuery AnalyzeQuery(const FString& Text, bool bConcepts = true);

	/** One ranked asset. */
	struct FRanked
	{
		const FHyperAIProjectAssetRow* Row = nullptr;
		float Score = 0.0f;
		/** name (every typed word is in its name, class or folder), meaning (concepts or stems), spelling (trigrams), or words (fallback). */
		FString Match;
	};

	struct FRankLimits
	{
		int32 MaxResults = 200;
		double BudgetSeconds = 0.050;
		/** Share of query words an asset must match (by word, concept or spelling), rounded up. */
		float MinCoverage = 0.6f;
	};

	/**
	 * Ranks the snapshot's assets for Query, best first (ties by object path). bOutTruncated is set when the time budget
	 * stopped the scoring early, so later folders were not seen.
	 */
	TArray<FRanked> Rank(const FHyperAIProjectAssetIndexSnapshot& Assets, const FQuery& Query, const FRankLimits& Limits, bool& bOutTruncated);
}
