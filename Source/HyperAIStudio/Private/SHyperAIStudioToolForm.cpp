// Games by Hyper 2026.

#include "SHyperAIStudioToolForm.h"

#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioMarkdown.h"
#include "HyperAIStudioPreviewImage.h"
#include "HyperAIStudioPreviewPanel.h"
#include "IDetailsView.h"
#include "IStructureDetailsView.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "ToolsetRegistry/ToolCallAsyncResultString.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StructOnScope.h"
#include "UObject/UObjectIterator.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioToolForm"

namespace HyperAIStudio::ToolForm
{
	constexpr double TimeoutSeconds = 600.0;
	constexpr int32 MaxImages = 12;

	/** The reflected function behind a registry toolset named Module.ClassName, if it has one. */
	UFunction* FindFunction(const FString& Toolset, const FString& Tool)
	{
		FString Module;
		FString ClassName;
		if (!Toolset.Split(TEXT("."), &Module, &ClassName, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			return nullptr;
		}
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UToolsetDefinition::StaticClass()) && It->GetName() == ClassName)
			{
				return It->FindFunctionByName(FName(*Tool));
			}
		}
		return nullptr;
	}

	FString Pretty(const TSharedPtr<FJsonValue>& Value)
	{
		FString Text;
		FJsonSerializer::Serialize(Value, FString(), TJsonWriterFactory<>::Create(&Text));
		return Text;
	}

	TSharedPtr<FJsonValue> Parse(const FString& Json)
	{
		TSharedPtr<FJsonValue> Value;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Value);
		return Value;
	}

	bool IsPicture(const FString& Text)
	{
		const FString Extension = FPaths::GetExtension(Text).ToLower();
		return Text.Len() < 1024 && (Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("webp"));
	}

	FString FullPath(const FString& Path)
	{
		return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(Path) ? FPaths::ProjectDir() / Path : Path);
	}

	FString StringField(const TSharedPtr<FJsonValue>& Value, const TCHAR* Field)
	{
		FString Text;
		if (Value.IsValid() && Value->Type == EJson::Object)
		{
			Value->AsObject()->TryGetStringField(Field, Text);
		}
		return Text;
	}
}

void SHyperAIStudioToolForm::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::ToolForm;
	Toolset = InArgs._Toolset;
	Tool = InArgs._Tool;
	Function = FindFunction(Toolset, Tool);

	TSharedRef<SWidget> Form = SNullWidget::NullWidget;
	if (Function)
	{
		TArray<FProperty*> Arguments;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Arguments.Add(*It);
			}
		}
		Single = Arguments.Num() == 1 ? CastField<FStructProperty>(Arguments[0]) : nullptr;
		Parameters = Single ? MakeShared<FStructOnScope>(Single->Struct) : MakeShared<FStructOnScope>(Function);
		FPropertyEditorModule& Editor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		FDetailsViewArgs ViewArgs;
		ViewArgs.bAllowSearch = true;
		ViewArgs.bHideSelectionTip = true;
		ViewArgs.bShowOptions = false;
		// A tool's fields carry no Edit flag: they are made for agents, and shown here all the same.
		ViewArgs.bForceHiddenPropertyVisibility = true;
		ViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		Details = Editor.CreateStructureDetailView(ViewArgs, FStructureDetailsViewArgs(), Parameters);
		Details->GetDetailsView()->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& In)
		{
			return !In.Property.HasAnyPropertyFlags(CPF_ReturnParm);
		}));
		Form = Details->GetWidget().ToSharedRef();
	}
	else
	{
		Form = SAssignNew(JsonBox, SMultiLineEditableTextBox)
			.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
			.Text(FText::FromString(TEXT("{}")))
			.HintText(LOCTEXT("JsonHint", "This tool has no reflected fields; write its arguments as JSON."));
	}

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(STextBlock)
			.Text(FText::FromString(Tool))
			.Font(FCoreStyle::GetDefaultFontStyle("Bold", 13))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(Toolset))
			.Font(FAppStyle::GetFontStyle("SmallFont"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 8.0f)
		[
			SNew(STextBlock)
			.Text(FText::FromString(InArgs._Description))
			.AutoWrapText(true)
		]
		+ SVerticalBox::Slot().FillHeight(1.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Vertical)
			+ SSplitter::Slot().Value(0.55f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(4.0f)
				[
					Form
				]
			]
			+ SSplitter::Slot().Value(0.45f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 6.0f, 0.0f, 6.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("Run", "Run"))
						.ToolTipText(LOCTEXT("RunTooltip", "Call the tool with these arguments, the way an agent's MCP call would."))
						.IsEnabled_Lambda([this]() { return !Pending.IsValid(); })
						.OnClicked_Lambda([this]()
						{
							Run();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Approve", "Approve"))
						.ToolTipText(LOCTEXT("ApproveTooltip", "This edit waits for approval, and you are the one who ran it: approve it here."))
						.Visibility_Lambda([this]() { return bAwaitingApproval && !OperationIdOf().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
						.OnClicked_Lambda([this]()
						{
							FString Error;
							const bool bApproved = FHyperAIStudioApprovalGate::Approve(OperationIdOf(), Error);
							Status = bApproved ? FString(TEXT("Approved; it runs now. Its result shows in the Activity panel.")) : Error;
							bStatusIsError = !bApproved;
							bAwaitingApproval = false;
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(6.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SButton)
						.Text(LOCTEXT("CopyCall", "Copy call"))
						.ToolTipText(LOCTEXT("CopyCallTooltip", "Copy this call as an MCP call_tool payload, to paste into a prompt or a script."))
						.OnClicked_Lambda([this]()
						{
							const TSharedRef<FJsonObject> Call = MakeShared<FJsonObject>();
							Call->SetStringField(TEXT("toolset_name"), Toolset);
							Call->SetStringField(TEXT("tool_name"), Tool);
							const TSharedPtr<FJsonValue> Arguments = HyperAIStudio::ToolForm::Parse(BuildArguments());
							Call->SetField(TEXT("arguments"), Arguments.IsValid() ? Arguments : MakeShared<FJsonValueObject>(MakeShared<FJsonObject>()));
							FPlatformApplicationMisc::ClipboardCopy(*HyperAIStudio::ToolForm::Pretty(MakeShared<FJsonValueObject>(Call)));
							Status = TEXT("Copied the call.");
							bStatusIsError = false;
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(10.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.Text_Lambda([this]() { return FText::FromString(Status); })
						.ColorAndOpacity_Lambda([this]()
						{
							return bStatusIsError ? FSlateColor(FLinearColor(1.0f, 0.45f, 0.4f)) : FSlateColor::UseSubduedForeground();
						})
					]
				]
				// The answer as a report, or as the JSON it is.
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 4.0f)
				[
					SNew(SHorizontalBox)
					.Visibility_Lambda([this]() { return Report.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SCheckBox)
						.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
						.IsChecked_Lambda([this]() { return bShowJson ? ECheckBoxState::Unchecked : ECheckBoxState::Checked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState) { bShowJson = false; })
						[
							SNew(STextBlock).Text(LOCTEXT("ReportView", "Report")).Margin(FMargin(8.0f, 1.0f))
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(2.0f, 0.0f, 0.0f, 0.0f)
					[
						SNew(SCheckBox)
						.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
						.IsChecked_Lambda([this]() { return bShowJson ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState) { bShowJson = true; })
						[
							SNew(STextBlock).Text(LOCTEXT("JsonView", "JSON")).Margin(FMargin(8.0f, 1.0f))
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						HyperAIStudio::Markdown::ReportActions(
							[this]() { return Tool; },
							[this]() { return FString::Printf(TEXT("# %s\n\n%s"), *Tool, *Report); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 0.0f, 0.0f, 6.0f)
				[
					SAssignNew(ImageBox, SWrapBox)
					.UseAllottedSize(true)
					.InnerSlotPadding(FVector2D(6.0f, 6.0f))
					.Visibility_Lambda([this]() { return Images.IsEmpty() || !IsJsonShown() ? EVisibility::Collapsed : EVisibility::Visible; })
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
					.Padding(FMargin(10.0f, 8.0f))
					.Visibility_Lambda([this]() { return IsJsonShown() ? EVisibility::Collapsed : EVisibility::Visible; })
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot()
						[
							SNew(SHyperAIStudioMarkdown)
							.Markdown_Lambda([this]() { return Report; })
						]
					]
				]
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					SNew(SMultiLineEditableTextBox)
					.IsReadOnly(true)
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
					.HintText(LOCTEXT("AnswerHint", "The tool's answer shows here."))
					.Text_Lambda([this]() { return FText::FromString(Answer); })
					.Visibility_Lambda([this]() { return IsJsonShown() ? EVisibility::Visible : EVisibility::Collapsed; })
				]
			]
		]
	];
}

void SHyperAIStudioToolForm::OpenWindow(const FString& Toolset, const FString& Tool, const FString& Description)
{
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(LOCTEXT("WindowTitle", "Run {0}"), FText::FromString(Tool)))
		.ClientSize(FVector2D(820.0f, 900.0f))
		.SupportsMaximize(true)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
			.Padding(10.0f)
			[
				SNew(SHyperAIStudioToolForm).Toolset(Toolset).Tool(Tool).Description(Description)
			]
		];
	FSlateApplication::Get().AddWindow(Window);
}

FString SHyperAIStudioToolForm::BuildArguments() const
{
	using namespace HyperAIStudio::ToolForm;
	if (!Function || !Parameters.IsValid())
	{
		return JsonBox.IsValid() ? JsonBox->GetText().ToString() : FString(TEXT("{}"));
	}
	const TSharedRef<FJsonObject> Arguments = MakeShared<FJsonObject>();
	if (Single)
	{
		const TSharedRef<FJsonObject> Fields = MakeShared<FJsonObject>();
		FJsonObjectConverter::UStructToJsonObject(Single->Struct, Parameters->GetStructMemory(), Fields);
		Arguments->SetObjectField(FJsonObjectConverter::StandardizeCase(Single->GetName()), Fields);
		return Pretty(MakeShared<FJsonValueObject>(Arguments));
	}
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		if (It->HasAnyPropertyFlags(CPF_ReturnParm))
		{
			continue;
		}
		// The registry names arguments as the JSON converter does: the parameter's name with its first letter lowered.
		if (const TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(*It, It->ContainerPtrToValuePtr<void>(Parameters->GetStructMemory())))
		{
			Arguments->SetField(FJsonObjectConverter::StandardizeCase(It->GetName()), Value);
		}
	}
	return Pretty(MakeShared<FJsonValueObject>(Arguments));
}

bool SHyperAIStudioToolForm::SetArguments(const FString& Json, FString& OutError)
{
	using namespace HyperAIStudio::ToolForm;
	const TSharedPtr<FJsonValue> Value = Parse(Json);
	if (!Value.IsValid() || Value->Type != EJson::Object)
	{
		OutError = TEXT("The arguments are not a JSON object.");
		return false;
	}
	if (!Function || !Parameters.IsValid())
	{
		if (JsonBox.IsValid())
		{
			JsonBox->SetText(FText::FromString(Pretty(Value)));
		}
		return true;
	}
	const TSharedPtr<FJsonObject> Object = Value->AsObject();
	if (Single)
	{
		const TSharedPtr<FJsonObject>* Fields = nullptr;
		if (Object->TryGetObjectField(FJsonObjectConverter::StandardizeCase(Single->GetName()), Fields) && Fields
			&& !FJsonObjectConverter::JsonObjectToUStruct(Fields->ToSharedRef(), Single->Struct, Parameters->GetStructMemory()))
		{
			OutError = FString::Printf(TEXT("The %s does not fit the tool's %s."), *Single->GetName(), *Single->Struct->GetName());
			return false;
		}
		if (Details.IsValid())
		{
			Details->SetStructureData(Parameters);
		}
		return true;
	}
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		const TSharedPtr<FJsonValue> Field = It->HasAnyPropertyFlags(CPF_ReturnParm) ? nullptr
			: Object->TryGetField(FJsonObjectConverter::StandardizeCase(It->GetName()));
		if (Field.IsValid() && !FJsonObjectConverter::JsonValueToUProperty(Field, *It, It->ContainerPtrToValuePtr<void>(Parameters->GetStructMemory())))
		{
			OutError = FString::Printf(TEXT("%s does not fit the tool's %s."), *It->GetName(), *It->GetCPPType());
			return false;
		}
	}
	if (Details.IsValid())
	{
		Details->SetStructureData(Parameters);
	}
	return true;
}

FString SHyperAIStudioToolForm::OperationIdOf() const
{
	using namespace HyperAIStudio::ToolForm;
	const TSharedPtr<FJsonValue> Arguments = Parse(BuildArguments());
	FString Id = StringField(Arguments, TEXT("operationId"));
	if (Id.IsEmpty() && Arguments.IsValid() && Arguments->Type == EJson::Object)
	{
		const TSharedPtr<FJsonObject>* Request = nullptr;
		if (Arguments->AsObject()->TryGetObjectField(TEXT("request"), Request) && Request)
		{
			(*Request)->TryGetStringField(TEXT("operationId"), Id);
		}
	}
	return Id;
}

void SHyperAIStudioToolForm::Run()
{
	using namespace HyperAIStudio::ToolForm;
	if (Pending.IsValid())
	{
		return;
	}
	const FString Arguments = BuildArguments();
	const TSharedPtr<FJsonValue> Parsed = Parse(Arguments);
	if (!Parsed.IsValid() || Parsed->Type != EJson::Object)
	{
		Status = TEXT("The arguments are not a JSON object.");
		bStatusIsError = true;
		return;
	}
	Answer.Reset();
	Report.Reset();
	Images.Reset();
	RebuildImages();
	bAwaitingApproval = false;
	Status = TEXT("Running...");
	bStatusIsError = false;
	StartedAt = FPlatformTime::Seconds();
	Pending.Reset(UToolsetRegistry::ExecuteTool(Toolset, Tool, Arguments));
}

void SHyperAIStudioToolForm::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	using namespace HyperAIStudio::ToolForm;
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	if (!Pending.IsValid())
	{
		return;
	}
	if (!Pending->bIsComplete)
	{
		if (FPlatformTime::Seconds() - StartedAt > TimeoutSeconds)
		{
			Status = TEXT("No answer after ten minutes. The call may still finish; its result would show in the Activity panel.");
			bStatusIsError = true;
			Pending.Reset();
		}
		return;
	}
	if (!Pending->Error.IsEmpty())
	{
		Status = Pending->Error;
		bStatusIsError = true;
	}
	else
	{
		// The registry answers {"returnValue": ...}; the tool's own answer is inside.
		TSharedPtr<FJsonValue> Value = Parse(Pending->Value);
		const TSharedPtr<FJsonObject>* Wrapped = nullptr;
		if (Value.IsValid() && Value->Type == EJson::Object && Value->AsObject()->TryGetObjectField(TEXT("returnValue"), Wrapped) && Wrapped)
		{
			Value = MakeShared<FJsonValueObject>(*Wrapped);
		}
		Answer = Value.IsValid() ? Pretty(Value) : Pending->Value;
		Report = HyperAIStudio::Markdown::FromJson(Answer);
		FindImages(Value);
		RebuildImages();
		const FString ToolStatus = StringField(Value, TEXT("status"));
		const FString Diagnostic = StringField(Value, TEXT("diagnostic"));
		bAwaitingApproval = ToolStatus == TEXT("awaiting_user_approval");
		Status = FString::Printf(TEXT("Answered in %.1f s%s%s"), FPlatformTime::Seconds() - StartedAt,
			ToolStatus.IsEmpty() ? TEXT("") : *(TEXT(": ") + ToolStatus), Diagnostic.IsEmpty() ? TEXT(".") : *(TEXT(". ") + Diagnostic.Left(300)));
		bStatusIsError = false;
	}
	Pending.Reset();
}

void SHyperAIStudioToolForm::FindImages(const TSharedPtr<FJsonValue>& Value)
{
	using namespace HyperAIStudio::ToolForm;
	if (!Value.IsValid() || Images.Num() >= MaxImages)
	{
		return;
	}
	switch (Value->Type)
	{
	case EJson::String:
	{
		const FString Text = Value->AsString();
		if (IsPicture(Text) && IFileManager::Get().FileExists(*FullPath(Text)))
		{
			Images.AddUnique(FullPath(Text));
		}
		break;
	}
	case EJson::Array:
		for (const TSharedPtr<FJsonValue>& Entry : Value->AsArray())
		{
			FindImages(Entry);
		}
		break;
	case EJson::Object:
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Value->AsObject()->Values)
		{
			FindImages(Entry.Value);
		}
		break;
	default:
		break;
	}
}

void SHyperAIStudioToolForm::RebuildImages()
{
	if (!ImageBox.IsValid())
	{
		return;
	}
	ImageBox->ClearChildren();
	Thumbnails.Reset();
	for (const FString& Path : Images)
	{
		const TSharedPtr<FHyperAIStudioPreviewImage> Image = Thumbnails.Add_GetRef(MakeShared<FHyperAIStudioPreviewImage>());
		ImageBox->AddSlot()
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ToolTipText(FText::Format(LOCTEXT("ImageMatchTooltip", "{0}\nClick to open it in the Match page."), FText::FromString(Path)))
			.OnClicked_Lambda([Path]()
			{
				FHyperAIStudioPreviewPanel::Open(NAME_None, Path);
				return FReply::Handled();
			})
			[
				SNew(SBox).WidthOverride(176.0f).HeightOverride(110.0f)
				[
					SNew(SScaleBox)
					.Stretch(EStretch::ScaleToFit)
					[
						SNew(SImage).Image_Lambda([Image, Path]() { return Image->Get(Path); })
					]
				]
			]
		];
	}
}

#undef LOCTEXT_NAMESPACE
