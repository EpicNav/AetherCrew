// Games by Hyper 2026.

#include "HyperAIStudioMatchStudies.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::Lighting::Studies
{
	namespace
	{
		constexpr int32 MaxNoteChars = 160;
		constexpr int32 MaxListed = 8;
		constexpr int32 MaxFilesRead = 200;
		constexpr int64 MaxStudyBytes = 64 * 1024;

		/** One line without control characters, cut to Limit: what an agent wrote, kept safe to put in a later prompt. */
		FString OneLine(const FString& Text, const int32 Limit)
		{
			FString Out;
			bool bSpace = false;
			for (const TCHAR Character : Text)
			{
				if (FChar::IsWhitespace(Character) || Character < 32)
				{
					bSpace = !Out.IsEmpty();
					continue;
				}
				if (bSpace)
				{
					Out.AppendChar(TEXT(' '));
					bSpace = false;
				}
				Out.AppendChar(Character);
			}
			return Out.Len() > Limit ? Out.Left(Limit - 3) + TEXT("...") : Out;
		}

		FString Points(const float Score)
		{
			return FString::FromInt(FMath::RoundToInt(Score));
		}

		/** Lowercase letters and digits joined by dashes: part of a file name. */
		FString Slug(const FString& Text)
		{
			FString Out;
			for (const TCHAR Character : Text)
			{
				if (FChar::IsAlnum(Character) && Character < 128)
				{
					Out.AppendChar(FChar::ToLower(Character));
				}
				else if (!Out.IsEmpty() && !Out.EndsWith(TEXT("-")))
				{
					Out.AppendChar(TEXT('-'));
				}
			}
			Out.RemoveFromEnd(TEXT("-"));
			return Out.Left(48);
		}

		FString Top(const TArray<FString>& Lines, const int32 Count)
		{
			TArray<FString> Out;
			for (int32 Index = 0; Index < Lines.Num() && Index < Count; ++Index)
			{
				Out.Add(Lines[Index]);
			}
			return FString::Join(Out, TEXT("; "));
		}
	}

	FString Directory(const FString& ProjectDir)
	{
		return FPaths::Combine(ProjectDir, TEXT(".hyperai"), TEXT("memory"), TEXT("studies"));
	}

	void Learn(const TArray<FHyperAIImageIteration>& History, TArray<FString>& OutHelped, TArray<FString>& OutHurt)
	{
		TArray<TPair<float, FString>> Helped;
		TArray<TPair<float, FString>> Hurt;
		float Best = -1.f;
		for (const FHyperAIImageIteration& Iteration : History)
		{
			// The first attempt is where the session started, not a change.
			if (Best < 0.f)
			{
				Best = Iteration.Score;
				continue;
			}
			const float Change = Iteration.Score - Best;
			const FString Note = OneLine(Iteration.Note, MaxNoteChars);
			const FString What = FString::Printf(TEXT("%s%d: %s (attempt %d)"), Change > 0.f ? TEXT("+") : TEXT(""), FMath::RoundToInt(Change),
				Note.IsEmpty() ? TEXT("no note") : *Note, Iteration.Index);
			if (Change >= 1.f)
			{
				Helped.Add({Change, What});
			}
			else if (Change <= -2.f)
			{
				Hurt.Add({-Change, What});
			}
			Best = FMath::Max(Best, Iteration.Score);
		}
		for (TArray<TPair<float, FString>>* Changes : {&Helped, &Hurt})
		{
			Changes->StableSort([](const TPair<float, FString>& A, const TPair<float, FString>& B) { return A.Key > B.Key; });
		}
		for (int32 Index = 0; Index < Helped.Num() && Index < MaxListed; ++Index)
		{
			OutHelped.Add(Helped[Index].Value);
		}
		for (int32 Index = 0; Index < Hurt.Num() && Index < MaxListed; ++Index)
		{
			OutHurt.Add(Hurt[Index].Value);
		}
	}

	FString Format(const FStudy& Study)
	{
		FString Text = TEXT("---\n");
		auto Field = [&Text](const TCHAR* Key, const FString& Value) { Text += FString::Printf(TEXT("%s: %s\n"), Key, *OneLine(Value, 512)); };
		Field(TEXT("goal"), Study.Goal);
		Field(TEXT("reference"), Study.Reference);
		Field(TEXT("level"), Study.Level);
		Field(TEXT("session"), Study.Session);
		Field(TEXT("result"), Study.Result);
		Field(TEXT("best"), Points(Study.Best));
		Field(TEXT("target"), Points(Study.Target));
		Field(TEXT("attempts"), FString::FromInt(Study.Attempts));
		Field(TEXT("date"), Study.Date);
		Text += TEXT("---\n\n");
		Text += FString::Printf(TEXT("# Match study: %s, %s\n\n"), *Study.Goal, *FPaths::GetCleanFilename(Study.Reference));
		Text += OneLine(Study.Status, 400) + TEXT("\n");
		auto Section = [&Text](const TCHAR* Title, const TArray<FString>& Lines)
		{
			if (Lines.IsEmpty()) return;
			Text += FString::Printf(TEXT("\n## %s\n\n"), Title);
			for (const FString& Line : Lines)
			{
				Text += TEXT("- ") + OneLine(Line, 300) + TEXT("\n");
			}
		};
		Section(TEXT("Helped"), Study.Helped);
		Section(TEXT("Hurt"), Study.Hurt);
		Section(TEXT("Still differed"), Study.StillDiffered);
		Section(TEXT("Suggested next"), Study.NextSteps);
		return Text;
	}

	bool Parse(const FString& Text, FStudy& OutStudy)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*InCullEmpty=*/false);
		if (Lines.Num() < 3 || Lines[0].TrimEnd() != TEXT("---"))
		{
			return false;
		}
		int32 Index = 1;
		for (; Index < Lines.Num() && Lines[Index].TrimEnd() != TEXT("---"); ++Index)
		{
			FString Key;
			FString Value;
			if (!Lines[Index].Split(TEXT(":"), &Key, &Value)) continue;
			Key.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (Key == TEXT("goal")) OutStudy.Goal = Value;
			else if (Key == TEXT("reference")) OutStudy.Reference = Value;
			else if (Key == TEXT("level")) OutStudy.Level = Value;
			else if (Key == TEXT("session")) OutStudy.Session = Value;
			else if (Key == TEXT("result")) OutStudy.Result = Value;
			else if (Key == TEXT("best")) OutStudy.Best = FCString::Atof(*Value);
			else if (Key == TEXT("target")) OutStudy.Target = FCString::Atof(*Value);
			else if (Key == TEXT("attempts")) OutStudy.Attempts = FCString::Atoi(*Value);
			else if (Key == TEXT("date")) OutStudy.Date = Value;
		}
		if (Index >= Lines.Num())
		{
			return false;
		}
		TArray<FString>* Section = nullptr;
		for (++Index; Index < Lines.Num(); ++Index)
		{
			const FString Line = Lines[Index].TrimStartAndEnd();
			if (Line.IsEmpty() || Line.StartsWith(TEXT("# "))) continue;
			if (Line.StartsWith(TEXT("## ")))
			{
				const FString Title = Line.RightChop(3);
				Section = Title == TEXT("Helped") ? &OutStudy.Helped : Title == TEXT("Hurt") ? &OutStudy.Hurt
					: Title == TEXT("Still differed") ? &OutStudy.StillDiffered : Title == TEXT("Suggested next") ? &OutStudy.NextSteps : nullptr;
				continue;
			}
			if (Section && Line.StartsWith(TEXT("- ")))
			{
				Section->Add(Line.RightChop(2));
			}
			else if (!Section && OutStudy.Status.IsEmpty())
			{
				OutStudy.Status = Line;
			}
		}
		return !OutStudy.Goal.IsEmpty() && !OutStudy.Date.IsEmpty();
	}

	bool Write(const FString& ProjectDir, const FStudy& Study, FString& OutPath, FString& OutError)
	{
		const FString Folder = Directory(ProjectDir);
		if (!IFileManager::Get().MakeDirectory(*Folder, /*Tree=*/true))
		{
			OutError = FString::Printf(TEXT("could not make %s"), *Folder);
			return false;
		}
		// The session's time keeps two jobs on one reference apart: match_lighting_20261001_101500.
		FString Time = Study.Session.Right(6);
		if (!Time.IsNumeric())
		{
			Time = FDateTime::Now().ToString(TEXT("%H%M%S"));
		}
		const FString Name = FString::Printf(TEXT("%s-%s-%s-%s.md"), *Study.Date, *Slug(Study.Goal), *Slug(FPaths::GetBaseFilename(Study.Reference)), *Time);
		OutPath = FPaths::Combine(Folder, Name);
		if (!FFileHelper::SaveStringToFile(Format(Study), *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("could not write %s"), *OutPath);
			return false;
		}
		return true;
	}

	TArray<FStudy> Load(const FString& ProjectDir, const int32 Limit)
	{
		const FString Folder = Directory(ProjectDir);
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(Folder, TEXT("*.md")), /*Files=*/true, /*Directories=*/false);
		// Each name starts with its date, so the newest sort first.
		Names.Sort([](const FString& A, const FString& B) { return A > B; });
		TArray<FStudy> Studies;
		for (int32 Index = 0; Index < Names.Num() && Index < MaxFilesRead && Studies.Num() < Limit; ++Index)
		{
			const FString Path = FPaths::Combine(Folder, Names[Index]);
			FString Text;
			FStudy Study;
			if (IFileManager::Get().FileSize(*Path) <= MaxStudyBytes && FFileHelper::LoadFileToString(Text, *Path) && Parse(Text, Study))
			{
				Studies.Add(MoveTemp(Study));
			}
		}
		Studies.StableSort([](const FStudy& A, const FStudy& B) { return A.Date != B.Date ? A.Date > B.Date : A.Session > B.Session; });
		return Studies;
	}

	FString Lessons(const TArray<FStudy>& Studies, const FString& Goal, const FString& Level, const int32 MaxStudies)
	{
		TArray<TPair<int32, const FStudy*>> Ranked;
		for (const FStudy& Study : Studies)
		{
			const bool bLevel = !Level.IsEmpty() && Study.Level == Level;
			const bool bGoal = Study.Goal == Goal;
			const int32 Tier = bLevel && bGoal ? 0 : bLevel ? 1 : bGoal ? 2 : 3;
			if (Tier < 3 && (!Study.Helped.IsEmpty() || !Study.Hurt.IsEmpty() || !Study.StillDiffered.IsEmpty()))
			{
				Ranked.Add({Tier, &Study});
			}
		}
		if (Ranked.IsEmpty())
		{
			return FString();
		}
		Ranked.StableSort([](const TPair<int32, const FStudy*>& A, const TPair<int32, const FStudy*>& B) { return A.Key < B.Key; });
		FString Text = TEXT("Lessons from earlier Match jobs in this project (HyperAI keeps a study of each in .hyperai/memory/studies). What helps depends on the reference, so take them as leads; but on the same level, do not repeat a change that lowered the score before unless this reference asks for it:\n");
		for (int32 Index = 0; Index < Ranked.Num() && Index < MaxStudies; ++Index)
		{
			const FStudy& Study = *Ranked[Index].Value;
			Text += FString::Printf(TEXT("- %s, %s on %s against %s: %s at %s of %s after %d attempts."), *Study.Date, *Study.Goal,
				Study.Level.IsEmpty() ? TEXT("a level") : *FPackageName::GetShortName(Study.Level), *FPaths::GetCleanFilename(Study.Reference), *Study.Result,
				*Points(Study.Best), *Points(Study.Target), Study.Attempts);
			if (!Study.Helped.IsEmpty()) Text += TEXT(" Helped: ") + Top(Study.Helped, 3) + TEXT(".");
			if (!Study.Hurt.IsEmpty()) Text += TEXT(" Hurt: ") + Top(Study.Hurt, 3) + TEXT(".");
			if (!Study.StillDiffered.IsEmpty()) Text += TEXT(" Still differed: ") + Top(Study.StillDiffered, 2) + TEXT(".");
			Text += TEXT("\n");
		}
		return Text + TEXT("\n");
	}
}
