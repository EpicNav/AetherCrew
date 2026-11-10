// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/WidgetSwitcher.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewCondition.h"
#include "MVVMBlueprintViewEvent.h"
#include "MVVMDeveloperProjectSettings.h"
#include "MVVMEditorSubsystem.h"
#include "MVVMTestFlags.h"
#include "MVVMTestFixtures59.h"
#include "MVVMToolset/MVVMToolset59.h"
#include "PackageTools.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/Tests/TestAssetSandbox.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Types/MVVMConditionOperation.h"
#include "WidgetBlueprint.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopeExit.h"

#include "ToolsetRegistry/ToolCallExceptionHandler.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

BEGIN_DEFINE_SPEC(FMVVMTest_WorkingPath59,
	"AIAssistant.Toolsets.MVVM.WorkingPath", MVVMToolSetTest::Flags)
	TUniquePtr<FTestAssetSandbox> Sandbox;
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	UBlueprint* ViewModel = nullptr;
	int32 TestCounter = 0;
	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> Handler;
END_DEFINE_SPEC(FMVVMTest_WorkingPath59)

void FMVVMTest_WorkingPath59::Define()
{
	// Shared by every test; each creates its assets under names no other test uses
	BeforeEach([this]() // UE 5.8: 5.8 specs have no BeforeAll; one sandbox per test
	{
		Sandbox = MakeUnique<FTestAssetSandbox>(TEXT("MVVMToolSetTest"));
	});

	AfterEach([this]() // UE 5.8: 5.8 specs have no AfterAll
	{
		Sandbox.Reset();
	});

	BeforeEach([this]()
	{
		FString ViewModelName = FString::Printf(TEXT("VM_WorkingPath_%d"), TestCounter);
		ViewModel = UMVVMToolset59::CreateViewModel(ViewModelName, Sandbox->Folder(), nullptr);
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_WorkingPath_%d"), TestCounter++);
		WidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		Handler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
		
		if (IsValid(WidgetBlueprint) && IsValid(ViewModel))
		{
			UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel->SkeletonGeneratedClass);
		}

		if (IsValid(ViewModel))
		{
			//Assumed ViewModel variables 
			UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Health"), TEXT("float"), TEXT(""));
		}
	});

	AfterEach([this]()
	{
		Handler.Reset();
		WidgetBlueprint = nullptr;
		ViewModel = nullptr;
	});

	// ---- CreateViewModel ----

	It("CreateViewModel returns non-null blueprint", [this]()
	{
		TestNotNull(TEXT("ViewModel created"), ViewModel);
	});

	It("CreateViewModel with native parent class succeeds", [this]()
	{
		FString Name = FString::Printf(TEXT("VM_CustomParent_%d"), TestCounter);
		UBlueprint* CustomViewModel = UMVVMToolset59::CreateViewModel(Name, Sandbox->Folder(), UMVVMTestBaseViewModel59::StaticClass());
		TestNotNull(TEXT("ViewModel created with custom parent"), CustomViewModel);
	});

	// ---- AddViewModelProperty ----

	It("AddViewModelProperty returns true for a valid float property", [this]()
	{
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(ViewModel))
		{
			AddInfo(TEXT("Skipped: [Invalid Test ViewModel Blueprint]"));
			return;
		}

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Stamina"), TEXT("float"), TEXT(""));
		});
		TestTrue(TEXT("Property added"), Handler->GetException().IsEmpty());
	});

	It("AddViewModelProperty returns true for a valid bool property with default value", [this]()
	{
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(ViewModel))
		{
			AddInfo(TEXT("Skipped: [Invalid Test ViewModel Blueprint]"));
			return;
		}

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("bIsAlive"), TEXT("bool"), TEXT("true"));
		});
		TestTrue(TEXT("Bool property added"), Handler->GetException().IsEmpty());
	});

	It("AddViewModelProperty adds multiple distinct properties", [this]()
	{
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(ViewModel))
		{
			AddInfo(TEXT("Skipped: [Invalid Test ViewModel Blueprint]"));
			return;
		}

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Stamina"), TEXT("float"), TEXT(""));
			UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Shield"),  TEXT("float"), TEXT(""));
		});
		TestTrue(TEXT("Properties added"), Handler->GetException().IsEmpty());
	});

	// ---- ListViewModels ----

	It("ListViewModels includes the created ViewModel", [this]()
	{
		TArray<UClass*> ViewModels = UMVVMToolset59::ListViewModels(Sandbox->Folder());
		TestTrue(TEXT("At least one ViewModel found"), ViewModels.Num() > 0);
	});
	
	It("ListViewModels honors search path argument", [this]()
	{
		TArray<UClass*> ViewModels = UMVVMToolset59::ListViewModels(TEXT("/Nonexistent/Path"));
		TestTrue(TEXT("No ViewModel found"), ViewModels.Num() == 0);
	});

	It("ListViewModels with empty search path returns all ViewModels", [this]()
	{
		TArray<UClass*> ViewModels = UMVVMToolset59::ListViewModels(TEXT(""));
		TestTrue(TEXT("Non-empty result with empty search path"), ViewModels.Num() > 0);
	});

	It("ListViewModels with returns native ViewModels", [this]()
	{
		TArray<UClass*> ViewModels = UMVVMToolset59::ListViewModels(TEXT(""));
		TestTrue(TEXT("Native ViewModel class in results"), ViewModels.Contains(UMVVMTestBaseViewModel59::StaticClass()));
	});

	It("ListViewModels returns the generated class of Blueprint ViewModels, never the SKEL_ class", [this]()
	{
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(ViewModel) || !IsValid(ViewModel->GeneratedClass) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		TArray<UClass*> ViewModels = UMVVMToolset59::ListViewModels(Sandbox->Folder());
		TestTrue(TEXT("Generated class in results"), ViewModels.Contains(ViewModel->GeneratedClass.Get()));
		TestFalse(TEXT("Skeleton class not in results"), ViewModels.Contains(ViewModel->SkeletonGeneratedClass.Get()));
		for (const UClass* Class : ViewModels)
		{
			TestFalse(FString::Printf(TEXT("'%s' is not a skeleton class"), *Class->GetName()), FKismetEditorUtilities::IsClassABlueprintSkeleton(Class));
			TestFalse(FString::Printf(TEXT("'%s' is not a stale class"), *Class->GetName()), Class->HasAnyClassFlags(CLASS_NewerVersionExists));
		}
	});

	It("AddViewModelToWidget registers the generated class when given the SKEL_ class", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->GeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		// BeforeEach passed the SkeletonGeneratedClass to AddViewModelToWidget
		TArray<UClass*> ViewModels = UMVVMToolset59::ListWidgetViewModels(WidgetBlueprint);
		TestTrue(TEXT("Generated class registered on widget"), ViewModels.Contains(ViewModel->GeneratedClass.Get()));
		TestFalse(TEXT("Skeleton class not registered on widget"), ViewModels.Contains(ViewModel->SkeletonGeneratedClass.Get()));
	});

	It("CreateViewBinding binds a newly added ViewModel property without a manual compile", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->GeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_BindUncompiled_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->GeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}

		// Use the classes an agent would get from ListViewModels: the generated classes, which don't
		// contain the newly added "Health" property until the ViewModel Blueprints are compiled.
		// Add the property after AddViewModelToWidget, which compiles the ViewModel, so both ViewModels
		// are out of date when CreateViewBinding is called (BeforeEach does the same for ViewModel).
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->GeneratedClass);
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		TestFalse(TEXT("Source ViewModel has uncompiled changes before binding"), ViewModel->IsUpToDate());
		TestFalse(TEXT("Destination ViewModel has uncompiled changes before binding"), ViewModel2->IsUpToDate());

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->GeneratedClass,  TEXT("Health"),
			ViewModel2->GeneratedClass, TEXT("Health"),
			NAME_None);
		TestTrue(TEXT("Binding GUID is valid"), BindingId.IsValid());
		TestTrue(TEXT("Source ViewModel compiled on demand"), ViewModel->IsUpToDate());
		TestTrue(TEXT("Destination ViewModel compiled on demand"), ViewModel2->IsUpToDate());

		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave);
		TestTrue(TEXT("Widget with the new binding compiles"), WidgetBlueprint->Status != BS_Error);
	});

	// ---- WidgetBlueprint with MVVM ----

	It("CreateTestWidgetBlueprintWithMVVM produces a valid blueprint", [this]()
	{
		TestNotNull(TEXT("Widget WidgetBlueprint created"), WidgetBlueprint);
	});

	// ---- AddViewModelToWidget ----

	It("AddViewModelToWidget makes the ViewModel visible via ListWidgetViewModels", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		// BeforeEach already called AddViewModelToWidget — verify it registered
		TArray<UClass*> ViewModels = UMVVMToolset59::ListWidgetViewModels(WidgetBlueprint);
		TestTrue(TEXT("ViewModel visible on widget"), ViewModels.Num() > 0);
	});

	It("AddViewModelToWidget makes native ViewModel visible via ListWidgetViewModels", [this]()
	{
		FString FreshName = FString::Printf(TEXT("WWidgetBlueprint_NoVM_%d"), TestCounter);
		UWidgetBlueprint* FreshWidget = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), FreshName);
		TestNotNull(TEXT("Fresh widget created"), FreshWidget);
		if (!IsValid(FreshWidget))
		{
			AddInfo(TEXT("Skipped: [Could not create fresh widget]"));
			return;
		}

		UMVVMToolset59::AddViewModelToWidget(FreshWidget, UMVVMTestBaseViewModel59::StaticClass());

		TArray<UClass*> ViewModels = UMVVMToolset59::ListWidgetViewModels(FreshWidget);
		TestTrue(TEXT("Native ViewModel on fresh widget"), ViewModels.Contains(UMVVMTestBaseViewModel59::StaticClass()));
	});


	// ---- ListWidgetViewModels ----

	It("ListWidgetViewModels returns empty for widget with no ViewModels assigned", [this]()
	{
		FString FreshName = FString::Printf(TEXT("WWidgetBlueprint_NoVM_%d"), TestCounter);
		UWidgetBlueprint* FreshWidget = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), FreshName);
		TestNotNull(TEXT("Fresh widget created"), FreshWidget);
		if (!IsValid(FreshWidget))
		{
			AddInfo(TEXT("Skipped: [Could not create fresh widget]"));
			return;
		}

		TArray<UClass*> ViewModels = UMVVMToolset59::ListWidgetViewModels(FreshWidget);
		TestEqual(TEXT("No ViewModels on fresh widget"), ViewModels.Num(), 0);
	});

	// ---- ListWidgetViewBindings ----

	It("ListWidgetViewBindings returns empty array on fresh blueprint", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("No bindings on fresh blueprint"), Bindings.Num(), 0);
	});

	// ---- CreateViewBinding ----

	It("CreateViewBinding returns a valid GUID", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_BindDest_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}

		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);
		TestTrue(TEXT("Binding GUID is valid"), BindingId.IsValid());
	});

	It("CreateViewBinding adds a binding visible via ListWidgetViewBindings", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_BindDestList_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding exists after create"), Bindings.Num(), 1);
	});

	It("CreateViewBinding ID matches what ListWidgetViewBindings reports", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_BindIdCheck_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid CreatedId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestEqual(TEXT("Binding IDs match"), Bindings[0].BindingId, CreatedId);
		}
	});

	It("CreateViewBinding without ConversionName keeps a direct binding between matching types", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_BindTextDirect_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Label"), TEXT("text"), TEXT(""));
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Label"), TEXT("text"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Label"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Label"),
			NAME_None);
		TestTrue(TEXT("Binding GUID is valid"), BindingId.IsValid());

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestTrue(TEXT("No conversion function inferred"), Bindings[0].SourceToDestinationConversion.FunctionName.IsEmpty());
			TestFalse(TEXT("Direct source path"), Bindings[0].SourcePath.IsEmpty());
		}
	});

	It("CreateViewBinding applies an explicit ConversionName between matching types", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass) || !IsValid(MVVMEditorSubsystem))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_BindTextConv_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Label"), TEXT("text"), TEXT(""));
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Label"), TEXT("text"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		// The available conversion functions depend on the project settings, so pick any FText -> FText one
		const FProperty* SourceProperty = ViewModel->SkeletonGeneratedClass->FindPropertyByName(TEXT("Label"));
		const FProperty* DestinationProperty = ViewModel2->SkeletonGeneratedClass->FindPropertyByName(TEXT("Label"));
		TestNotNull(TEXT("Source text property exists"), SourceProperty);
		TestNotNull(TEXT("Destination text property exists"), DestinationProperty);
		if (SourceProperty == nullptr || DestinationProperty == nullptr)
		{
			return;
		}

		FName ConversionName;
		for (const UE::MVVM::FConversionFunctionValue& ConversionFunction : MVVMEditorSubsystem->GetConversionFunctions(WidgetBlueprint, SourceProperty, DestinationProperty))
		{
			if (ConversionFunction.IsFunction())
			{
				ConversionName = ConversionFunction.GetFName();
				break;
			}
		}
		if (ConversionName.IsNone())
		{
			AddInfo(TEXT("Skipped: [No FText -> FText conversion function available in this project]"));
			return;
		}

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Label"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Label"),
			ConversionName);
		TestTrue(TEXT("Binding GUID is valid"), BindingId.IsValid());

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestEqual(TEXT("Requested conversion function applied"), Bindings[0].SourceToDestinationConversion.FunctionName, ConversionName.ToString());
		}
	});

	It("Multiple CreateViewBinding calls each produce a unique binding", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_BindUniq_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid IdA = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);
		FGuid IdB = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		TestNotEqual(TEXT("Binding IDs are unique"), IdA, IdB);

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("Two bindings exist"), Bindings.Num(), 2);
	});

	// ---- RemoveWidgetViewBinding ----

	It("RemoveWidgetViewBinding returns true and removes the binding", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_RemoveTest_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::RemoveWidgetViewBinding(WidgetBlueprint, BindingId);
		});
		TestTrue(TEXT("Remove raised no error"), Handler->GetException().IsEmpty());

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("Binding list is empty after remove"), Bindings.Num(), 0);
	});

	It("RemoveWidgetViewBinding removes only the targeted binding", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_RemoveSelective_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid IdA = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);
		FGuid IdB = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		UMVVMToolset59::RemoveWidgetViewBinding(WidgetBlueprint, IdA);

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding remains"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestEqual(TEXT("Remaining binding is IdB"), Bindings[0].BindingId, IdB);
		}
	});

	// ---- SetWidgetViewBindingEnabled ----

	It("SetWidgetViewBindingEnabled returns true and disables the targeted binding", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_EnableDisable_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewBindingEnabled(WidgetBlueprint, BindingId, /*bEnabled=*/false, /*bCompile=*/true);
		});
		TestTrue(TEXT("Set raised no error"), Handler->GetException().IsEmpty());

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding exists"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestFalse(TEXT("Binding disabled"), Bindings[0].bEnabled);
			TestTrue(TEXT("Binding still compiles"), Bindings[0].bCompile);
		}
	});

	It("SetWidgetViewBindingEnabled toggles the compile flag", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_Compile_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewBindingEnabled(WidgetBlueprint, BindingId, /*bEnabled=*/true, /*bCompile=*/false);
		});
		TestTrue(TEXT("Set raised no error"), Handler->GetException().IsEmpty());

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding exists"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestTrue(TEXT("Binding still enabled"), Bindings[0].bEnabled);
			TestFalse(TEXT("Binding excluded from compilation"), Bindings[0].bCompile);
		}
	});

	It("SetWidgetViewBindingEnabled can re-enable a previously disabled binding", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_ReEnable_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewBindingEnabled(WidgetBlueprint, BindingId, /*bEnabled=*/false, /*bCompile=*/false);
			UMVVMToolset59::SetWidgetViewBindingEnabled(WidgetBlueprint, BindingId, /*bEnabled=*/true, /*bCompile=*/true);
		});
		TestTrue(TEXT("Re-enable raised no error"), Handler->GetException().IsEmpty());

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding exists"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestTrue(TEXT("Binding enabled again"), Bindings[0].bEnabled);
			TestTrue(TEXT("Binding compiles again"), Bindings[0].bCompile);
		}
	});

	It("SetWidgetViewBindingEnabled only affects the targeted binding", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_EnableSelective_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FGuid IdA = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);
		FGuid IdB = UMVVMToolset59::CreateViewBinding(
			WidgetBlueprint,
			ViewModel->SkeletonGeneratedClass,  TEXT("Health"),
			ViewModel2->SkeletonGeneratedClass, TEXT("Health"),
			NAME_None);

		UMVVMToolset59::SetWidgetViewBindingEnabled(WidgetBlueprint, IdA, /*bEnabled=*/false, /*bCompile=*/false);

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("Two bindings exist"), Bindings.Num(), 2);
		for (const FMVVMViewBindingDescription& Binding : Bindings)
		{
			if (Binding.BindingId == IdA)
			{
				TestFalse(TEXT("Target binding disabled"), Binding.bEnabled);
				TestFalse(TEXT("Target binding compile cleared"), Binding.bCompile);
			}
			else if (Binding.BindingId == IdB)
			{
				TestTrue(TEXT("Other binding still enabled"), Binding.bEnabled);
				TestTrue(TEXT("Other binding still compiles"), Binding.bCompile);
			}
		}
	});

	// ---- SetBindingMode ----

	It("SetBindingMode switches a binding to TwoWay", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		UWidget* Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		FGuid Id = UMVVMToolset59::CreateViewBinding(WidgetBlueprint, ViewModel->SkeletonGeneratedClass, TEXT("Health"), Img, TEXT("RenderOpacity"), NAME_None);
		TestTrue(TEXT("binding created"), Id.IsValid());

		Handler->CaptureErrorsIn([&]()
		{
			TestTrue(TEXT("mode set"), UMVVMToolset59::SetBindingMode(WidgetBlueprint, Id, EMVVMBindingMode::TwoWay));
		});
		TestTrue(TEXT("Set raised no error"), Handler->GetException().IsEmpty());

		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint);
		TestEqual(TEXT("One binding exists"), Bindings.Num(), 1);
		if (Bindings.Num() > 0)
		{
			TestEqual(TEXT("Binding mode is TwoWay"), Bindings[0].BindingType, EMVVMBindingMode::TwoWay);
		}
	});

	// ---- ListBindableWidgetProperties ----

	It("ListBindableWidgetProperties lists a known property on a widget", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		UWidget* Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		TArray<FString> Properties;
		Handler->CaptureErrorsIn([&]()
		{
			Properties = UMVVMToolset59::ListBindableWidgetProperties(WidgetBlueprint, Img);
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());
		TestTrue(TEXT("RenderOpacity is a bindable property"), Properties.Contains(TEXT("RenderOpacity")));
	});

	// ---- ListClassBindableFields ----

	It("ListClassBindableFields matches the fixture ViewModel's known fields exactly", [this]()
	{
		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListClassBindableFields(UMVVMTestBindableFieldsViewModel::StaticClass(), nullptr);
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		auto FindField = [&Result](const TCHAR* FieldName) -> const FMVVMBindableFieldDescription*
		{
			return Result.Fields.FindByPredicate([FieldName](const FMVVMBindableFieldDescription& Field)
			{
				return Field.FieldName == FieldName;
			});
		};

		// UMVVMViewModelBase/UObject may also contribute inherited bindable fields (GetAvailableBindings
		// iterates with IncludeSuper) -- this test only asserts the fixture's own three fields match exactly,
		// rather than asserting Fields.Num() == 3.
		const FMVVMBindableFieldDescription* PlainFloatField = FindField(TEXT("PlainFloat"));
		if (TestNotNull(TEXT("PlainFloat found"), PlainFloatField))
		{
			TestEqual(TEXT("PlainFloat type"), PlainFloatField->FieldType, FString(TEXT("float")));
			TestTrue(TEXT("PlainFloat is readable"), PlainFloatField->bIsReadable);
			TestTrue(TEXT("PlainFloat is writable"), PlainFloatField->bIsWritable);
			TestFalse(TEXT("PlainFloat has no notify"), PlainFloatField->bHasNotify);
			TestFalse(TEXT("PlainFloat is not private"), PlainFloatField->bIsPrivate);
			TestFalse(TEXT("PlainFloat is not protected"), PlainFloatField->bIsProtected);
			TestTrue(TEXT("PlainFloat has no function argument type"), PlainFloatField->FunctionArgumentType.IsEmpty());
			TestTrue(TEXT("PlainFloat has no function return type"), PlainFloatField->FunctionReturnType.IsEmpty());
		}

		const FMVVMBindableFieldDescription* NotifyTextField = FindField(TEXT("NotifyText"));
		if (TestNotNull(TEXT("NotifyText found"), NotifyTextField))
		{
			TestEqual(TEXT("NotifyText type"), NotifyTextField->FieldType, FString(TEXT("FText")));
			TestTrue(TEXT("NotifyText is readable"), NotifyTextField->bIsReadable);
			TestTrue(TEXT("NotifyText is writable"), NotifyTextField->bIsWritable);
			TestTrue(TEXT("NotifyText has notify"), NotifyTextField->bHasNotify);
		}

		const FMVVMBindableFieldDescription* WriteOnlyBoolField = FindField(TEXT("SetWriteOnlyBool"));
		if (TestNotNull(TEXT("SetWriteOnlyBool found"), WriteOnlyBoolField))
		{
			TestEqual(TEXT("SetWriteOnlyBool type"), WriteOnlyBoolField->FieldType, FString(TEXT("bool")));
			TestFalse(TEXT("SetWriteOnlyBool is not readable"), WriteOnlyBoolField->bIsReadable);
			TestTrue(TEXT("SetWriteOnlyBool is writable"), WriteOnlyBoolField->bIsWritable);
			TestFalse(TEXT("SetWriteOnlyBool has no notify"), WriteOnlyBoolField->bHasNotify);
			TestEqual(TEXT("SetWriteOnlyBool function argument type"), WriteOnlyBoolField->FunctionArgumentType, FString(TEXT("bool")));
			TestTrue(TEXT("SetWriteOnlyBool has no function return type"), WriteOnlyBoolField->FunctionReturnType.IsEmpty());
		}

		TestEqual(TEXT("AvailableFieldCount matches Fields.Num()"), Result.AvailableFieldCount, Result.Fields.Num());
	});

	It("ListClassBindableFields does not crash on a non-MVVM-relevant class", [this]()
	{
		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListClassBindableFields(AActor::StaticClass(), nullptr);
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		for (const FMVVMBindableFieldDescription& Field : Result.Fields)
		{
			TestFalse(FString::Printf(TEXT("%s has no notify (AActor has no INotifyFieldValueChanged)"), *Field.FieldName), Field.bHasNotify);
		}
	});

	It("ListClassBindableFields reports bIsPrivate/bIsProtected on native access-restricted fields visible to a full-access AccessorClass", [this]()
	{
		// Self-accessor grants full access, so the private/protected fields show up here too.
		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListClassBindableFields(UMVVMTestBindableFieldsViewModel::StaticClass(), UMVVMTestBindableFieldsViewModel::StaticClass());
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		auto FindField = [&Result](const TCHAR* FieldName) -> const FMVVMBindableFieldDescription*
		{
			return Result.Fields.FindByPredicate([FieldName](const FMVVMBindableFieldDescription& Field)
			{
				return Field.FieldName == FieldName;
			});
		};

		const FMVVMBindableFieldDescription* PrivateField = FindField(TEXT("PrivateFloat"));
		if (TestNotNull(TEXT("PrivateFloat found"), PrivateField))
		{
			TestTrue(TEXT("PrivateFloat is private"), PrivateField->bIsPrivate);
			TestFalse(TEXT("PrivateFloat is not protected"), PrivateField->bIsProtected);
		}

		const FMVVMBindableFieldDescription* ProtectedField = FindField(TEXT("ProtectedFloat"));
		if (TestNotNull(TEXT("ProtectedFloat found"), ProtectedField))
		{
			TestFalse(TEXT("ProtectedFloat is not private"), ProtectedField->bIsPrivate);
			TestTrue(TEXT("ProtectedFloat is protected"), ProtectedField->bIsProtected);
		}
	});

	// ---- ListPropertyPathBindableFields ----

	It("ListPropertyPathBindableFields resolves a plain widget property path", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		UWidget* Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, Img, TEXT("RenderOpacity"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		TestEqual(TEXT("Exactly one field resolved"), Result.Fields.Num(), 1);
		if (Result.Fields.Num() == 1)
		{
			const FMVVMBindableFieldDescription& Field = Result.Fields[0];
			TestEqual(TEXT("Field name"), Field.FieldName, FString(TEXT("RenderOpacity")));
			TestEqual(TEXT("Field type"), Field.FieldType, FString(TEXT("float")));
			TestTrue(TEXT("RenderOpacity is readable"), Field.bIsReadable);
			TestTrue(TEXT("RenderOpacity is writable"), Field.bIsWritable);
			TestFalse(TEXT("RenderOpacity has no notify"), Field.bHasNotify);
			TestFalse(TEXT("RenderOpacity is not private"), Field.bIsPrivate);
			// RenderOpacity really is protected: in Widget.h.
			TestTrue(TEXT("RenderOpacity is protected"), Field.bIsProtected);
			TestTrue(TEXT("RenderOpacity has no function argument type"), Field.FunctionArgumentType.IsEmpty());
			TestTrue(TEXT("RenderOpacity has no function return type"), Field.FunctionReturnType.IsEmpty());
		}
		TestEqual(TEXT("AvailableFieldCount matches Fields.Num()"), Result.AvailableFieldCount, Result.Fields.Num());
	});

	It("ListPropertyPathBindableFields resolves a struct-nested property path and still populates FieldType", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		// "ColorAndOpacity.R" resolves through UImage::ColorAndOpacity (an FLinearColor-typed FStructProperty)
		// into FLinearColor's own reflected "R" float property -- a genuine, reachable multi-segment path into
		// a struct, per UE::MVVM::Private::FindFieldChainFromPath's own struct-descent branch (walks into
		// FStructProperty::Struct for a non-leaf segment). The leaf field's GetOwner() is FLinearColor's
		// UScriptStruct, not a UClass, which is exactly the shape that used to silently lose FieldType.
		UWidget* Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, Img, TEXT("ColorAndOpacity.R"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		TestEqual(TEXT("Exactly one field resolved"), Result.Fields.Num(), 1);
		if (Result.Fields.Num() == 1)
		{
			const FMVVMBindableFieldDescription& Field = Result.Fields[0];
			TestEqual(TEXT("Field name"), Field.FieldName, FString(TEXT("R")));
			TestEqual(TEXT("Struct-nested leaf's FieldType is populated"), Field.FieldType, FString(TEXT("float")));
			TestTrue(TEXT("R is readable"), Field.bIsReadable);
			TestTrue(TEXT("R is writable"), Field.bIsWritable);
		}
	});

	It("ListPropertyPathBindableFields resolves a function-backed setter field", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		// UWidget::SetVisibility is a genuine setter-only function-backed field (single argument, void
		// return, no matching zero-arg "SetVisibility" property) reachable from the standard widget fixture,
		// without needing Phase 1's UMVVMTestBindableFieldsViewModel class-based fixture.
		UWidget* Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, Img, TEXT("SetVisibility"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		TestEqual(TEXT("Exactly one field resolved"), Result.Fields.Num(), 1);
		if (Result.Fields.Num() == 1)
		{
			const FMVVMBindableFieldDescription& Field = Result.Fields[0];
			TestEqual(TEXT("Field name"), Field.FieldName, FString(TEXT("SetVisibility")));
			TestFalse(TEXT("SetVisibility is not readable"), Field.bIsReadable);
			TestTrue(TEXT("SetVisibility is writable"), Field.bIsWritable);
			TestFalse(TEXT("SetVisibility has no function argument type"), Field.FunctionArgumentType.IsEmpty());
			TestTrue(TEXT("SetVisibility has no function return type (void setter)"), Field.FunctionReturnType.IsEmpty());
		}
	});

	It("ListPropertyPathBindableFields resolves a Blueprint-generated ViewModel property path", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint or ViewModel]"));
			return;
		}

		// Regression coverage: FMVVMBlueprintPropertyPath::GetFields(SelfContext) can silently return a
		// null-property leaf field for a Blueprint-generated class member (e.g. this Blueprint ViewModel's
		// "Health" property, added via AddViewModelProperty) on a freshly-resolved, not-yet-stored path --
		// GetCompleteFields(WidgetBlueprint) is the mechanism proven correct for this case (see
		// ListPropertyPathBindableFields's own implementation comment). Context here is the ViewModel's
		// registered UClass, matching CreateViewBinding's own ViewModel-as-UClass context contract.
		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, ViewModel->SkeletonGeneratedClass, TEXT("Health"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		TestEqual(TEXT("Exactly one field resolved"), Result.Fields.Num(), 1);
		if (Result.Fields.Num() == 1)
		{
			const FMVVMBindableFieldDescription& Field = Result.Fields[0];
			TestEqual(TEXT("Field name"), Field.FieldName, FString(TEXT("Health")));
			TestEqual(TEXT("Field type"), Field.FieldType, FString(TEXT("float")));
			TestTrue(TEXT("Health is readable"), Field.bIsReadable);
			TestTrue(TEXT("Health is writable"), Field.bIsWritable);
			TestTrue(TEXT("Health has notify (AddViewModelProperty always tags FieldNotify)"), Field.bHasNotify);
		}
	});

	It("ListPropertyPathBindableFields returns an empty, non-error result for an unresolvable path", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, nullptr, TEXT("__NoSuchProperty"));
		});
		TestTrue(TEXT("Unresolvable path raises no error"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("No fields for unresolvable path"), Result.Fields.Num(), 0);
		TestEqual(TEXT("AvailableFieldCount is zero for unresolvable path"), Result.AvailableFieldCount, 0);
		TestFalse(TEXT("ErrorMessage carries the resolution failure reason"), Result.ErrorMessage.IsEmpty());
	});

	It("ListPropertyPathBindableFields reports bIsPrivate and bIsProtected for native access-restricted fields", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		// No accessor gating here, so both resolve regardless.
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, UMVVMTestBindableFieldsViewModel::StaticClass());

		FMVVMBindableFieldListResult PrivateResult;
		Handler->CaptureErrorsIn([&]()
		{
			PrivateResult = UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, UMVVMTestBindableFieldsViewModel::StaticClass(), TEXT("PrivateFloat"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("PrivateFloat resolved"), PrivateResult.Fields.Num(), 1);
		if (PrivateResult.Fields.Num() == 1)
		{
			TestTrue(TEXT("PrivateFloat is private"), PrivateResult.Fields[0].bIsPrivate);
			TestFalse(TEXT("PrivateFloat is not protected"), PrivateResult.Fields[0].bIsProtected);
		}

		FMVVMBindableFieldListResult ProtectedResult;
		Handler->CaptureErrorsIn([&]()
		{
			ProtectedResult = UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, UMVVMTestBindableFieldsViewModel::StaticClass(), TEXT("ProtectedFloat"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("ProtectedFloat resolved"), ProtectedResult.Fields.Num(), 1);
		if (ProtectedResult.Fields.Num() == 1)
		{
			TestFalse(TEXT("ProtectedFloat is not private"), ProtectedResult.Fields[0].bIsPrivate);
			TestTrue(TEXT("ProtectedFloat is protected"), ProtectedResult.Fields[0].bIsProtected);
		}
	});

	// ---- ListBindingConversionOptions ----

	It("ListBindingConversionOptions finds only type-matching, allowlisted conversion functions", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		// PlainFloat (float) -> NotifyText (FText) requires a real, allowlisted conversion function (e.g.
		// UKismetTextLibrary::Conv_DoubleToText, a BlueprintFunctionLibrary function -- allowlisted per this
		// project's actual DefaultModelViewViewModel.ini ConversionFunctionFilter=AllowedList).
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, UMVVMTestBindableFieldsViewModel::StaticClass());

		FMVVMBindingConversionResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListBindingConversionOptions(
				WidgetBlueprint,
				UMVVMTestBindableFieldsViewModel::StaticClass(), TEXT("PlainFloat"),
				UMVVMTestBindableFieldsViewModel::StaticClass(), TEXT("NotifyText"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		TestTrue(TEXT("At least one allowlisted float-to-FText conversion option found"), Result.ConversionOptions.Num() > 0);
		bool bFoundAtLeastOneFunctionBackedOption = false;
		for (const FMVVMConversionOptionDescription& Option : Result.ConversionOptions)
		{
			TestFalse(TEXT("Option has a name"), Option.Name.IsEmpty());
			TestFalse(TEXT("Option has a display name"), Option.DisplayName.IsEmpty());
			if (Option.bIsFunction)
			{
				bFoundAtLeastOneFunctionBackedOption = true;
				// Verifies type-match exclusivity, not just non-emptiness: GetConversionFunctions filters via
				// ArePropertiesCompatible (not exact type equality), so a widening-compatible argument type
				// (e.g. "double" for a "float" source) is a real, valid match -- not exact-type equality --
				// while the return type must still land on the destination's own exact type (FText), since
				// nothing else could serve as this destination's value. A mismatched, non-numeric argument
				// type (e.g. "FText" or "bool") or a non-FText return type leaking through would mean the
				// type filter isn't actually being applied.
				TestTrue(FString::Printf(TEXT("Function-backed option's expected argument type ('%s') is numeric-compatible with the float source"), *Option.ExpectedArgumentType),
					Option.ExpectedArgumentType == TEXT("float") || Option.ExpectedArgumentType == TEXT("double") || Option.ExpectedArgumentType == TEXT("int32") || Option.ExpectedArgumentType == TEXT("int64"));
				TestEqual(TEXT("Function-backed option's expected return type matches the FText destination"), Option.ExpectedReturnType, FString(TEXT("FText")));
			}
		}
		TestTrue(TEXT("At least one function-backed option was found to type-check"), bFoundAtLeastOneFunctionBackedOption);
		TestEqual(TEXT("AvailableConversionOptionCount matches ConversionOptions.Num()"), Result.AvailableConversionOptionCount, Result.ConversionOptions.Num());
	});

	It("ListBindingConversionOptions reports direct compatibility for matching float types", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString VM2Name = FString::Printf(TEXT("VM_ConversionDirect_%d"), TestCounter);
		UBlueprint* ViewModel2 = UMVVMToolset59::CreateViewModel(VM2Name, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel2) || !IsValid(ViewModel2->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Could not create second ViewModel]"));
			return;
		}
		UMVVMToolset59::AddViewModelProperty(ViewModel2, TEXT("Health"), TEXT("float"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel2->SkeletonGeneratedClass);

		FMVVMBindingConversionResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListBindingConversionOptions(
				WidgetBlueprint,
				ViewModel->SkeletonGeneratedClass, TEXT("Health"),
				ViewModel2->SkeletonGeneratedClass, TEXT("Health"));
		});
		TestTrue(TEXT("List raised no error"), Handler->GetException().IsEmpty());

		// bDirectlyCompatible is the dedicated "no conversion needed" signal (FR4) -- distinguishing this case
		// from an unbridgeable pair does not require ConversionOptions to be empty. Per design Decision 5,
		// ConversionOptions is always computed independently regardless of bDirectlyCompatible: float is
		// implicitly convertible with several other numeric/text types, so real, allowlisted conversion
		// functions genuinely exist for a float source/destination even though the pair already binds
		// directly -- verified against this project's actual conversion function registry.
		TestTrue(TEXT("float-to-float is directly compatible"), Result.bDirectlyCompatible);
		TestTrue(TEXT("ConversionOptions is still independently computed, not skipped, when directly compatible"), Result.AvailableConversionOptionCount > 0);
	});

	It("ListBindingConversionOptions returns a default-constructed, non-error result for a function-backed leaf", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		// UWidget::SetVisibility is a genuine setter-only function-backed field (see the "ListPropertyPathBindableFields
		// resolves a function-backed setter field" test above). CreateViewBinding's own mutating path
		// (TryAutoConvertBinding) only extracts a comparable FProperty* when the leaf field IsProperty() -- it
		// silently no-ops for a function-backed leaf, assigning no conversion function even with an explicit
		// ConversionName. This tool must therefore treat a function-backed leaf as unresolvable for conversion
		// purposes too (NFR4), not report options CreateViewBinding could never apply.
		UWidget* Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		FMVVMBindingConversionResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListBindingConversionOptions(
				WidgetBlueprint,
				ViewModel->SkeletonGeneratedClass, TEXT("Health"),
				Img, TEXT("SetVisibility"));
		});
		TestTrue(TEXT("Function-backed leaf raises no error -- this is a normal unresolvable outcome, not a caller mistake"), Handler->GetException().IsEmpty());
		TestFalse(TEXT("bDirectlyCompatible defaults to false"), Result.bDirectlyCompatible);
		TestEqual(TEXT("No conversion options for a function-backed leaf"), Result.ConversionOptions.Num(), 0);
	});

	// ---- Mutation safety (NFR1) ----

	It("ListClassBindableFields, ListPropertyPathBindableFields, and ListBindingConversionOptions do not mutate the WidgetBlueprint or ViewModel", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		TestNotNull(TEXT("ViewModel valid"), ViewModel);
		if (!IsValid(WidgetBlueprint) || !IsValid(ViewModel) || !IsValid(ViewModel->SkeletonGeneratedClass))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		// FObjectWriter serializes an object's full UPROPERTY graph (the same mechanism the engine's own
		// undo/diffing systems use to detect object mutation) -- a faithful, disk-I/O-free stand-in for
		// "the asset's serialized state" that doesn't require a real package save.
		auto SnapshotSerializedState = [](UObject* Object) -> TArray<uint8>
		{
			TArray<uint8> Bytes;
			FObjectWriter Writer(Object, Bytes);
			return Bytes;
		};

		const TArray<uint8> WidgetBlueprintBefore = SnapshotSerializedState(WidgetBlueprint);
		const TArray<uint8> ViewModelBefore = SnapshotSerializedState(ViewModel);

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::ListClassBindableFields(ViewModel->SkeletonGeneratedClass, nullptr);
			UMVVMToolset59::ListPropertyPathBindableFields(WidgetBlueprint, ViewModel->SkeletonGeneratedClass, TEXT("Health"));
			UMVVMToolset59::ListBindingConversionOptions(
				WidgetBlueprint,
				ViewModel->SkeletonGeneratedClass, TEXT("Health"),
				ViewModel->SkeletonGeneratedClass, TEXT("Health"));
		});
		TestTrue(TEXT("All three calls raised no error"), Handler->GetException().IsEmpty());

		const TArray<uint8> WidgetBlueprintAfter = SnapshotSerializedState(WidgetBlueprint);
		const TArray<uint8> ViewModelAfter = SnapshotSerializedState(ViewModel);

		TestEqual(TEXT("WidgetBlueprint serialized byte count unchanged"), WidgetBlueprintAfter.Num(), WidgetBlueprintBefore.Num());
		TestTrue(TEXT("WidgetBlueprint serialized state is byte-identical after all three calls"),
			WidgetBlueprintAfter.Num() == WidgetBlueprintBefore.Num() &&
			FMemory::Memcmp(WidgetBlueprintBefore.GetData(), WidgetBlueprintAfter.GetData(), WidgetBlueprintBefore.Num()) == 0);

		TestEqual(TEXT("ViewModel serialized byte count unchanged"), ViewModelAfter.Num(), ViewModelBefore.Num());
		TestTrue(TEXT("ViewModel serialized state is byte-identical after all three calls"),
			ViewModelAfter.Num() == ViewModelBefore.Num() &&
			FMemory::Memcmp(ViewModelBefore.GetData(), ViewModelAfter.GetData(), ViewModelBefore.Num()) == 0);
	});

	// ---- GetWidgetViewConfig / SetWidgetViewConfig ----

	It("GetWidgetViewConfig returns defaults on a fresh widget", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		FMVVMViewConfig Config;
		Handler->CaptureErrorsIn([&]()
		{
			Config = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
		});
		TestTrue(TEXT("Get raised no error"), Handler->GetException().IsEmpty());
		TestTrue(TEXT("bInitializeSourcesOnConstruct defaults to true"), Config.bInitializeSourcesOnConstruct);
		TestTrue(TEXT("bInitializeBindingsOnConstruct defaults to true"), Config.bInitializeBindingsOnConstruct);
		TestTrue(TEXT("bInitializeEventsOnConstruct defaults to true"), Config.bInitializeEventsOnConstruct);
		TestFalse(TEXT("bCreateViewWithoutBindings defaults to false"), Config.bCreateViewWithoutBindings);
	});

	It("SetWidgetViewConfig returns true on a valid widget", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		FMVVMViewConfig Config;
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewConfig(WidgetBlueprint, Config);
		});
		TestTrue(TEXT("Set raised no error"), Handler->GetException().IsEmpty());
	});

	It("SetWidgetViewConfig round-trips all fields through GetWidgetViewConfig", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		FMVVMViewConfig NewConfig;
		NewConfig.bInitializeSourcesOnConstruct = false;
		NewConfig.bInitializeBindingsOnConstruct = false;
		NewConfig.bInitializeEventsOnConstruct = false;
		NewConfig.bCreateViewWithoutBindings = true;

		FMVVMViewConfig ReadBack;
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewConfig(WidgetBlueprint, NewConfig);
			ReadBack = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
		});
		TestTrue(TEXT("Set/Get raised no error"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("bInitializeSourcesOnConstruct round-trips"), ReadBack.bInitializeSourcesOnConstruct, NewConfig.bInitializeSourcesOnConstruct);
		TestEqual(TEXT("bInitializeBindingsOnConstruct round-trips"), ReadBack.bInitializeBindingsOnConstruct, NewConfig.bInitializeBindingsOnConstruct);
		TestEqual(TEXT("bInitializeEventsOnConstruct round-trips"), ReadBack.bInitializeEventsOnConstruct, NewConfig.bInitializeEventsOnConstruct);
		TestEqual(TEXT("bCreateViewWithoutBindings round-trips"), ReadBack.bCreateViewWithoutBindings, NewConfig.bCreateViewWithoutBindings);
	});

	It("SetWidgetViewConfig can flip a single field independently", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		FMVVMViewConfig Config;
		FMVVMViewConfig ReadBack;
		Handler->CaptureErrorsIn([&]()
		{
			Config = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
			Config.bCreateViewWithoutBindings = true;
			UMVVMToolset59::SetWidgetViewConfig(WidgetBlueprint, Config);
			ReadBack = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
		});
		TestTrue(TEXT("Config round-trip raised no error"), Handler->GetException().IsEmpty());
		TestTrue(TEXT("bCreateViewWithoutBindings flipped to true"), ReadBack.bCreateViewWithoutBindings);
		TestTrue(TEXT("Other defaults preserved (Sources)"), ReadBack.bInitializeSourcesOnConstruct);
		TestTrue(TEXT("Other defaults preserved (Bindings)"), ReadBack.bInitializeBindingsOnConstruct);
		TestTrue(TEXT("Other defaults preserved (Events)"), ReadBack.bInitializeEventsOnConstruct);
	});

	It("SetWidgetViewConfig persists across successive GetWidgetViewConfig calls", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		FMVVMViewConfig First;
		First.bInitializeSourcesOnConstruct = false;
		First.bInitializeBindingsOnConstruct = false;
		First.bInitializeEventsOnConstruct = true;
		First.bCreateViewWithoutBindings = true;
		FMVVMViewConfig ReadA;
		FMVVMViewConfig ReadB;
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewConfig(WidgetBlueprint, First);
			ReadA = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
			ReadB = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
		});
		TestTrue(TEXT("Config persistence raised no error"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("Sources stable across reads"), ReadA.bInitializeSourcesOnConstruct, ReadB.bInitializeSourcesOnConstruct);
		TestEqual(TEXT("Bindings stable across reads"), ReadA.bInitializeBindingsOnConstruct, ReadB.bInitializeBindingsOnConstruct);
		TestEqual(TEXT("Events stable across reads"), ReadA.bInitializeEventsOnConstruct, ReadB.bInitializeEventsOnConstruct);
		TestEqual(TEXT("CreateWithoutBindings stable across reads"), ReadA.bCreateViewWithoutBindings, ReadB.bCreateViewWithoutBindings);
	});

	It("SetWidgetViewConfig dirties the WidgetBlueprint package", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		if (UPackage* Package = WidgetBlueprint->GetPackage())
		{
			Package->SetDirtyFlag(false);

			FMVVMViewConfig NewConfig;
			NewConfig.bCreateViewWithoutBindings = true;
			UMVVMToolset59::SetWidgetViewConfig(WidgetBlueprint, NewConfig);

			TestTrue(TEXT("Package dirty after Set"), Package->IsDirty());
		}
	});

	It("SetWidgetViewConfig clamps bInitializeBindingsOnConstruct when Sources is false (EditCondition)", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [Invalid Test Widget Blueprint]"));
			return;
		}

		FMVVMViewConfig Inconsistent;
		Inconsistent.bInitializeSourcesOnConstruct = false;
		Inconsistent.bInitializeBindingsOnConstruct = true;
		FMVVMViewConfig ReadBack;
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewConfig(WidgetBlueprint, Inconsistent);
			ReadBack = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
		});
		TestTrue(TEXT("Config clamp raised no error"), Handler->GetException().IsEmpty());
		TestFalse(TEXT("Sources stays false"), ReadBack.bInitializeSourcesOnConstruct);
		TestFalse(TEXT("Bindings clamped to false because Sources is false"), ReadBack.bInitializeBindingsOnConstruct);
	});

	It("SetWidgetViewConfig is undoable through the transaction system", [this]()
	{
		TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint);
		if (!IsValid(GEditor) || !IsValid(WidgetBlueprint))
		{
			AddInfo(TEXT("Skipped: [No GEditor or invalid Widget Blueprint]"));
			return;
		}

		FMVVMViewConfig Initial = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);

		{
			FScopedTransaction Transaction(NSLOCTEXT("MVVMToolsetTest", "SetConfig", "Set MVVM View Config"));
			FMVVMViewConfig NewConfig = Initial;
			NewConfig.bCreateViewWithoutBindings = !Initial.bCreateViewWithoutBindings;
			UMVVMToolset59::SetWidgetViewConfig(WidgetBlueprint, NewConfig);
		}

		FMVVMViewConfig AfterSet = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
		TestNotEqual(TEXT("Value changed after Set"), AfterSet.bCreateViewWithoutBindings, Initial.bCreateViewWithoutBindings);

		GEditor->UndoTransaction();

		FMVVMViewConfig AfterUndo = UMVVMToolset59::GetWidgetViewConfig(WidgetBlueprint);
		TestEqual(TEXT("Undo restored bCreateViewWithoutBindings"), AfterUndo.bCreateViewWithoutBindings, Initial.bCreateViewWithoutBindings);
	});
}

// ============================================================================
// Error / Negative Path Tests
// ============================================================================

BEGIN_DEFINE_SPEC(FMVVMTest_Errors59,
	"AIAssistant.Toolsets.MVVM.Errors", MVVMToolSetTest::Flags)
	TUniquePtr<FTestAssetSandbox> Sandbox;
	int32 TestCounter = 0;
	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> Handler;
END_DEFINE_SPEC(FMVVMTest_Errors59)

void FMVVMTest_Errors59::Define()
{
	// Shared by every test; each creates its assets under names no other test uses
	BeforeEach([this]() // UE 5.8: 5.8 specs have no BeforeAll; one sandbox per test
	{
		Sandbox = MakeUnique<FTestAssetSandbox>(TEXT("MVVMToolSetTest"));
	});

	AfterEach([this]() // UE 5.8: 5.8 specs have no AfterAll
	{
		Sandbox.Reset();
	});

	BeforeEach([this]()
	{
		Handler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
		// Tool failures raise a script error; the logged text is ignored here and asserted via GetException().
		AddExpectedError(TEXT("Failed to"), EAutomationExpectedErrorFlags::Contains, -1);
	});

	AfterEach([this]()
	{
		Handler.Reset();
	});

	// ---- CreateViewModel errors ----

	It("CreateViewModel with invalid package path returns null", [this]()
	{
		UBlueprint* BadVM = UMVVMToolset59::CreateViewModel(TEXT("VM_Bad"), TEXT("not_a_valid/path!"), nullptr);
		TestNull(TEXT("Null on invalid path"), BadVM);
	});

	It("CreateViewModel with non-ViewModel parent class returns null", [this]()
	{
		UBlueprint* BadVM = UMVVMToolset59::CreateViewModel(TEXT("VM_BadParent"), Sandbox->Folder(), UObject::StaticClass());
		TestNull(TEXT("Null on incompatible parent"), BadVM);
	});

	// ---- AddViewModelProperty errors ----

	It("AddViewModelProperty with null ViewModel returns false", [this]()
	{
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::AddViewModelProperty(nullptr, TEXT("Prop"), TEXT("float"), TEXT(""));
		});
		TestFalse(TEXT("Exception on null ViewModel"), Handler->GetException().IsEmpty());
	});

	It("AddViewModelProperty with invalid type string returns false", [this]()
	{
		FString ViewModelName = FString::Printf(TEXT("VM_InvalidType_%d"), TestCounter++);
		UBlueprint* ViewModel = UMVVMToolset59::CreateViewModel(ViewModelName, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel))
		{
			AddInfo(TEXT("Skipped: [Invalid Test ViewModel Blueprint]"));
			return;
		}

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Prop"), TEXT("not_a_valid_type"), TEXT(""));
		});
		TestFalse(TEXT("Exception on invalid type string"), Handler->GetException().IsEmpty());
	});

	It("AddViewModelProperty with duplicate name returns false", [this]()
	{
		FString ViewModelName = FString::Printf(TEXT("VM_Duplicate_Property_%d"), TestCounter++);
		UBlueprint* ViewModel = UMVVMToolset59::CreateViewModel(ViewModelName, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel))
		{
			AddInfo(TEXT("Skipped: [Invalid Test ViewModel Blueprint]"));
			return;
		}

		UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Health"), TEXT("float"), TEXT(""));
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Health"), TEXT("float"), TEXT(""));
		});
		TestFalse(TEXT("Duplicate property name rejected"), Handler->GetException().IsEmpty());
	});

	// ---- AddViewModelToWidget errors ----

	It("AddViewModelToWidget with null widget does not crash", [this]()
	{
		FString ViewModelName = FString::Printf(TEXT("VM_NullWidget_%d"), TestCounter++);
		UBlueprint* ViewModel = UMVVMToolset59::CreateViewModel(ViewModelName, Sandbox->Folder(), nullptr);
		if (!IsValid(ViewModel))
		{
			AddInfo(TEXT("Skipped: [Invalid Test ViewModel Blueprint]"));
			return;
		}

		UMVVMToolset59::AddViewModelToWidget(nullptr, ViewModel->SkeletonGeneratedClass);
		// No assert — just verifying no crash. ListWidgetViewModels(nullptr) handled separately.
		TestTrue(TEXT("No crash on null widget"), true);
	});

	It("AddViewModelToWidget with null class does not crash", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_NullClass_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		UMVVMToolset59::AddViewModelToWidget(LocalWidgetBlueprint, nullptr);
		TArray<UClass*> ViewModels = UMVVMToolset59::ListWidgetViewModels(LocalWidgetBlueprint);
		TestEqual(TEXT("No ViewModel added with null class"), ViewModels.Num(), 0);
	});

	// ---- ListWidgetViewModels errors ----

	It("ListWidgetViewModels with null widget returns empty array", [this]()
	{
		TArray<UClass*> ViewModels = UMVVMToolset59::ListWidgetViewModels(nullptr);
		TestEqual(TEXT("Empty on null widget"), ViewModels.Num(), 0);
	});

	// ---- ListWidgetViewBindings errors ----

	It("ListWidgetViewBindings with null widget returns empty array", [this]()
	{
		TArray<FMVVMViewBindingDescription> Bindings = UMVVMToolset59::ListWidgetViewBindings(nullptr);
		TestEqual(TEXT("Empty on null widget"), Bindings.Num(), 0);
	});

	// ---- CreateViewBinding errors ----

	It("CreateViewBinding with null widget returns invalid GUID", [this]()
	{
		FGuid BindingId = UMVVMToolset59::CreateViewBinding(nullptr, nullptr, TEXT("Health"), nullptr, TEXT("Health"), NAME_None);
		TestFalse(TEXT("Invalid GUID on null widget"), BindingId.IsValid());
	});

	It("CreateViewBinding with unresolvable property path returns invalid GUID", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_BadPath_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		FGuid BindingId = UMVVMToolset59::CreateViewBinding(
			LocalWidgetBlueprint,
			nullptr, TEXT("__NoSuchProperty"),
			nullptr, TEXT("__NoSuchProperty"),
			NAME_None);
		TestFalse(TEXT("Invalid GUID on unresolvable path"), BindingId.IsValid());
	});

	It("CreateViewBinding with unknown ConversionName returns invalid GUID and adds no binding", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_BadConversion_%d"), TestCounter);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		UBlueprint* SourceViewModel = UMVVMToolset59::CreateViewModel(FString::Printf(TEXT("VM_BadConversionSrc_%d"), TestCounter), Sandbox->Folder(), nullptr);
		UBlueprint* DestinationViewModel = UMVVMToolset59::CreateViewModel(FString::Printf(TEXT("VM_BadConversionDst_%d"), TestCounter++), Sandbox->Folder(), nullptr);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint) || !IsValid(SourceViewModel) || !IsValid(DestinationViewModel))
		{
			return;
		}

		// Matching FText types, so a fallback to any of the available FText -> FText conversions would be caught
		UMVVMToolset59::AddViewModelProperty(SourceViewModel, TEXT("Label"), TEXT("text"), TEXT(""));
		UMVVMToolset59::AddViewModelProperty(DestinationViewModel, TEXT("Label"), TEXT("text"), TEXT(""));
		UMVVMToolset59::AddViewModelToWidget(LocalWidgetBlueprint, SourceViewModel->SkeletonGeneratedClass);
		UMVVMToolset59::AddViewModelToWidget(LocalWidgetBlueprint, DestinationViewModel->SkeletonGeneratedClass);

		FGuid BindingId;
		Handler->CaptureErrorsIn([&]()
		{
			BindingId = UMVVMToolset59::CreateViewBinding(
				LocalWidgetBlueprint,
				SourceViewModel->SkeletonGeneratedClass,      TEXT("Label"),
				DestinationViewModel->SkeletonGeneratedClass, TEXT("Label"),
				TEXT("__NoSuchConversion"));
		});
		TestFalse(TEXT("Invalid GUID on unknown conversion"), BindingId.IsValid());
		TestFalse(TEXT("Error raised"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("No binding left behind"), UMVVMToolset59::ListWidgetViewBindings(LocalWidgetBlueprint).Num(), 0);

		// The add and the rollback are a single undo step, so undoing it must not bring the rejected binding back
		GEditor->UndoTransaction();
		TestEqual(TEXT("No binding after undo"), UMVVMToolset59::ListWidgetViewBindings(LocalWidgetBlueprint).Num(), 0);
	});

	// ---- RemoveWidgetViewBinding errors ----

	It("RemoveWidgetViewBinding with bogus binding ID returns false", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_RemoveErr_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::RemoveWidgetViewBinding(LocalWidgetBlueprint, FGuid::NewGuid());
		});
		TestFalse(TEXT("Exception on bogus binding ID"), Handler->GetException().IsEmpty());
	});

	It("RemoveWidgetViewBinding with null widget returns false", [this]()
	{
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::RemoveWidgetViewBinding(nullptr, FGuid::NewGuid());
		});
		TestFalse(TEXT("Exception on null widget"), Handler->GetException().IsEmpty());
	});

	// ---- SetWidgetViewBindingEnabled errors ----

	It("SetWidgetViewBindingEnabled with bogus binding ID returns false", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_SetEnabledErr_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewBindingEnabled(LocalWidgetBlueprint, FGuid::NewGuid(), /*bEnabled=*/false, /*bCompile=*/false);
		});
		TestFalse(TEXT("Exception on bogus binding ID"), Handler->GetException().IsEmpty());
	});

	It("SetWidgetViewBindingEnabled with null widget returns false", [this]()
	{
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewBindingEnabled(nullptr, FGuid::NewGuid(), /*bEnabled=*/false, /*bCompile=*/false);
		});
		TestFalse(TEXT("Exception on null widget"), Handler->GetException().IsEmpty());
	});

	// ---- SetBindingMode errors ----

	It("SetBindingMode with bogus binding ID returns false", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_SetModeErr_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		bool bResult = true;
		Handler->CaptureErrorsIn([&]()
		{
			bResult = UMVVMToolset59::SetBindingMode(LocalWidgetBlueprint, FGuid::NewGuid(), EMVVMBindingMode::TwoWay);
		});
		TestFalse(TEXT("Exception on bogus binding ID"), Handler->GetException().IsEmpty());
		TestFalse(TEXT("Returns false on bogus binding ID"), bResult);
	});

	It("SetBindingMode with null widget returns false", [this]()
	{
		bool bResult = true;
		Handler->CaptureErrorsIn([&]()
		{
			bResult = UMVVMToolset59::SetBindingMode(nullptr, FGuid::NewGuid(), EMVVMBindingMode::TwoWay);
		});
		TestFalse(TEXT("Exception on null widget"), Handler->GetException().IsEmpty());
		TestFalse(TEXT("Returns false on null widget"), bResult);
	});

	// ---- ListBindableWidgetProperties errors ----

	It("ListBindableWidgetProperties with null widget blueprint returns empty array", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_ListBindablePropsErr_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		UWidget* Img = LocalWidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));

		TArray<FString> Properties;
		Handler->CaptureErrorsIn([&]()
		{
			Properties = UMVVMToolset59::ListBindableWidgetProperties(nullptr, Img);
		});
		TestFalse(TEXT("Exception on null widget blueprint"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("Empty on null widget blueprint"), Properties.Num(), 0);
	});

	It("ListBindableWidgetProperties with null widget returns empty array", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_ListBindablePropsErr2_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		TArray<FString> Properties;
		Handler->CaptureErrorsIn([&]()
		{
			Properties = UMVVMToolset59::ListBindableWidgetProperties(LocalWidgetBlueprint, nullptr);
		});
		TestFalse(TEXT("Exception on null widget"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("Empty on null widget"), Properties.Num(), 0);
	});

	// ---- ListClassBindableFields errors ----

	It("ListClassBindableFields with null TargetClass returns empty result", [this]()
	{
		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListClassBindableFields(nullptr, nullptr);
		});
		TestFalse(TEXT("Exception on null TargetClass"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("Empty on null TargetClass"), Result.Fields.Num(), 0);
	});

	// ---- ListPropertyPathBindableFields errors ----

	It("ListPropertyPathBindableFields with null widget blueprint returns empty result", [this]()
	{
		FMVVMBindableFieldListResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListPropertyPathBindableFields(nullptr, nullptr, TEXT("Health"));
		});
		TestFalse(TEXT("Exception on null widget blueprint"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("Empty on null widget blueprint"), Result.Fields.Num(), 0);
		TestTrue(TEXT("ErrorMessage stays empty for a caller-mistake script error, not a normal resolution failure"), Result.ErrorMessage.IsEmpty());
	});

	// ---- ListBindingConversionOptions errors ----

	It("ListBindingConversionOptions with null widget blueprint returns default-constructed result", [this]()
	{
		FMVVMBindingConversionResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListBindingConversionOptions(nullptr, nullptr, TEXT("Health"), nullptr, TEXT("Health"));
		});
		TestFalse(TEXT("Exception on null widget blueprint"), Handler->GetException().IsEmpty());
		TestFalse(TEXT("bDirectlyCompatible defaults to false"), Result.bDirectlyCompatible);
		TestEqual(TEXT("No conversion options on null widget blueprint"), Result.ConversionOptions.Num(), 0);
	});

	It("ListBindingConversionOptions with an unresolvable property path returns a default-constructed result and raises no error", [this]()
	{
		FString WidgetBlueprintName = FString::Printf(TEXT("WWidgetBlueprint_ConversionBadPath_%d"), TestCounter++);
		UWidgetBlueprint* LocalWidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), WidgetBlueprintName);
		TestNotNull(TEXT("WidgetBlueprint created"), LocalWidgetBlueprint);
		if (!IsValid(LocalWidgetBlueprint))
		{
			return;
		}

		FMVVMBindingConversionResult Result;
		Handler->CaptureErrorsIn([&]()
		{
			Result = UMVVMToolset59::ListBindingConversionOptions(
				LocalWidgetBlueprint,
				nullptr, TEXT("__NoSuchProperty"),
				nullptr, TEXT("__NoSuchProperty"));
		});
		TestTrue(TEXT("Unresolvable path raises no error -- this is the fact the tool exists to report"), Handler->GetException().IsEmpty());
		TestFalse(TEXT("bDirectlyCompatible defaults to false"), Result.bDirectlyCompatible);
		TestEqual(TEXT("No conversion options for unresolvable path"), Result.ConversionOptions.Num(), 0);
	});

	// ---- GetWidgetViewConfig / SetWidgetViewConfig errors ----

	It("GetWidgetViewConfig with null widget returns false and leaves out-param untouched", [this]()
	{
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::GetWidgetViewConfig(nullptr);
		});
		TestFalse(TEXT("Exception on null widget"), Handler->GetException().IsEmpty());
	});

	It("SetWidgetViewConfig with null widget returns false", [this]()
	{
		FMVVMViewConfig Config;
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::SetWidgetViewConfig(nullptr, Config);
		});
		TestFalse(TEXT("Exception on null widget"), Handler->GetException().IsEmpty());
	});
}

// ============================================================================
// Conversion Function Argument Tests
// ============================================================================

BEGIN_DEFINE_SPEC(FMVVMTest_ConversionFunctionArgument,
	"AIAssistant.Toolsets.MVVM.ConversionFunctionArgument", MVVMToolSetTest::Flags)
	TUniquePtr<FTestAssetSandbox> Sandbox;
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	FGuid BindingId;
	int32 TestCounter = 0;

	// Calls a tool with a script context and returns the script errors it raised.
	FString CallTool(TFunction<void()> Tool)
	{
		UE::ToolsetRegistry::FToolCallExceptionHandler LocalHandler;
		LocalHandler.CaptureErrorsIn([&Tool]()
		{
			Tool();
		});
		return LocalHandler.GetException();
	}

	// The source ListWidgetViewBindings reports for a pin of the binding's source-to-destination conversion, if any.
	TOptional<FMVVMViewBindingSource> FindSource(const FString& ArgumentName)
	{
		for (const FMVVMViewBindingDescription& BindingDescription : UMVVMToolset59::ListWidgetViewBindings(WidgetBlueprint))
		{
			if (BindingDescription.BindingId == BindingId)
			{
				for (const FMVVMViewBindingSource& Source : BindingDescription.SourceToDestinationConversion.Sources)
				{
					if (Source.ArgumentName == ArgumentName)
					{
						return Source;
					}
				}
			}
		}
		return {};
	}
END_DEFINE_SPEC(FMVVMTest_ConversionFunctionArgument)

void FMVVMTest_ConversionFunctionArgument::Define()
{
	BeforeEach([this]() // UE 5.8: 5.8 specs have no BeforeAll; one sandbox per test
	{
		Sandbox = MakeUnique<FTestAssetSandbox>(TEXT("MVVMToolSetTest"));
	});

	AfterEach([this]() // UE 5.8: 5.8 specs have no AfterAll
	{
		Sandbox.Reset();
	});

	// A ViewModel float bound to an Image's Brush through Conv_SetScalarParameter(Brush, ParameterName, Value): the Value pin
	// is bound to the ViewModel's Health and ParameterName holds a literal.
	BeforeEach([this]()
	{
		UBlueprint* ViewModel = UMVVMToolset59::CreateViewModel(FString::Printf(TEXT("VM_ConversionArgument_%d"), TestCounter), Sandbox->Folder(), nullptr);
		WidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), FString::Printf(TEXT("WWidgetBlueprint_ConversionArgument_%d"), TestCounter++));
		if (!TestNotNull(TEXT("ViewModel created"), ViewModel) || !TestNotNull(TEXT("WidgetBlueprint created"), WidgetBlueprint))
		{
			return;
		}

		UMVVMToolset59::AddViewModelProperty(ViewModel, TEXT("Health"), TEXT("float"), TEXT(""));
		// The widget blueprint compiles in these tests, so it references the ViewModel's compiled class rather than its skeleton class.
		FKismetEditorUtilities::CompileBlueprint(ViewModel, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
		UMVVMToolset59::AddViewModelToWidget(WidgetBlueprint, ViewModel->GeneratedClass);

		UImage* Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		Img->bIsVariable = true;
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		BindingId = UMVVMToolset59::CreateViewBinding(WidgetBlueprint, ViewModel->GeneratedClass, TEXT("Health"), Img, TEXT("Brush"), TEXT("Conv_SetScalarParameter"));
		TestTrue(TEXT("Conv_SetScalarParameter binding created"), BindingId.IsValid());
	});

	AfterEach([this]()
	{
		WidgetBlueprint = nullptr;
		BindingId.Invalidate();
	});

	It("SetConversionFunctionArgumentLiteral sets a literal that ListWidgetViewBindings reads back and recompiles only on a change", [this]()
	{
		if (!IsValid(WidgetBlueprint) || !BindingId.IsValid())
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		int32 CompileCount = 0;
		const FDelegateHandle CompiledHandle = WidgetBlueprint->OnCompiled().AddLambda([&CompileCount](UBlueprint*) { ++CompileCount; });
		ON_SCOPE_EXIT
		{
			WidgetBlueprint->OnCompiled().Remove(CompiledHandle);
		};

		FString Exception;
		Exception = CallTool([this]() { UMVVMToolset59::SetConversionFunctionArgumentLiteral(WidgetBlueprint, BindingId, TEXT("ParameterName"), TEXT("Anim_StartValue")); });
		TestEqual(TEXT("Set raised no error"), Exception, FString());
		TestEqual(TEXT("Set recompiled the blueprint once"), CompileCount, 1);

		const TOptional<FMVVMViewBindingSource> Source = FindSource(TEXT("ParameterName"));
		if (TestTrue(TEXT("ListWidgetViewBindings reports ParameterName"), Source.IsSet()))
		{
			TestEqual(TEXT("Read-back value"), Source->Path, FString(TEXT("Anim_StartValue")));
			TestTrue(TEXT("Read-back is a literal"), Source->bIsLiteralValue);
		}

		// Same check as UMGToolSet.CompileWidgetBlueprint.
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave);
		TestTrue(TEXT("Widget blueprint compiles after the set"), WidgetBlueprint->Status != BS_Error);

		CompileCount = 0;
		Exception = CallTool([this]() { UMVVMToolset59::SetConversionFunctionArgumentLiteral(WidgetBlueprint, BindingId, TEXT("ParameterName"), TEXT("Anim_StartValue")); });
		TestEqual(TEXT("Second set raised no error"), Exception, FString());
		TestEqual(TEXT("Second set did not recompile"), CompileCount, 0);
	});

	It("SetConversionFunctionArgumentLiteral and ClearConversionFunctionArgumentPath report the refusal reason as a script error", [this]()
	{
		if (!IsValid(WidgetBlueprint) || !BindingId.IsValid())
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		AddExpectedErrorPlain(TEXT("Failed to set conversion function argument"), EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedErrorPlain(TEXT("Failed to clear the path of conversion function argument"), EAutomationExpectedErrorFlags::Contains, 0);

		FString Exception;
		Exception = CallTool([this]() { UMVVMToolset59::SetConversionFunctionArgumentLiteral(WidgetBlueprint, BindingId, TEXT("NoSuchPin"), TEXT("1")); });
		TestTrue(FString::Printf(TEXT("Set on an unknown pin reports it ('%s')"), *Exception), Exception.Contains(TEXT("has no input pin")));

		Exception = CallTool([this]() { UMVVMToolset59::ClearConversionFunctionArgumentPath(WidgetBlueprint, BindingId, TEXT("NoSuchPin")); });
		TestTrue(FString::Printf(TEXT("Clear on an unknown pin reports it ('%s')"), *Exception), Exception.Contains(TEXT("no such input pin")));

		Exception = CallTool([this]() { UMVVMToolset59::ClearConversionFunctionArgumentPath(WidgetBlueprint, BindingId, TEXT("ParameterName")); });
		TestTrue(FString::Printf(TEXT("Clear on a literal pin reports it is not bound ('%s')"), *Exception), Exception.Contains(TEXT("not bound to a property path")));

		const TOptional<FMVVMViewBindingSource> Source = FindSource(TEXT("Value"));
		TestTrue(TEXT("Value is still bound to a path"), Source.IsSet() && !Source->bIsLiteralValue);
	});

	It("ClearConversionFunctionArgumentPath turns a path-bound pin into a literal that can then be set, even when the blueprint fails to compile", [this]()
	{
		UMVVMEditorSubsystem* MVVMEditorSubsystem = GEditor->GetEditorSubsystem<UMVVMEditorSubsystem>();
		if (!IsValid(WidgetBlueprint) || !BindingId.IsValid() || !IsValid(MVVMEditorSubsystem))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		const TOptional<FMVVMViewBindingSource> BoundSource = FindSource(TEXT("Value"));
		if (!TestTrue(TEXT("Value starts bound to a path"), BoundSource.IsSet() && !BoundSource->bIsLiteralValue))
		{
			return;
		}

		// A second binding without paths makes every compile of the blueprint fail; both edits are kept and a warning names the blueprint.
		MVVMEditorSubsystem->AddBinding(WidgetBlueprint);
		AddExpectedErrorPlain(TEXT("A source path is required, but not set."), EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedMessagePlain(FString::Printf(TEXT("widget blueprint '%s' failed to compile"), *WidgetBlueprint->GetPathName()), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);

		FString Exception;
		Exception = CallTool([this]() { UMVVMToolset59::ClearConversionFunctionArgumentPath(WidgetBlueprint, BindingId, TEXT("Value")); });
		TestEqual(TEXT("Clear raised no error"), Exception, FString());
		TestTrue(TEXT("The blueprint failed to compile"), WidgetBlueprint->Status == BS_Error);

		const TOptional<FMVVMViewBindingSource> ClearedSource = FindSource(TEXT("Value"));
		TestTrue(TEXT("Value is a literal after the clear"), ClearedSource.IsSet() && ClearedSource->bIsLiteralValue);

		Exception = CallTool([this]() { UMVVMToolset59::SetConversionFunctionArgumentLiteral(WidgetBlueprint, BindingId, TEXT("Value"), TEXT("0.5")); });
		TestEqual(TEXT("Set raised no error"), Exception, FString());

		const TOptional<FMVVMViewBindingSource> SetSource = FindSource(TEXT("Value"));
		if (TestTrue(TEXT("ListWidgetViewBindings reports Value"), SetSource.IsSet()))
		{
			TestEqual(TEXT("Read-back value"), SetSource->Path, FString(TEXT("0.5")));
			TestTrue(TEXT("Read-back is a literal"), SetSource->bIsLiteralValue);
		}
	});
}

// ============================================================================
// Condition Tests
// ============================================================================

BEGIN_DEFINE_SPEC(FMVVMTest_Conditions,
	"AIAssistant.Toolsets.MVVM.Conditions", MVVMToolSetTest::Flags)
	TUniquePtr<FTestAssetSandbox> Sandbox;
	int32 TestCounter = 0;
	bool bAllowConditionBindingBefore = true;
	TUniquePtr<UE::ToolsetRegistry::FToolCallExceptionHandler> Handler;
	// Img (the root widget) and the variables bFlag (bool) and Label (string), compiled.
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	UWidget* Img = nullptr;

	UMVVMBlueprintViewCondition* AddFlagCondition()
	{
		UMVVMBlueprintViewCondition* Condition = nullptr;
		Handler->CaptureErrorsIn([&]()
		{
			Condition = UMVVMToolset59::AddViewCondition(WidgetBlueprint, nullptr, TEXT("bFlag"), Img, TEXT("SetOpacity"), EMVVMConditionOperation::Equal, 1.0f);
		});
		return Condition;
	}
END_DEFINE_SPEC(FMVVMTest_Conditions)

void FMVVMTest_Conditions::Define()
{
	BeforeEach([this]() // UE 5.8: 5.8 specs have no BeforeAll; one sandbox per test
	{
		Sandbox = MakeUnique<FTestAssetSandbox>(TEXT("MVVMToolSetTest"));
	});

	AfterEach([this]() // UE 5.8: 5.8 specs have no AfterAll
	{
		Sandbox.Reset();
	});

	BeforeEach([this]()
	{
		Handler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
		bAllowConditionBindingBefore = GetDefault<UMVVMDeveloperProjectSettings>()->bAllowConditionBinding;
		GetMutableDefault<UMVVMDeveloperProjectSettings>()->bAllowConditionBinding = true;

		WidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), FString::Printf(TEXT("WWidgetBlueprint_Conditions_%d"), TestCounter++));
		if (!IsValid(WidgetBlueprint))
		{
			return;
		}

		Img = WidgetBlueprint->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("Img"));
		WidgetBlueprint->WidgetTree->RootWidget = Img;

		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(WidgetBlueprint, TEXT("bFlag"), BoolType);
		FEdGraphPinType StringType;
		StringType.PinCategory = UEdGraphSchema_K2::PC_String;
		FBlueprintEditorUtils::AddMemberVariable(WidgetBlueprint, TEXT("Label"), StringType);
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
	});

	AfterEach([this]()
	{
		GetMutableDefault<UMVVMDeveloperProjectSettings>()->bAllowConditionBinding = bAllowConditionBindingBefore;
		Handler.Reset();
		WidgetBlueprint = nullptr;
		Img = nullptr;
	});

	It("AddViewCondition adds a condition visible via ListViewConditions, undone in one step", [this]()
	{
		if (!TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint))
		{
			return;
		}

		UMVVMBlueprintViewCondition* Condition = AddFlagCondition();
		TestTrue(TEXT("Add raised no error"), Handler->GetException().IsEmpty());
		TestNotNull(TEXT("Add returns the condition"), Condition);

		const TArray<FMVVMViewConditionDescription> Descriptions = UMVVMToolset59::ListViewConditions(WidgetBlueprint);
		if (TestEqual(TEXT("One condition listed"), Descriptions.Num(), 1))
		{
			TestTrue(TEXT("Listed condition"), Descriptions[0].Condition == Condition);
			TestEqual(TEXT("Source path"), Descriptions[0].SourcePath, WidgetBlueprint->GetName() + TEXT(".bFlag"));
			TestEqual(TEXT("Destination path"), Descriptions[0].DestinationPath, FString(TEXT("Img.SetOpacity")));
			TestTrue(TEXT("Operation"), Descriptions[0].Operation == EMVVMConditionOperation::Equal);
			TestEqual(TEXT("Value"), Descriptions[0].Value, 1.0f);
			TestEqual(TEXT("No compile errors"), FString::Join(Descriptions[0].CompileErrors, TEXT(" ")), FString());
		}
		TestTrue(TEXT("Widget blueprint compiles"), WidgetBlueprint->Status != BS_Error);

		GEditor->UndoTransaction();
		TestEqual(TEXT("Undo removed the condition"), UMVVMToolset59::ListViewConditions(WidgetBlueprint).Num(), 0);
	});

	It("SetViewConditionOperation changes the comparison of a condition", [this]()
	{
		if (!TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint))
		{
			return;
		}

		UMVVMBlueprintViewCondition* Condition = AddFlagCondition();
		bool bSet = false;
		Handler->CaptureErrorsIn([&]()
		{
			bSet = UMVVMToolset59::SetViewConditionOperation(WidgetBlueprint, Condition, EMVVMConditionOperation::BetweenInclusive, 2.0f, 5.0f);
		});
		TestTrue(TEXT("Set raised no error"), Handler->GetException().IsEmpty());
		TestTrue(TEXT("Set returns true"), bSet);
		TestTrue(TEXT("Operation"), Condition->GetOperation() == EMVVMConditionOperation::BetweenInclusive);
		TestEqual(TEXT("Value"), Condition->GetOperationValue(), 2.0f);
		TestEqual(TEXT("MaxValue"), Condition->GetOperationMaxValue(), 5.0f);
	});

	It("RemoveViewCondition removes the condition", [this]()
	{
		if (!TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint))
		{
			return;
		}

		UMVVMBlueprintViewCondition* Condition = AddFlagCondition();
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::RemoveViewCondition(WidgetBlueprint, Condition);
		});
		TestTrue(TEXT("Remove raised no error"), Handler->GetException().IsEmpty());
		TestEqual(TEXT("No condition listed"), UMVVMToolset59::ListViewConditions(WidgetBlueprint).Num(), 0);
	});

	It("AddViewCondition with an unsupported source type returns null and adds no condition", [this]()
	{
		AddExpectedError(TEXT("Failed to add view condition"), EAutomationExpectedErrorFlags::Contains, -1);
		if (!TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint))
		{
			return;
		}

		UMVVMBlueprintViewCondition* Condition = nullptr;
		Handler->CaptureErrorsIn([&]()
		{
			Condition = UMVVMToolset59::AddViewCondition(WidgetBlueprint, nullptr, TEXT("Label"), Img, TEXT("SetOpacity"), EMVVMConditionOperation::Equal, 1.0f);
		});
		TestNull(TEXT("Add returns null"), Condition);
		TestTrue(TEXT("Error names the supported types"), Handler->GetException().Contains(TEXT("Supported source types")));
		TestEqual(TEXT("No condition listed"), UMVVMToolset59::ListViewConditions(WidgetBlueprint).Num(), 0);
	});

	It("AddViewCondition while condition bindings are disabled returns null", [this]()
	{
		AddExpectedError(TEXT("Failed to add view condition"), EAutomationExpectedErrorFlags::Contains, -1);
		if (!TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint))
		{
			return;
		}

		GetMutableDefault<UMVVMDeveloperProjectSettings>()->bAllowConditionBinding = false;
		TestNull(TEXT("Add returns null"), AddFlagCondition());
		TestTrue(TEXT("Error names the setting"), Handler->GetException().Contains(TEXT("bAllowConditionBinding")));
	});

	It("SetViewConditionOperation and RemoveViewCondition with a removed condition raise an error", [this]()
	{
		AddExpectedError(TEXT("view condition"), EAutomationExpectedErrorFlags::Contains, -1);
		if (!TestNotNull(TEXT("WidgetBlueprint valid"), WidgetBlueprint))
		{
			return;
		}

		UMVVMBlueprintViewCondition* Condition = AddFlagCondition();
		UMVVMToolset59::RemoveViewCondition(WidgetBlueprint, Condition);

		bool bSet = true;
		Handler->CaptureErrorsIn([&]()
		{
			bSet = UMVVMToolset59::SetViewConditionOperation(WidgetBlueprint, Condition, EMVVMConditionOperation::NotEqual, 0.0f);
		});
		TestFalse(TEXT("Set returns false"), bSet);
		TestTrue(TEXT("Set error"), Handler->GetException().Contains(TEXT("is not in the MVVM View")));

		Handler = MakeUnique<UE::ToolsetRegistry::FToolCallExceptionHandler>();
		Handler->CaptureErrorsIn([&]()
		{
			UMVVMToolset59::RemoveViewCondition(WidgetBlueprint, Condition);
		});
		TestTrue(TEXT("Remove error"), Handler->GetException().Contains(TEXT("is not in the MVVM View")));
	});
}

// ============================================================================
// Event Argument Tests
// ============================================================================

BEGIN_DEFINE_SPEC(FMVVMTest_EventArgument,
	"AIAssistant.Toolsets.MVVM.EventArgument", MVVMToolSetTest::Flags)
	TUniquePtr<FTestAssetSandbox> Sandbox;
	UWidgetBlueprint* WidgetBlueprint = nullptr;
	UMVVMBlueprintViewEvent* EventA = nullptr;
	UMVVMBlueprintViewEvent* EventB = nullptr;

	// Calls a tool with a script context and returns the script errors it raised.
	FString CallTool(TFunction<void()> Tool)
	{
		UE::ToolsetRegistry::FToolCallExceptionHandler LocalHandler;
		LocalHandler.CaptureErrorsIn([&Tool]()
		{
			Tool();
		});
		return LocalHandler.GetException();
	}

	// Tests that ListWidgetViewEventBindings reports Expected as the Index literal of the event raised by the widget WidgetName.
	void TestIndexLiteral(UWidgetBlueprint* InWidgetBlueprint, FName WidgetName, const FString& Expected)
	{
		for (const FMVVMViewEventBindingDescription& EventDescription : UMVVMToolset59::ListWidgetViewEventBindings(InWidgetBlueprint))
		{
			const FMVVMViewBindingSource* Source = EventDescription.Arguments.FindByPredicate([](const FMVVMViewBindingSource& Other) { return Other.ArgumentName == TEXT("Index"); });
			if (EventDescription.EventWidgetName == WidgetName && Source != nullptr)
			{
				TestEqual(FString::Printf(TEXT("%s Index read-back value"), *WidgetName.ToString()), Source->Path, Expected);
				TestTrue(FString::Printf(TEXT("%s Index read-back is a literal"), *WidgetName.ToString()), Source->bIsLiteralValue);
				return;
			}
		}
		AddError(FString::Printf(TEXT("ListWidgetViewEventBindings reports no Index for %s"), *WidgetName.ToString()));
	}
END_DEFINE_SPEC(FMVVMTest_EventArgument)

void FMVVMTest_EventArgument::Define()
{
	BeforeEach([this]() // UE 5.8: 5.8 specs have no BeforeAll; one sandbox per test
	{
		Sandbox = MakeUnique<FTestAssetSandbox>(TEXT("MVVMToolSetTest"));
	});

	AfterEach([this]() // UE 5.8: 5.8 specs have no AfterAll
	{
		Sandbox.Reset();
	});

	// BtnA and BtnB inside Switcher, each button's OnClicked bound to Switcher.SetActiveWidgetIndex(Index), compiled.
	BeforeEach([this]()
	{
		// A unique name, so the package the save/reload test writes to disk never collides with one from another run.
		WidgetBlueprint = MVVMToolSetTest::CreateTestWidgetBlueprintWithMVVM(Sandbox->Folder(), FTestAssetSandbox::UniqueName(TEXT("WWidgetBlueprint_EventArgument")));
		if (!TestNotNull(TEXT("WidgetBlueprint created"), WidgetBlueprint))
		{
			return;
		}

		UWidgetSwitcher* Switcher = WidgetBlueprint->WidgetTree->ConstructWidget<UWidgetSwitcher>(UWidgetSwitcher::StaticClass(), TEXT("Switcher"));
		Switcher->bIsVariable = true;
		WidgetBlueprint->WidgetTree->RootWidget = Switcher;
		UButton* BtnA = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("BtnA"));
		BtnA->bIsVariable = true;
		Switcher->AddChild(BtnA);
		UButton* BtnB = WidgetBlueprint->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), TEXT("BtnB"));
		BtnB->bIsVariable = true;
		Switcher->AddChild(BtnB);
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);

		EventA = UMVVMToolset59::CreateViewEventBinding(WidgetBlueprint, BtnA, TEXT("OnClicked"), Switcher, TEXT("SetActiveWidgetIndex"));
		EventB = UMVVMToolset59::CreateViewEventBinding(WidgetBlueprint, BtnB, TEXT("OnClicked"), Switcher, TEXT("SetActiveWidgetIndex"));
		TestNotNull(TEXT("BtnA event binding created"), EventA);
		TestNotNull(TEXT("BtnB event binding created"), EventB);

		// Start up to date, so that only an edit recompiles.
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint, EBlueprintCompileOptions::SkipSave | EBlueprintCompileOptions::SkipGarbageCollection);
	});

	AfterEach([this]()
	{
		WidgetBlueprint = nullptr;
		EventA = nullptr;
		EventB = nullptr;
	});

	It("SetEventBindingArgumentLiteral sets a different literal per event that ListWidgetViewEventBindings reads back and recompiles only on a change", [this]()
	{
		if (!IsValid(WidgetBlueprint) || !IsValid(EventA) || !IsValid(EventB))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		int32 CompileCount = 0;
		const FDelegateHandle CompiledHandle = WidgetBlueprint->OnCompiled().AddLambda([&CompileCount](UBlueprint*) { ++CompileCount; });
		ON_SCOPE_EXIT
		{
			WidgetBlueprint->OnCompiled().Remove(CompiledHandle);
		};

		FString Exception;
		Exception = CallTool([this]() { UMVVMToolset59::SetEventBindingArgumentLiteral(WidgetBlueprint, EventA, TEXT("Index"), TEXT("1")); });
		TestEqual(TEXT("Set on BtnA raised no error"), Exception, FString());
		Exception = CallTool([this]() { UMVVMToolset59::SetEventBindingArgumentLiteral(WidgetBlueprint, EventB, TEXT("Index"), TEXT("2")); });
		TestEqual(TEXT("Set on BtnB raised no error"), Exception, FString());
		TestEqual(TEXT("Each set recompiled the blueprint once"), CompileCount, 2);
		TestTrue(TEXT("Widget blueprint compiles after the sets"), WidgetBlueprint->Status != BS_Error);

		TestIndexLiteral(WidgetBlueprint, TEXT("BtnA"), TEXT("1"));
		TestIndexLiteral(WidgetBlueprint, TEXT("BtnB"), TEXT("2"));

		CompileCount = 0;
		Exception = CallTool([this]() { UMVVMToolset59::SetEventBindingArgumentLiteral(WidgetBlueprint, EventA, TEXT("Index"), TEXT("1")); });
		TestEqual(TEXT("Second set on BtnA raised no error"), Exception, FString());
		TestEqual(TEXT("Second set on BtnA did not recompile"), CompileCount, 0);
	});

	It("SetEventBindingArgumentLiteral reports the refusal reason as a script error", [this]()
	{
		if (!IsValid(WidgetBlueprint) || !IsValid(EventA))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		AddExpectedErrorPlain(TEXT("Failed to set event binding argument"), EAutomationExpectedErrorFlags::Contains, 0);

		const FString Exception = CallTool([this]() { UMVVMToolset59::SetEventBindingArgumentLiteral(WidgetBlueprint, EventA, TEXT("NoSuchPin"), TEXT("1")); });
		TestTrue(FString::Printf(TEXT("Set on an unknown pin reports it ('%s')"), *Exception), Exception.Contains(TEXT("Failed to set event binding argument 'NoSuchPin'")) && Exception.Contains(TEXT("has no input pin")));
		TestIndexLiteral(WidgetBlueprint, TEXT("BtnA"), TEXT("0"));
	});

	It("A literal set by SetEventBindingArgumentLiteral is still reported after the widget blueprint is saved and reloaded", [this]()
	{
		if (!IsValid(WidgetBlueprint) || !IsValid(EventA) || !IsValid(EventB))
		{
			AddInfo(TEXT("Skipped: [Invalid test fixtures]"));
			return;
		}

		FString Exception;
		Exception = CallTool([this]() { UMVVMToolset59::SetEventBindingArgumentLiteral(WidgetBlueprint, EventA, TEXT("Index"), TEXT("1")); });
		TestEqual(TEXT("Set on BtnA raised no error"), Exception, FString());
		Exception = CallTool([this]() { UMVVMToolset59::SetEventBindingArgumentLiteral(WidgetBlueprint, EventB, TEXT("Index"), TEXT("2")); });
		TestEqual(TEXT("Set on BtnB raised no error"), Exception, FString());

		UPackage* Package = WidgetBlueprint->GetPackage();
		const FString PackageName = Package->GetName();
		const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		// The sandbox cleans up in-memory packages only, so the saved file is deleted here on every exit path.
		ON_SCOPE_EXIT
		{
			// The reloaded package keeps the file open until its loader is reset.
			if (UPackage* LoadedPackage = FindPackage(nullptr, *PackageName))
			{
				ResetLoaders(LoadedPackage);
			}
			const bool bRequireExists = false;
			const bool bEvenReadOnly = true;
			IFileManager::Get().Delete(*Filename, bRequireExists, bEvenReadOnly);
		};
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!TestTrue(TEXT("Widget blueprint saved"), UPackage::SavePackage(Package, WidgetBlueprint, *Filename, SaveArgs)))
		{
			return;
		}

		// The reload replaces the widget blueprint and its events with new objects.
		const FString ObjectPath = WidgetBlueprint->GetPathName();
		WidgetBlueprint = nullptr;
		EventA = nullptr;
		EventB = nullptr;
		FText ReloadError;
		const bool bReloaded = UPackageTools::ReloadPackages(TArray<UPackage*>{ Package }, ReloadError, EReloadPackagesInteractionMode::AssumePositive);
		if (!TestTrue(FString::Printf(TEXT("Widget blueprint reloaded ('%s')"), *ReloadError.ToString()), bReloaded))
		{
			return;
		}

		UWidgetBlueprint* ReloadedWidgetBlueprint = FindObject<UWidgetBlueprint>(nullptr, *ObjectPath);
		if (!TestNotNull(TEXT("Reloaded widget blueprint"), ReloadedWidgetBlueprint))
		{
			return;
		}

		TestIndexLiteral(ReloadedWidgetBlueprint, TEXT("BtnA"), TEXT("1"));
		TestIndexLiteral(ReloadedWidgetBlueprint, TEXT("BtnB"), TEXT("2"));
	});
}

// ============================================================================
// Registration Test
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMVVMTest_Registration59,
	"AIAssistant.Toolsets.MVVM.Registration.ToolsetRegistered",
	MVVMToolSetTest::Flags)

bool FMVVMTest_Registration59::RunTest(const FString&)
{
	FString Schemas = UToolsetRegistry::GetAllToolsetJsonSchemas();
	TestTrue(TEXT("MVVMToolset in registry"), Schemas.Contains(TEXT("MVVMToolset59.MVVMToolset59"))); // UE 5.8: 5.8's own MVVMToolset would match the bare name
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
