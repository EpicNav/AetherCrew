// Games by Hyper 2026.

#include "SHyperAIStudioPlaytestPanel.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"
#include "HyperAIStudioMarkdown.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioPlaytestPanel"

namespace HyperAIStudio::PlaytestPanel
{
	namespace Verify = HyperAIStudio::Verify;

	/** A point Distance ahead of where the open level starts the player, for templates that need somewhere to go. */
	FVector Ahead(const double Distance)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (World)
		{
			for (TActorIterator<APlayerStart> It(World); It; ++It)
			{
				return It->GetActorLocation() + FRotator(0.0, It->GetActorRotation().Yaw, 0.0).Vector() * Distance;
			}
		}
		return FVector(Distance, 0.0, 0.0);
	}

	FString Where(const FVector& Point)
	{
		return FString::Printf(TEXT("{\"x\": %.0f, \"y\": %.0f, \"z\": %.0f}"), Point.X, Point.Y, Point.Z);
	}

	/** One case of a kind, as a person would start writing it; names such as Coin and Enemy are to be changed to the game's. */
	FString Template(const FString& Kind)
	{
		if (Kind == TEXT("Survey"))
		{
			return TEXT(R"({"name": "survey", "timeoutSeconds": 5, "steps": [{"action": "Wait", "seconds": 0.5}, {"action": "Survey"}]})");
		}
		if (Kind == TEXT("Reach"))
		{
			return FString::Printf(TEXT(R"({"name": "reach", "timeoutSeconds": 25, "steps": [{"action": "Play", "goal": "Reach", "location": %s, "radius": 150, "timeoutSeconds": 20}, {"action": "Capture", "text": "reached"}]})"),
				*Where(Ahead(1000.0)));
		}
		if (Kind == TEXT("Collect"))
		{
			return TEXT(R"({"name": "collect", "timeoutSeconds": 35, "steps": [{"action": "Play", "goal": "Collect", "target": "Coin", "timeoutSeconds": 30}]})");
		}
		if (Kind == TEXT("Defeat"))
		{
			return TEXT(R"({"name": "defeat", "timeoutSeconds": 35, "steps": [{"action": "Wait", "seconds": 0.5}, {"action": "Play", "goal": "Defeat", "target": "Enemy", "skill": 0.6, "timeoutSeconds": 30}, {"action": "Capture", "text": "after-the-fight"}]})");
		}
		if (Kind == TEXT("Survive"))
		{
			return TEXT(R"({"name": "survive", "timeoutSeconds": 20, "steps": [{"action": "Play", "goal": "Survive", "target": "Enemy", "seconds": 15}]})");
		}
		if (Kind == TEXT("Explore"))
		{
			return TEXT(R"({"name": "explore", "timeoutSeconds": 25, "steps": [{"action": "Play", "goal": "Explore", "seconds": 20}, {"action": "Capture", "text": "explored"}]})");
		}
		if (Kind == TEXT("Duel"))
		{
			return FString::Printf(TEXT(R"({"name": "duel", "timeoutSeconds": 50, "steps": [{"action": "AddPlayer", "location": %s}, {"action": "Wait", "seconds": 0.5}, {"action": "Play", "goal": "Defeat", "target": "Player0", "playerIndex": 1, "skill": 0.4, "bBackground": true, "timeoutSeconds": 40}, {"action": "Play", "goal": "Defeat", "target": "Player1", "skill": 0.7, "timeoutSeconds": 40}, {"action": "Capture", "text": "duel-over"}]})"),
				*Where(Ahead(1500.0)));
		}
		return FString();
	}

	TSharedPtr<FJsonObject> Parse(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) ? Object : nullptr;
	}

	FString Pretty(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Text));
		return Text;
	}

	/** Text for a table cell: a pipe would end the cell, and a new line the row. */
	FString Cell(const FString& Text)
	{
		return Text.Replace(TEXT("|"), TEXT("\\|")).Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
	}

	FString Seconds(const double Value)
	{
		return FString::Printf(TEXT("%.1f s"), Value);
	}
}

void SHyperAIStudioPlaytestPanel::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::PlaytestPanel;
	const TSharedRef<SWrapBox> Templates = SNew(SWrapBox).UseAllottedSize(true).InnerSlotPadding(FVector2D(4.0f, 4.0f));
	struct FKind
	{
		const TCHAR* Name;
		FText Tooltip;
	};
	const FKind Kinds[] = {
		{TEXT("Survey"), LOCTEXT("SurveyTooltip", "What a player learns first: the controls, their health, the actors and who else is playing.")},
		{TEXT("Reach"), LOCTEXT("ReachTooltip", "Walk to a place, round what is in the way and over what can be jumped.")},
		{TEXT("Collect"), LOCTEXT("CollectTooltip", "Pick up every actor of a kind, nearest first.")},
		{TEXT("Defeat"), LOCTEXT("DefeatTooltip", "Find, aim at and attack every actor of a kind until each is defeated.")},
		{TEXT("Survive"), LOCTEXT("SurviveTooltip", "Stay alive for a while, keeping away from threats and dodging them.")},
		{TEXT("Explore"), LOCTEXT("ExploreTooltip", "Wander toward places not yet visited; reports coverage, stuck spots and falls.")},
		{TEXT("Duel"), LOCTEXT("DuelTooltip", "A second player joins, and the two players fight each other.")}};
	for (const FKind& Kind : Kinds)
	{
		const FString Name = Kind.Name;
		Templates->AddSlot()
		[
			SNew(SButton)
			.Text(FText::FromString(Name))
			.ToolTipText(Kind.Tooltip)
			.OnClicked_Lambda([this, Name]()
			{
				AddTemplate(Name);
				return FReply::Handled();
			})
		];
	}
	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(10.0f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Title", "Playtest"))
				.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Intro", "The HyperAI bot plays the open level the way a person would, through the game's own controls. Every case plays in its own copy of the level, all at the same time, and answers within a minute. Nothing in the level changes."))
				.Font(FAppStyle::GetFontStyle("SmallFont"))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				SNew(SSplitter)
				+ SSplitter::Slot().Value(0.42f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock).Text(LOCTEXT("AddCase", "Add a case"))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
					[
						Templates
					]
					+ SVerticalBox::Slot().FillHeight(1.0f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
						.Padding(4.0f)
						[
							SAssignNew(CasesBox, SMultiLineEditableTextBox)
							.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
							.HintText(LOCTEXT("CasesHint", "hyper_verify_run's request: a deadline and cases of bot steps."))
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth()
						[
							SNew(SButton)
							.Text(LOCTEXT("Play", "Play in copies"))
							.ToolTipText(LOCTEXT("PlayTooltip", "Play every case at once, each in its own copy of the open level, as hyper_verify_run does."))
							.IsEnabled_Lambda([this]() { return !bRunning; })
							.OnClicked_Lambda([this]()
							{
								FString Error;
								Run(Error);
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("Calibrate", "Calibrate"))
							.ToolTipText(LOCTEXT("CalibrateTooltip", "Measure how many cases this machine plays at once on this level, in about a minute, as hyper_verify_calibrate does."))
							.IsEnabled_Lambda([this]() { return !bRunning; })
							.OnClicked_Lambda([this]()
							{
								FString Error;
								const bool bStarted = HyperAIStudio::Verify::Calibrate(HyperAIStudio::Verify::MaxCases, [this, Alive = bAlive](const HyperAIStudio::Verify::FCalibration& Calibration)
								{
									if (*Alive)
									{
										bRunning = false;
										Status = FText::Format(LOCTEXT("Calibrated", "Up to {0} cases at once on this machine and level. {1}"), Calibration.Limit, FText::FromString(Calibration.Diagnostic));
									}
								}, Error);
								bRunning = bStarted;
								Status = bStarted ? LOCTEXT("Calibrating", "Measuring how many cases this machine plays at once...") : FText::FromString(Error);
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(SButton)
							.Text(LOCTEXT("Stop", "Stop"))
							.IsEnabled_Lambda([this]() { return bRunning; })
							.OnClicked_Lambda([]()
							{
								HyperAIStudio::Verify::Stop(TEXT("Stopped from the Playtest tab."));
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(8.0f, 0.0f, 0.0f, 0.0f)
						[
							SNew(STextBlock)
							.Text_Lambda([this]() { return Status; })
							.AutoWrapText(true)
						]
					]
				]
				+ SSplitter::Slot().Value(0.58f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot().Padding(8.0f, 0.0f, 0.0f, 0.0f)
					[
						SAssignNew(Results, SVerticalBox)
					]
				]
			]
		]
	];
	SetCases(TEXT("{\"deadlineSeconds\": 60, \"cases\": []}"));
	AddTemplate(TEXT("Survey"));
	AddTemplate(TEXT("Explore"));
	Status = FText::Format(LOCTEXT("Limit", "Up to {0} cases at once on this machine and level."), HyperAIStudio::Verify::GetLimit());
	Rebuild();
}

SHyperAIStudioPlaytestPanel::~SHyperAIStudioPlaytestPanel()
{
	*bAlive = false;
}

FString SHyperAIStudioPlaytestPanel::GetCases() const
{
	return CasesBox.IsValid() ? CasesBox->GetText().ToString() : FString();
}

void SHyperAIStudioPlaytestPanel::SetCases(const FString& Json)
{
	if (CasesBox.IsValid())
	{
		CasesBox->SetText(FText::FromString(Json));
	}
}

void SHyperAIStudioPlaytestPanel::AddTemplate(const FString& Kind)
{
	using namespace HyperAIStudio::PlaytestPanel;
	const TSharedPtr<FJsonObject> Case = Parse(Template(Kind));
	if (!Case)
	{
		return;
	}
	TSharedPtr<FJsonObject> Request = Parse(GetCases());
	if (!Request)
	{
		Request = MakeShared<FJsonObject>();
		Request->SetNumberField(TEXT("deadlineSeconds"), 60);
	}
	TArray<TSharedPtr<FJsonValue>> Cases;
	const TArray<TSharedPtr<FJsonValue>>* Existing = nullptr;
	if (Request->TryGetArrayField(TEXT("cases"), Existing))
	{
		Cases = *Existing;
	}
	// Each case keeps a name of its own, so its result is easy to find.
	const FString Base = Case->GetStringField(TEXT("name"));
	FString Name = Base;
	for (int32 Number = 2; Cases.ContainsByPredicate([&Name](const TSharedPtr<FJsonValue>& Value)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			FString Other;
			return Value->TryGetObject(Object) && (*Object)->TryGetStringField(TEXT("name"), Other) && Other == Name;
		}); ++Number)
	{
		Name = FString::Printf(TEXT("%s-%d"), *Base, Number);
	}
	Case->SetStringField(TEXT("name"), Name);
	Cases.Add(MakeShared<FJsonValueObject>(Case));
	Request->SetArrayField(TEXT("cases"), Cases);
	SetCases(Pretty(Request.ToSharedRef()));
}

bool SHyperAIStudioPlaytestPanel::Run(FString& OutError)
{
	namespace Verify = HyperAIStudio::Verify;
	FHyperAIVerifyRequest Request;
	FText Why;
	if (!FJsonObjectConverter::JsonObjectStringToUStruct(GetCases(), &Request, 0, 0, false, &Why))
	{
		OutError = TEXT("The cases are not a hyper_verify_run request: ") + Why.ToString();
		Status = FText::FromString(OutError);
		return false;
	}
	TArray<Verify::FCase> Cases;
	for (const FHyperAIVerifyCase& In : Request.Cases)
	{
		Verify::FCase& Case = Cases.AddDefaulted_GetRef();
		Case.Name = In.Name.Left(64);
		Case.Steps = In.Steps;
		Case.TimeoutSeconds = In.TimeoutSeconds;
	}
	const int32 Count = Cases.Num();
	if (!Verify::Admit(Cases, Request.DeadlineSeconds, Verify::GetLimit(), OutError)
		|| !Verify::Start(MoveTemp(Cases), Request.DeadlineSeconds, [this, Alive = bAlive](const Verify::FBatchOutcome& Result)
		{
			if (!*Alive)
			{
				return;
			}
			int32 Passed = 0;
			for (const Verify::FCaseOutcome& Case : Result.Cases)
			{
				Passed += Case.Status == TEXT("passed");
			}
			Outcome = Result;
			bRunning = false;
			Status = Result.Diagnostic.IsEmpty()
				? FText::Format(LOCTEXT("Done", "{0} of {1} cases passed in {2} s."), Passed, Result.Cases.Num(), FText::AsNumber(FMath::RoundToInt(Result.WallSeconds * 10.0) / 10.0))
				: FText::FromString(Result.Diagnostic);
			Rebuild();
		}, OutError))
	{
		Status = FText::FromString(OutError);
		return false;
	}
	bRunning = true;
	Outcome.Reset();
	Status = FText::Format(LOCTEXT("Playing", "Playing {0} cases..."), Count);
	Rebuild();
	return true;
}

FString SHyperAIStudioPlaytestPanel::BuildReport(const HyperAIStudio::Verify::FBatchOutcome& Batch)
{
	using namespace HyperAIStudio::PlaytestPanel;
	int32 Passed = 0;
	for (const Verify::FCaseOutcome& Case : Batch.Cases)
	{
		Passed += Case.Status == TEXT("passed") ? 1 : 0;
	}
	const FString Level = Batch.Map.IsEmpty() ? FString(TEXT("the level")) : FPaths::GetBaseFilename(Batch.Map);
	FString Out = TEXT("## ") + FText::Format(LOCTEXT("ReportTitle", "Playtest: {0}"), FText::FromString(Level)).ToString() + TEXT("\n\n");
	// What happened in one sentence, then the results.
	Out += FText::Format(LOCTEXT("ReportLead", "{0} of {1} {1}|plural(one=case,other=cases) passed, each in its own copy of the level, in {2}."),
		Passed, Batch.Cases.Num(), FText::FromString(Seconds(Batch.WallSeconds))).ToString();
	if (!Batch.Diagnostic.IsEmpty() && Batch.Status != TEXT("passed"))
	{
		Out += TEXT(" ") + Batch.Diagnostic.TrimStartAndEnd();
	}
	Out += TEXT("\n\n| ") + LOCTEXT("ReportCase", "Case").ToString() + TEXT(" | ") + LOCTEXT("ReportResult", "Result").ToString() + TEXT(" | ")
		+ LOCTEXT("ReportTime", "Simulated").ToString() + TEXT(" |\n|---|---|---|\n");
	for (const Verify::FCaseOutcome& Case : Batch.Cases)
	{
		Out += FString::Printf(TEXT("| %s | %s %s | %s |\n"), *Cell(Case.Name), *Case.Status.ToUpper(), *Cell(Case.Diagnostic.Left(240)), *Seconds(Case.SimulatedSeconds));
	}
	for (const Verify::FCaseOutcome& Case : Batch.Cases)
	{
		Out += TEXT("\n### ") + Case.Name + TEXT("\n\n| # | ") + LOCTEXT("ReportStep", "Step").ToString() + TEXT(" | ")
			+ LOCTEXT("ReportSaw", "What it saw").ToString() + TEXT(" |\n|---|---|---|\n");
		TArray<FString> Pictures;
		for (int32 Index = 0; Index < Case.Steps.Num(); ++Index)
		{
			const Verify::FStepOutcome& Step = Case.Steps[Index];
			const FString Saw = Step.Status == TEXT("passed") || Step.Code.IsEmpty() ? Step.Detail : Step.Code + TEXT(": ") + Step.Detail;
			Out += FString::Printf(TEXT("| %d | %s | %s %s |\n"), Index + 1, *Cell(Step.Action), *Step.Status.ToUpper(), *Cell(Saw.Left(400)));
			if (Step.File.EndsWith(TEXT(".png")))
			{
				Pictures.Add(Step.File.Replace(TEXT("\\"), TEXT("/")));
			}
		}
		for (const FString& Picture : Pictures)
		{
			Out += TEXT("\n![") + FPaths::GetBaseFilename(Picture) + TEXT("](") + Picture + TEXT(")");
		}
		Out += TEXT("\n");
	}
	return Out;
}

void SHyperAIStudioPlaytestPanel::ShowOutcome(const HyperAIStudio::Verify::FBatchOutcome& InOutcome)
{
	Outcome = InOutcome;
	Rebuild();
}

void SHyperAIStudioPlaytestPanel::Rebuild()
{
	if (!Results.IsValid())
	{
		return;
	}
	Results->ClearChildren();
	if (!Outcome.IsSet())
	{
		Report.Reset();
		Results->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(bRunning ? LOCTEXT("Running", "The cases are playing; their results show here when the batch ends.")
				: LOCTEXT("Empty", "Results show here: each case, what every step saw, and the maps and pictures they made."))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.AutoWrapText(true)
		];
		return;
	}
	Report = BuildReport(Outcome.GetValue());
	Results->AddSlot().AutoHeight().HAlign(HAlign_Right)
	[
		HyperAIStudio::Markdown::ReportActions(
			[this]() { return Outcome.IsSet() ? TEXT("Playtest ") + FPaths::GetBaseFilename(Outcome->Map) : FString(TEXT("Playtest")); },
			[this]() { return Report; })
	];
	Results->AddSlot().AutoHeight()
	[
		SNew(SHyperAIStudioMarkdown).Markdown(Report)
	];
}

#undef LOCTEXT_NAMESPACE
