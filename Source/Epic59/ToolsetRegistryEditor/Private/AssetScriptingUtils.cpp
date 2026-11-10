// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: the path functions of UE 5.9's AssetTools/Private/AssetScriptingUtils.cpp (see the header).

#include "AssetScriptingUtils.h"

#include "ContentBrowserDataSubsystem.h"
#include "IContentBrowserDataModule.h"
#include "Internationalization/Regex.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace AssetScriptingUtils
{
	namespace Private
	{
		/** Replaces each package path of Text by what Convert returns for it, unless that is empty. */
		FString ConvertPackagePathsInText(const FString& Text, TFunctionRef<FString(const FString& PackagePath)> Convert)
		{
			if (!Text.Contains(TEXT("/")))
			{
				return Text;
			}
			static const FRegexPattern PackagePathPattern(TEXT("(?<![A-Za-z0-9_.])/[A-Za-z0-9_\\-]+(/[A-Za-z0-9_\\-]+)+"));
			FRegexMatcher Matcher(PackagePathPattern, Text);
			FString Result;
			int32 Copied = 0;
			while (Matcher.FindNext())
			{
				const int32 Begin = Matcher.GetMatchBeginning();
				const int32 End = Matcher.GetMatchEnding();
				const FString Converted = Convert(Text.Mid(Begin, End - Begin));
				if (!Converted.IsEmpty())
				{
					Result += Text.Mid(Copied, Begin - Copied) + Converted;
					Copied = End;
				}
			}
			return Copied == 0 ? Text : Result + Text.Mid(Copied);
		}
	}

	bool TryConvertToInternalPath(const FString& Path, FString& OutInternalPath)
	{
		OutInternalPath = Path;
		// A stray "//" left behind by collapsing ".." can dodge a permission list's exact-prefix match
		FPaths::RemoveDuplicateSlashes(OutInternalPath);
		// An uncollapsed ".." can hide a real mount boundary from the virtual path resolver below
		const bool bCollapsed = FPaths::CollapseRelativeDirectories(OutInternalPath);
		FPaths::RemoveDuplicateSlashes(OutInternalPath); // UE 5.9: and the "//" the collapse leaves
		if (UContentBrowserDataSubsystem* ContentBrowserData = IContentBrowserDataModule::Get().GetSubsystem())
		{
			FString InternalPath;
			if (ContentBrowserData->TryConvertVirtualPath(OutInternalPath, InternalPath) == EContentBrowserPathType::Internal)
			{
				OutInternalPath = MoveTemp(InternalPath);
			}
		}
		return bCollapsed;
	}

	FString ConvertToVirtualPath(const FString& InternalPath)
	{
		if (UContentBrowserDataSubsystem* ContentBrowserData = IContentBrowserDataModule::Get().GetSubsystem())
		{
			FName VirtualPath;
			ContentBrowserData->ConvertInternalPathToVirtual(InternalPath, VirtualPath);
			if (!VirtualPath.IsNone())
			{
				return VirtualPath.ToString();
			}
		}
		return InternalPath;
	}

	FString ConvertToVirtualPath(const UObject& Asset)
	{
		return ConvertToVirtualPath(Asset.GetPackage()->GetName());
	}

	FString ConvertToVirtualPathsInText(const FString& Text)
	{
		return Private::ConvertPackagePathsInText(Text, [](const FString& PackagePath) -> FString
		{
			// Only the paths of packages: classes and transient objects have no content browser path, and text can hold any path-like word
			if (FPackageName::IsScriptPackage(PackagePath) || PackagePath.StartsWith(GetTransientPackage()->GetName())
				|| !FPackageName::IsValidLongPackageName(PackagePath) || (!FindPackage(nullptr, *PackagePath) && !FPackageName::DoesPackageExist(PackagePath)))
			{
				return FString();
			}
			const FString VirtualPath = ConvertToVirtualPath(PackagePath);
			return VirtualPath == PackagePath ? FString() : VirtualPath;
		});
	}

	FString ConvertToInternalPathsInText(const FString& Text)
	{
		return Private::ConvertPackagePathsInText(Text, [](const FString& Path) -> FString
		{
			FString InternalPath;
			TryConvertToInternalPath(Path, InternalPath);
			return InternalPath != Path && FPackageName::IsValidLongPackageName(InternalPath) ? InternalPath : FString();
		});
	}
}
