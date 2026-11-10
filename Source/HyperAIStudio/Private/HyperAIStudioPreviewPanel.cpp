// Games by Hyper 2026.

#include "HyperAIStudioPreviewPanel.h"

#include "HAL/FileManager.h"
#include "HyperAIStudioShell.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioPreviewPanel"

namespace HyperAIStudio::Preview::Private
{
	TArray<FHyperAIStudioPreviewPanel::FSource>& Sources()
	{
		static TArray<FHyperAIStudioPreviewPanel::FSource> Storage;
		return Storage;
	}
}

void FHyperAIStudioPreviewPanel::RegisterSource(FSource Source)
{
	using namespace HyperAIStudio::Preview::Private;
	if (Source.Id.IsNone() || !IsInGameThread())
	{
		return;
	}
	Sources().RemoveAll([&Source](const FSource& Existing) { return Existing.Id == Source.Id; });
	Sources().Add(MoveTemp(Source));
	Sources().StableSort([](const FSource& Left, const FSource& Right) { return Left.Label.CompareTo(Right.Label) < 0; });
	OnSourcesChanged().Broadcast();
}

void FHyperAIStudioPreviewPanel::UnregisterSource(const FName Id)
{
	using namespace HyperAIStudio::Preview::Private;
	if (!IsInGameThread() || Sources().RemoveAll([Id](const FSource& Source) { return Source.Id == Id; }) == 0)
	{
		return;
	}
	OnSourcesChanged().Broadcast();
}

TArray<FHyperAIStudioPreviewPanel::FSource> FHyperAIStudioPreviewPanel::GetSources()
{
	return HyperAIStudio::Preview::Private::Sources();
}

const FHyperAIStudioPreviewPanel::FSource* FHyperAIStudioPreviewPanel::FindSource(const FName Id)
{
	return HyperAIStudio::Preview::Private::Sources().FindByPredicate([Id](const FSource& Source) { return Source.Id == Id; });
}

FSimpleMulticastDelegate& FHyperAIStudioPreviewPanel::OnSourcesChanged()
{
	static FSimpleMulticastDelegate Delegate;
	return Delegate;
}

FHyperAIStudioPreviewPanel::FOnOpenRequested& FHyperAIStudioPreviewPanel::OnOpenRequested()
{
	static FOnOpenRequested Delegate;
	return Delegate;
}

void FHyperAIStudioPreviewPanel::Open(const FName SourceId, const FString& ReferenceImagePath)
{
	if (!IsInGameThread())
	{
		return;
	}
	FHyperAIStudioShell::Open(TEXT("Match"));
	// The page may have just been created, so tell whichever panel is live what to show.
	OnOpenRequested().Broadcast(SourceId, ReferenceImagePath);
}

bool FHyperAIStudioPreviewPanel::StartAgentAction(const FName SourceId, const FName ActionId, const FString& ReferenceImagePath, FString& OutError)
{
	const FSource* Source = FindSource(SourceId);
	const FAgentAction* Action = Source ? Source->AgentActions.FindByPredicate([ActionId](const FAgentAction& Candidate) { return Candidate.Id == ActionId; }) : nullptr;
	if (!Action || !Action->Start)
	{
		OutError = FString::Printf(TEXT("Nothing offers the agent action %s here; its pack may not be loaded."), *ActionId.ToString());
		return false;
	}
	if (!IsSupportedProjectImage(ReferenceImagePath, OutError))
	{
		return false;
	}
	return Action->Start(FPaths::ConvertRelativePathToFull(ReferenceImagePath), OutError);
}

bool FHyperAIStudioPreviewPanel::IsSupportedProjectImage(const FString& Path, FString& OutError)
{
	OutError.Reset();
	if (Path.IsEmpty())
	{
		OutError = TEXT("No image selected.");
		return false;
	}
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
	const FString Extension = FPaths::GetExtension(Full).ToLower();
	if (Extension != TEXT("png") && Extension != TEXT("jpg") && Extension != TEXT("jpeg")
		&& Extension != TEXT("bmp") && Extension != TEXT("tga"))
	{
		OutError = TEXT("Pick a PNG, JPG, BMP or TGA.");
		return false;
	}
	// The image tools refuse anything outside the project, so the picker should too rather than fail later.
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (!Full.StartsWith(ProjectDir, ESearchCase::IgnoreCase))
	{
		OutError = TEXT("Pick an image inside the project folder; copy it in first if it lives elsewhere.");
		return false;
	}
	if (!IFileManager::Get().FileExists(*Full))
	{
		OutError = TEXT("That file is no longer there.");
		return false;
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
