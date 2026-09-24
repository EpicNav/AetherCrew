// Games by Hyper 2026.

#include "HyperAIStudioMarkdown.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Text/SlateHyperlinkRun.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioPreviewImage.h"
#include "HyperAIStudioPreviewPanel.h"
#include "HyperAIStudioStyle.h"
#include "HyperAIStudioWidgets.h"
#include "Misc/Base64.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SGridPanel.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioMarkdown"

namespace HyperAIStudio::Markdown
{
	namespace
	{
		enum class EInline : uint8
		{
			Text,
			Code,
			Bold,
			Italic,
			Link
		};

		struct FInline
		{
			EInline Kind = EInline::Text;
			FString Text;
			FString Url;
		};

		bool IsWordChar(const TCHAR C)
		{
			return FChar::IsAlnum(C) || C == TEXT('_');
		}

		/** Inline markdown as runs. Styles do not nest: what is inside bold or italic is plain. */
		TArray<FInline> Tokenize(const FString& Inline)
		{
			TArray<FInline> Runs;
			FString Plain;
			auto Flush = [&Runs, &Plain]()
			{
				if (!Plain.IsEmpty())
				{
					Runs.Add({EInline::Text, Plain, FString()});
					Plain.Reset();
				}
			};
			auto Add = [&Runs, &Flush](const EInline Kind, const FString& Text, const FString& Url)
			{
				Flush();
				Runs.Add({Kind, Text, Url});
			};
			const int32 Length = Inline.Len();
			for (int32 Index = 0; Index < Length;)
			{
				const TCHAR C = Inline[Index];
				const TCHAR Next = Index + 1 < Length ? Inline[Index + 1] : TEXT('\0');
				// An escaped mark stands for itself.
				if (C == TEXT('\\') && Next != TEXT('\0') && FChar::IsPunct(Next))
				{
					Plain.AppendChar(Next);
					Index += 2;
					continue;
				}
				if (C == TEXT('`'))
				{
					const int32 End = Inline.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1);
					if (End > Index)
					{
						Add(EInline::Code, Inline.Mid(Index + 1, End - Index - 1), FString());
						Index = End + 1;
						continue;
					}
				}
				if ((C == TEXT('*') || C == TEXT('_')) && Next == C)
				{
					const int32 End = Inline.Find(FString::ChrN(2, C), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 2);
					if (End > Index + 2)
					{
						Add(EInline::Bold, Inline.Mid(Index + 2, End - Index - 2), FString());
						Index = End + 2;
						continue;
					}
				}
				// Italic opens before a word and closes after one; the underscore inside a name such as IA_Move is not italic.
				if ((C == TEXT('*') || C == TEXT('_')) && Next != TEXT('\0') && !FChar::IsWhitespace(Next) && Next != C
					&& (C == TEXT('*') || Index == 0 || !IsWordChar(Inline[Index - 1])))
				{
					int32 End = Index + 1;
					for (; End < Length; ++End)
					{
						if (Inline[End] == C && !FChar::IsWhitespace(Inline[End - 1])
							&& (C == TEXT('*') || End + 1 >= Length || !IsWordChar(Inline[End + 1])))
						{
							break;
						}
					}
					if (End < Length && End > Index + 1)
					{
						Add(EInline::Italic, Inline.Mid(Index + 1, End - Index - 1), FString());
						Index = End + 1;
						continue;
					}
				}
				if (C == TEXT('['))
				{
					const int32 Close = Inline.Find(TEXT("]("), ESearchCase::CaseSensitive, ESearchDir::FromStart, Index + 1);
					const int32 End = Close > Index ? Inline.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Close + 2) : INDEX_NONE;
					if (Close > Index && End > Close)
					{
						Add(EInline::Link, Inline.Mid(Index + 1, Close - Index - 1), Inline.Mid(Close + 2, End - Close - 2).TrimStartAndEnd());
						Index = End + 1;
						continue;
					}
				}
				// A bare web address is a link too; a sentence's full stop after it is not part of it.
				if (C == TEXT('h') && (Inline.Mid(Index, 8).Equals(TEXT("https://")) || Inline.Mid(Index, 7).Equals(TEXT("http://"))))
				{
					int32 End = Index;
					while (End < Length && !FChar::IsWhitespace(Inline[End]) && Inline[End] != TEXT(')') && Inline[End] != TEXT('>'))
					{
						++End;
					}
					while (End > Index && (Inline[End - 1] == TEXT('.') || Inline[End - 1] == TEXT(',')))
					{
						--End;
					}
					const FString Url = Inline.Mid(Index, End - Index);
					Add(EInline::Link, Url, Url);
					Index = End;
					continue;
				}
				Plain.AppendChar(C);
				++Index;
			}
			Flush();
			return Runs;
		}

		FString EscapeRich(const FString& Text)
		{
			return Text.Replace(TEXT("&"), TEXT("&amp;")).Replace(TEXT("<"), TEXT("&lt;")).Replace(TEXT(">"), TEXT("&gt;")).Replace(TEXT("\""), TEXT("&quot;"));
		}

		FString EscapeHtml(const FString& Text)
		{
			return Text.Replace(TEXT("&"), TEXT("&amp;")).Replace(TEXT("<"), TEXT("&lt;")).Replace(TEXT(">"), TEXT("&gt;")).Replace(TEXT("\""), TEXT("&quot;"));
		}

		bool IsFence(const FString& Line, FString* OutLanguage = nullptr)
		{
			const FString Trimmed = Line.TrimStart();
			if (!Trimmed.StartsWith(TEXT("```")) && !Trimmed.StartsWith(TEXT("~~~")))
			{
				return false;
			}
			if (OutLanguage)
			{
				*OutLanguage = Trimmed.Mid(3).TrimStartAndEnd();
			}
			return true;
		}

		bool IsRule(const FString& Trimmed)
		{
			if (Trimmed.Len() < 3 || (Trimmed[0] != TEXT('-') && Trimmed[0] != TEXT('*') && Trimmed[0] != TEXT('_')))
			{
				return false;
			}
			for (const TCHAR C : Trimmed)
			{
				if (C != Trimmed[0] && C != TEXT(' '))
				{
					return false;
				}
			}
			return true;
		}

		bool IsTableRow(const FString& Line)
		{
			return Line.TrimStart().StartsWith(TEXT("|"));
		}

		TArray<FString> SplitRow(const FString& Line)
		{
			FString Row = Line.TrimStartAndEnd();
			Row.RemoveFromStart(TEXT("|"));
			Row.RemoveFromEnd(TEXT("|"));
			TArray<FString> Cells;
			FString Cell;
			for (int32 Index = 0; Index < Row.Len(); ++Index)
			{
				// A pipe inside code, or escaped, belongs to the cell.
				if (Row[Index] == TEXT('\\') && Index + 1 < Row.Len() && Row[Index + 1] == TEXT('|'))
				{
					Cell.AppendChar(TEXT('|'));
					++Index;
					continue;
				}
				if (Row[Index] == TEXT('|'))
				{
					Cells.Add(Cell.TrimStartAndEnd());
					Cell.Reset();
					continue;
				}
				Cell.AppendChar(Row[Index]);
			}
			Cells.Add(Cell.TrimStartAndEnd());
			return Cells;
		}

		bool IsSeparatorRow(const FString& Line)
		{
			if (!IsTableRow(Line))
			{
				return false;
			}
			for (const FString& Cell : SplitRow(Line))
			{
				FString Rest = Cell.Replace(TEXT(":"), TEXT("")).Replace(TEXT(" "), TEXT(""));
				if (Rest.IsEmpty() || Rest.Replace(TEXT("-"), TEXT("")).Len() > 0)
				{
					return false;
				}
			}
			return true;
		}

		/** A whole-line picture: ![caption](path). */
		bool IsImageLine(const FString& Trimmed, FString& OutCaption, FString& OutPath)
		{
			if (!Trimmed.StartsWith(TEXT("![")) || !Trimmed.EndsWith(TEXT(")")))
			{
				return false;
			}
			const int32 Close = Trimmed.Find(TEXT("]("));
			if (Close < 2)
			{
				return false;
			}
			OutCaption = Trimmed.Mid(2, Close - 2);
			OutPath = Trimmed.Mid(Close + 2, Trimmed.Len() - Close - 3).TrimStartAndEnd();
			return !OutPath.IsEmpty();
		}

		int32 Indent(const FString& Line)
		{
			int32 Spaces = 0;
			for (const TCHAR C : Line)
			{
				if (C == TEXT(' '))
				{
					++Spaces;
				}
				else if (C == TEXT('\t'))
				{
					Spaces += 4;
				}
				else
				{
					break;
				}
			}
			return Spaces;
		}

		/** A list marker: "- ", "* ", "+ " or "12. " / "12) ". */
		bool IsListItem(const FString& Trimmed, bool& bOutNumbered, int32& OutNumber, FString& OutText)
		{
			if (Trimmed.Len() >= 2 && (Trimmed[0] == TEXT('-') || Trimmed[0] == TEXT('*') || Trimmed[0] == TEXT('+')) && Trimmed[1] == TEXT(' '))
			{
				bOutNumbered = false;
				OutText = Trimmed.Mid(2).TrimStart();
				return true;
			}
			int32 Digits = 0;
			while (Digits < Trimmed.Len() && Digits < 4 && FChar::IsDigit(Trimmed[Digits]))
			{
				++Digits;
			}
			if (Digits > 0 && Digits + 1 < Trimmed.Len() && (Trimmed[Digits] == TEXT('.') || Trimmed[Digits] == TEXT(')')) && Trimmed[Digits + 1] == TEXT(' '))
			{
				bOutNumbered = true;
				OutNumber = FCString::Atoi(*Trimmed.Left(Digits));
				OutText = Trimmed.Mid(Digits + 2).TrimStart();
				return true;
			}
			return false;
		}

		FString InlineHtml(const FString& Inline)
		{
			FString Out;
			for (const FInline& Run : Tokenize(Inline))
			{
				switch (Run.Kind)
				{
				case EInline::Code:
					Out += TEXT("<code>") + EscapeHtml(Run.Text) + TEXT("</code>");
					break;
				case EInline::Bold:
					Out += TEXT("<b>") + EscapeHtml(Run.Text) + TEXT("</b>");
					break;
				case EInline::Italic:
					Out += TEXT("<i>") + EscapeHtml(Run.Text) + TEXT("</i>");
					break;
				case EInline::Link:
					Out += FString::Printf(TEXT("<a href=\"%s\">%s</a>"), *EscapeHtml(Run.Url), *EscapeHtml(Run.Text));
					break;
				default:
					Out += EscapeHtml(Run.Text);
					break;
				}
			}
			return Out;
		}

		/** A picture on disk, embedded, so the page opens anywhere; anything else stays as written. */
		FString ImageSource(const FString& Path)
		{
			FString Full = FPaths::IsRelative(Path) ? FPaths::Combine(FPaths::ProjectDir(), Path) : Path;
			Full = FPaths::ConvertRelativePathToFull(Full);
			const int64 Size = IFileManager::Get().FileSize(*Full);
			TArray<uint8> Bytes;
			if (Size > 0 && Size < 16ll * 1024 * 1024 && FFileHelper::LoadFileToArray(Bytes, *Full))
			{
				const FString Extension = FPaths::GetExtension(Full).ToLower();
				const TCHAR* Mime = Extension == TEXT("jpg") || Extension == TEXT("jpeg") ? TEXT("image/jpeg")
					: Extension == TEXT("webp") ? TEXT("image/webp") : TEXT("image/png");
				return FString::Printf(TEXT("data:%s;base64,%s"), Mime, *FBase64::Encode(Bytes));
			}
			return Path;
		}

		/** A link or picture path as a file on disk, relative ones from the project; empty when it is not one. */
		FString LocalFile(const FString& Target)
		{
			if (Target.StartsWith(TEXT("http://")) || Target.StartsWith(TEXT("https://")) || Target.IsEmpty())
			{
				return FString();
			}
			FString Path = Target;
			Path.RemoveFromStart(TEXT("file:///"));
			const FString Full = FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::Combine(FPaths::ProjectDir(), Path) : Path);
			return FPaths::FileExists(Full) || FPaths::DirectoryExists(Full) ? Full : FString();
		}

		bool IsPicture(const FString& Path)
		{
			const FString Extension = FPaths::GetExtension(Path).ToLower();
			return Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg");
		}

		void OpenLink(const FSlateHyperlinkRun::FMetadata& Metadata)
		{
			const FString* Href = Metadata.Find(TEXT("href"));
			if (!Href)
			{
				return;
			}
			const FString Target = Href->Replace(TEXT("&amp;"), TEXT("&"));
			if (Target.StartsWith(TEXT("http://")) || Target.StartsWith(TEXT("https://")))
			{
				FPlatformProcess::LaunchURL(*Target, nullptr, nullptr);
				return;
			}
			const FString File = LocalFile(Target);
			if (File.IsEmpty())
			{
				return;
			}
			if (IsPicture(File))
			{
				FHyperAIStudioPreviewPanel::Open(NAME_None, File);
			}
			else
			{
				FPlatformProcess::ExploreFolder(*File);
			}
		}

		TSharedRef<SWidget> Rich(const FString& Inline, const FName Style)
		{
			return SNew(SRichTextBlock)
				.Text(FText::FromString(ToRichText(Inline)))
				.TextStyle(&FHyperAIStudioStyle::Get(), Style)
				.DecoratorStyleSet(&FHyperAIStudioStyle::Get())
				.AutoWrapText(true)
				+ SRichTextBlock::HyperlinkDecorator(TEXT("md"), FSlateHyperlinkRun::FOnClick::CreateStatic(&OpenLink));
		}

		FSlateColor StatusColor(const FString& Status)
		{
			return Status == TEXT("pass") ? FStyleColors::Success
				: Status == TEXT("fail") ? FStyleColors::Error
				: Status == TEXT("warn") ? FStyleColors::Warning : FStyleColors::AccentBlue;
		}

		/** A status as a small coloured pill, the way results tables mark a row. */
		TSharedRef<SWidget> Chip(const FString& Status, const FString& Word)
		{
			const FLinearColor Color = StatusColor(Status).GetSpecifiedColor();
			return SNew(SBorder)
				.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.Chip"))
				.BorderBackgroundColor(Color.CopyWithNewOpacity(0.18f))
				.Padding(FMargin(7.0f, 1.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(Word.ToUpper()))
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 8))
					.ColorAndOpacity(Color)
				];
		}
	}

	TArray<FBlock> Parse(const FString& Markdown)
	{
		TArray<FString> Lines;
		Markdown.Replace(TEXT("\r"), TEXT("")).ParseIntoArray(Lines, TEXT("\n"), /*InCullEmpty=*/false);
		TArray<FBlock> Blocks;
		FString Paragraph;
		auto FlushParagraph = [&Blocks, &Paragraph]()
		{
			if (!Paragraph.IsEmpty())
			{
				FBlock& Block = Blocks.AddDefaulted_GetRef();
				Block.Kind = EBlock::Paragraph;
				Block.Text = Paragraph;
				Paragraph.Reset();
			}
		};
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FString& Line = Lines[Index];
			FString Label;
			if (IsFence(Line, &Label))
			{
				FlushParagraph();
				FBlock& Code = Blocks.AddDefaulted_GetRef();
				Code.Kind = EBlock::Code;
				Code.Label = Label;
				TArray<FString> Body;
				for (++Index; Index < Lines.Num() && !IsFence(Lines[Index]); ++Index)
				{
					Body.Add(Lines[Index]);
				}
				Code.Text = FString::Join(Body, TEXT("\n"));
				continue;
			}
			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				FlushParagraph();
				continue;
			}
			if (IsTableRow(Line) && Index + 1 < Lines.Num() && IsSeparatorRow(Lines[Index + 1]))
			{
				FlushParagraph();
				FBlock& Table = Blocks.AddDefaulted_GetRef();
				Table.Kind = EBlock::Table;
				Table.Rows.Add(SplitRow(Line));
				for (Index += 2; Index < Lines.Num() && IsTableRow(Lines[Index]); ++Index)
				{
					Table.Rows.Add(SplitRow(Lines[Index]));
				}
				--Index;
				continue;
			}
			int32 Hashes = 0;
			while (Hashes < Trimmed.Len() && Hashes < 7 && Trimmed[Hashes] == TEXT('#'))
			{
				++Hashes;
			}
			if (Hashes >= 1 && Hashes <= 6 && Hashes < Trimmed.Len() && Trimmed[Hashes] == TEXT(' '))
			{
				FlushParagraph();
				FBlock& Heading = Blocks.AddDefaulted_GetRef();
				Heading.Kind = EBlock::Heading;
				Heading.Level = FMath::Min(Hashes, 3);
				Heading.Text = Trimmed.Mid(Hashes + 1).TrimStartAndEnd();
				continue;
			}
			if (IsRule(Trimmed))
			{
				FlushParagraph();
				Blocks.AddDefaulted_GetRef().Kind = EBlock::Rule;
				continue;
			}
			FString Caption;
			FString Path;
			if (IsImageLine(Trimmed, Caption, Path))
			{
				FlushParagraph();
				FBlock& Image = Blocks.AddDefaulted_GetRef();
				Image.Kind = EBlock::Image;
				Image.Text = Path;
				Image.Label = Caption;
				continue;
			}
			if (Trimmed.StartsWith(TEXT(">")))
			{
				FlushParagraph();
				FString Quote = Trimmed.Mid(1).TrimStart();
				while (Index + 1 < Lines.Num() && Lines[Index + 1].TrimStart().StartsWith(TEXT(">")))
				{
					Quote += TEXT(" ") + Lines[++Index].TrimStart().Mid(1).TrimStart();
				}
				FBlock& Block = Blocks.AddDefaulted_GetRef();
				Block.Kind = EBlock::Quote;
				Block.Text = Quote.TrimEnd();
				continue;
			}
			bool bNumbered = false;
			int32 Number = 0;
			FString ItemText;
			if (IsListItem(Trimmed, bNumbered, Number, ItemText))
			{
				FlushParagraph();
				const int32 Depth = FMath::Clamp(Indent(Line) / 2, 0, 2);
				// A line indented under an item, and not an item itself, continues it.
				while (Index + 1 < Lines.Num() && Indent(Lines[Index + 1]) > Indent(Line) && !Lines[Index + 1].TrimStartAndEnd().IsEmpty())
				{
					bool bNextNumbered = false;
					int32 NextNumber = 0;
					FString NextText;
					if (IsListItem(Lines[Index + 1].TrimStartAndEnd(), bNextNumbered, NextNumber, NextText) || IsFence(Lines[Index + 1]))
					{
						break;
					}
					ItemText += TEXT(" ") + Lines[++Index].TrimStartAndEnd();
				}
				FBlock& Item = Blocks.AddDefaulted_GetRef();
				Item.Kind = bNumbered ? EBlock::Numbered : EBlock::Bullet;
				Item.Level = Depth;
				Item.Number = Number;
				Item.Text = ItemText;
				continue;
			}
			Paragraph += (Paragraph.IsEmpty() ? FString() : FString(TEXT(" "))) + Trimmed;
		}
		FlushParagraph();
		return Blocks;
	}

	FString ToRichText(const FString& Inline)
	{
		FString Out;
		for (const FInline& Run : Tokenize(Inline))
		{
			switch (Run.Kind)
			{
			case EInline::Code:
				Out += TEXT("<Md.Code>") + EscapeRich(Run.Text) + TEXT("</>");
				break;
			case EInline::Bold:
				Out += TEXT("<Md.Bold>") + EscapeRich(Run.Text) + TEXT("</>");
				break;
			case EInline::Italic:
				Out += TEXT("<Md.Italic>") + EscapeRich(Run.Text) + TEXT("</>");
				break;
			case EInline::Link:
				Out += FString::Printf(TEXT("<a id=\"md\" href=\"%s\" style=\"Md.Link\">%s</>"), *EscapeRich(Run.Url), *EscapeRich(Run.Text));
				break;
			default:
				Out += EscapeRich(Run.Text);
				break;
			}
		}
		return Out;
	}

	FString CellStatus(const FString& Cell, FString& OutRest)
	{
		struct FWord
		{
			const TCHAR* Word;
			const TCHAR* Status;
		};
		static const FWord Words[] = {
			{TEXT("PASSED"), TEXT("pass")}, {TEXT("PASS"), TEXT("pass")}, {TEXT("OK"), TEXT("pass")}, {TEXT("✅"), TEXT("pass")},
			{TEXT("FAILED"), TEXT("fail")}, {TEXT("FAIL"), TEXT("fail")}, {TEXT("ERROR"), TEXT("fail")}, {TEXT("❌"), TEXT("fail")},
			{TEXT("WARNING"), TEXT("warn")}, {TEXT("WARN"), TEXT("warn")}, {TEXT("TIMED_OUT"), TEXT("warn")}, {TEXT("⚠️"), TEXT("warn")},
			{TEXT("⚠"), TEXT("warn")}, {TEXT("SKIPPED"), TEXT("info")}, {TEXT("INCONCLUSIVE"), TEXT("info")}};
		const FString Trimmed = Cell.TrimStartAndEnd();
		for (const FWord& Word : Words)
		{
			const int32 Length = FCString::Strlen(Word.Word);
			if (Trimmed.StartsWith(Word.Word, ESearchCase::IgnoreCase) && (Trimmed.Len() == Length || !FChar::IsAlnum(Trimmed[Length])))
			{
				OutRest = Trimmed.Mid(Length).TrimStart();
				OutRest.RemoveFromStart(TEXT(":"));
				OutRest.RemoveFromStart(TEXT("-"));
				OutRest = OutRest.TrimStart();
				return Word.Status;
			}
		}
		// Words a status field holds on its own, such as a tool's "completed". Only a whole cell counts, so a sentence
		// that starts with "Success" stays a sentence.
		static const FWord Whole[] = {
			{TEXT("COMPLETED"), TEXT("pass")}, {TEXT("SUCCEEDED"), TEXT("pass")}, {TEXT("SUCCESS"), TEXT("pass")}, {TEXT("DONE"), TEXT("pass")},
			{TEXT("FAILURE"), TEXT("fail")}, {TEXT("DECLINED"), TEXT("fail")}, {TEXT("CANCELLED"), TEXT("warn")}, {TEXT("CANCELED"), TEXT("warn")},
			{TEXT("RUNNING"), TEXT("info")}, {TEXT("QUEUED"), TEXT("info")}, {TEXT("PENDING"), TEXT("info")}};
		for (const FWord& Word : Whole)
		{
			if (Trimmed.Equals(Word.Word, ESearchCase::IgnoreCase))
			{
				OutRest.Reset();
				return Word.Status;
			}
		}
		OutRest = Trimmed;
		return FString();
	}

	namespace
	{
		/** A JSON key as words in sentence case: bestScore and best_score read "Best score"; a bool's b goes. */
		FString Humanize(const FString& Key)
		{
			FString Name = Key;
			if (Name.Len() > 1 && Name[0] == TEXT('b') && FChar::IsUpper(Name[1]))
			{
				Name.RightChopInline(1);
			}
			FString Words;
			for (int32 Index = 0; Index < Name.Len(); ++Index)
			{
				const TCHAR C = Name[Index];
				if (C == TEXT('_') || C == TEXT('-') || C == TEXT('.'))
				{
					Words.AppendChar(TEXT(' '));
					continue;
				}
				const bool bPreviousUpper = Index > 0 && FChar::IsUpper(Name[Index - 1]);
				const bool bNextLower = Index + 1 < Name.Len() && FChar::IsLower(Name[Index + 1]);
				const bool bStartsWord = Index > 0 && FChar::IsUpper(C)
					&& (FChar::IsLower(Name[Index - 1]) || FChar::IsDigit(Name[Index - 1]) || (bPreviousUpper && bNextLower));
				if (bStartsWord)
				{
					Words.AppendChar(TEXT(' '));
				}
				// Capitals in a run, such as MCP, stay; one that only starts a word is lowered.
				const bool bInRun = (Index + 1 < Name.Len() && FChar::IsUpper(Name[Index + 1])) || (bPreviousUpper && !bStartsWord);
				Words.AppendChar(FChar::IsUpper(C) && !bInRun ? FChar::ToLower(C) : C);
			}
			TArray<FString> Parts;
			Words.ParseIntoArray(Parts, TEXT(" "));
			for (FString& Part : Parts)
			{
				if (Part == TEXT("id") || Part == TEXT("url") || Part == TEXT("ui") || Part == TEXT("uri"))
				{
					Part = Part.ToUpper();
				}
			}
			FString Out = FString::Join(Parts, TEXT(" "));
			if (!Out.IsEmpty())
			{
				Out[0] = FChar::ToUpper(Out[0]);
			}
			return Out;
		}

		/** Marks escaped, so a name or path in a value shows as written; a value stays on its table row. */
		FString EscapeMarkdown(const FString& Text)
		{
			FString Out;
			Out.Reserve(Text.Len());
			for (const TCHAR C : Text)
			{
				if (C == TEXT('\\') || C == TEXT('`') || C == TEXT('*') || C == TEXT('_') || C == TEXT('[') || C == TEXT(']') || C == TEXT('|'))
				{
					Out.AppendChar(TEXT('\\'));
				}
				Out.AppendChar(C == TEXT('\n') || C == TEXT('\r') || C == TEXT('\t') ? TEXT(' ') : C);
			}
			return Out;
		}

		FString NumberText(const double Value)
		{
			if (FMath::Abs(Value) < 1e15 && FMath::IsNearlyEqual(Value, FMath::RoundToDouble(Value)))
			{
				return FString::Printf(TEXT("%lld"), static_cast<long long>(FMath::RoundToDouble(Value)));
			}
			FString Text = FString::Printf(TEXT("%.3f"), Value);
			while (Text.EndsWith(TEXT("0")))
			{
				Text.LeftChopInline(1);
			}
			Text.RemoveFromEnd(TEXT("."));
			return Text;
		}

		bool IsScalar(const TSharedPtr<FJsonValue>& Value)
		{
			return Value.IsValid() && Value->Type != EJson::Object && Value->Type != EJson::Array;
		}

		FString CompactJson(const TSharedPtr<FJsonValue>& Value)
		{
			FString Out;
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Value, FString(), Writer);
			return Out;
		}

		/** A value as a table cell; a picture on disk becomes a link that opens it, and is kept to show below. */
		FString CellText(const TSharedPtr<FJsonValue>& Value, TArray<FString>& Pictures)
		{
			if (!Value.IsValid() || Value->Type == EJson::Null || Value->Type == EJson::None)
			{
				return LOCTEXT("JsonNone", "none").ToString();
			}
			if (Value->Type == EJson::Boolean)
			{
				return Value->AsBool() ? LOCTEXT("JsonYes", "yes").ToString() : LOCTEXT("JsonNo", "no").ToString();
			}
			if (Value->Type == EJson::Number)
			{
				return NumberText(Value->AsNumber());
			}
			if (Value->Type == EJson::String)
			{
				const FString Text = Value->AsString();
				const FString File = IsPicture(Text) ? LocalFile(Text).Replace(TEXT("\\"), TEXT("/")) : FString();
				if (!File.IsEmpty())
				{
					Pictures.AddUnique(File);
					return FString::Printf(TEXT("[%s](%s)"), *EscapeMarkdown(FPaths::GetCleanFilename(File)), *File);
				}
				return EscapeMarkdown(Text.Len() > 300 ? Text.Left(300) + TEXT("...") : Text);
			}
			const FString Json = CompactJson(Value);
			return TEXT("`") + (Json.Len() > 200 ? Json.Left(200) + TEXT("...") : Json).Replace(TEXT("`"), TEXT("'")).Replace(TEXT("|"), TEXT("/")) + TEXT("`");
		}

		void AppendTable(FString& Out, const TArray<FString>& Head, const TArray<TArray<FString>>& Rows)
		{
			Out += TEXT("| ") + FString::Join(Head, TEXT(" | ")) + TEXT(" |\n|");
			for (int32 Index = 0; Index < Head.Num(); ++Index)
			{
				Out += TEXT("---|");
			}
			Out += TEXT("\n");
			for (const TArray<FString>& Row : Rows)
			{
				Out += TEXT("| ") + FString::Join(Row, TEXT(" | ")) + TEXT(" |\n");
			}
			Out += TEXT("\n");
		}

		/** A list of records as one table: a column for each simple field the first records have, six at most. */
		void AppendRecords(FString& Out, const TArray<TSharedPtr<FJsonValue>>& Items, TArray<FString>& Pictures)
		{
			constexpr int32 MaxRows = 40;
			TArray<FString> Columns;
			for (int32 Index = 0; Index < FMath::Min(Items.Num(), 20); ++Index)
			{
				if (Items[Index].IsValid() && Items[Index]->Type == EJson::Object)
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Items[Index]->AsObject()->Values)
					{
						if (IsScalar(Pair.Value) && (Columns.Num() < 6 || Columns.Contains(Pair.Key)))
						{
							Columns.AddUnique(Pair.Key);
						}
					}
				}
			}
			TArray<FString> Head;
			for (const FString& Column : Columns)
			{
				Head.Add(Humanize(Column));
			}
			if (Head.IsEmpty())
			{
				Head.Add(LOCTEXT("JsonValue", "Value").ToString());
			}
			TArray<TArray<FString>> Rows;
			for (int32 Index = 0; Index < FMath::Min(Items.Num(), MaxRows); ++Index)
			{
				TArray<FString>& Row = Rows.AddDefaulted_GetRef();
				const TSharedPtr<FJsonObject> Object = Items[Index].IsValid() && Items[Index]->Type == EJson::Object ? Items[Index]->AsObject() : nullptr;
				if (!Object.IsValid() || Columns.IsEmpty())
				{
					Row.Add(CellText(Items[Index], Pictures));
					continue;
				}
				for (const FString& Column : Columns)
				{
					const TSharedPtr<FJsonValue> Field = Object->TryGetField(Column);
					Row.Add(Field.IsValid() ? CellText(Field, Pictures) : FString());
				}
				// A picture a record names in a field without a column is still shown.
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
				{
					if (!Columns.Contains(Pair.Key) && Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
					{
						CellText(Pair.Value, Pictures);
					}
				}
			}
			AppendTable(Out, Head, Rows);
			if (Items.Num() > MaxRows)
			{
				Out += FText::Format(LOCTEXT("JsonMore", "{0} more not shown."), Items.Num() - MaxRows).ToString() + TEXT("\n\n");
			}
		}

		void AppendObject(FString& Out, const TSharedPtr<FJsonObject>& Object, const int32 Depth, TArray<FString>& Pictures, const bool bLead)
		{
			// What happened, in the tool's own words, comes first.
			FString LeadKey;
			if (bLead)
			{
				for (const TCHAR* Key : {TEXT("summary"), TEXT("message"), TEXT("diagnostic"), TEXT("detail"), TEXT("description"), TEXT("error")})
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
					{
						if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase) && Pair.Value.IsValid() && Pair.Value->Type == EJson::String
							&& !Pair.Value->AsString().TrimStartAndEnd().IsEmpty())
						{
							LeadKey = Pair.Key;
							Out += EscapeMarkdown(Pair.Value->AsString().TrimStartAndEnd()) + TEXT("\n\n");
							break;
						}
					}
					if (!LeadKey.IsEmpty())
					{
						break;
					}
				}
			}
			TArray<TArray<FString>> Rows;
			TArray<TPair<FString, TSharedPtr<FJsonValue>>> Nested;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
			{
				if (Pair.Key == LeadKey || !Pair.Value.IsValid())
				{
					continue;
				}
				if (IsScalar(Pair.Value))
				{
					Rows.Add({Humanize(Pair.Key), CellText(Pair.Value, Pictures)});
					continue;
				}
				if (Pair.Value->Type == EJson::Array)
				{
					const TArray<TSharedPtr<FJsonValue>>& Items = Pair.Value->AsArray();
					if (Items.IsEmpty())
					{
						Rows.Add({Humanize(Pair.Key), LOCTEXT("JsonNone", "none").ToString()});
						continue;
					}
					if (Items.ContainsByPredicate([](const TSharedPtr<FJsonValue>& Item) { return !IsScalar(Item); }))
					{
						Nested.Add(Pair);
						continue;
					}
					TArray<FString> Values;
					for (int32 Index = 0; Index < FMath::Min(Items.Num(), 12); ++Index)
					{
						Values.Add(CellText(Items[Index], Pictures));
					}
					if (Items.Num() > 12)
					{
						Values.Add(FText::Format(LOCTEXT("JsonAndMore", "and {0} more"), Items.Num() - 12).ToString());
					}
					Rows.Add({Humanize(Pair.Key), FString::Join(Values, TEXT(", "))});
					continue;
				}
				Nested.Add(Pair);
			}
			if (!Rows.IsEmpty())
			{
				AppendTable(Out, {LOCTEXT("JsonField", "Field").ToString(), LOCTEXT("JsonValue", "Value").ToString()}, Rows);
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Nested)
			{
				const FString Heading = FString::ChrN(FMath::Min(Depth + 2, 3), TEXT('#')) + TEXT(" ") + Humanize(Pair.Key);
				if (Pair.Value->Type == EJson::Array)
				{
					Out += FString::Printf(TEXT("%s (%d)\n\n"), *Heading, Pair.Value->AsArray().Num());
					AppendRecords(Out, Pair.Value->AsArray(), Pictures);
				}
				else if (Depth < 2)
				{
					Out += Heading + TEXT("\n\n");
					AppendObject(Out, Pair.Value->AsObject(), Depth + 1, Pictures, false);
				}
				else
				{
					Out += Heading + TEXT("\n\n```json\n") + CompactJson(Pair.Value) + TEXT("\n```\n\n");
				}
			}
		}
	}

	FString FromJson(const FString& Json)
	{
		const FString Trimmed = Json.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("{")) && !Trimmed.StartsWith(TEXT("[")))
		{
			return FString();
		}
		TSharedPtr<FJsonValue> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Trimmed), Root) || !Root.IsValid())
		{
			return FString();
		}
		// Epic's registry wraps a tool's report as {"returnValue": ...}.
		if (Root->Type == EJson::Object && Root->AsObject()->Values.Num() == 1 && Root->AsObject()->HasField(TEXT("returnValue")))
		{
			Root = Root->AsObject()->TryGetField(TEXT("returnValue"));
		}
		FString Out;
		TArray<FString> Pictures;
		if (!Root.IsValid())
		{
			return FString();
		}
		if (Root->Type == EJson::Object)
		{
			AppendObject(Out, Root->AsObject(), 0, Pictures, /*bLead=*/true);
		}
		else if (Root->Type == EJson::Array)
		{
			AppendRecords(Out, Root->AsArray(), Pictures);
		}
		else
		{
			Out = CellText(Root, Pictures) + TEXT("\n\n");
		}
		for (int32 Index = 0; Index < FMath::Min(Pictures.Num(), 8); ++Index)
		{
			Out += FString::Printf(TEXT("![%s](%s)\n"), *FPaths::GetCleanFilename(Pictures[Index]), *Pictures[Index]);
		}
		return Out.TrimEnd();
	}

	FString ExportReport(const FString& Title, const FString& Markdown)
	{
		const FString File = SaveReport(Title, Markdown);
		FNotificationInfo Info(File.IsEmpty() ? LOCTEXT("ReportFailed", "The report could not be saved.")
			: FText::Format(LOCTEXT("ReportSaved", "Saved the report as {0}"), FText::FromString(FPaths::GetCleanFilename(File))));
		Info.ExpireDuration = 6.0f;
		if (!File.IsEmpty())
		{
			Info.Hyperlink = FSimpleDelegate::CreateLambda([File]() { FPlatformProcess::LaunchFileInDefaultExternalApplication(*File); });
			Info.HyperlinkText = LOCTEXT("OpenReport", "Open it");
		}
		if (FSlateApplication::IsInitialized())
		{
			FSlateNotificationManager::Get().AddNotification(Info);
		}
		return File;
	}

	TSharedRef<SWidget> ReportActions(TFunction<FString()> Title, TFunction<FString()> Markdown)
	{
		using namespace HyperAIStudio::UI;
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				IconButton(FAppStyle::GetBrush("Icons.Clipboard"), LOCTEXT("CopyReport", "Copy as Markdown"),
					FOnClicked::CreateLambda([Markdown]()
					{
						FPlatformApplicationMisc::ClipboardCopy(*Markdown());
						return FReply::Handled();
					}))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(2.0f, 0.0f, 0.0f, 0.0f))
			[
				IconButton(Icon("HyperAIStudio.Icon.Report"), LOCTEXT("SaveReport", "Save as an HTML report, to share or keep."),
					FOnClicked::CreateLambda([Title, Markdown]()
					{
						ExportReport(Title(), Markdown());
						return FReply::Handled();
					}))
			];
	}

	FString ToHtml(const FString& Markdown)
	{
		const TArray<FBlock> Blocks = Parse(Markdown);
		FString Html;
		for (int32 Index = 0; Index < Blocks.Num(); ++Index)
		{
			const FBlock& Block = Blocks[Index];
			const bool bList = Block.Kind == EBlock::Bullet || Block.Kind == EBlock::Numbered;
			const bool bOpens = bList && (Index == 0 || Blocks[Index - 1].Kind != Block.Kind);
			const bool bCloses = bList && (Index + 1 == Blocks.Num() || Blocks[Index + 1].Kind != Block.Kind);
			switch (Block.Kind)
			{
			case EBlock::Heading:
				Html += FString::Printf(TEXT("<h%d>%s</h%d>\n"), Block.Level, *InlineHtml(Block.Text), Block.Level);
				break;
			case EBlock::Paragraph:
				Html += TEXT("<p>") + InlineHtml(Block.Text) + TEXT("</p>\n");
				break;
			case EBlock::Bullet:
			case EBlock::Numbered:
				if (bOpens)
				{
					Html += Block.Kind == EBlock::Bullet ? FString(TEXT("<ul>\n")) : FString::Printf(TEXT("<ol start=\"%d\">\n"), FMath::Max(1, Block.Number));
				}
				Html += FString::Printf(TEXT("<li style=\"margin-left:%dpx\">%s</li>\n"), Block.Level * 18, *InlineHtml(Block.Text));
				if (bCloses)
				{
					Html += Block.Kind == EBlock::Bullet ? TEXT("</ul>\n") : TEXT("</ol>\n");
				}
				break;
			case EBlock::Quote:
				Html += TEXT("<blockquote>") + InlineHtml(Block.Text) + TEXT("</blockquote>\n");
				break;
			case EBlock::Code:
				Html += TEXT("<pre><code>") + EscapeHtml(Block.Text) + TEXT("</code></pre>\n");
				break;
			case EBlock::Table:
			{
				Html += TEXT("<table>\n");
				for (int32 Row = 0; Row < Block.Rows.Num(); ++Row)
				{
					Html += TEXT("<tr>");
					for (int32 Column = 0; Column < Block.Rows[Row].Num(); ++Column)
					{
						const FString& Cell = Block.Rows[Row][Column];
						if (Row == 0)
						{
							Html += TEXT("<th>") + InlineHtml(Cell) + TEXT("</th>");
							continue;
						}
						// The first column names the row; a status is in the columns after it.
						FString Rest;
						const FString Status = Column == 0 ? FString() : CellStatus(Cell, Rest);
						Html += Status.IsEmpty() ? TEXT("<td>") + InlineHtml(Cell) + TEXT("</td>")
							: FString::Printf(TEXT("<td><span class=\"chip %s\">%s</span> %s</td>"), *Status,
								*EscapeHtml(Cell.TrimStartAndEnd().Left(Cell.TrimStartAndEnd().Len() - Rest.Len()).TrimEnd().ToUpper()), *InlineHtml(Rest));
					}
					Html += TEXT("</tr>\n");
				}
				Html += TEXT("</table>\n");
				break;
			}
			case EBlock::Rule:
				Html += TEXT("<hr>\n");
				break;
			case EBlock::Image:
				Html += FString::Printf(TEXT("<figure><img src=\"%s\" alt=\"%s\"><figcaption>%s</figcaption></figure>\n"),
					*ImageSource(Block.Text), *EscapeHtml(Block.Label), *EscapeHtml(Block.Label.IsEmpty() ? FPaths::GetCleanFilename(Block.Text) : Block.Label));
				break;
			}
		}
		return Html;
	}

	FString HtmlPage(const FString& Title, const FString& BodyHtml)
	{
		static const TCHAR* Css = TEXT(
			"body{font:14px 'Segoe UI',Roboto,system-ui,sans-serif;margin:0 auto;padding:28px 32px;background:#1a1a1a;color:#dcdcdc;max-width:1120px}"
			"h1{font-size:22px;margin:0 0 6px;color:#f0f0f0}h2{font-size:16px;margin:24px 0 8px;padding-bottom:5px;border-bottom:1px solid #303030;color:#ececec}"
			"h3{font-size:14px;margin:18px 0 6px;color:#e6e6e6}p{line-height:1.55;margin:7px 0}a{color:#4ea1ff}"
			"code{font-family:Consolas,'Roboto Mono',monospace;font-size:12.5px;background:#262626;padding:1px 5px;border-radius:3px;color:#e8c07d}"
			"pre{background:#121212;border:1px solid #303030;border-radius:4px;padding:10px 12px;overflow:auto}pre code{background:none;padding:0;color:#d4d4d4}"
			"table{border-collapse:collapse;width:100%;margin:10px 0;background:#202020;border:1px solid #2c2c2c}"
			"th,td{padding:7px 10px;border-bottom:1px solid #2c2c2c;text-align:left;vertical-align:top}th{background:#262626;color:#a8a8a8;font-weight:600}"
			"tr:nth-child(odd) td{background:#1d1d1d}.chip{display:inline-block;font-size:11px;font-weight:700;padding:1px 7px;border-radius:9px}"
			".chip.pass{color:#6fd98f;background:rgba(111,217,143,.14)}.chip.fail{color:#ff6b6b;background:rgba(255,107,107,.14)}"
			".chip.warn{color:#f2c14e;background:rgba(242,193,78,.14)}.chip.info{color:#6aa8ff;background:rgba(106,168,255,.14)}"
			"blockquote{margin:10px 0;padding:4px 14px;border-left:3px solid #2f80ed;color:#b4b4b4}ul,ol{margin:6px 0 6px 22px;padding:0}li{margin:3px 0;line-height:1.5}"
			"hr{border:0;border-top:1px solid #303030;margin:18px 0}figure{margin:12px 0}img{max-width:100%;border-radius:4px;border:1px solid #2c2c2c}"
			"figcaption{color:#9a9a9a;font-size:12px;margin-top:4px}.meta{color:#8c8c8c;font-size:12px;margin:0 0 18px}");
		const bool bOwnTitle = BodyHtml.StartsWith(TEXT("<h1>"));
		return FString::Printf(TEXT("<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>%s</title><style>%s</style></head><body>\n%s<p class=\"meta\">HyperAI Studio, %s</p>\n%s</body></html>\n"),
			*EscapeHtml(Title), Css, bOwnTitle ? TEXT("") : *FString::Printf(TEXT("<h1>%s</h1>\n"), *EscapeHtml(Title)),
			*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M")), *BodyHtml);
	}

	FString SaveReport(const FString& Title, const FString& Markdown)
	{
		FString Name;
		for (const TCHAR C : Title.Left(60))
		{
			Name.AppendChar(FChar::IsAlnum(C) ? FChar::ToLower(C) : TEXT('-'));
		}
		while (Name.Contains(TEXT("--")))
		{
			Name.ReplaceInline(TEXT("--"), TEXT("-"));
		}
		Name = Name.IsEmpty() ? FString(TEXT("report")) : Name;
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("Reports")
			/ FString::Printf(TEXT("%s-%s.html"), *Name, *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"))));
		return FFileHelper::SaveStringToFile(HtmlPage(Title, ToHtml(Markdown)), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) ? Path : FString();
	}
}

void SHyperAIStudioMarkdown::Construct(const FArguments& InArgs)
{
	BodyStyle = InArgs._BodyStyle.IsNone() ? FName(TEXT("Md.Body")) : InArgs._BodyStyle;
	ChildSlot
	[
		SAssignNew(Blocks, SVerticalBox)
	];
	Source = InArgs._Markdown;
	Show(Source.Get());
}

void SHyperAIStudioMarkdown::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	// Text that streams in is read a few times a second, rather than rebuilt for every piece of it. The wall clock is
	// used because a widget drawn off screen is given the same time on every frame.
	const double Now = FPlatformTime::Seconds();
	if (Source.IsBound() && Now >= NextRead)
	{
		NextRead = Now + 0.1;
		Show(Source.Get());
	}
}

void SHyperAIStudioMarkdown::SetMarkdown(const FString& InMarkdown)
{
	Source.Set(InMarkdown);
	Show(InMarkdown);
}

void SHyperAIStudioMarkdown::Show(const FString& InMarkdown)
{
	if (bBuilt && InMarkdown.Equals(Markdown, ESearchCase::CaseSensitive))
	{
		return;
	}
	Markdown = InMarkdown;
	bBuilt = true;
	Rebuild();
}

void SHyperAIStudioMarkdown::Rebuild()
{
	using namespace HyperAIStudio::Markdown;
	Blocks->ClearChildren();
	Pictures.Reset();
	const TArray<FBlock> Parsed = Parse(Markdown);
	for (int32 Index = 0; Index < Parsed.Num(); ++Index)
	{
		const FBlock& Block = Parsed[Index];
		const bool bFirst = Index == 0;
		const bool bAfterItem = !bFirst && (Parsed[Index - 1].Kind == EBlock::Bullet || Parsed[Index - 1].Kind == EBlock::Numbered);
		FMargin Padding(0.0f, bFirst ? 0.0f : 7.0f, 0.0f, 0.0f);
		TSharedRef<SWidget> Widget = SNullWidget::NullWidget;
		switch (Block.Kind)
		{
		case EBlock::Heading:
			Widget = Rich(Block.Text, Block.Level == 1 ? FName(TEXT("Md.H1")) : Block.Level == 2 ? FName(TEXT("Md.H2")) : FName(TEXT("Md.H3")));
			Padding.Top = bFirst ? 0.0f : 12.0f;
			break;
		case EBlock::Paragraph:
			Widget = Rich(Block.Text, BodyStyle);
			break;
		case EBlock::Bullet:
		case EBlock::Numbered:
			Padding.Top = bFirst ? 0.0f : bAfterItem ? 3.0f : 7.0f;
			Widget = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(4.0f + Block.Level * 16.0f, 0.0f, 6.0f, 0.0f))
				[
					SNew(SBox)
					.MinDesiredWidth(Block.Kind == EBlock::Numbered ? 18.0f : 10.0f)
					[
						SNew(STextBlock)
						.TextStyle(&FHyperAIStudioStyle::Get(), BodyStyle)
						.Text(Block.Kind == EBlock::Numbered ? FText::FromString(FString::Printf(TEXT("%d."), Block.Number)) : FText::FromString(TEXT("•")))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					Rich(Block.Text, BodyStyle)
				];
			break;
		case EBlock::Quote:
			Widget = SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("WhiteBrush"))
					.BorderBackgroundColor(FStyleColors::AccentBlue)
					.Padding(FMargin(1.5f, 0.0f))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(10.0f, 2.0f, 0.0f, 2.0f))
				[
					Rich(Block.Text, TEXT("Md.Quote"))
				];
			break;
		case EBlock::Code:
			Widget = MakeCode(Block);
			break;
		case EBlock::Table:
			Widget = MakeTable(Block);
			Padding.Top = bFirst ? 0.0f : 9.0f;
			break;
		case EBlock::Rule:
			Widget = SNew(SSeparator).Thickness(1.0f);
			Padding = FMargin(0.0f, 12.0f, 0.0f, 4.0f);
			break;
		case EBlock::Image:
			Widget = MakeImage(Block);
			break;
		}
		Blocks->AddSlot().AutoHeight().Padding(Padding)
		[
			Widget
		];
	}
}

TSharedRef<SWidget> SHyperAIStudioMarkdown::MakeCode(const HyperAIStudio::Markdown::FBlock& Block) const
{
	const FString Code = Block.Text;
	return SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.CodeBlock"))
		.Padding(FMargin(10.0f, 5.0f, 5.0f, 8.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(Block.Label.IsEmpty() ? LOCTEXT("Code", "code") : FText::FromString(Block.Label))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ToolTipText(LOCTEXT("CopyCode", "Copy"))
					.ContentPadding(FMargin(3.0f, 1.0f))
					.OnClicked_Lambda([Code]()
					{
						FPlatformApplicationMisc::ClipboardCopy(*Code);
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("Icons.Clipboard"))
						.ColorAndOpacity(FSlateColor::UseForeground())
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 5.0f, 0.0f))
			[
				SNew(SMultiLineEditableText)
				.Text(FText::FromString(Code))
				.IsReadOnly(true)
				.AutoWrapText(true)
				.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			]
		];
}

TSharedRef<SWidget> SHyperAIStudioMarkdown::MakeTable(const HyperAIStudio::Markdown::FBlock& Block) const
{
	using namespace HyperAIStudio::Markdown;
	const TSharedRef<SGridPanel> Grid = SNew(SGridPanel);
	int32 Columns = 0;
	for (const TArray<FString>& Row : Block.Rows)
	{
		Columns = FMath::Max(Columns, Row.Num());
	}
	// Every column wraps; the ones with longer text get more of the width.
	for (int32 Column = 0; Column < Columns; ++Column)
	{
		int32 Longest = 0;
		for (const TArray<FString>& Row : Block.Rows)
		{
			Longest = FMath::Max(Longest, Row.IsValidIndex(Column) ? Row[Column].Len() : 0);
		}
		Grid->SetColumnFill(Column, FMath::Clamp(Longest / 14.0f, 0.5f, 5.0f));
	}
	for (int32 Row = 0; Row < Block.Rows.Num(); ++Row)
	{
		for (int32 Column = 0; Column < Columns; ++Column)
		{
			const FString Cell = Block.Rows[Row].IsValidIndex(Column) ? Block.Rows[Row][Column] : FString();
			TSharedRef<SWidget> Content = SNullWidget::NullWidget;
			FString Rest;
			// The first column names the row; a status is in the columns after it.
			const FString Status = Row == 0 || Column == 0 ? FString() : CellStatus(Cell, Rest);
			if (Row == 0)
			{
				Content = Rich(Cell, TEXT("Md.TableHead"));
			}
			else if (Status.IsEmpty())
			{
				Content = Rich(Cell, BodyStyle);
			}
			else
			{
				const FString Word = Cell.TrimStartAndEnd().Left(Cell.TrimStartAndEnd().Len() - Rest.Len()).TrimEnd();
				Content = SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(FMargin(0.0f, 1.0f, Rest.IsEmpty() ? 0.0f : 6.0f, 0.0f))
					[
						Chip(Status, Word.Replace(TEXT(":"), TEXT("")).TrimEnd())
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						Rest.IsEmpty() ? SNullWidget::NullWidget : Rich(Rest, BodyStyle)
					];
			}
			Grid->AddSlot(Column, Row)
			[
				SNew(SBorder)
				.BorderImage(FHyperAIStudioStyle::Get().GetBrush(Row == 0 ? "HyperAIStudio.TableHeader" : Row % 2 ? "HyperAIStudio.TableRow" : "HyperAIStudio.TableRowAlt"))
				.Padding(FMargin(9.0f, 5.0f))
				[
					Content
				]
			];
		}
	}
	return SNew(SBorder)
		.BorderImage(FHyperAIStudioStyle::Get().GetBrush("HyperAIStudio.TableFrame"))
		.Padding(1.0f)
		[
			Grid
		];
}

TSharedRef<SWidget> SHyperAIStudioMarkdown::MakeImage(const HyperAIStudio::Markdown::FBlock& Block)
{
	using namespace HyperAIStudio::Markdown;
	const FString Path = LocalFile(Block.Text);
	const FText Caption = FText::FromString(Block.Label.IsEmpty() ? FPaths::GetCleanFilename(Block.Text) : Block.Label);
	if (Path.IsEmpty() || !IsPicture(Path))
	{
		// Not a picture on disk: the caption, as a link to wherever it points.
		return Rich(FString::Printf(TEXT("[%s](%s)"), *Caption.ToString(), *Block.Text), BodyStyle);
	}
	const TSharedPtr<FHyperAIStudioPreviewImage> Image = Pictures.Add_GetRef(MakeShared<FHyperAIStudioPreviewImage>());
	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(0.0f)
			.ToolTipText(FText::Format(LOCTEXT("PictureTooltip", "{0}\nClick to open it in Preview."), FText::FromString(Path)))
			.OnClicked_Lambda([Path]()
			{
				FHyperAIStudioPreviewPanel::Open(NAME_None, Path);
				return FReply::Handled();
			})
			[
				SNew(SBox)
				.MaxDesiredWidth(440.0f)
				.MaxDesiredHeight(270.0f)
				[
					SNew(SScaleBox)
					.Stretch(EStretch::ScaleToFit)
					.StretchDirection(EStretchDirection::DownOnly)
					[
						SNew(SImage).Image_Lambda([Image, Path]() { return Image->Get(Path); })
					]
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 3.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(Caption)
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
}

#undef LOCTEXT_NAMESPACE
