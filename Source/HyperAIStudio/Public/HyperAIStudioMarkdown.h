// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FHyperAIStudioPreviewImage;
class SVerticalBox;

/**
 * Markdown as agents write it, read as a report: parsed into blocks, shown with Slate, or written out as an HTML page.
 * The subset is the one replies and reports use: headings, paragraphs, bullet and numbered lists, quotes, fenced
 * code, pipe tables, rules and pictures, with bold, italic, inline code and links inside them.
 */
namespace HyperAIStudio::Markdown
{
	enum class EBlock : uint8
	{
		Heading,
		Paragraph,
		Bullet,
		Numbered,
		Quote,
		Code,
		Table,
		Rule,
		Image
	};

	struct FBlock
	{
		EBlock Kind = EBlock::Paragraph;
		/** A heading's level, 1 to 3, or a list item's depth, 0 to 2. */
		int32 Level = 0;
		/** A numbered item's number. */
		int32 Number = 0;
		/** Inline markdown for text blocks; the code of a code block; the path of a picture. */
		FString Text;
		/** A code block's language, or a picture's caption. */
		FString Label;
		/** A table's cells, the header row first. */
		TArray<TArray<FString>> Rows;
	};

	HYPERAISTUDIO_API TArray<FBlock> Parse(const FString& Markdown);

	/** Inline markdown as SRichTextBlock markup, using the plugin style's Md.* text styles and the md link decorator. */
	HYPERAISTUDIO_API FString ToRichText(const FString& Inline);

	/** The markdown as HTML, for a report; pictures on disk are embedded, so the page stands alone. */
	HYPERAISTUDIO_API FString ToHtml(const FString& Markdown);

	/** A whole page around Body, in the editor's dark look. */
	HYPERAISTUDIO_API FString HtmlPage(const FString& Title, const FString& BodyHtml);

	/**
	 * Writes a report as a standalone HTML page under Saved/HyperAIStudio/Reports, named after Title and the time.
	 * Returns the file, or empty when it could not be written.
	 */
	HYPERAISTUDIO_API FString SaveReport(const FString& Title, const FString& Markdown);

	/** A table cell's leading status (pass, fail, warn, info), or empty; OutRest is the text after it. */
	HYPERAISTUDIO_API FString CellStatus(const FString& Cell, FString& OutRest);

	/**
	 * A tool's JSON answer as a report: its summary or message as a sentence, then its fields as a table, lists of
	 * records as tables of their own, and the pictures it names. Empty when Json is not JSON.
	 */
	HYPERAISTUDIO_API FString FromJson(const FString& Json);

	/** Saves Markdown as an HTML report, says so with a notification that opens it, and returns the file. */
	HYPERAISTUDIO_API FString ExportReport(const FString& Title, const FString& Markdown);

	/** Copy and Save as HTML buttons for a report; Title and Markdown are read when one is clicked. */
	HYPERAISTUDIO_API TSharedRef<SWidget> ReportActions(TFunction<FString()> Title, TFunction<FString()> Markdown);
}

/**
 * Markdown shown as a report in Slate: headings, wrapped paragraphs, lists, quotes, tables with status chips, code
 * with a copy button, rules, and pictures that open in the Preview page. Links open in the browser, and local
 * pictures in Preview.
 */
class HYPERAISTUDIO_API SHyperAIStudioMarkdown : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SHyperAIStudioMarkdown) {}
		/** The markdown. Bound to a function, it is read again a few times a second, for text that streams in. */
		SLATE_ATTRIBUTE(FString, Markdown)
		/** The body's text style in the plugin style set; Md.Body unless set. */
		SLATE_ARGUMENT(FName, BodyStyle)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	void SetMarkdown(const FString& InMarkdown);
	const FString& GetMarkdown() const { return Markdown; }

private:
	void Show(const FString& InMarkdown);
	void Rebuild();
	TSharedRef<SWidget> MakeTable(const HyperAIStudio::Markdown::FBlock& Block) const;
	TSharedRef<SWidget> MakeCode(const HyperAIStudio::Markdown::FBlock& Block) const;
	TSharedRef<SWidget> MakeImage(const HyperAIStudio::Markdown::FBlock& Block);

	TAttribute<FString> Source;
	double NextRead = 0.0;
	FString Markdown;
	FName BodyStyle;
	bool bBuilt = false;
	TSharedPtr<SVerticalBox> Blocks;
	TArray<TSharedPtr<FHyperAIStudioPreviewImage>> Pictures;
};
