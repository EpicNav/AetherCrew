// Games by Hyper 2026.

#include "HyperAIStudioPlaytestRecorder.h"

#include "Dom/JsonObject.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioBotAim.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElements.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/CoreStyle.h"
#include "UnrealClient.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SViewport.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	/** Wide enough to follow any playtest, small enough to capture every frame without a hitch. */
	constexpr int32 MaxVideoWidth = 1280;
	/** How long a finished step's result stays on screen. */
	constexpr double ResultSeconds = 2.0;
	/** Above the game's own UI, which starts at 0. */
	constexpr int32 OverlayZOrder = 10000;

	UGameViewportClient* ViewportClient(UWorld* World)
	{
		return World ? World->GetGameViewport() : nullptr;
	}

	/**
	 * The bot's aims over the game: a ring and a cross on the point each player aims at, and a line to it from the
	 * player, coloured by how it aims (cursor amber, stick blue, view green). A game that hides its cursor shows where
	 * the bot put it.
	 */
	class SHyperAIStudioAimLayer final : public SLeafWidget
	{
	public:
		SLATE_BEGIN_ARGS(SHyperAIStudioAimLayer) {}
		SLATE_END_ARGS()

		void Construct(const FArguments&, UWorld& InWorld)
		{
			World = &InWorld;
			// Aims move every frame.
			ForceVolatile(true);
		}

		virtual FVector2D ComputeDesiredSize(float) const override
		{
			return FVector2D::ZeroVector;
		}

		virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& Culling, FSlateWindowElementList& Out,
			const int32 LayerId, const FWidgetStyle& Style, const bool bParentEnabled) const override
		{
			const UWorld* W = World.Get();
			if (!W)
			{
				return LayerId;
			}
			const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 11);
			for (const HyperAIStudio::Bot::Aim::FView& View : HyperAIStudio::Bot::Aim::Active(*W))
			{
				if (!View.bPointOnScreen || View.ViewportSize.X < 1 || View.ViewportSize.Y < 1)
				{
					continue;
				}
				const FVector2f Scale = FVector2f(Geometry.GetLocalSize()) / FVector2f(View.ViewportSize);
				const FVector2f At = FVector2f(View.Point) * Scale;
				const FLinearColor Color = View.Mode == EHyperAIBotAim::Cursor ? FLinearColor(1.f, 0.7f, 0.1f)
					: View.Mode == EHyperAIBotAim::Stick ? FLinearColor(0.3f, 0.7f, 1.f) : FLinearColor(0.35f, 1.f, 0.45f);
				if (View.bFromOnScreen)
				{
					FSlateDrawElement::MakeLines(Out, LayerId, Geometry.ToPaintGeometry(), TArray<FVector2f>{FVector2f(View.From) * Scale, At},
						ESlateDrawEffect::None, Color.CopyWithNewOpacity(0.75f), true, 2.f);
				}
				TArray<FVector2f> Ring;
				for (int32 Index = 0; Index <= 16; ++Index)
				{
					const float Angle = static_cast<float>(Index * UE_TWO_PI / 16.0);
					Ring.Add(At + FVector2f(FMath::Cos(Angle), FMath::Sin(Angle)) * 11.f);
				}
				FSlateDrawElement::MakeLines(Out, LayerId, Geometry.ToPaintGeometry(), Ring, ESlateDrawEffect::None, Color, true, 2.f);
				for (const FVector2f& Arm : {FVector2f(1.f, 0.f), FVector2f(-1.f, 0.f), FVector2f(0.f, 1.f), FVector2f(0.f, -1.f)})
				{
					FSlateDrawElement::MakeLines(Out, LayerId, Geometry.ToPaintGeometry(), TArray<FVector2f>{At + Arm * 5.f, At + Arm * 17.f},
						ESlateDrawEffect::None, Color, true, 2.f);
				}
				FSlateDrawElement::MakeText(Out, LayerId + 1, Geometry.ToPaintGeometry(FVector2f(220.f, 20.f), FSlateLayoutTransform(At + FVector2f(16.f, -26.f))),
					FString::Printf(TEXT("P%d aim: %s"), View.PlayerIndex, *HyperAIStudio::Bot::Aim::ModeName(View.Mode)), Font, ESlateDrawEffect::None, Color);
			}
			return LayerId + 1;
		}

	private:
		TWeakObjectPtr<UWorld> World;
	};
}

FHyperAIStudioPlaytestRecorder::~FHyperAIStudioPlaytestRecorder()
{
	FString Ignored;
	Stop(Ignored);
}

double FHyperAIStudioPlaytestRecorder::Elapsed() const
{
	return FPlatformTime::Seconds() - StartedSeconds;
}

bool FHyperAIStudioPlaytestRecorder::Start(UWorld& InWorld, const FString& InFolder, const int32 FramesPerSecond, FString& OutError)
{
	if (IsRecording())
	{
		OutError = TEXT("A recording is already running.");
		return false;
	}
	UGameViewportClient* Client = ViewportClient(&InWorld);
	const FIntPoint Size = Client && Client->Viewport ? Client->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
	if (Size.X < 16 || Size.Y < 16 || !Client->GetGameViewportWidget().IsValid() || !FSlateApplication::IsInitialized())
	{
		OutError = TEXT("The game has no screen to record.");
		return false;
	}
	const double Scale = FMath::Min(1.0, static_cast<double>(MaxVideoWidth) / Size.X);
	IFileManager::Get().MakeDirectory(*InFolder, /*Tree=*/true);
	VideoPath = FPaths::ConvertRelativePathToFull(InFolder / TEXT("playtest.mp4"));
	Fps = FMath::Clamp(FramesPerSecond, 1, 60);
	if (!Writer.Open(VideoPath, FMath::RoundToInt32(Size.X * Scale), FMath::RoundToInt32(Size.Y * Scale), Fps, OutError))
	{
		return false;
	}
	World = &InWorld;
	Folder = InFolder;
	Timeline.Reset();
	WriteError.Reset();
	StartedSeconds = FPlatformTime::Seconds();

	const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 14);
	Overlay = SNew(SOverlay)
		.Visibility(EVisibility::HitTestInvisible)
		+ SOverlay::Slot()
		[
			SNew(SHyperAIStudioAimLayer, InWorld)
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(16.f))
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("GenericWhiteBox"))
			.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.6f))
			.Padding(FMargin(10.f, 6.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(Font).ColorAndOpacity(FLinearColor(1.f, 0.35f, 0.35f))
					.Text_Raw(this, &FHyperAIStudioPlaytestRecorder::ClockText)
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).Font(Font).ColorAndOpacity(FLinearColor::White)
					.Text_Raw(this, &FHyperAIStudioPlaytestRecorder::StepText)
				]
			]
		];
	Client->AddViewportWidgetContent(Overlay.ToSharedRef(), OverlayZOrder);
	Ticker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FHyperAIStudioPlaytestRecorder::Tick), 1.f / Fps);
	return true;
}

void FHyperAIStudioPlaytestRecorder::BeginStep(const FString& Label)
{
	if (!IsRecording())
	{
		return;
	}
	FSpan& Span = Timeline.AddDefaulted_GetRef();
	Span.Label = Label.Left(120);
	Span.Start = Elapsed();
}

void FHyperAIStudioPlaytestRecorder::EndStep(const FString& Status, const FString& Detail)
{
	if (!IsRecording() || Timeline.IsEmpty() || Timeline.Last().End >= 0.0)
	{
		return;
	}
	FSpan& Span = Timeline.Last();
	Span.End = Elapsed();
	Span.Status = Status.Left(32);
	Span.Detail = Detail.Left(160);
}

FText FHyperAIStudioPlaytestRecorder::ClockText() const
{
	const int32 Tenths = FMath::FloorToInt32(Elapsed() * 10.0);
	return FText::FromString(FString::Printf(TEXT("REC %02d:%02d.%d"), Tenths / 600, Tenths / 10 % 60, Tenths % 10));
}

FText FHyperAIStudioPlaytestRecorder::StepText() const
{
	if (Timeline.IsEmpty())
	{
		return FText::FromString(TEXT("starting"));
	}
	const FSpan& Span = Timeline.Last();
	if (Span.End < 0.0)
	{
		return FText::FromString(Span.Label);
	}
	if (Elapsed() - Span.End >= ResultSeconds)
	{
		return FText::FromString(FString::Printf(TEXT("%s: done"), *Span.Label));
	}
	const FString Result = Span.Status == TEXT("completed") ? FString(TEXT("PASS")) : Span.Status.ToUpper();
	return FText::FromString(Span.Detail.IsEmpty() ? FString::Printf(TEXT("%s: %s"), *Span.Label, *Result)
		: FString::Printf(TEXT("%s: %s\n%s"), *Span.Label, *Result, *Span.Detail.Left(100)));
}

bool FHyperAIStudioPlaytestRecorder::Capture()
{
	// The view as a person sees it: the scene, the game's UI and the overlay, which are Slate drawn above the scene
	// and missing from the viewport's own pixels, so the widget is captured whole.
	UGameViewportClient* Client = ViewportClient(World.Get());
	const TSharedPtr<SViewport> Widget = Client ? Client->GetGameViewportWidget() : nullptr;
	TArray<FColor> Pixels;
	FIntVector Size = FIntVector::ZeroValue;
	if (!Widget.IsValid() || !FSlateApplication::IsInitialized()
		|| !FSlateApplication::Get().TakeScreenshot(Widget.ToSharedRef(), Pixels, Size)
		|| Size.X < 1 || Size.Y < 1 || Pixels.Num() != Size.X * Size.Y)
	{
		return false;
	}
	// The window may be resized mid-run; the video keeps the size it started with.
	const int32 Width = Writer.GetWidth();
	const int32 Height = Writer.GetHeight();
	Frame.SetNumUninitialized(Width * Height);
	for (int32 Y = 0; Y < Height; ++Y)
	{
		const FColor* Row = &Pixels[static_cast<int64>(Y) * Size.Y / Height * Size.X];
		FColor* Out = &Frame[Y * Width];
		for (int32 X = 0; X < Width; ++X)
		{
			Out[X] = Row[static_cast<int64>(X) * Size.X / Width];
			Out[X].A = 255;
		}
	}
	return true;
}

bool FHyperAIStudioPlaytestRecorder::Tick(float)
{
	if (!IsRecording())
	{
		return false;
	}
	// A frame that cannot be read keeps the last picture, so the video's clock stays the session's clock.
	if (!Capture() && Frame.IsEmpty())
	{
		return true;
	}
	// A stall repeats the last picture for as long as it lasted, which keeps the timeline's times true.
	// ponytail: unbounded within a run, which playtests cap at 300 s.
	int64 Missing = FMath::FloorToInt64(Elapsed() * Fps) + 1 - Writer.GetFrameCount();
	while (Missing-- > 0)
	{
		if (!Writer.WriteFrame(Frame, WriteError))
		{
			// Keep what was recorded; Stop finishes the file.
			FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
			Ticker.Reset();
			return false;
		}
	}
	return true;
}

FString FHyperAIStudioPlaytestRecorder::Stop(FString& OutSummary)
{
	OutSummary.Reset();
	if (!IsRecording())
	{
		return FString();
	}
	if (Ticker.IsValid())
	{
		// The session's last moment, so the video ends where the run did.
		Tick(0.f);
		FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
		Ticker.Reset();
	}
	if (Overlay.IsValid())
	{
		if (UGameViewportClient* Client = ViewportClient(World.Get()))
		{
			Client->RemoveViewportWidgetContent(Overlay.ToSharedRef());
		}
		Overlay.Reset();
	}
	const double Seconds = static_cast<double>(Writer.GetFrameCount()) / Fps;
	const int64 Frames = Writer.GetFrameCount();
	FString CloseError;
	if (!Writer.Close(CloseError))
	{
		OutSummary = !CloseError.IsEmpty() ? CloseError : !WriteError.IsEmpty() ? WriteError : FString(TEXT("No frame was recorded."));
		return FString();
	}

	TArray<TSharedPtr<FJsonValue>> Steps;
	for (const FSpan& Span : Timeline)
	{
		const TSharedRef<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("label"), Span.Label);
		// Milliseconds into the video, as whole numbers so they read exactly.
		Step->SetNumberField(TEXT("start_ms"), static_cast<double>(FMath::RoundToInt64(Span.Start * 1000.0)));
		Step->SetNumberField(TEXT("end_ms"), static_cast<double>(FMath::RoundToInt64((Span.End < 0.0 ? Seconds : Span.End) * 1000.0)));
		Step->SetStringField(TEXT("status"), Span.End < 0.0 ? TEXT("unfinished") : Span.Status);
		Step->SetStringField(TEXT("detail"), Span.Detail);
		Steps.Add(MakeShared<FJsonValueObject>(Step));
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("video"), FPaths::GetCleanFilename(VideoPath));
	Root->SetNumberField(TEXT("fps"), Fps);
	Root->SetNumberField(TEXT("frames"), static_cast<double>(Frames));
	Root->SetNumberField(TEXT("duration_ms"), static_cast<double>(FMath::RoundToInt64(Seconds * 1000.0)));
	Root->SetArrayField(TEXT("steps"), Steps);
	FString Json;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Json));
	FFileHelper::SaveStringToFile(Json, *(Folder / TEXT("timeline.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	OutSummary = FString::Printf(TEXT("%.1f s at %d fps, %d steps on the timeline in timeline.json beside it."), Seconds, Fps, Timeline.Num());
	return VideoPath;
}
