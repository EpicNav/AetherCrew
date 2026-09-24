// Games by Hyper 2026.

#include "SHyperAIStudioSettingsPage.h"

#include "HyperAIStudioSettings.h"
#include "HyperAIStudioWidgets.h"
#include "IDetailsView.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "SHyperAIStudioWindow.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/StyleDefaults.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "SHyperAIStudioSettingsPage"

namespace HyperAIStudio::SettingsPage
{
	const FName Setup(TEXT("Setup"));
	const FName Agents(TEXT("Agents"));
	const FName Servers(TEXT("Servers"));
	const FName Images(TEXT("Images"));
	const FName Preferences(TEXT("Preferences"));
	const FName Docs(TEXT("Docs"));

	/** A property's category, or its outermost parent's: struct members have none of their own. */
	FString CategoryOf(const FPropertyAndParent& PropertyAndParent)
	{
		FString Category = PropertyAndParent.Property.GetMetaData(TEXT("Category"));
		for (const FProperty* Parent : PropertyAndParent.ParentProperties)
		{
			const FString ParentCategory = Parent ? Parent->GetMetaData(TEXT("Category")) : FString();
			if (!ParentCategory.IsEmpty())
			{
				Category = ParentCategory;
			}
		}
		return Category;
	}
}

void SHyperAIStudioSettingsPage::Construct(const FArguments& InArgs)
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::SettingsPage;
	struct FEntry
	{
		FName Id;
		FText Label;
		FText Description;
	};
	const FEntry Entries[] = {
		{Setup, LOCTEXT("Setup", "Setup"), LOCTEXT("SetupDescription", "Unreal MCP, agent files, and what is missing.")},
		{Agents, LOCTEXT("Agents", "Agents"), LOCTEXT("AgentsDescription", "The agent CLIs HyperAI works with, and their files.")},
		{Servers, LOCTEXT("Servers", "MCP servers"), LOCTEXT("ServersDescription", "The Unreal MCP server, and extra servers agents can use.")},
		{Images, LOCTEXT("Images", "Image services"), LOCTEXT("ImagesDescription", "Image models, and the services and keys they need.")},
		{Preferences, LOCTEXT("Preferences", "Preferences"), LOCTEXT("PreferencesDescription", "Every HyperAI setting, in one place.")},
		{Docs, LOCTEXT("Docs", "Documentation"), LOCTEXT("DocsDescription", "Guides, the agent instructions, and troubleshooting.")}};
	const TSharedRef<SVerticalBox> List = SNew(SVerticalBox);
	for (const FEntry& Entry : Entries)
	{
		Sections.Add(Entry.Id);
		List->AddSlot().AutoHeight().Padding(FMargin(0.0f, 1.0f))
		[
			BuildSectionButton(Entry.Id, Entry.Label, Entry.Description)
		];
	}
	ChildSlot
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox)
			.WidthOverride(236.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(20.0f, 20.0f, 16.0f, 12.0f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Title", "Settings"))
					.TextStyle(&Style(), "HyperAIStudio.Text.PageTitle")
				]
				+ SVerticalBox::Slot().FillHeight(1.0f).Padding(FMargin(10.0f, 0.0f))
				[
					List
				]
			]
		]
		+ SHorizontalBox::Slot().AutoWidth()
		[
			SNew(SBox).WidthOverride(1.0f)
			[
				SNew(SImage).Image(Style().GetBrush("HyperAIStudio.Divider"))
			]
		]
		+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(24.0f, 20.0f, 12.0f, 0.0f))
		[
			SAssignNew(Switcher, SWidgetSwitcher)
		]
	];
	ShowSection(Setup);
}

TSharedRef<SWidget> SHyperAIStudioSettingsPage::BuildSectionButton(const FName Id, const FText& Label, const FText& Description)
{
	using namespace HyperAIStudio::UI;
	return SNew(SButton)
		.ButtonStyle(&Style(), "HyperAIStudio.NavItem")
		.ContentPadding(0.0f)
		.ToolTipText(Description)
		.OnClicked_Lambda([this, Id]()
		{
			ShowSection(Id);
			return FReply::Handled();
		})
		[
			SNew(SBorder)
			.BorderImage_Lambda([this, Id]() { return Section == Id ? HyperAIStudio::UI::Style().GetBrush("HyperAIStudio.NavItem.Selected") : FStyleDefaults::GetNoBrush(); })
			.Padding(FMargin(12.0f, 7.0f))
			[
				SNew(STextBlock)
				.Text(Label)
				.TextStyle(&Style(), "HyperAIStudio.Text.Nav")
				.Font_Lambda([this, Id]() { return FCoreStyle::GetDefaultFontStyle(Section == Id ? "Bold" : "Regular", 10); })
				.ColorAndOpacity_Lambda([this, Id]() { return Section == Id ? FStyleColors::ForegroundHover : FStyleColors::Foreground; })
			]
		];
}

void SHyperAIStudioSettingsPage::ShowSection(const FName InSection)
{
	if (!Sections.Contains(InSection) || !Switcher.IsValid())
	{
		return;
	}
	Section = InSection;
	// Each section is built the first time it is shown: the agent and server sections read the machine when they are made.
	if (!Built.Contains(Section))
	{
		Built.Add(Section, Switcher->GetNumWidgets());
		Switcher->AddSlot()
		[
			BuildSection(Section)
		];
	}
	Switcher->SetActiveWidgetIndex(Built[Section]);
}

TSharedRef<SWidget> SHyperAIStudioSettingsPage::BuildSection(const FName Id)
{
	using namespace HyperAIStudio::UI;
	using namespace HyperAIStudio::SettingsPage;
	if (Id == Images)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 12.0f, 14.0f))
			[
				PageHeader(LOCTEXT("ImagesTitle", "Image services"),
					LOCTEXT("ImagesBody", "The models the Images page and agents draw with. Keys are read from environment variables and never stored."))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				BuildDetails([](const FString& Category)
				{
					return Category.StartsWith(TEXT("External Services")) || Category.StartsWith(TEXT("Image Generation"));
				})
			];
	}
	if (Id == Preferences)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 0.0f, 12.0f, 14.0f))
			[
				PageHeader(LOCTEXT("PreferencesTitle", "Preferences"),
					LOCTEXT("PreferencesBody", "Every HyperAI setting. Changes save as you make them."))
			]
			+ SVerticalBox::Slot().FillHeight(1.0f)
			[
				BuildDetails(nullptr)
			];
	}
	const int32 Legacy = Id == Setup ? 0 : Id == Agents ? 1 : Id == Servers ? 2 : 3;
	return SNew(SHyperAIStudioWindow).Section(Legacy);
}

TSharedRef<SWidget> SHyperAIStudioSettingsPage::BuildDetails(TFunction<bool(const FString& Category)> Shows) const
{
	FPropertyEditorModule& PropertyEditor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	FDetailsViewArgs Args;
	Args.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	Args.bAllowSearch = true;
	Args.bHideSelectionTip = true;
	Args.bUpdatesFromSelection = false;
	Args.bLockable = false;
	Args.bShowPropertyMatrixButton = false;
	Args.bAllowFavoriteSystem = false;
	const TSharedRef<IDetailsView> View = PropertyEditor.CreateDetailView(Args);
	if (Shows)
	{
		View->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([Shows](const FPropertyAndParent& PropertyAndParent)
		{
			return Shows(HyperAIStudio::SettingsPage::CategoryOf(PropertyAndParent));
		}));
	}
	View->SetObject(GetMutableDefault<UHyperAIStudioSettings>());
	View->OnFinishedChangingProperties().AddLambda([](const FPropertyChangedEvent&)
	{
		GetMutableDefault<UHyperAIStudioSettings>()->SaveConfig();
	});
	return View;
}

#undef LOCTEXT_NAMESPACE
