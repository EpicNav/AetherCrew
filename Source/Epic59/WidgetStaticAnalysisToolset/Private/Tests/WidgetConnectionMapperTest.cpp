// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "WidgetConnectionMapper.h"
#include "WidgetConnectionMapperTestFixtures.h"
#include "WidgetStaticAnalysisSchemaTestFixtures.h"
#include "WidgetStaticAnalysisSchemaTestFlags.h"
#include "WidgetStaticAnalysisToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "DataValidationSettings.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/PackageName.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMPropertyPath.h"
#include "MVVMViewModelBase.h"
#include "Types/MVVMFieldVariant.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

BEGIN_DEFINE_SPEC(
	FWidgetConnectionMapperTest,
	"AIAssistant.Toolsets.WidgetStaticAnalysisToolset.ConnectionMap",
	WidgetStaticAnalysisSchemaTest::Flags)

UWidgetBlueprint* TestBP = nullptr;
int32 TestCounter = 0;

/** GEditor's MVVM editor subsystem, or nullptr. Every It that needs it checks and early-outs. */
UMVVMEditorSubsystem* GetMVVMSubsystem();

/** Creates and compiles a fresh Blueprint ViewModel class parented to
 *  UWidgetConnectionMapperTestViewModel, so it has a real on-disk-saveable package identity
 *  (unlike a plain native ViewModel class, which has no referencers to find). Returns nullptr on
 *  failure. */
UBlueprint* MakeViewModelBlueprint(const TCHAR* NamePrefix);

/** Saves Asset's package to a real file under WidgetStaticAnalysisSchemaTest::TestMountPoint and
 *  appends the saved path to OutSavedFilePaths, so a later IAssetRegistry::ScanFilesSynchronous
 *  call can pick up its dependency edges. An in-memory-only package has none. */
void SaveTestPackage(UObject* Asset, TArray<FString>& OutSavedFilePaths);

/** Builds a fresh TestBP with one Connected LegacyPropertyBinding row and one child Button
 *  contributing several Unconnected Delegate rows. Shared setup for tests that just need at least
 *  one Connected and one Unconnected row present. */
void BuildMixedConnectionsWidget();

END_DEFINE_SPEC(FWidgetConnectionMapperTest)

UMVVMEditorSubsystem* FWidgetConnectionMapperTest::GetMVVMSubsystem()
{
	return GEditor ? GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>() : nullptr;
}

UBlueprint* FWidgetConnectionMapperTest::MakeViewModelBlueprint(const TCHAR* NamePrefix)
{
	const FString Name = FString::Printf(TEXT("%s%d"), NamePrefix, TestCounter++);
	const FString PackagePath = WidgetStaticAnalysisSchemaTest::TestMountPoint / Name;
	UPackage* Package = CreatePackage(*PackagePath);

	UBlueprint* ViewModelBP = FKismetEditorUtilities::CreateBlueprint(
		UWidgetConnectionMapperTestViewModel::StaticClass(), Package, FName(*Name), BPTYPE_Normal,
		UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
	if (ViewModelBP)
	{
		ViewModelBP->SetFlags(RF_Public | RF_Standalone);
		FKismetEditorUtilities::CompileBlueprint(ViewModelBP);
	}
	return ViewModelBP;
}

void FWidgetConnectionMapperTest::SaveTestPackage(UObject* Asset, TArray<FString>& OutSavedFilePaths)
{
	if (!Asset)
	{
		return;
	}

	// Avoids a spurious ValidateOnSave ensure on transient test assets.
	GetMutableDefault<UDataValidationSettings>()->bValidateOnSave = false;

	Asset->SetFlags(RF_Public | RF_Standalone);
	UPackage* Package = Asset->GetOutermost();
	Package->MarkPackageDirty();

	const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	if (UPackage::SavePackage(Package, Asset, *Filename, SaveArgs))
	{
		OutSavedFilePaths.Add(Filename);
	}
}

void FWidgetConnectionMapperTest::BuildMixedConnectionsWidget()
{
	TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);

	UVerticalBox* Stack = TestBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("Root"));
	TestBP->WidgetTree->RootWidget = Stack;

	UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
	Stack->AddChildToVerticalBox(Text);

	UButton* Button = TestBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Btn"));
	Stack->AddChildToVerticalBox(Button);

	FDelegateEditorBinding LegacyBinding;
	LegacyBinding.ObjectName = TEXT("Solo");
	LegacyBinding.PropertyName = TEXT("Text");
	LegacyBinding.FunctionName = TEXT("GetSoloText");
	LegacyBinding.Kind = EBindingKind::Function;
	TestBP->Bindings.Add(LegacyBinding);
}

void FWidgetConnectionMapperTest::Define()
{
	BeforeEach([this]()
	{
		WidgetStaticAnalysisSchemaTest::RegisterTestMountPoint();
	});
	AfterEach([this]()
	{
		TestBP = nullptr;
		WidgetStaticAnalysisSchemaTest::UnregisterTestMountPoint();
	});

	Describe("GetWidgetConnectionMap VMFieldBinding rows", [this]()
	{
		It("an enabled, type-compatible VM field binding reports Connected", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* IntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("IntDestination"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			TestNotNull(TEXT("IntDestination property exists"), IntDestinationProperty);
			if (!TestFieldProperty || !IntDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(IntDestinationProperty));

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Connected"), Row->Status == EWidgetConnectionStatus::Connected);
			TestEqual(TEXT("widget"), Row->Widget, FString(TEXT("Solo")));
		});

		It("a VM field binding whose destination is a SelfContext class member variable reports the handler name in TargetVariable, not a blank Widget", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);

			FEdGraphPinType IntPinType;
			IntPinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			TestTrue(TEXT("Blueprint-only int member variable added to TestBP"),
				FBlueprintEditorUtils::AddMemberVariable(TestBP, TEXT("MyIntHandler"), IntPinType));
			FKismetEditorUtilities::CompileBlueprint(TestBP);
			TestNotNull(TEXT("TestBP has a GeneratedClass"), TestBP->GeneratedClass.Get());
			if (!TestBP->GeneratedClass)
			{
				return;
			}

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* HandlerProperty = TestBP->GeneratedClass->FindPropertyByName(TEXT("MyIntHandler"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			TestNotNull(TEXT("MyIntHandler property exists on the generated class"), HandlerProperty);
			if (!TestFieldProperty || !HandlerProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetSelfContext();
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(HandlerProperty));

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestEqual(TEXT("Widget is empty since the destination is not a widget-tree property"), Row->Widget, FString());
			TestEqual(TEXT("TargetVariable names the SelfContext handler"), Row->TargetVariable, FString(TEXT("MyIntHandler")));
		});

		It("a disabled VM field binding reports Unconnected", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* IntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("IntDestination"));
			if (!TestFieldProperty || !IntDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(IntDestinationProperty));
			Binding.bEnabled = false;

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Unconnected"), Row->Status == EWidgetConnectionStatus::Unconnected);
		});

		It("a VM field binding whose ViewModel context has a missing/unresolved class reports Misconfigured/Unknown, not Connected", [this]()
		{
			// Null ViewModel class only exists live on a dirty Blueprint; set via raw field write to avoid a recompile.
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			UMVVMBlueprintView* View = Subsystem->RequestView(TestBP);
			TestNotNull(TEXT("view"), View);
			if (!View)
			{
				return;
			}
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* IntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("IntDestination"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			TestNotNull(TEXT("IntDestination property exists"), IntDestinationProperty);
			if (!TestFieldProperty || !IntDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(IntDestinationProperty));

			FMVVMBlueprintViewModelContext* MutableContext = View->FindViewModel(ViewModelId);
			TestNotNull(TEXT("context found for mutation"), MutableContext);
			if (!MutableContext)
			{
				return;
			}
			MutableContext->NotifyFieldValueClass = nullptr;
			TestNull(TEXT("precondition: context's ViewModel class is now unresolved"), MutableContext->GetViewModelClass());

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Misconfigured, not a false-positive Connected"), Row->Status == EWidgetConnectionStatus::Misconfigured);
			TestTrue(TEXT("fixability is Unknown, since there is no type to check without a resolved ViewModel class"), Row->Fixability == EWidgetConnectionFixability::Unknown);
			TestTrue(TEXT("fix names the unresolved ViewModel class"), Row->Fix.Contains(TEXT("ViewModel class")));
		});

		It("an unbound VM field reports Unconnected via the reverse fallback pass", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Unconnected"), Row->Status == EWidgetConnectionStatus::Unconnected);
			TestEqual(TEXT("declared-in names the searched scope"), Row->DeclaredIn, FString(TEXT("MVVM View")));
			TestTrue(TEXT("no destination widget for an unbound field"), Row->Widget.IsEmpty());
		});

		It("a mismatched-type VM field binding with an available conversion function reports Misconfigured/Fixable", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* TextDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("TextDestination"));
			if (!TestFieldProperty || !TextDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TextDestinationProperty));

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Misconfigured"), Row->Status == EWidgetConnectionStatus::Misconfigured);
			TestTrue(TEXT("fixability is Fixable"), Row->Fixability == EWidgetConnectionFixability::Fixable);
			TestTrue(TEXT("fix names a conversion function"), Row->Fix.Contains(TEXT("conversion function")));
		});

		It("a mismatched-type VM field binding with no available conversion function reports Misconfigured/Unbridgeable", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* ExoticDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("ExoticDestination"));
			if (!TestFieldProperty || !ExoticDestinationProperty)
			{
				return;
			}

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(ExoticDestinationProperty));

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Misconfigured"), Row->Status == EWidgetConnectionStatus::Misconfigured);
			TestTrue(TEXT("fixability is Unbridgeable"), Row->Fixability == EWidgetConnectionFixability::Unbridgeable);
			TestTrue(TEXT("fix carries the allowlist caveat"), Row->Fix.Contains(TEXT("allowlist")));
		});

		It("a type-compatible VM field binding IsBindingValid still rejects for a non-type reason reports Misconfigured/Unknown", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			FProperty* ReadOnlyIntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("ReadOnlyIntDestination"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			TestNotNull(TEXT("ReadOnlyIntDestination property exists"), ReadOnlyIntDestinationProperty);
			if (!TestFieldProperty || !ReadOnlyIntDestinationProperty)
			{
				return;
			}
			TestTrue(TEXT("precondition: destination is CPF_BlueprintReadOnly"), ReadOnlyIntDestinationProperty->HasAnyPropertyFlags(CPF_BlueprintReadOnly));

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(ReadOnlyIntDestinationProperty));

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Misconfigured"), Row->Status == EWidgetConnectionStatus::Misconfigured);
			TestTrue(TEXT("fixability is Unknown since IsBindingValid rejected it for a non-type reason"), Row->Fixability == EWidgetConnectionFixability::Unknown);
			TestTrue(TEXT("fix explains the rejection isn't a type mismatch"), Row->Fix.Contains(TEXT("other than a type mismatch")));
		});

		It("a VM field binding whose destination field path names a property the widget class doesn't actually have reports Misconfigured/Unbridgeable", [this]()
		{
			// AppendPropertyPath doesn't check the owning class, so a wrong-class leaf still resolves.
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Widget = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Widget;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelId = Subsystem->AddViewModel(TestBP, UWidgetConnectionMapperTestViewModel::StaticClass());

			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			TestNotNull(TEXT("TestField property exists on the ViewModel"), TestFieldProperty);
			if (!TestFieldProperty)
			{
				return;
			}
			TestNull(TEXT("TestField does not exist on the widget class (precondition for this test)"),
				UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("TestField")));

			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelId);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::VMFieldBinding && Entry.ConnectionName == TEXT("TestField");
			});
			TestNotNull(TEXT("TestField row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Misconfigured"), Row->Status == EWidgetConnectionStatus::Misconfigured);
			TestTrue(TEXT("fixability is Unbridgeable"), Row->Fixability == EWidgetConnectionFixability::Unbridgeable);
			TestTrue(TEXT("fix names the destination field"), Row->Fix.Contains(TEXT("destination field")));
			TestTrue(TEXT("fix explains the field doesn't resolve to a live property"), Row->Fix.Contains(TEXT("does not resolve to a live property")));
		});
	});

	Describe("GetWidgetConnectionMap EventBinding rows", [this]()
	{
		It("an enabled MVVM event binding reports Connected with KindDetail=mvvm", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UButton* Button = TestBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Btn"));
			TestBP->WidgetTree->RootWidget = Button;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			UMVVMBlueprintView* View = Subsystem->RequestView(TestBP);
			TestNotNull(TEXT("view"), View);
			if (!View)
			{
				return;
			}
			// Bypasses AddEvent's bAllowBindingEvent project-setting gate.
			UMVVMBlueprintViewEvent* Event = View->AddDefaultEvent();
			TestNotNull(TEXT("event created"), Event);
			if (!Event)
			{
				return;
			}

			FMulticastDelegateProperty* OnClickedProperty = CastField<FMulticastDelegateProperty>(UButton::StaticClass()->FindPropertyByName(TEXT("OnClicked")));
			TestNotNull(TEXT("UButton::OnClicked exists"), OnClickedProperty);
			if (!OnClickedProperty)
			{
				return;
			}
			FMVVMBlueprintPropertyPath EventPath;
			EventPath.SetWidgetName(TEXT("Btn"));
			EventPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(OnClickedProperty));
			Event->SetEventPath(EventPath);

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::EventBinding && Entry.ConnectionName == TEXT("OnClicked");
			});
			TestNotNull(TEXT("OnClicked event-binding row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Connected"), Row->Status == EWidgetConnectionStatus::Connected);
			TestEqual(TEXT("kind detail"), Row->KindDetail, FString(TEXT("mvvm")));
			TestEqual(TEXT("widget"), Row->Widget, FString(TEXT("Btn")));
		});

		It("a classic Bind-Event-to-X node reports Connected with KindDetail=blueprint_bind", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UButton* Button = TestBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Btn"));
			TestBP->WidgetTree->RootWidget = Button;
			Button->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			FObjectProperty* ButtonProperty = CastField<FObjectProperty>(TestBP->GeneratedClass->FindPropertyByName(TEXT("Btn")));
			FMulticastDelegateProperty* OnClickedProperty = CastField<FMulticastDelegateProperty>(UButton::StaticClass()->FindPropertyByName(TEXT("OnClicked")));
			TestNotNull(TEXT("Btn variable property"), ButtonProperty);
			TestNotNull(TEXT("UButton::OnClicked exists"), OnClickedProperty);
			if (!ButtonProperty || !OnClickedProperty)
			{
				return;
			}

			UK2Node_ComponentBoundEvent* BoundEvent = NewObject<UK2Node_ComponentBoundEvent>(EventGraph);
			BoundEvent->InitializeComponentBoundEventParams(ButtonProperty, OnClickedProperty);
			BoundEvent->CreateNewGuid();
			EventGraph->AddNode(BoundEvent, /*bUserAction=*/false, /*bSelectNewNode=*/false);
			BoundEvent->AllocateDefaultPins();

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::EventBinding && Entry.KindDetail == TEXT("blueprint_bind");
			});
			TestNotNull(TEXT("blueprint_bind event-binding row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Connected"), Row->Status == EWidgetConnectionStatus::Connected);
			TestEqual(TEXT("widget"), Row->Widget, FString(TEXT("Btn")));
		});
	});

	Describe("GetWidgetConnectionMap Delegate rows", [this]()
	{
		It("a wired AddDelegate registration reports Connected", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UButton* Button = TestBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Btn"));
			TestBP->WidgetTree->RootWidget = Button;
			Button->bIsVariable = true;
			FKismetEditorUtilities::CompileBlueprint(TestBP);

			UEdGraph* EventGraph = TestBP->UbergraphPages.Num() > 0 ? ToRawPtr(TestBP->UbergraphPages[0]) : nullptr;
			TestNotNull(TEXT("EventGraph"), EventGraph);
			if (!EventGraph)
			{
				return;
			}

			UK2Node_VariableGet* ButtonGet = NewObject<UK2Node_VariableGet>(EventGraph);
			ButtonGet->VariableReference.SetSelfMember(TEXT("Btn"));
			ButtonGet->CreateNewGuid();
			EventGraph->AddNode(ButtonGet, /*bUserAction=*/false, /*bSelectNewNode=*/false);
			ButtonGet->AllocateDefaultPins();

			FMulticastDelegateProperty* OnClickedProperty = CastField<FMulticastDelegateProperty>(UButton::StaticClass()->FindPropertyByName(TEXT("OnClicked")));
			TestNotNull(TEXT("UButton::OnClicked exists"), OnClickedProperty);
			if (!OnClickedProperty)
			{
				return;
			}
			UK2Node_AddDelegate* AddDelegateNode = NewObject<UK2Node_AddDelegate>(EventGraph);
			AddDelegateNode->SetFromProperty(OnClickedProperty, /*bSelfContext=*/false, UButton::StaticClass());
			AddDelegateNode->CreateNewGuid();
			EventGraph->AddNode(AddDelegateNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
			AddDelegateNode->AllocateDefaultPins();

			UEdGraphPin* SelfPin = AddDelegateNode->FindPin(UEdGraphSchema_K2::PN_Self);
			TestNotNull(TEXT("AddDelegate has a self pin"), SelfPin);
			if (SelfPin)
			{
				SelfPin->MakeLinkTo(ButtonGet->GetValuePin());
			}

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::Delegate && Entry.Widget == TEXT("Btn") && Entry.ConnectionName == TEXT("OnClicked");
			});
			TestNotNull(TEXT("OnClicked delegate row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Connected"), Row->Status == EWidgetConnectionStatus::Connected);
		});

		It("a widget delegate with no listener reports Unconnected as its own disclosed, not-broken, outward dispatcher", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UButton* Button = TestBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("Btn"));
			TestBP->WidgetTree->RootWidget = Button;

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::Delegate && Entry.Widget == TEXT("Btn") && Entry.ConnectionName == TEXT("OnClicked");
			});
			TestNotNull(TEXT("OnClicked delegate row found"), Row);
			if (!Row)
			{
				return;
			}
			TestTrue(TEXT("status is Unconnected"), Row->Status == EWidgetConnectionStatus::Unconnected);
			TestEqual(TEXT("declared-in names the searched scope"), Row->DeclaredIn, FString(TEXT("Widget's own graphs")));
		});
	});

	Describe("GetWidgetConnectionMap LegacyPropertyBinding rows", [this]()
	{
		It("a legacy property binding reports Connected", [this]()
		{
			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UTextBlock* Text = TestBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Text;

			FDelegateEditorBinding LegacyBinding;
			LegacyBinding.ObjectName = TEXT("Solo");
			LegacyBinding.PropertyName = TEXT("Text");
			LegacyBinding.FunctionName = TEXT("GetSoloText");
			LegacyBinding.Kind = EBindingKind::Function;
			TestBP->Bindings.Add(LegacyBinding);

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);

			const FWidgetConnectionEntry* Row = Result.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
			{
				return Entry.Kind == EWidgetConnectionKind::LegacyPropertyBinding;
			});
			TestNotNull(TEXT("legacy property binding row found"), Row);
			if (!Row)
			{
				return;
			}
			TestEqual(TEXT("widget"), Row->Widget, FString(TEXT("Solo")));
			TestEqual(TEXT("connection name"), Row->ConnectionName, FString(TEXT("Text")));
			TestTrue(TEXT("status is Connected"), Row->Status == EWidgetConnectionStatus::Connected);
		});
	});

	Describe("GetWidgetConnectionMap cross-Kind invariants", [this]()
	{
		It("pre-sorts Unconnected/Misconfigured rows before Connected rows", [this]()
		{
			BuildMixedConnectionsWidget();

			const FWidgetConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetWidgetConnectionMap(TestBP);
			TestTrue(TEXT("has at least one Connected row"), Result.ConnectedCount > 0);
			TestTrue(TEXT("has at least one Unconnected row"), Result.UnconnectedCount > 0);

			bool bSeenConnected = false;
			bool bOrderOk = true;
			for (const FWidgetConnectionEntry& Entry : Result.Connections)
			{
				if (Entry.Status == EWidgetConnectionStatus::Connected)
				{
					bSeenConnected = true;
				}
				else if (bSeenConnected)
				{
					bOrderOk = false;
					break;
				}
			}
			TestTrue(TEXT("all Unconnected/Misconfigured rows precede all Connected rows"), bOrderOk);
		});
	});

	Describe("GetViewModelConnectionMap", [this]()
	{
		It("a ViewModel class with no discoverable referencing Widget Blueprints returns a valid, non-error empty result", [this]()
		{
			const FViewModelConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetViewModelConnectionMap(UWidgetConnectionMapperTestViewModel::StaticClass());
			TestTrue(TEXT("Widgets empty"), Result.Widgets.IsEmpty());
			TestEqual(TEXT("AvailableWidgetCount zero"), Result.AvailableWidgetCount, 0);
			TestEqual(TEXT("AvailableConnectionCount zero"), Result.AvailableConnectionCount, 0);
		});

		It("groups per-widget with correct per-widget counts, and narrows each widget's rows to the queried ViewModel class", [this]()
		{
			// GetReferencers needs packages saved to disk and rescanned.
			UBlueprint* ViewModelBPA = MakeViewModelBlueprint(TEXT("WBP_ConnMapperTest_VM_A_"));
			UBlueprint* ViewModelBPB = MakeViewModelBlueprint(TEXT("WBP_ConnMapperTest_VM_B_"));
			TestNotNull(TEXT("ViewModelBPA created"), ViewModelBPA);
			TestNotNull(TEXT("ViewModelBPB created"), ViewModelBPB);
			if (!ViewModelBPA || !ViewModelBPB || !ViewModelBPA->GeneratedClass || !ViewModelBPB->GeneratedClass)
			{
				return;
			}

			TestBP = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_"), TestCounter);
			UWidgetConnectionMapperTestWidget* Solo = TestBP->WidgetTree->ConstructWidget<UWidgetConnectionMapperTestWidget>(UWidgetConnectionMapperTestWidget::StaticClass(), TEXT("Solo"));
			TestBP->WidgetTree->RootWidget = Solo;

			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}
			Subsystem->RequestView(TestBP);
			const FGuid ViewModelIdA = Subsystem->AddViewModel(TestBP, ViewModelBPA->GeneratedClass);
			Subsystem->AddViewModel(TestBP, ViewModelBPB->GeneratedClass);

			// VM_B left unbound so a class leak is catchable.
			FProperty* TestFieldProperty = UWidgetConnectionMapperTestViewModel::StaticClass()->FindPropertyByName(TEXT("TestField"));
			TestNotNull(TEXT("TestField property exists"), TestFieldProperty);
			if (!TestFieldProperty)
			{
				return;
			}
			FMVVMBlueprintViewBinding& Binding = Subsystem->AddBinding(TestBP);
			Binding.BindingType = EMVVMBindingMode::OneWayToDestination;
			Binding.SourcePath.SetViewModelId(ViewModelIdA);
			Binding.SourcePath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(TestFieldProperty));
			Binding.DestinationPath.SetWidgetName(TEXT("Solo"));
			// Exact int32 match; MVVM requires exact types or a conversion function.
			FProperty* IntDestinationProperty = UWidgetConnectionMapperTestWidget::StaticClass()->FindPropertyByName(TEXT("IntDestination"));
			TestNotNull(TEXT("UWidgetConnectionMapperTestWidget::IntDestination exists"), IntDestinationProperty);
			if (!IntDestinationProperty)
			{
				return;
			}
			Binding.DestinationPath.AppendPropertyPath(TestBP, UE::MVVM::FMVVMConstFieldVariant(IntDestinationProperty));

			FKismetEditorUtilities::CompileBlueprint(TestBP);
			FAssetRegistryModule::AssetCreated(TestBP);

			TArray<FString> SavedFilePaths;
			SaveTestPackage(ViewModelBPA, SavedFilePaths);
			SaveTestPackage(ViewModelBPB, SavedFilePaths);
			SaveTestPackage(TestBP, SavedFilePaths);

			TestEqual(TEXT("all three packages saved"), SavedFilePaths.Num(), 3);
			if (SavedFilePaths.Num() != 3)
			{
				return;
			}

			IAssetRegistry& AssetRegistry = *IAssetRegistry::Get();
			AssetRegistry.ScanFilesSynchronous(SavedFilePaths, /*bForceRescan=*/true);

			const FViewModelConnectionMapResult ResultA = UWidgetStaticAnalysisToolset::GetViewModelConnectionMap(TSubclassOf<UMVVMViewModelBase>(ViewModelBPA->GeneratedClass));
			TestEqual(TEXT("one referencing widget found for VM_A"), ResultA.AvailableWidgetCount, 1);
			if (ResultA.Widgets.Num() == 1)
			{
				const FViewModelWidgetConnections& WidgetConnectionsA = ResultA.Widgets[0];
				TestEqual(TEXT("VM_A widget has exactly its own two rows (TestField + Inventory)"), WidgetConnectionsA.Connections.Num(), 2);
				TestEqual(TEXT("VM_A ConnectedCount"), WidgetConnectionsA.ConnectedCount, 1);
				TestEqual(TEXT("VM_A UnconnectedCount"), WidgetConnectionsA.UnconnectedCount, 1);
				const FWidgetConnectionEntry* TestFieldRowA = WidgetConnectionsA.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
				{
					return Entry.ConnectionName == TEXT("TestField");
				});
				TestNotNull(TEXT("VM_A TestField row found"), TestFieldRowA);
				if (TestFieldRowA)
				{
					TestTrue(TEXT("VM_A TestField is Connected"), TestFieldRowA->Status == EWidgetConnectionStatus::Connected);
				}
			}

			const FViewModelConnectionMapResult ResultB = UWidgetStaticAnalysisToolset::GetViewModelConnectionMap(TSubclassOf<UMVVMViewModelBase>(ViewModelBPB->GeneratedClass));
			TestEqual(TEXT("one referencing widget found for VM_B"), ResultB.AvailableWidgetCount, 1);
			if (ResultB.Widgets.Num() == 1)
			{
				const FViewModelWidgetConnections& WidgetConnectionsB = ResultB.Widgets[0];
				TestEqual(TEXT("VM_B widget has exactly its own two rows (TestField + Inventory), not VM_A's"), WidgetConnectionsB.Connections.Num(), 2);
				TestEqual(TEXT("VM_B ConnectedCount"), WidgetConnectionsB.ConnectedCount, 0);
				TestEqual(TEXT("VM_B UnconnectedCount"), WidgetConnectionsB.UnconnectedCount, 2);
				const FWidgetConnectionEntry* TestFieldRowB = WidgetConnectionsB.Connections.FindByPredicate([](const FWidgetConnectionEntry& Entry)
				{
					return Entry.ConnectionName == TEXT("TestField");
				});
				TestNotNull(TEXT("VM_B TestField row found"), TestFieldRowB);
				if (TestFieldRowB)
				{
					TestTrue(TEXT("VM_B TestField is Unconnected, not leaked as VM_A's Connected row"), TestFieldRowB->Status == EWidgetConnectionStatus::Unconnected);
				}
			}
		});

		It("stress: GetViewModelConnectionMap over many referencing widgets; logs time and exported size", [this]()
		{
			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}

			UBlueprint* ViewModelBP = MakeViewModelBlueprint(TEXT("WBP_ConnMapperTest_StressVM_"));
			TestNotNull(TEXT("ViewModelBP created"), ViewModelBP);
			if (!ViewModelBP || !ViewModelBP->GeneratedClass)
			{
				return;
			}

			TArray<FString> SavedFilePaths;
			SaveTestPackage(ViewModelBP, SavedFilePaths);

			constexpr int32 NumReferencingWidgets = 100;
			for (int32 Index = 0; Index < NumReferencingWidgets; ++Index)
			{
				UWidgetBlueprint* ReferencingWidget = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_StressReferencer_"), TestCounter);
				if (!ReferencingWidget)
				{
					continue;
				}
				Subsystem->RequestView(ReferencingWidget);
				Subsystem->AddViewModel(ReferencingWidget, ViewModelBP->GeneratedClass);
				FKismetEditorUtilities::CompileBlueprint(ReferencingWidget);
				FAssetRegistryModule::AssetCreated(ReferencingWidget);
				SaveTestPackage(ReferencingWidget, SavedFilePaths);
			}
			if (SavedFilePaths.Num() != NumReferencingWidgets + 1)
			{
				AddError(TEXT("Fixture setup did not save every package"));
				return;
			}

			IAssetRegistry& AssetRegistry = *IAssetRegistry::Get();
			AssetRegistry.ScanFilesSynchronous(SavedFilePaths, /*bForceRescan=*/true);

			const double StartSeconds = FPlatformTime::Seconds();
			const FViewModelConnectionMapResult Result = UWidgetStaticAnalysisToolset::GetViewModelConnectionMap(TSubclassOf<UMVVMViewModelBase>(ViewModelBP->GeneratedClass));
			const double ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;

			FString Exported;
			FViewModelConnectionMapResult::StaticStruct()->ExportText(Exported, &Result, nullptr, nullptr, PPF_None, nullptr);

			AddInfo(FString::Printf(TEXT("GetViewModelConnectionMap: %d widgets, %d connections in %.3f s, exported text %d chars"),
				Result.Widgets.Num(), Result.AvailableConnectionCount, ElapsedSeconds, Exported.Len()));
			TestEqual(TEXT("AvailableWidgetCount"), Result.AvailableWidgetCount, NumReferencingWidgets);
			TestEqual(TEXT("Widgets.Num()"), Result.Widgets.Num(), NumReferencingWidgets);
		});

		It("FWidgetConnectionMapper::MapViewModelConnections loads and maps every referencer it finds", [this]()
		{
			UMVVMEditorSubsystem* Subsystem = GetMVVMSubsystem();
			TestNotNull(TEXT("MVVM editor subsystem available"), Subsystem);
			if (!Subsystem)
			{
				return;
			}

			UBlueprint* ViewModelBP = MakeViewModelBlueprint(TEXT("WBP_ConnMapperTest_CapVM_"));
			TestNotNull(TEXT("ViewModelBP created"), ViewModelBP);
			if (!ViewModelBP || !ViewModelBP->GeneratedClass)
			{
				return;
			}

			TArray<FString> SavedFilePaths;
			SaveTestPackage(ViewModelBP, SavedFilePaths);

			constexpr int32 NumReferencingWidgets = 5;
			for (int32 Index = 0; Index < NumReferencingWidgets; ++Index)
			{
				UWidgetBlueprint* ReferencingWidget = WidgetStaticAnalysisSchemaTest::MakeEmptyWidgetBlueprintForTest(TEXT("WBP_ConnMapperTest_CapReferencer_"), TestCounter);
				TestNotNull(TEXT("ReferencingWidget created"), ReferencingWidget);
				if (!ReferencingWidget)
				{
					continue;
				}
				// A declared context alone creates an on-disk reference.
				Subsystem->RequestView(ReferencingWidget);
				Subsystem->AddViewModel(ReferencingWidget, ViewModelBP->GeneratedClass);
				FKismetEditorUtilities::CompileBlueprint(ReferencingWidget);
				FAssetRegistryModule::AssetCreated(ReferencingWidget);
				SaveTestPackage(ReferencingWidget, SavedFilePaths);
			}
			TestEqual(TEXT("6 total packages saved (5 widgets + 1 ViewModel)"), SavedFilePaths.Num(), NumReferencingWidgets + 1);
			if (SavedFilePaths.Num() != NumReferencingWidgets + 1)
			{
				return;
			}

			IAssetRegistry& AssetRegistry = *IAssetRegistry::Get();
			AssetRegistry.ScanFilesSynchronous(SavedFilePaths, /*bForceRescan=*/true);

			const TArray<FMappedViewModelWidgetConnections> Mapped = FWidgetConnectionMapper::MapViewModelConnections(
				TSubclassOf<UMVVMViewModelBase>(ViewModelBP->GeneratedClass));

			TestEqual(TEXT("every referencer was loaded and mapped"), Mapped.Num(), NumReferencingWidgets);

			for (const FMappedViewModelWidgetConnections& WidgetConnections : Mapped)
			{
				TestFalse(TEXT("each loaded widget has a real WidgetPath"), WidgetConnections.WidgetPath.IsEmpty());
			}
		});
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
