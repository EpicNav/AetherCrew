// Games by Hyper 2026.

#include "HyperAIStudioMentions.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioContextSearchToolsets.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioProjectAssetIndex.h"
#include "HyperAIStudioSemanticSearch.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "HyperAIStudioSkills.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "String/Find.h"

namespace HyperAIStudio::Mentions
{
	namespace
	{
		constexpr int32 MaxActorsScanned = 4096;
		constexpr int32 MaxOperations = 30;
		constexpr int32 PerKindWhenEmpty = 8;
		constexpr double AssetScanSeconds = 0.015;
		constexpr double FileCacheSeconds = 30.0;

		struct FFileCache
		{
			TArray<FString> Files;
			double ReadSeconds = -1.0e9;
			FString Root;
			TOptional<TArray<FString>> ForTests;
		};

		FFileCache& Files()
		{
			static FFileCache Cache;
			return Cache;
		}

		bool IsBoundary(const FStringView Text, const int32 Index)
		{
			if (Index == 0)
			{
				return true;
			}
			const TCHAR Prev = Text[Index - 1];
			const TCHAR Current = Text[Index];
			return Prev == TEXT('_') || Prev == TEXT('-') || Prev == TEXT('.') || Prev == TEXT('/') || Prev == TEXT(' ')
				|| Prev == TEXT('\\') || (FChar::IsLower(Prev) && FChar::IsUpper(Current)) || (FChar::IsDigit(Prev) != FChar::IsDigit(Current));
		}

		int32 KindOrder(const EHyperAIStudioMentionKind Kind)
		{
			return static_cast<int32>(Kind);
		}

		bool IsTokenCharacter(const TCHAR C)
		{
			return FChar::IsAlnum(C) || C == TEXT('_') || C == TEXT('-') || C == TEXT('.');
		}

		const TArray<FString>& ProjectFiles()
		{
			FFileCache& Cache = Files();
			if (Cache.ForTests.IsSet())
			{
				return Cache.ForTests.GetValue();
			}
			const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			if (Cache.Root == Root && FPlatformTime::Seconds() - Cache.ReadSeconds < FileCacheSeconds)
			{
				return Cache.Files;
			}
			Cache.Root = Root;
			Cache.ReadSeconds = FPlatformTime::Seconds();
			Cache.Files.Reset();
			static const TSet<FString> Extensions = {
				TEXT("h"), TEXT("cpp"), TEXT("cs"), TEXT("ini"), TEXT("md"), TEXT("json"), TEXT("txt"), TEXT("py"), TEXT("uproject"),
				TEXT("uplugin"), TEXT("usf"), TEXT("ush"), TEXT("yaml"), TEXT("yml"), TEXT("toml"), TEXT("html"), TEXT("csv")
			};
			const auto Wanted = [](const FString& File)
			{
				return Extensions.Contains(FPaths::GetExtension(File).ToLower());
			};
			TArray<FString> RootFiles;
			IFileManager::Get().FindFiles(RootFiles, *(Root / TEXT("*")), /*Files=*/true, /*Directories=*/false);
			for (const FString& File : RootFiles)
			{
				if (Wanted(File))
				{
					Cache.Files.Add(Root / File);
				}
			}
			// The places people and agents write: code, config, HyperAI's shared workspace and docs. Content is assets, and
			// those come from the asset index.
			for (const TCHAR* Folder : {TEXT("Source"), TEXT("Config"), TEXT(".hyperai"), TEXT("Docs"), TEXT(".agents")})
			{
				IFileManager::Get().IterateDirectoryRecursively(*(Root / Folder), [&Cache, &Wanted](const TCHAR* Path, const bool bDirectory)
				{
					if (Cache.Files.Num() >= MaxProjectFiles)
					{
						return false;
					}
					const FString File(Path);
					if (!bDirectory && Wanted(File) && !File.Contains(TEXT("/.hyperai/runtime/")) && !File.Contains(TEXT("/Intermediate/")))
					{
						Cache.Files.Add(File);
					}
					return true;
				});
			}
			Cache.Files.Sort();
			return Cache.Files;
		}

		FString Clip(const FString& Text, const int32 Max)
		{
			return Text.Len() <= Max ? Text : Text.Left(Max - 1) + TEXT("...");
		}

		void AddAssetCandidates(const FString& Query, TArray<FHyperAIStudioMention>& Out)
		{
			FHyperAIStudioProjectIndexStore& Store = FHyperAIStudioProjectIndexStore::Get();
			Store.EnsureAssetIndexStarted();
			const TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> Assets = Store.GetAssetSnapshotShared();
			const auto Make = [](const FHyperAIProjectAssetRow& Row)
			{
				FHyperAIStudioMention Mention;
				Mention.Kind = EHyperAIStudioMentionKind::Asset;
				Mention.Label = Row.ObjectPath.GetAssetName();
				Mention.Reference = Row.ObjectPath.ToString();
				Mention.Detail = Row.ClassPath.GetAssetName().ToString();
				return Mention;
			};
			struct FScored
			{
				int32 Score;
				const FHyperAIProjectAssetRow* Row;
			};
			TArray<FScored> Scored;
			const double Start = FPlatformTime::Seconds();
			int32 Seen = 0;
			for (const TSharedRef<const FHyperAIProjectAssetFolder, ESPMode::ThreadSafe>& Folder : Assets->Folders)
			{
				for (const FHyperAIProjectAssetRow& Row : Folder->Rows)
				{
					if ((++Seen & 255) == 0 && FPlatformTime::Seconds() - Start > AssetScanSeconds)
					{
						break;
					}
					const int32 Score = FuzzyScore(Query, Row.ObjectPath.GetAssetName());
					if (Score >= 0)
					{
						Scored.Add({Score, &Row});
					}
				}
			}
			Scored.Sort([](const FScored& Left, const FScored& Right) { return Left.Score > Right.Score; });
			for (int32 Index = 0; Index < Scored.Num() && Index < MaxPickerResults; ++Index)
			{
				Out.Add(Make(*Scored[Index].Row));
			}
			// A longer query also finds assets by meaning: "@mossy" brings MI_Cobble_Overgrown.
			if (Query.Len() >= 4 && GetDefault<UHyperAIStudioSettings>()->bSearchByMeaning)
			{
				const SemanticSearch::FQuery Analyzed = SemanticSearch::AnalyzeQuery(Query.Replace(TEXT("_"), TEXT(" ")), true);
				SemanticSearch::FRankLimits Limits;
				Limits.MaxResults = 12;
				Limits.BudgetSeconds = 0.020;
				bool bTruncated = false;
				for (const SemanticSearch::FRanked& Ranked : SemanticSearch::Rank(*Assets, Analyzed, Limits, bTruncated))
				{
					FHyperAIStudioMention Mention = Make(*Ranked.Row);
					if (!Out.Contains(Mention))
					{
						Mention.Detail += TEXT(" - by ") + Ranked.Match;
						Out.Add(MoveTemp(Mention));
					}
				}
			}
		}

		void AddActorCandidates(TArray<FHyperAIStudioMention>& Out)
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				return;
			}
			int32 Count = 0;
			for (TActorIterator<AActor> It(World); It && Count < MaxActorsScanned; ++It, ++Count)
			{
				if (const AActor* Actor = *It; Actor && !Actor->IsTemplate() && Actor->IsEditable() && Actor->IsListedInSceneOutliner())
				{
					Out.Add(FromActor(*Actor));
				}
			}
		}

		void AddFileCandidates(TArray<FHyperAIStudioMention>& Out)
		{
			const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
			for (const FString& File : ProjectFiles())
			{
				FHyperAIStudioMention Mention;
				Mention.Kind = EHyperAIStudioMentionKind::File;
				Mention.Label = FPaths::GetCleanFilename(File);
				Mention.Reference = File;
				FString Relative = File;
				FPaths::MakePathRelativeTo(Relative, *Root);
				Mention.Detail = Relative;
				Out.Add(MoveTemp(Mention));
			}
		}

		void AddSkillCandidates(TArray<FHyperAIStudioMention>& Out)
		{
			for (const FHyperAIStudioSkill& Skill : Skills::ListProjectSkills(FHyperAIStudioService::GetProjectRoot()))
			{
				FHyperAIStudioMention Mention;
				Mention.Kind = EHyperAIStudioMentionKind::Skill;
				Mention.Label = Skill.Name;
				Mention.Reference = FPaths::ConvertRelativePathToFull(Skill.Folder / TEXT("SKILL.md"));
				Mention.Detail = Clip(Skill.Description, 90);
				Out.Add(MoveTemp(Mention));
			}
		}

		void AddOperationCandidates(TArray<FHyperAIStudioMention>& Out)
		{
			TSet<FString> Seen;
			for (const FHyperAIStudioActivityEntry& Entry : FHyperAIStudioAgentActivityLog::GetSnapshot())
			{
				if (Entry.OperationId.IsEmpty() || Seen.Contains(Entry.OperationId))
				{
					continue;
				}
				Seen.Add(Entry.OperationId);
				FHyperAIStudioMention Mention;
				Mention.Kind = EHyperAIStudioMentionKind::Operation;
				const FString Tool = Entry.ToolName.IsEmpty() ? FString(TEXT("operation")) : Entry.ToolName;
				Mention.Label = Tool + TEXT(" ") + Entry.OperationId.Left(12);
				Mention.Reference = Entry.OperationId;
				Mention.Detail = Entry.Target.IsEmpty()
					? FString(FHyperAIStudioAgentActivityLog::LexToString(Entry.Kind))
					: FString::Printf(TEXT("%s, %s"), FHyperAIStudioAgentActivityLog::LexToString(Entry.Kind), *Entry.Target);
				Out.Add(MoveTemp(Mention));
				if (Seen.Num() >= MaxOperations)
				{
					break;
				}
			}
		}
	}

	const TCHAR* KindName(const EHyperAIStudioMentionKind Kind)
	{
		switch (Kind)
		{
		case EHyperAIStudioMentionKind::Asset: return TEXT("asset");
		case EHyperAIStudioMentionKind::Actor: return TEXT("actor");
		case EHyperAIStudioMentionKind::File: return TEXT("file");
		case EHyperAIStudioMentionKind::Skill: return TEXT("skill");
		default: return TEXT("operation");
		}
	}

	int32 FuzzyScore(const FStringView Query, const FStringView Candidate)
	{
		if (Query.IsEmpty())
		{
			return 0;
		}
		int32 QueryIndex = 0;
		int32 Score = 0;
		int32 Previous = -2;
		for (int32 Index = 0; Index < Candidate.Len() && QueryIndex < Query.Len(); ++Index)
		{
			if (FChar::ToLower(Candidate[Index]) != FChar::ToLower(Query[QueryIndex]))
			{
				continue;
			}
			int32 Bonus = 1;
			if (Index == 0)
			{
				Bonus += 8;
			}
			else if (IsBoundary(Candidate, Index))
			{
				Bonus += 5;
			}
			if (Previous == Index - 1)
			{
				Bonus += 4;
			}
			Score += Bonus;
			Previous = Index;
			++QueryIndex;
		}
		if (QueryIndex < Query.Len())
		{
			return -1;
		}
		const int32 Found = UE::String::FindFirst(Candidate, Query, ESearchCase::IgnoreCase);
		if (Found != INDEX_NONE)
		{
			Score += Found == 0 ? 20 : 10;
		}
		// Of two equal matches, the shorter name is the closer one.
		return FMath::Max(0, Score * 4 - Candidate.Len() / 4);
	}

	TArray<FHyperAIStudioMention> Filter(const TArray<FHyperAIStudioMention>& Candidates, const FString& Query, const int32 Max)
	{
		struct FScored
		{
			int32 Score;
			int32 Index;
		};
		TArray<FScored> Scored;
		const FString Trimmed = Query.TrimStartAndEnd();
		for (int32 Index = 0; Index < Candidates.Num(); ++Index)
		{
			const FHyperAIStudioMention& Candidate = Candidates[Index];
			int32 Score = FuzzyScore(Trimmed, Candidate.Label);
			const int32 ByReference = FuzzyScore(Trimmed, Candidate.Kind == EHyperAIStudioMentionKind::File ? Candidate.Detail : Candidate.Reference);
			Score = FMath::Max(Score, ByReference >= 0 ? ByReference / 2 : -1);
			if (Score >= 0)
			{
				Scored.Add({Score, Index});
			}
		}
		Scored.Sort([&Candidates](const FScored& Left, const FScored& Right)
		{
			if (Left.Score != Right.Score)
			{
				return Left.Score > Right.Score;
			}
			const FHyperAIStudioMention& A = Candidates[Left.Index];
			const FHyperAIStudioMention& B = Candidates[Right.Index];
			if (A.Kind != B.Kind)
			{
				return KindOrder(A.Kind) < KindOrder(B.Kind);
			}
			return A.Label < B.Label;
		});
		TArray<FHyperAIStudioMention> Out;
		for (const FScored& Entry : Scored)
		{
			const FHyperAIStudioMention& Candidate = Candidates[Entry.Index];
			if (!Out.Contains(Candidate))
			{
				Out.Add(Candidate);
			}
			if (Out.Num() >= Max)
			{
				break;
			}
		}
		return Out;
	}

	bool FindActiveQuery(const FString& Text, const int32 Cursor, int32& OutStart, FString& OutQuery)
	{
		if (Cursor <= 0 || Cursor > Text.Len())
		{
			return false;
		}
		for (int32 Index = Cursor - 1; Index >= 0; --Index)
		{
			const TCHAR C = Text[Index];
			if (FChar::IsWhitespace(C))
			{
				return false;
			}
			if (C == TEXT('@'))
			{
				if (Index > 0 && !FChar::IsWhitespace(Text[Index - 1]))
				{
					// name@example.com is an address, not a mention.
					return false;
				}
				OutStart = Index;
				OutQuery = Text.Mid(Index + 1, Cursor - Index - 1);
				return true;
			}
		}
		return false;
	}

	FString Token(const FHyperAIStudioMention& Mention)
	{
		FString Label = Mention.Label.TrimStartAndEnd();
		for (TCHAR& C : Label)
		{
			if (FChar::IsWhitespace(C))
			{
				C = TEXT('_');
			}
		}
		return TEXT("@") + Label;
	}

	FString ReplaceQuery(const FString& Text, const int32 Start, const int32 Cursor, const FHyperAIStudioMention& Mention, int32& OutCursor)
	{
		const int32 SafeStart = FMath::Clamp(Start, 0, Text.Len());
		const int32 SafeCursor = FMath::Clamp(Cursor, SafeStart, Text.Len());
		const FString Inserted = Token(Mention) + TEXT(" ");
		FString Rest = Text.Mid(SafeCursor);
		// A space already after the query is the one the token needs.
		if (Rest.StartsWith(TEXT(" ")))
		{
			Rest.RightChopInline(1);
		}
		OutCursor = SafeStart + Inserted.Len();
		return Text.Left(SafeStart) + Inserted + Rest;
	}

	TArray<FHyperAIStudioMention> StillReferenced(const FString& Text, const TArray<FHyperAIStudioMention>& Mentions)
	{
		TArray<FHyperAIStudioMention> Out;
		for (const FHyperAIStudioMention& Mention : Mentions)
		{
			const FString Needle = Token(Mention);
			bool bFound = false;
			for (int32 From = 0; !bFound;)
			{
				const int32 At = Text.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
				if (At == INDEX_NONE)
				{
					break;
				}
				const int32 End = At + Needle.Len();
				// @SM_Rock is not in @SM_Rock2; a sentence's full stop after it still counts.
				const bool bWholeToken = End >= Text.Len() || !IsTokenCharacter(Text[End])
					|| (Text[End] == TEXT('.') && (End + 1 >= Text.Len() || FChar::IsWhitespace(Text[End + 1])));
				bFound = bWholeToken;
				From = At + 1;
			}
			if (bFound && !Out.Contains(Mention))
			{
				Out.Add(Mention);
			}
		}
		return Out;
	}

	FString Serialize(const FString& Text, const TArray<FHyperAIStudioMention>& Mentions)
	{
		const TArray<FHyperAIStudioMention> Kept = StillReferenced(Text, Mentions);
		if (Kept.IsEmpty())
		{
			return Text;
		}
		FString Out = Text.TrimEnd() + TEXT("\n\nReferenced with @ (exact references):");
		for (const FHyperAIStudioMention& Mention : Kept)
		{
			Out += TEXT("\n- ") + Token(Mention) + TEXT(": ");
			switch (Mention.Kind)
			{
			case EHyperAIStudioMentionKind::Asset:
				Out += FString::Printf(TEXT("asset %s"), *Mention.Reference);
				break;
			case EHyperAIStudioMentionKind::Actor:
				Out += FString::Printf(TEXT("level actor \"%s\" %s"), *Mention.Label, *Mention.Reference);
				break;
			case EHyperAIStudioMentionKind::File:
				Out += FString::Printf(TEXT("file %s"), *Mention.Reference);
				break;
			case EHyperAIStudioMentionKind::Skill:
				Out += FString::Printf(TEXT("skill %s"), *Mention.Reference);
				break;
			default:
				Out += FString::Printf(TEXT("HyperAI operation %s"), *Mention.Reference);
				break;
			}
			if (!Mention.Detail.IsEmpty() && Mention.Kind != EHyperAIStudioMentionKind::File)
			{
				Out += TEXT(" (") + Mention.Detail + TEXT(")");
			}
		}
		return Out;
	}

	TArray<FHyperAIStudioMention> FromAssets(const TConstArrayView<FAssetData> Assets)
	{
		TArray<FHyperAIStudioMention> Out;
		for (const FAssetData& Asset : Assets)
		{
			if (!Asset.IsValid())
			{
				continue;
			}
			FHyperAIStudioMention Mention;
			Mention.Kind = EHyperAIStudioMentionKind::Asset;
			Mention.Label = Asset.AssetName.ToString();
			Mention.Reference = Asset.GetSoftObjectPath().ToString();
			Mention.Detail = Asset.AssetClassPath.GetAssetName().ToString();
			Out.AddUnique(MoveTemp(Mention));
		}
		return Out;
	}

	FHyperAIStudioMention FromActor(const AActor& Actor)
	{
		FHyperAIStudioMention Mention;
		Mention.Kind = EHyperAIStudioMentionKind::Actor;
		Mention.Label = Actor.GetActorLabel();
		Mention.Reference = Actor.GetPathName();
		Mention.Detail = Actor.GetClass()->GetName();
		return Mention;
	}

	TArray<FHyperAIStudioMention> FromPastedText(const FString& Text)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*CullEmpty=*/true);
		if (Lines.IsEmpty() || Lines.Num() > 64)
		{
			return {};
		}
		const IAssetRegistry* Registry = IAssetRegistry::Get();
		TArray<FHyperAIStudioMention> Out;
		for (FString Line : Lines)
		{
			Line.TrimStartAndEndInline();
			if (Line.IsEmpty())
			{
				continue;
			}
			// A Content Browser copy: /Script/Engine.StaticMesh'/Game/Env/SM_Rock.SM_Rock'
			int32 Quote = INDEX_NONE;
			if (Line.EndsWith(TEXT("'")) && Line.FindChar(TEXT('\''), Quote) && Quote < Line.Len() - 1)
			{
				Line = Line.Mid(Quote + 1, Line.Len() - Quote - 2);
			}
			if (!Line.StartsWith(TEXT("/")) || Line.Contains(TEXT(" ")) || !FHyperAIStudioNames::IsObjectPath(Line, nullptr))
			{
				return {};
			}
			if (Registry)
			{
				FAssetData Asset = Registry->GetAssetByObjectPath(FSoftObjectPath(Line));
				if (!Asset.IsValid() && !Line.Contains(TEXT(".")) && FPackageName::IsValidLongPackageName(Line))
				{
					TArray<FAssetData> InPackage;
					Registry->GetAssetsByPackageName(FName(*Line), InPackage);
					Asset = InPackage.IsEmpty() ? FAssetData() : InPackage[0];
				}
				if (Asset.IsValid())
				{
					Out.Append(FromAssets({Asset}));
					continue;
				}
			}
			if (const AActor* Actor = FindObject<AActor>(nullptr, *Line))
			{
				Out.AddUnique(FromActor(*Actor));
				continue;
			}
			return {};
		}
		return Out;
	}

	TArray<FHyperAIStudioMention> Gather(const FString& Query, const int32 Max)
	{
		const FString Trimmed = Query.TrimStartAndEnd();
		TArray<FHyperAIStudioMention> Candidates;
		if (!Trimmed.IsEmpty())
		{
			AddAssetCandidates(Trimmed, Candidates);
		}
		AddActorCandidates(Candidates);
		AddFileCandidates(Candidates);
		AddSkillCandidates(Candidates);
		AddOperationCandidates(Candidates);
		if (Trimmed.IsEmpty())
		{
			// Nothing typed yet: a few of each kind, so the picker shows what it can find.
			TArray<FHyperAIStudioMention> Out;
			TMap<EHyperAIStudioMentionKind, int32> PerKind;
			for (const FHyperAIStudioMention& Candidate : Candidates)
			{
				int32& Count = PerKind.FindOrAdd(Candidate.Kind);
				if (Count < PerKindWhenEmpty && Out.Num() < Max)
				{
					++Count;
					Out.Add(Candidate);
				}
			}
			return Out;
		}
		TArray<FHyperAIStudioMention> Out = Filter(Candidates, Trimmed, Max);
		// Assets found by meaning match no letters of the query in order, so Filter drops them; they follow the rest.
		for (const FHyperAIStudioMention& Candidate : Candidates)
		{
			if (Out.Num() >= Max)
			{
				break;
			}
			if (Candidate.Kind == EHyperAIStudioMentionKind::Asset && Candidate.Detail.Contains(TEXT(" - by ")) && !Out.Contains(Candidate))
			{
				Out.Add(Candidate);
			}
		}
		return Out;
	}

	FString DescribeIndex()
	{
		const TSharedRef<const FHyperAIProjectAssetIndexSnapshot, ESPMode::ThreadSafe> Assets =
			FHyperAIStudioProjectIndexStore::Get().GetAssetSnapshotShared();
		const FString State = Assets->bPaused ? FString(TEXT("paused while Play In Editor runs")) : Assets->Status;
		return FString::Printf(TEXT("Asset index: %s, %d assets, %.1f MB in editor memory. Searched on this computer only."),
			*State, Assets->AssetCount, Assets->ApproxBytes / (1024.0 * 1024.0));
	}

#if WITH_DEV_AUTOMATION_TESTS
	void SetProjectFilesForTests(const TOptional<TArray<FString>>& InFiles)
	{
		Files().ForTests = InFiles;
	}
#endif
}
