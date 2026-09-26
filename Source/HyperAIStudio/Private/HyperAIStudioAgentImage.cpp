// Games by Hyper 2026.

#include "HyperAIStudioAgentImage.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HyperAIStudioNativeChat.h"
#include "HyperAIStudioService.h"
#include "HyperAIStudioSettings.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace HyperAIStudio::AgentImage::Private
{
	const TCHAR* const Instructions = TEXT(
		"You make pictures for a game studio's editor tool. For each message, draw exactly one image with your image ")
		TEXT("generation tool from the description, or change the attached picture as the message says, then reply with ")
		TEXT("one short line saying what you drew. Do not run commands, read or write files, browse, or ask questions. If ")
		TEXT("you cannot draw it, say why in one line.");

	bool IsPicture(const FString& Path)
	{
		const FString Extension = FPaths::GetExtension(Path).ToLower();
		return Extension == TEXT("png") || Extension == TEXT("jpg") || Extension == TEXT("jpeg") || Extension == TEXT("webp");
	}

	FString GeminiExtensionsFolder()
	{
		FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
		if (Home.IsEmpty())
		{
			Home = FPlatformProcess::UserHomeDir();
		}
		return FPaths::Combine(Home, TEXT(".gemini"), TEXT("extensions"));
	}
}

bool FHyperAIStudioAgentImage::CanDraw(const FString& Agent, FString& OutIssue)
{
	using namespace HyperAIStudio::AgentImage::Private;
	OutIssue.Reset();
	FString Executable;
	if (Agent.Equals(TEXT("codex"), ESearchCase::IgnoreCase))
	{
		if (!FHyperAIStudioService::FindExecutableOnPath(TEXT("codex"), Executable))
		{
			OutIssue = TEXT("Codex CLI is not installed on this machine, and it is what draws these pictures.");
			return false;
		}
		return true;
	}
	if (Agent.Equals(TEXT("gemini"), ESearchCase::IgnoreCase))
	{
		if (!FHyperAIStudioService::FindExecutableOnPath(TEXT("gemini"), Executable))
		{
			OutIssue = TEXT("Gemini CLI is not installed on this machine.");
			return false;
		}
		// Gemini CLI draws only through an extension; nanobanana is Google's own.
		TArray<FString> Extensions;
		IFileManager::Get().FindFiles(Extensions, *(GeminiExtensionsFolder() / TEXT("*")), /*Files=*/false, /*Directories=*/true);
		if (!Extensions.ContainsByPredicate([](const FString& Name)
			{
				return Name.Contains(TEXT("banana"), ESearchCase::IgnoreCase) || Name.Contains(TEXT("image"), ESearchCase::IgnoreCase);
			}))
		{
			OutIssue = TEXT("Gemini CLI has no image generator of its own. Install its nanobanana extension ")
				TEXT("(gemini extensions install https://github.com/gemini-cli-extensions/nanobanana), which needs a Gemini API key, ")
				TEXT("or use codex_image_gen.");
			return false;
		}
		return true;
	}
	OutIssue = FString::Printf(TEXT("%s cannot draw; codex and gemini can."), *Agent);
	return false;
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioAgentImage::SetCommandForTests(const FString& Command)
{
	FHyperAIStudioNativeChat::SetCommandForTests(Command);
}
#endif

TSharedPtr<FHyperAIStudioAgentImage> FHyperAIStudioAgentImage::Start(const FString& Agent, const FString& Prompt, const FIntPoint& SizeHint,
	FString& OutError, const FString& InputImage)
{
	using namespace HyperAIStudio::AgentImage::Private;
	const bool bGemini = Agent.Equals(TEXT("gemini"), ESearchCase::IgnoreCase);
	TSharedPtr<FHyperAIStudioAgentImage> Image = MakeShareable(new FHyperAIStudioAgentImage());
	Image->Agent = bGemini ? TEXT("Gemini") : TEXT("Codex");
	// A folder of its own: the CLI runs there, so an extension that writes files writes them where they are found.
	Image->WorkDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio") / TEXT("AgentImages")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!IFileManager::Get().MakeDirectory(*Image->WorkDirectory, /*Tree=*/true))
	{
		OutError = FString::Printf(TEXT("Could not make %s."), *Image->WorkDirectory);
		return nullptr;
	}
	// In the folder the CLI runs in, so an extension whose edit tool takes a file name can open it.
	if (!InputImage.IsEmpty())
	{
		Image->InputCopy = Image->WorkDirectory / (TEXT("input.") + FPaths::GetExtension(InputImage).ToLower());
		if (IFileManager::Get().Copy(*Image->InputCopy, *InputImage) != COPY_OK)
		{
			OutError = FString::Printf(TEXT("Could not copy the picture to change, %s."), *InputImage);
			return nullptr;
		}
	}
	Image->Chat = FHyperAIStudioNativeChat::Create(Image->Agent, Image->WorkDirectory, FString(), FString());
	if (!Image->Chat.IsValid())
	{
		OutError = FString::Printf(TEXT("%s has no headless protocol here."), *Image->Agent);
		return nullptr;
	}
	Image->Chat->SetHeadless(Instructions);
	if (bGemini)
	{
		// The nanobanana extension takes its model and its key from the environment Gemini CLI starts in. A model
		// chosen in settings wins; one set in the user's own environment stays when settings leave it to the extension.
		const FString NanoModel = GetDefault<UHyperAIStudioSettings>()->GetGeminiCliImageModelId();
		if (!NanoModel.IsEmpty())
		{
			FPlatformMisc::SetEnvironmentVar(TEXT("NANOBANANA_MODEL"), *NanoModel);
		}
		if (FPlatformMisc::GetEnvironmentVariable(TEXT("NANOBANANA_API_KEY")).IsEmpty())
		{
			const FString Key = FPlatformMisc::GetEnvironmentVariable(TEXT("GEMINI_API_KEY"));
			if (!Key.IsEmpty())
			{
				FPlatformMisc::SetEnvironmentVar(TEXT("NANOBANANA_API_KEY"), *Key);
			}
		}
	}
	if (!Image->Chat->Start(OutError))
	{
		return nullptr;
	}
	const TCHAR* Shape = SizeHint.X > SizeHint.Y * 1.15 ? TEXT("landscape") : SizeHint.Y > SizeHint.X * 1.15 ? TEXT("portrait") : TEXT("square");
	if (Image->InputCopy.IsEmpty())
	{
		Image->Chat->Send(FString::Printf(TEXT("Draw this picture (%s, about %dx%d pixels): %s"), Shape, SizeHint.X, SizeHint.Y, *Prompt));
	}
	else
	{
		Image->Chat->Send(FString::Printf(TEXT("Change the attached picture, which is also %s in your working folder, keeping it %s, about %dx%d pixels: %s"),
			*FPaths::GetCleanFilename(Image->InputCopy), Shape, SizeHint.X, SizeHint.Y, *Prompt), {Image->InputCopy});
	}
	Image->StartedAt = FPlatformTime::Seconds();
	return Image;
}

FHyperAIStudioAgentImage::~FHyperAIStudioAgentImage()
{
	if (Chat.IsValid())
	{
		Chat->Stop();
	}
	// The caller has copied the picture by now; Codex keeps its own copy under its home folder.
	if (!WorkDirectory.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*WorkDirectory, /*RequireExists=*/false, /*Tree=*/true);
	}
}

bool FHyperAIStudioAgentImage::IsDone()
{
	using namespace HyperAIStudio::AgentImage::Private;
	using EKind = FHyperAIStudioChatItem::EKind;
	if (bDone)
	{
		return true;
	}
	if (!Chat.IsValid())
	{
		Finish(TEXT("The agent is gone."));
		return true;
	}
	const TArray<TSharedPtr<FHyperAIStudioChatItem>>& Items = Chat->GetItems();
	const bool bFailedToStart = !Chat->IsReady() && Items.ContainsByPredicate([](const TSharedPtr<FHyperAIStudioChatItem>& Item)
	{
		return Item->Kind == EKind::Error;
	});
	const bool bTurnEnded = Chat->IsReady() && !Chat->IsBusy() && Chat->NumQueued() == 0;
	const bool bTimedOut = FPlatformTime::Seconds() - StartedAt > TimeoutSeconds;
	if (Chat->IsRunning() && !bFailedToStart && !bTurnEnded && !bTimedOut)
	{
		return false;
	}
	// The newest picture the agent reported, or one its tool wrote into the folder; the picture it was given is neither.
	const auto IsDrawn = [this](const FString& Path) { return IsPicture(Path) && (InputCopy.IsEmpty() || !FPaths::IsSamePath(Path, InputCopy)); };
	for (int32 Index = Items.Num() - 1; Index >= 0 && ImagePath.IsEmpty(); --Index)
	{
		const FHyperAIStudioChatItem& Item = *Items[Index];
		if (Item.Kind == EKind::Image && !Item.Images.IsEmpty() && IsDrawn(Item.Images[0]) && IFileManager::Get().FileExists(*Item.Images[0]))
		{
			ImagePath = Item.Images[0];
			RevisedPrompt = Item.Text;
		}
	}
	if (ImagePath.IsEmpty())
	{
		TArray<FString> Written;
		IFileManager::Get().FindFilesRecursive(Written, *WorkDirectory, TEXT("*.*"), /*Files=*/true, /*Directories=*/false);
		Written = Written.FilterByPredicate(IsDrawn);
		if (!Written.IsEmpty())
		{
			ImagePath = Written.Last();
		}
	}
	if (!ImagePath.IsEmpty())
	{
		Finish(FString());
		return true;
	}
	// Why not, in the agent's words: a failed picture or an error first, then its last reply.
	FString Why;
	for (int32 Index = Items.Num() - 1; Index >= 0 && Why.IsEmpty(); --Index)
	{
		const FHyperAIStudioChatItem& Item = *Items[Index];
		if ((Item.Kind == EKind::Image && Item.Status == TEXT("failed")) || Item.Kind == EKind::Error)
		{
			Why = Item.Text;
		}
	}
	for (int32 Index = Items.Num() - 1; Index >= 0 && Why.IsEmpty(); --Index)
	{
		if (Items[Index]->Kind == EKind::Agent)
		{
			Why = Items[Index]->Text;
		}
	}
	Finish(bTimedOut ? FString::Printf(TEXT("%s did not finish the picture within %.0f s."), *Agent, TimeoutSeconds)
		: Why.IsEmpty() ? FString::Printf(TEXT("%s ended without drawing a picture."), *Agent)
		: FString::Printf(TEXT("%s did not draw it: %s"), *Agent, *Why.TrimStartAndEnd().Left(400)));
	return true;
}

void FHyperAIStudioAgentImage::Finish(const FString& InError)
{
	bDone = true;
	Error = InError;
	if (Chat.IsValid())
	{
		Chat->Stop();
	}
}
