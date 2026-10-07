// Games by Hyper 2026.

#include "HyperAIStudioChatHistoryPrefs.h"

namespace HyperAIStudio::ChatPrefs
{
	bool IsSaneTitle(const FString& Title)
	{
		if (Title.Len() > UHyperAIStudioChatHistoryPrefs::MaxTitleChars)
		{
			return false;
		}
		for (const TCHAR Character : Title)
		{
			if (Character < 0x20 || Character == 0x7F)
			{
				return false;
			}
		}
		return true;
	}
}

FString UHyperAIStudioChatHistoryPrefs::GetTitle(const FString& SessionId)
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	const FString* Title = Prefs ? Prefs->Titles.Find(SessionId) : nullptr;
	return Title ? *Title : FString();
}

bool UHyperAIStudioChatHistoryPrefs::IsPinned(const FString& SessionId)
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	return Prefs && Prefs->Pinned.Contains(SessionId);
}

bool UHyperAIStudioChatHistoryPrefs::IsRemote(const FString& SessionId)
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	return Prefs && !SessionId.IsEmpty() && Prefs->Remote.Contains(SessionId);
}

void UHyperAIStudioChatHistoryPrefs::SetRemote(const FString& SessionId, const bool bRemote)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || SessionId.IsEmpty() || Prefs->Remote.Contains(SessionId) == bRemote)
	{
		return;
	}
	if (!bRemote)
	{
		Prefs->Remote.Remove(SessionId);
	}
	else if (Prefs->Remote.Num() < MaxTrackedSessions)
	{
		Prefs->Remote.Add(SessionId);
	}
	Prefs->SaveConfig();
}

void UHyperAIStudioChatHistoryPrefs::SetTitle(const FString& SessionId, const FString& Title)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	const FString Trimmed = Title.TrimStartAndEnd();
	if (!Prefs || SessionId.IsEmpty() || !HyperAIStudio::ChatPrefs::IsSaneTitle(Trimmed))
	{
		return;
	}
	if (Trimmed.IsEmpty())
	{
		Prefs->Titles.Remove(SessionId);
	}
	else
	{
		// Bounded: a long-lived project must not grow this file without limit.
		if (!Prefs->Titles.Contains(SessionId) && Prefs->Titles.Num() >= MaxTrackedSessions)
		{
			return;
		}
		Prefs->Titles.Add(SessionId, Trimmed);
	}
	Prefs->SaveConfig();
}

void UHyperAIStudioChatHistoryPrefs::TogglePinned(const FString& SessionId)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || SessionId.IsEmpty())
	{
		return;
	}
	if (Prefs->Pinned.Remove(SessionId) == 0 && Prefs->Pinned.Num() < MaxTrackedSessions)
	{
		Prefs->Pinned.Add(SessionId);
	}
	Prefs->SaveConfig();
}

namespace HyperAIStudio::ChatPrefs
{
	bool IsFreeGroupName(const UHyperAIStudioChatHistoryPrefs& Prefs, const FString& Name, const FString& Except = FString())
	{
		return !Name.IsEmpty() && Name.Len() <= UHyperAIStudioChatHistoryPrefs::MaxGroupNameChars && IsSaneTitle(Name)
			&& !Prefs.Groups.ContainsByPredicate([&Name, &Except](const FString& Group)
			{
				return Group.Equals(Name, ESearchCase::IgnoreCase) && !Group.Equals(Except, ESearchCase::IgnoreCase);
			});
	}
}

TArray<FString> UHyperAIStudioChatHistoryPrefs::GetGroups()
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	return Prefs ? Prefs->Groups : TArray<FString>();
}

bool UHyperAIStudioChatHistoryPrefs::AddGroup(const FString& Name)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	const FString Trimmed = Name.TrimStartAndEnd();
	if (!Prefs || Prefs->Groups.Num() >= MaxGroups || !HyperAIStudio::ChatPrefs::IsFreeGroupName(*Prefs, Trimmed))
	{
		return false;
	}
	Prefs->Groups.Add(Trimmed);
	Prefs->SaveConfig();
	return true;
}

bool UHyperAIStudioChatHistoryPrefs::RenameGroup(const FString& From, const FString& To)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	const FString Trimmed = To.TrimStartAndEnd();
	const int32 Index = Prefs ? Prefs->Groups.IndexOfByKey(From) : INDEX_NONE;
	if (Index == INDEX_NONE || !HyperAIStudio::ChatPrefs::IsFreeGroupName(*Prefs, Trimmed, From))
	{
		return false;
	}
	Prefs->Groups[Index] = Trimmed;
	for (TPair<FString, FString>& Entry : Prefs->ChatGroups)
	{
		if (Entry.Value == From)
		{
			Entry.Value = Trimmed;
		}
	}
	const int32 Folded = Prefs->Collapsed.IndexOfByKey(TEXT("group:") + From);
	if (Folded != INDEX_NONE)
	{
		Prefs->Collapsed[Folded] = TEXT("group:") + Trimmed;
	}
	Prefs->SaveConfig();
	return true;
}

void UHyperAIStudioChatHistoryPrefs::RemoveGroup(const FString& Name)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || Prefs->Groups.Remove(Name) == 0)
	{
		return;
	}
	for (auto It = Prefs->ChatGroups.CreateIterator(); It; ++It)
	{
		if (It->Value == Name)
		{
			It.RemoveCurrent();
		}
	}
	Prefs->Collapsed.Remove(TEXT("group:") + Name);
	Prefs->SaveConfig();
}

FString UHyperAIStudioChatHistoryPrefs::GetGroup(const FString& SessionId)
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	const FString* Group = Prefs ? Prefs->ChatGroups.Find(SessionId) : nullptr;
	// A group that was removed some other way no longer holds anything.
	return Group && Prefs->Groups.Contains(*Group) ? *Group : FString();
}

void UHyperAIStudioChatHistoryPrefs::SetGroup(const FString& SessionId, const FString& Group)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || SessionId.IsEmpty())
	{
		return;
	}
	if (Group.IsEmpty() || !Prefs->Groups.Contains(Group))
	{
		Prefs->ChatGroups.Remove(SessionId);
	}
	else if (Prefs->ChatGroups.Contains(SessionId) || Prefs->ChatGroups.Num() < MaxGroupedSessions)
	{
		Prefs->ChatGroups.Add(SessionId, Group);
	}
	Prefs->SaveConfig();
}

bool UHyperAIStudioChatHistoryPrefs::IsCollapsed(const FString& Section)
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	return Prefs && Prefs->Collapsed.Contains(Section);
}

void UHyperAIStudioChatHistoryPrefs::SetCollapsed(const FString& Section, const bool bCollapsed)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || Section.IsEmpty() || Prefs->Collapsed.Contains(Section) == bCollapsed)
	{
		return;
	}
	if (bCollapsed)
	{
		// Bounded like the rest: months pile up over a long project.
		if (Prefs->Collapsed.Num() >= MaxTrackedSessions)
		{
			Prefs->Collapsed.RemoveAt(0);
		}
		Prefs->Collapsed.Add(Section);
	}
	else
	{
		Prefs->Collapsed.Remove(Section);
	}
	Prefs->SaveConfig();
}

FString UHyperAIStudioChatHistoryPrefs::GetAgentFilter()
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	return Prefs ? Prefs->AgentFilter : FString();
}

void UHyperAIStudioChatHistoryPrefs::SetAgentFilter(const FString& AgentName)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || Prefs->AgentFilter == AgentName)
	{
		return;
	}
	Prefs->AgentFilter = AgentName;
	Prefs->SaveConfig();
}

void UHyperAIStudioChatHistoryPrefs::Forget(const FString& SessionId)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || SessionId.IsEmpty())
	{
		return;
	}
	Prefs->Titles.Remove(SessionId);
	Prefs->Pinned.Remove(SessionId);
	Prefs->Remote.Remove(SessionId);
	Prefs->ChatGroups.Remove(SessionId);
	Prefs->SaveConfig();
}

TArray<FString> UHyperAIStudioChatHistoryPrefs::GetRecentPromptPresets()
{
	const UHyperAIStudioChatHistoryPrefs* Prefs = GetDefault<UHyperAIStudioChatHistoryPrefs>();
	TArray<FString> Out;
	if (!Prefs) return Out;
	// Each once, as the ini may list one twice after a hand edit, a merge or a copied Saved folder: the picker's list
	// would hold that preset twice, which Slate's list view asserts on. AddUnique ignores case, as the lookups do.
	for (const FString& Id : Prefs->RecentPromptPresets)
	{
		if (Out.Num() >= MaxRecentPromptPresets) break;
		Out.AddUnique(Id);
	}
	return Out;
}

void UHyperAIStudioChatHistoryPrefs::AddRecentPromptPreset(const FString& PresetId)
{
	UHyperAIStudioChatHistoryPrefs* Prefs = GetMutableDefault<UHyperAIStudioChatHistoryPrefs>();
	if (!Prefs || PresetId.IsEmpty() || !HyperAIStudio::ChatPrefs::IsSaneTitle(PresetId))
	{
		return;
	}
	Prefs->RecentPromptPresets.Remove(PresetId);
	Prefs->RecentPromptPresets.Insert(PresetId, 0);
	if (Prefs->RecentPromptPresets.Num() > MaxRecentPromptPresets)
	{
		Prefs->RecentPromptPresets.SetNum(MaxRecentPromptPresets);
	}
	Prefs->SaveConfig();
}
