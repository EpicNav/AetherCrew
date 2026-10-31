// Games by Hyper 2026.

#include "HyperAIStudioModelCreator.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "DetailCategoryBuilder.h"
#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "EditorFramework/AssetImportData.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HttpModule.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioExternalService.h"
#include "HyperAIStudioMeshGenJob.h"
#include "HyperAIStudioMeshGenToolset.h"
#include "HyperAIStudioTripoSettings.h"
#include "IDetailCustomization.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PropertyEditorModule.h"
#include "Subsystems/ImportSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioModelCreatorWatch"

/**
 * Models from Tripo Studio. The user generates on tripo3d.ai and presses Send to Unreal in Tripo's own DCC Bridge plugin,
 * which imports the model as any importer does. HyperAI never speaks to the bridge or to Tripo Studio: it only notices a
 * static or skeletal mesh arriving that looks like Tripo's (by folder, name or source file) and offers to check it and
 * make the free fixes, as an approval in Activity and a notification. Also Tripo's API account, read for the settings.
 */
namespace HyperAIStudio::MeshGen::Creator
{
	using namespace HyperAIStudio::MeshGen::Private;

	namespace WatchDetail
	{
		int32 OwnImports = 0;
		FDelegateHandle PostImportHandle;
		FDelegateHandle AddedHandle;
		TSet<FString> OfferedImportPaths;
		TArray<FString> Offers;

		FString SourceFileOf(UObject& Mesh)
		{
			const UAssetImportData* Data = nullptr;
			if (const UStaticMesh* Static = Cast<UStaticMesh>(&Mesh))
			{
				Data = Static->GetAssetImportData();
			}
			else if (const USkeletalMesh* Skeletal = Cast<USkeletalMesh>(&Mesh))
			{
				Data = Skeletal->GetAssetImportData();
			}
			return Data ? Data->GetFirstFilename() : FString();
		}

		void Consider(UObject* Object)
		{
			const UHyperAIStudioTripoSettings* Settings = GetDefault<UHyperAIStudioTripoSettings>();
			if (!Object || OwnImports > 0 || !Settings->bWatchTripoBridgeImports || (!Cast<UStaticMesh>(Object) && !Cast<USkeletalMesh>(Object)))
			{
				return;
			}
			const FString Path = Object->GetPathName();
			if (OfferedImportPaths.Contains(Path) || !IsTripoImport(Object->GetClass()->GetName(), Path, SourceFileOf(*Object), Settings->TripoBridgeFolders, Settings->TripoNameMarkers))
			{
				return;
			}
			OfferedImportPaths.Add(Path);
			// The importer may not be done with it this frame: the offer is made a moment later.
			const FSoftObjectPath Soft(Object);
			const bool bNotify = Settings->bNotifyTripoBridgeImports;
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Soft, bNotify](float)
			{
				if (UObject* Mesh = Soft.ResolveObject())
				{
					OfferCheckAndFix(*Mesh, bNotify);
				}
				return false;
			}), 1.0f);
		}

		void OnPostImport(UFactory*, UObject* Object)
		{
			Consider(Object);
		}

		void OnAssetAdded(const FAssetData& Asset)
		{
			// Only once the start-up scan is done: an asset found on disk is not a new import.
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			if (Registry.IsLoadingAssets() || OwnImports > 0)
			{
				return;
			}
			const FString Class = Asset.AssetClassPath.GetAssetName().ToString();
			if (Class != TEXT("StaticMesh") && Class != TEXT("SkeletalMesh"))
			{
				return;
			}
			if (UObject* Object = Asset.FastGetAsset(/*bLoad=*/false))
			{
				Consider(Object);
			}
		}

		// ------------------------------------------------------------ the account

		struct FAccount
		{
			FHyperAIModelAccountReport Report;
			bool bPending = false;
			double ReadAt = -1.0;
			TArray<TFunction<void(const FHyperAIModelAccountReport&)>> Waiters;
			/** Tripo's read and Hi3D's run side by side; the waiters hear once both are in. */
			int32 Outstanding = 0;
			FString Hi3DStatus;
			float Hi3DBalance = 0.f;
			FString Hi3DDiagnostic;
		};

		FAccount& Account()
		{
			static FAccount Value;
			return Value;
		}

		/** The report with Hi3D's part, which Tripo's read does not touch. */
		FHyperAIModelAccountReport WithHi3D(const FAccount& State)
		{
			FHyperAIModelAccountReport Report = State.Report;
			Report.Hi3DStatus = State.Hi3DStatus.IsEmpty() && State.bPending ? FString(TEXT("pending")) : State.Hi3DStatus;
			Report.Hi3DBalance = State.Hi3DBalance;
			Report.Hi3DDiagnostic = State.Hi3DDiagnostic;
			return Report;
		}

		void Done(FAccount& State)
		{
			if (--State.Outstanding > 0)
			{
				return;
			}
			State.Outstanding = 0;
			State.bPending = false;
			State.ReadAt = FPlatformTime::Seconds();
			State.Report.ReadAt = FDateTime::UtcNow().ToIso8601();
			TArray<TFunction<void(const FHyperAIModelAccountReport&)>> Waiters = MoveTemp(State.Waiters);
			const FHyperAIModelAccountReport Report = WithHi3D(State);
			for (const TFunction<void(const FHyperAIModelAccountReport&)>& Waiter : Waiters)
			{
				if (Waiter)
				{
					Waiter(Report);
				}
			}
		}

		/** One GET with the key as Tripo documents it (Authorization: Bearer); the key goes nowhere else. */
		void Get(const FHyperAIStudioExternalServiceSettings& Service, const FString& Path, TFunction<void(int32, const TArray<uint8>&)> OnReply)
		{
			const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
			Request->SetURL(Service.BaseUrl + Path);
			Request->SetVerb(TEXT("GET"));
			Request->SetTimeout(30.f);
			Request->SetHeader(Service.ApiKeyHeader.IsEmpty() ? FString(TEXT("Authorization")) : Service.ApiKeyHeader, Service.ApiKeyPrefix + Key(Service));
			Request->OnProcessRequestComplete().BindLambda([OnReply = MoveTemp(OnReply)](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				OnReply(bConnected && Response.IsValid() ? Response->GetResponseCode() : 0, bConnected && Response.IsValid() ? Response->GetContent() : TArray<uint8>());
			});
			Request->ProcessRequest();
		}

		FText BalanceText()
		{
			const FHyperAIModelAccountReport& Report = Account().Report;
			if (Report.Status == TEXT("ok"))
			{
				return FText::FromString(FString::Printf(TEXT("%.2f API credits available (about $%.2f), %.2f held for tasks running now. Read %s UTC."),
					Report.Balance, Report.Balance * 0.01f, Report.Frozen, *Report.ReadAt.Left(19).Replace(TEXT("T"), TEXT(" "))));
			}
			return FText::FromString(Account().bPending ? FString(TEXT("Reading Tripo's balance...")) : Report.Diagnostic.IsEmpty() ? FString(TEXT("Not read yet: press Refresh.")) : Report.Diagnostic);
		}

		FText Hi3DBalanceText()
		{
			const FAccount& State = Account();
			if (State.Hi3DStatus.IsEmpty())
			{
				return FText::FromString(State.bPending ? TEXT("Hi3D: reading its balance...") : TEXT("Hi3D: not read yet."));
			}
			return FText::FromString(TEXT("Hi3D: ") + State.Hi3DDiagnostic);
		}

		FText UsageText()
		{
			const FHyperAIModelAccountReport& Report = Account().Report;
			if (Report.Usage.IsEmpty())
			{
				return FText::FromString(Report.Status == TEXT("ok") ? TEXT("No recent usage.") : TEXT(""));
			}
			FString Text;
			for (int32 Index = 0; Index < Report.Usage.Num() && Index < 10; ++Index)
			{
				const FHyperAIModelUsage& Row = Report.Usage[Index];
				Text += FString::Printf(TEXT("%s  %-22s %7.2f credits  %s\n"), *Row.CreatedAt.Left(16).Replace(TEXT("T"), TEXT(" ")), *Row.Type, Row.Credits, *Row.TaskId.Left(12));
			}
			return FText::FromString(Text.TrimEnd());
		}

		/** The Tripo account at the top of the settings page: the balance and recent usage, never the key. */
		class FTripoSettingsCustomization final : public IDetailCustomization
		{
		public:
			static TSharedRef<IDetailCustomization> Make() { return MakeShared<FTripoSettingsCustomization>(); }

			virtual void CustomizeDetails(IDetailLayoutBuilder& Builder) override
			{
				IDetailCategoryBuilder& Category = Builder.EditCategory(TEXT("Tripo API account"), LOCTEXT("AccountCategory", "Tripo API account"), ECategoryPriority::Important);
				Category.AddCustomRow(LOCTEXT("AccountRow", "Tripo API account balance usage"))
				.WholeRowContent()
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
					[
						SNew(STextBlock).AutoWrapText(true).Text_Static(&BalanceText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
					[
						SNew(STextBlock).Font(IDetailLayoutBuilder::GetDetailFont()).Text_Static(&UsageText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
					[
						SNew(STextBlock).AutoWrapText(true).Text_Static(&Hi3DBalanceText)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 2.f)
					[
						SNew(STextBlock).AutoWrapText(true).Font(IDetailLayoutBuilder::GetDetailFont())
						.Text(LOCTEXT("Hi3DAccountNote", "Hi3D (Hitem3D) is the creator's second provider, for the densest, most faithful models from a picture. Its key is client_id:client_secret "
							"from Hi3D's developer platform, kept in Settings > Agents > Keys as Hi3D (Hitem3D) or in HITEM3D_API_KEY or HYPERAI_HI3D_KEY, and never shown. Credits are $0.02 each."))
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)
					[
						SNew(STextBlock).AutoWrapText(true).Font(IDetailLayoutBuilder::GetDetailFont())
						.Text(LOCTEXT("AccountNote", "The key is read from Settings > Agents > Keys (Windows Credential Manager) or TRIPO_API_KEY and is never shown. "
							"These are API credits: a Tripo Studio subscription's credits are a separate balance the API cannot read or spend. "
							"Models made in Tripo Studio come in through Tripo's own DCC Bridge, and HyperAI checks them."))
					]
					+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.f, 2.f)
					[
						SNew(SButton)
						.Text(LOCTEXT("Refresh", "Refresh"))
						.IsEnabled_Lambda([]() { return !Account().bPending; })
						.OnClicked_Lambda([]() { RefreshAccount(); return FReply::Handled(); })
					]
				];
				if (Account().ReadAt < 0.0 && !Account().bPending)
				{
					RefreshAccount();
				}
			}
		};
	}

	using namespace WatchDetail;

	FOwnImport::FOwnImport()
	{
		++OwnImports;
	}

	FOwnImport::~FOwnImport()
	{
		--OwnImports;
	}

	bool IsTripoImport(const FString& ClassName, const FString& ObjectPath, const FString& SourceFile, const TArray<FString>& Folders, const TArray<FString>& Markers)
	{
		if (ClassName != TEXT("StaticMesh") && ClassName != TEXT("SkeletalMesh"))
		{
			return false;
		}
		FString Package = ObjectPath;
		int32 Dot = INDEX_NONE;
		if (Package.FindChar(TEXT('.'), Dot))
		{
			Package.LeftInline(Dot);
		}
		for (FString Folder : Folders)
		{
			Folder = Folder.TrimStartAndEnd();
			Folder.RemoveFromEnd(TEXT("/"));
			if (!Folder.IsEmpty() && Package.StartsWith(Folder + TEXT("/"), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		const FString Name = FPackageName::GetShortName(Package);
		const FString File = FPaths::GetCleanFilename(SourceFile);
		for (const FString& Marker : Markers)
		{
			const FString Word = Marker.TrimStartAndEnd();
			if (!Word.IsEmpty() && (Name.Contains(Word, ESearchCase::IgnoreCase) || File.Contains(Word, ESearchCase::IgnoreCase)
				|| FPaths::GetPath(SourceFile).Contains(Word, ESearchCase::IgnoreCase)))
			{
				return true;
			}
		}
		return false;
	}

	FString OfferCheckAndFix(UObject& Mesh, const bool bNotify)
	{
		const FString Path = Mesh.GetPathName();
		const FString OperationId = TEXT("tripo-studio-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(10).ToLower();
		FHyperAIStudioExternalServiceSettings Service;
		const bool bKey = FHyperAIStudioExternalServices::Find(TEXT("tripo"), Service) && !Key(Service).IsEmpty();
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = FHyperAIStudioMeshGenContracts::PackId;
		Summary.ToolName = TEXT("hyper_model_check_and_fix");
		Summary.VariantId = TEXT("model.tripo_studio.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Path;
		Summary.Effects = {FString::Printf(TEXT("Check %s, a model from Tripo Studio, and make the free fixes in the editor (pivot, scale, normals, stray bits, holes, collision, materials)"), *Path),
			bKey ? FString(TEXT("Paid Tripo fixes, if checks still fail, are each priced and asked for on their own (API credits, not Studio credits)"))
				: FString(TEXT("Free fixes only: no Tripo API key is set"))};
		Summary.Touches = {Path};
		const FSoftObjectPath Soft(&Mesh);
		FString Error;
		const bool bQueued = FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Soft, OperationId, bKey]()
		{
			FHyperAIModelCheckFixRequest Request;
			Request.AssetPath = Soft.ToString();
			Request.bCheckOnly = false;
			Request.bAllowPaidFixes = bKey;
			Request.OperationId = OperationId + TEXT("-run");
			FString RunError;
			StartApprovedCheckAndFix(Request, RunError);
		}, Error);
		if (!bQueued)
		{
			UE_LOG(LogTemp, Warning, TEXT("HyperAI could not offer to check %s: %s"), *Path, *Error);
			return FString();
		}
		Offers.Add(Path);
		if (bNotify && FSlateApplication::IsInitialized())
		{
			FNotificationInfo Info(FText::Format(LOCTEXT("Arrived", "{0} arrived from Tripo Studio. Check it and make the free fixes?"), FText::FromString(Mesh.GetName())));
			Info.bFireAndForget = false;
			Info.bUseLargeFont = false;
			Info.FadeOutDuration = 0.5f;
			Info.ExpireDuration = 0.f;
			TSharedPtr<TWeakPtr<SNotificationItem>> Item = MakeShared<TWeakPtr<SNotificationItem>>();
			Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("CheckAndFix", "Check and fix"), LOCTEXT("CheckAndFixTip", "Approve the free check and fixes (also waiting in Activity)"),
				FSimpleDelegate::CreateLambda([OperationId, Item]()
				{
					FString ApproveError;
					FHyperAIStudioApprovalGate::Approve(OperationId, ApproveError);
					if (const TSharedPtr<SNotificationItem> Pinned = Item->Pin())
					{
						Pinned->SetCompletionState(SNotificationItem::CS_Success);
						Pinned->ExpireAndFadeout();
					}
				}), SNotificationItem::CS_Pending));
			Info.ButtonDetails.Add(FNotificationButtonInfo(LOCTEXT("Dismiss", "Dismiss"), LOCTEXT("DismissTip", "Leave the model as it came"),
				FSimpleDelegate::CreateLambda([OperationId, Item]()
				{
					FHyperAIStudioApprovalGate::Reject(OperationId, TEXT("Dismissed from the notification."));
					if (const TSharedPtr<SNotificationItem> Pinned = Item->Pin())
					{
						Pinned->ExpireAndFadeout();
					}
				}), SNotificationItem::CS_Pending));
			const TSharedPtr<SNotificationItem> Shown = FSlateNotificationManager::Get().AddNotification(Info);
			if (Shown.IsValid())
			{
				Shown->SetCompletionState(SNotificationItem::CS_Pending);
				*Item = Shown;
			}
		}
		return OperationId;
	}

	TArray<FString> OfferedAssets()
	{
		return Offers;
	}

	void StartWatch()
	{
		if (GEditor && !PostImportHandle.IsValid())
		{
			if (UImportSubsystem* Imports = GEditor->GetEditorSubsystem<UImportSubsystem>())
			{
				PostImportHandle = Imports->OnAssetPostImport.AddStatic(&OnPostImport);
			}
		}
		if (!AddedHandle.IsValid())
		{
			AddedHandle = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().OnAssetAdded().AddStatic(&OnAssetAdded);
		}
		FPropertyEditorModule& Editor = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
		Editor.RegisterCustomClassLayout(UHyperAIStudioTripoSettings::StaticClass()->GetFName(), FOnGetDetailCustomizationInstance::CreateStatic(&FTripoSettingsCustomization::Make));
	}

	void StopWatch()
	{
		if (PostImportHandle.IsValid() && GEditor)
		{
			if (UImportSubsystem* Imports = GEditor->GetEditorSubsystem<UImportSubsystem>())
			{
				Imports->OnAssetPostImport.Remove(PostImportHandle);
			}
		}
		PostImportHandle.Reset();
		if (AddedHandle.IsValid() && FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
		{
			FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().OnAssetAdded().Remove(AddedHandle);
		}
		AddedHandle.Reset();
		if (FPropertyEditorModule* Editor = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
		{
			Editor->UnregisterCustomClassLayout(UHyperAIStudioTripoSettings::StaticClass()->GetFName());
		}
	}

	void RefreshAccount(TFunction<void(const FHyperAIModelAccountReport&)> OnDone)
	{
		FAccount& State = Account();
		if (OnDone)
		{
			State.Waiters.Add(MoveTemp(OnDone));
		}
		if (State.bPending)
		{
			return;
		}
		State.bPending = true;
		State.Outstanding = 2;
		State.Hi3DStatus.Reset();
		// Hi3D's balance beside Tripo's: its key traded for a token, the token sent only to Hi3D.
		Hi3DReadBalance([](const FString& Status, const float Balance, const FString& Diagnostic)
		{
			FAccount& State = Account();
			State.Hi3DStatus = Status;
			State.Hi3DBalance = Balance;
			State.Hi3DDiagnostic = Diagnostic;
			Done(State);
		});
		FHyperAIStudioExternalServiceSettings Service;
		if (!FHyperAIStudioExternalServices::Find(TEXT("tripo"), Service) || Key(Service).IsEmpty())
		{
			State.Report = FHyperAIModelAccountReport();
			State.Report.Status = TEXT("missing_key");
			State.Report.Diagnostic = FString::Printf(TEXT("No Tripo API key: add one in Settings > Agents > Keys, or set %s."), *Service.ApiKeyVariable);
			Done(State);
			return;
		}
		Get(Service, TEXT("/account/balance"), [Service](const int32 Code, const TArray<uint8>& Body)
		{
			FAccount& State = Account();
			const TSharedPtr<FJsonObject> Json = ParseJson(Body);
			const FString Balance = Field(Json, TEXT("data.balance"));
			if (Code != 200 || Balance.IsEmpty())
			{
				State.Report = FHyperAIModelAccountReport();
				State.Report.Status = Code == 401 ? TEXT("bad_key") : TEXT("error");
				State.Report.Diagnostic = FString::Printf(TEXT("Tripo answered %d%s"), Code, Code == 401 ? TEXT(": the key was refused.") : TEXT("."));
				Done(State);
				return;
			}
			State.Report.Balance = FCString::Atof(*Balance);
			State.Report.Frozen = FCString::Atof(*Field(Json, TEXT("data.frozen")));
			Get(Service, TEXT("/account/usage"), [](const int32 UsageCode, const TArray<uint8>& UsageBody)
			{
				FAccount& State = Account();
				State.Report.Usage.Reset();
				const TSharedPtr<FJsonObject> Usage = ParseJson(UsageBody);
				const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
				if (UsageCode == 200 && Usage.IsValid() && Usage->TryGetArrayField(TEXT("data"), Rows))
				{
					for (const TSharedPtr<FJsonValue>& Row : *Rows)
					{
						const TSharedPtr<FJsonObject>* Object = nullptr;
						if (Row.IsValid() && Row->TryGetObject(Object))
						{
							FHyperAIModelUsage& Out = State.Report.Usage.AddDefaulted_GetRef();
							Out.TaskId = (*Object)->GetStringField(TEXT("task_id"));
							Out.Type = (*Object)->GetStringField(TEXT("type"));
							Out.Credits = static_cast<float>((*Object)->GetNumberField(TEXT("credits_consumed")));
							Out.CreatedAt = (*Object)->GetStringField(TEXT("created_at"));
						}
					}
				}
				State.Report.bOk = true;
				State.Report.Status = TEXT("ok");
				State.Report.Diagnostic = FString::Printf(TEXT("%.2f API credits available, %.2f held for running tasks; %d recent task(s)%s."), State.Report.Balance,
					State.Report.Frozen, State.Report.Usage.Num(), UsageCode == 200 ? TEXT("") : TEXT(" (usage could not be read)"));
				Done(State);
			});
		});
	}

	FHyperAIModelAccountReport Account(const FHyperAIModelAccountRequest& Request)
	{
		FAccount& State = WatchDetail::Account();
		const bool bStale = State.ReadAt < 0.0 || FPlatformTime::Seconds() - State.ReadAt > 60.0;
		if (bStale && !State.bPending)
		{
			RefreshAccount();
		}
		FHyperAIModelAccountReport Report = WithHi3D(State);
		if (State.bPending && State.ReadAt < 0.0)
		{
			Report.bOk = true;
			Report.Status = TEXT("pending");
			Report.Diagnostic = TEXT("Reading Tripo's balance; call again in a moment.");
		}
		if (!Request.bUsage)
		{
			Report.Usage.Reset();
		}
		const int32 Limit = Request.Limit > 0 ? Request.Limit : 20;
		if (Report.Usage.Num() > Limit)
		{
			Report.Usage.SetNum(Limit);
		}
		return Report;
	}
}

#undef LOCTEXT_NAMESPACE
