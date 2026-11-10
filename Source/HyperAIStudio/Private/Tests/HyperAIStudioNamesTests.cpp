// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioPIEPlaytestToolset.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioNamesTest,
	"HyperAIStudio.NativeTools.Names",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioNamesTest::RunTest(const FString& Parameters)
{
	(void)Parameters;
	using N = FHyperAIStudioNames;
	FString Reason;
	// Every name and path that passes still fits an FName with a '.', its short name and "_C" after it.
	TestTrue(TEXT("the limit leaves room for a short name and _C"), N::MaxLength * 2 + 3 < NAME_SIZE);

	TestTrue(TEXT("a name"), N::IsName(TEXT("Pick"), TEXT("TagFilter"), &Reason));
	TestTrue(TEXT("an empty name is the caller's question"), N::IsName(TEXT(""), TEXT("TagFilter"), &Reason));
	TestTrue(TEXT("a name at the limit"), N::IsName(FString::ChrN(N::MaxLength, TEXT('a')), TEXT("TagFilter"), &Reason));
	TestFalse(TEXT("a name past the limit"), N::IsName(FString::ChrN(N::MaxLength + 1, TEXT('a')), TEXT("TagFilter"), &Reason));
	TestTrue(TEXT("the reason names the field and the limit"), Reason.StartsWith(TEXT("TagFilter is 501 characters long")) && Reason.Contains(TEXT("500")));
	TestFalse(TEXT("an FName's limit is far past it"), N::IsName(FString::ChrN(NAME_SIZE + 100, TEXT('a')), nullptr, &Reason));
	TestTrue(TEXT("an unnamed field reads as the name"), Reason.StartsWith(TEXT("The name ")));

	for (const TCHAR* Path : {TEXT("/Game/A"), TEXT("/Game/Folder/Asset.Asset"), TEXT("/Game/Maps/Level.Level:PersistentLevel.StaticMeshActor_1"),
		TEXT("/Script/Engine.StaticMeshActor"), TEXT("/Temp/Untitled_1.Untitled_1:PersistentLevel.Cube"), TEXT("/Game/BP_Door.BP_Door_C")})
	{
		TestTrue(FString::Printf(TEXT("%s is an object path"), Path), N::IsObjectPath(Path, TEXT("Asset"), &Reason));
	}
	const FString Long = TEXT("/Game/") + FString::ChrN(1100, TEXT('a'));
	TestFalse(TEXT("a pasted blob is no path"), N::IsObjectPath(Long, TEXT("Asset"), &Reason));
	TestTrue(TEXT("and the reply does not echo it"), Reason.Len() < 200);
	TestFalse(TEXT("a long name after the dot"), N::IsObjectPath(TEXT("/Game/A.") + FString::ChrN(1100, TEXT('a')), TEXT("Asset"), &Reason));
	TestFalse(TEXT("a double slash, which CreatePackage and LoadObject stop on"), N::IsObjectPath(TEXT("/Game//A.A"), TEXT("Asset"), &Reason));
	TestTrue(TEXT("said so"), Reason.Contains(TEXT("double slash")));
	TestFalse(TEXT("a short name, which FTopLevelAssetPath ensures on"), N::IsObjectPath(TEXT("StaticMeshActor"), TEXT("Class"), &Reason));
	TestFalse(TEXT("an export path's quote, which FTopLevelAssetPath ensures on"), N::IsObjectPath(TEXT("/Game/A'"), TEXT("Asset"), &Reason));
	// The editor's Copy Reference form: every lookup strips the class and quotes, so the path inside is what is checked.
	TestTrue(TEXT("a copied reference"), N::IsObjectPath(TEXT("/Script/Engine.StaticMesh'/Game/Props/SM_Rock.SM_Rock'"), TEXT("Asset"), &Reason));
	TestTrue(TEXT("and its short-class form"), N::IsObjectPath(TEXT("StaticMesh'/Game/A.A'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("a copied reference whose path has a double slash"), N::IsObjectPath(TEXT("/Script/Engine.StaticMesh'/Game//A.A'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("a quoted short name"), N::IsObjectPath(TEXT("StaticMesh'SM_Rock'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("a class that is neither a name nor a path"), N::IsObjectPath(TEXT("Static//Mesh'/Game/A.A'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("quotes with no class"), N::IsObjectPath(TEXT("'/Game/A.A'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("a third quote"), N::IsObjectPath(TEXT("StaticMesh'/Game/A'.A'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("a copied reference past the limit"), N::IsObjectPath(TEXT("StaticMesh'/Game/") + FString::ChrN(N::MaxLength, TEXT('a')) + TEXT("'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("but it names no package to make"), N::IsPackagePath(TEXT("Package'/Game/A'"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("nor a free one"), N::IsFreePackage(TEXT("Package'/Game/HyperAINamesTestNothingHere'")));
	TestFalse(TEXT("a trailing slash"), N::IsObjectPath(TEXT("/Game/A/"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("a root alone is no package"), N::IsObjectPath(TEXT("/A"), TEXT("Asset"), &Reason));
	TestFalse(TEXT("empty"), N::IsObjectPath(TEXT(""), TEXT("Asset"), &Reason));

	TestTrue(TEXT("a package to make"), N::IsPackagePath(TEXT("/Game/PCG/PCG_Forest"), TEXT("Graph"), &Reason));
	TestFalse(TEXT("an object path is not a package"), N::IsPackagePath(TEXT("/Game/PCG/PCG_Forest.PCG_Forest"), TEXT("Graph"), &Reason));
	TestFalse(TEXT("a space"), N::IsPackagePath(TEXT("/Game/PCG/PCG Forest"), TEXT("Graph"), &Reason));
	TestFalse(TEXT("a root nothing is saved to"), N::IsPackagePath(TEXT("/Script/PCG_Forest"), TEXT("Graph"), &Reason));
	TestFalse(TEXT("an unmounted root"), N::IsPackagePath(TEXT("/NoSuchRoot/PCG_Forest"), TEXT("Graph"), &Reason));
	TestFalse(TEXT("a double slash"), N::IsPackagePath(TEXT("/Game//PCG_Forest"), TEXT("Graph"), &Reason));

	TestTrue(TEXT("a root is a folder"), N::IsFolderPath(TEXT("/Game"), TEXT("LayerInfoFolder"), &Reason));
	TestTrue(TEXT("a folder"), N::IsFolderPath(TEXT("/Game/Landscape/LayerInfos"), TEXT("LayerInfoFolder"), &Reason));
	TestFalse(TEXT("a folder's trailing slash"), N::IsFolderPath(TEXT("/Game/Landscape/"), TEXT("LayerInfoFolder"), &Reason));
	TestFalse(TEXT("a folder's double slash"), N::IsFolderPath(TEXT("/Game//Landscape"), TEXT("LayerInfoFolder"), &Reason));
	TestFalse(TEXT("a folder's trailing double slash"), N::IsFolderPath(TEXT("/Game/Landscape//"), TEXT("LayerInfoFolder"), &Reason));
	TestFalse(TEXT("a folder with a dot"), N::IsFolderPath(TEXT("/Game/Land.scape"), TEXT("LayerInfoFolder"), &Reason));
	TestFalse(TEXT("a long folder"), N::IsFolderPath(TEXT("/Game/") + FString::ChrN(1100, TEXT('a')), TEXT("LayerInfoFolder"), &Reason));
	TestFalse(TEXT("an empty folder"), N::IsFolderPath(TEXT(""), TEXT("LayerInfoFolder"), &Reason));

	// A request's strings, through its arrays and structs: the field that does not fit is named by its path.
	FHyperAIPIEPlaytestRequest Request;
	Request.Steps.AddDefaulted(2);
	Request.Steps[0].TargetObjectPath = TEXT("/Game/Maps/Level.Level:PersistentLevel.Door");
	TestTrue(TEXT("a request of names fits"), N::CheckRequest(Request, &Reason));
	Request.Steps[1].Name = FString::ChrN(1100, TEXT('a'));
	TestFalse(TEXT("a name past the limit, two levels down, is found"), N::CheckRequest(Request, &Reason));
	TestTrue(TEXT("and named by its path: ") + Reason.Left(80), Reason.StartsWith(TEXT("Steps[1].Name is 1100 characters long")));
	Request.Steps[1].Name.Reset();
	Request.Steps[1].Value.String = FString::ChrN(1100, TEXT('b'));
	TestFalse(TEXT("a long value is refused unless it is free text"), N::CheckRequest(Request, &Reason));
	TestTrue(TEXT("which the tool says it is"), N::CheckRequest(Request, &Reason, {TEXT("String")}));

	// Text imported into a property: names and references are held to the rules, strings are not.
	const FProperty* Socket = FindFProperty<FNameProperty>(USceneComponent::StaticClass(), TEXT("AttachSocketName"));
	const FProperty* Mesh = FindFProperty<FObjectPropertyBase>(UStaticMeshComponent::StaticClass(), TEXT("StaticMesh"));
	const FProperty* Location = FindFProperty<FStructProperty>(USceneComponent::StaticClass(), TEXT("RelativeLocation"));
	const FProperty* Label = FindFProperty<FStrProperty>(AActor::StaticClass(), TEXT("ActorLabel"));
	if (TestTrue(TEXT("the properties are found"), Socket && Mesh && Location && Label))
	{
		TestTrue(TEXT("a name"), N::CanImportText(*Socket, TEXT("Hand_R"), TEXT("Value"), &Reason));
		TestFalse(TEXT("a name past the limit"), N::CanImportText(*Socket, FString::ChrN(1100, TEXT('a')), TEXT("Value"), &Reason));
		TestTrue(TEXT("an object path"), N::CanImportText(*Mesh, TEXT("/Engine/BasicShapes/Cube.Cube"), TEXT("Value"), &Reason));
		TestTrue(TEXT("None"), N::CanImportText(*Mesh, TEXT("None"), TEXT("Value"), &Reason));
		TestFalse(TEXT("an object path with a double slash, whose load would make that package"), N::CanImportText(*Mesh, TEXT("/Game//SM_Nope.SM_Nope"), TEXT("Value"), &Reason));
		TestFalse(TEXT("and in export form"), N::CanImportText(*Mesh, TEXT("StaticMesh'/Game//SM_Nope.SM_Nope'"), TEXT("Value"), &Reason));
		TestFalse(TEXT("an object path past the limit"), N::CanImportText(*Mesh, TEXT("/Game/") + FString::ChrN(1100, TEXT('a')), TEXT("Value"), &Reason));
		TestTrue(TEXT("a struct"), N::CanImportText(*Location, TEXT("(X=1.0,Y=2.0,Z=3.0)"), TEXT("Value"), &Reason));
		TestFalse(TEXT("a struct referencing a path with a double slash"), N::CanImportText(*Location, TEXT("(X=/Game//SM_Nope.SM_Nope)"), TEXT("Value"), &Reason));
		TestTrue(TEXT("but not a quoted string in it"), N::CanImportText(*Location, TEXT("(X=\"http://example.com\")"), TEXT("Value"), &Reason));
		TestTrue(TEXT("a string takes anything"), N::CanImportText(*Label, TEXT("http://example.com//") + FString::ChrN(1100, TEXT('a')), TEXT("Value"), &Reason));
	}
	return true;
}

#endif
