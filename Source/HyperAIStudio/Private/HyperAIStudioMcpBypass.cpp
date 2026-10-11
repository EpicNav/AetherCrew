// Games by Hyper 2026.

#include "HyperAIStudioMcpBypass.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioMcpBypass"

namespace HyperAIStudio::McpBypass::Private
{
	/** Larger config files are not read: none of the agents' is anywhere near this. */
	constexpr int64 MaxConfigBytes = 8 * 1024 * 1024;
	constexpr int32 MaxTomlStatements = 65536;
	constexpr int32 MaxServersPerFile = 1024;
	/** The ports the Nwiro Integration Kit's server uses (5353; 5354 as configured on the user's machine). */
	constexpr int32 NwiroPorts[] = {5353, 5354};

	const TCHAR* const ClaudeCode = TEXT("Claude Code");
	const TCHAR* const Codex = TEXT("Codex");
	const TCHAR* const Gemini = TEXT("Gemini");
	const TCHAR* const Cursor = TEXT("Cursor");
	const TCHAR* const VSCode = TEXT("VS Code");

	bool IsAlnum(const TCHAR Character)
	{
		return (Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'));
	}

	/** Lower-case runs of letters and digits. */
	TArray<FString> Tokens(const FString& Text)
	{
		TArray<FString> Result;
		FString Current;
		for (const TCHAR Character : Text)
		{
			if (IsAlnum(Character))
			{
				Current.AppendChar(FChar::ToLower(Character));
			}
			else if (!Current.IsEmpty())
			{
				Result.Add(MoveTemp(Current));
				Current.Reset();
			}
		}
		if (!Current.IsEmpty())
		{
			Result.Add(MoveTemp(Current));
		}
		return Result;
	}

	/** The vocabulary in a server's own name or command: nwiro, rider, jetbrains, unreal (ue, ue5, uemcp count). */
	void AddNameWords(const FString& Text, TSet<FString>& OutWords)
	{
		for (const FString& Token : Tokens(Text))
		{
			if (Token.Contains(TEXT("nwiro")))
			{
				OutWords.Add(TEXT("nwiro"));
			}
			if (Token.Contains(TEXT("jetbrains")))
			{
				OutWords.Add(TEXT("jetbrains"));
			}
			if (Token == TEXT("rider") || Token == TEXT("riderlink"))
			{
				OutWords.Add(TEXT("rider"));
			}
			if (Token.Contains(TEXT("unreal")) || Token == TEXT("ue") || Token == TEXT("ue4") || Token == TEXT("ue5")
				|| Token == TEXT("uemcp"))
			{
				OutWords.Add(TEXT("unreal"));
			}
		}
	}

	/**
	 * The vocabulary in an argument or URL path, more strictly: a project folder such as "E:/Unreal Projects/Game" or an
	 * engine path does not make a file server an Unreal one, but a package such as unreal-mcp or ue5_mcp does.
	 */
	void AddArgumentWords(const FString& Text, TSet<FString>& OutWords)
	{
		const TArray<FString> Parts = Tokens(Text);
		for (int32 Index = 0; Index < Parts.Num(); ++Index)
		{
			const FString& Token = Parts[Index];
			if (Token.Contains(TEXT("nwiro")))
			{
				OutWords.Add(TEXT("nwiro"));
			}
			if (Token.Contains(TEXT("jetbrains")))
			{
				OutWords.Add(TEXT("jetbrains"));
			}
			if (Token == TEXT("rider") || Token == TEXT("riderlink"))
			{
				OutWords.Add(TEXT("rider"));
			}
			if (Token == TEXT("unrealmcp") || Token == TEXT("uemcp"))
			{
				OutWords.Add(TEXT("unreal"));
			}
			const bool bUnrealWord = Token == TEXT("unreal") || Token == TEXT("ue") || Token == TEXT("ue4") || Token == TEXT("ue5");
			if (bUnrealWord && Parts.IsValidIndex(Index + 1) && Parts[Index + 1] == TEXT("mcp"))
			{
				OutWords.Add(TEXT("unreal"));
			}
		}
	}

	/** Tool names such as ue_execute_python or unreal_spawn are Unreal tools. */
	void AddToolWords(const FString& ToolName, TSet<FString>& OutWords)
	{
		const TArray<FString> Parts = Tokens(ToolName);
		if (!Parts.IsEmpty() && (Parts[0] == TEXT("ue") || Parts[0].Contains(TEXT("unreal"))))
		{
			OutWords.Add(TEXT("unreal"));
		}
	}

	/** Like ParseUrl, and the path for AddArgumentWords. */
	bool SplitUrl(const FString& Url, FString& OutScheme, FString& OutHost, int32& OutPort, FString& OutPath)
	{
		OutScheme.Reset();
		OutHost.Reset();
		OutPath.Reset();
		OutPort = 0;
		const FString Trimmed = Url.TrimStartAndEnd();
		const int32 SchemeEnd = Trimmed.Find(TEXT("://"));
		if (SchemeEnd <= 0)
		{
			return false;
		}
		const FString Scheme = Trimmed.Left(SchemeEnd).ToLower();
		for (const TCHAR Character : Scheme)
		{
			if (!IsAlnum(Character) && Character != TEXT('+') && Character != TEXT('-') && Character != TEXT('.'))
			{
				return false;
			}
		}
		const FString Rest = Trimmed.Mid(SchemeEnd + 3);
		int32 AuthorityEnd = Rest.Len();
		for (int32 Index = 0; Index < Rest.Len(); ++Index)
		{
			if (Rest[Index] == TEXT('/') || Rest[Index] == TEXT('?') || Rest[Index] == TEXT('#'))
			{
				AuthorityEnd = Index;
				break;
			}
		}
		FString Authority = Rest.Left(AuthorityEnd);
		if (AuthorityEnd < Rest.Len() && Rest[AuthorityEnd] == TEXT('/'))
		{
			// The path only; a query or fragment can hold a token, and is never looked at.
			FString Path = Rest.Mid(AuthorityEnd);
			int32 Cut = INDEX_NONE;
			if (Path.FindChar(TEXT('?'), Cut))
			{
				Path.LeftInline(Cut);
			}
			if (Path.FindChar(TEXT('#'), Cut))
			{
				Path.LeftInline(Cut);
			}
			OutPath = MoveTemp(Path);
		}
		// User info (user:password@) is dropped before anything is kept.
		int32 At = INDEX_NONE;
		if (Authority.FindLastChar(TEXT('@'), At))
		{
			Authority.RightChopInline(At + 1);
		}
		FString Host;
		FString PortText;
		if (Authority.StartsWith(TEXT("[")))
		{
			int32 Close = INDEX_NONE;
			if (!Authority.FindChar(TEXT(']'), Close))
			{
				return false;
			}
			Host = Authority.Mid(1, Close - 1);
			const FString After = Authority.Mid(Close + 1);
			if (After.StartsWith(TEXT(":")))
			{
				PortText = After.Mid(1);
			}
			else if (!After.IsEmpty())
			{
				return false;
			}
		}
		else
		{
			int32 Colon = INDEX_NONE;
			if (Authority.FindLastChar(TEXT(':'), Colon))
			{
				Host = Authority.Left(Colon);
				PortText = Authority.Mid(Colon + 1);
			}
			else
			{
				Host = Authority;
			}
		}
		Host.TrimStartAndEndInline();
		if (Host.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Character : Host)
		{
			if (FChar::IsWhitespace(Character) || Character == TEXT('$') || Character == TEXT('{') || Character == TEXT('}'))
			{
				return false;
			}
		}
		int32 Port = 0;
		if (!PortText.IsEmpty())
		{
			if (!PortText.IsNumeric() || PortText.Contains(TEXT(".")) || PortText.Contains(TEXT("-")))
			{
				return false;
			}
			Port = FCString::Atoi(*PortText);
			if (Port <= 0 || Port > 65535)
			{
				return false;
			}
		}
		else if (Scheme == TEXT("https") || Scheme == TEXT("wss"))
		{
			Port = 443;
		}
		else if (Scheme == TEXT("http") || Scheme == TEXT("ws"))
		{
			Port = 80;
		}
		OutScheme = Scheme;
		OutHost = Host.ToLower();
		OutPort = Port;
		return true;
	}

	FString FormatAddress(const FString& Scheme, const FString& Host, const int32 Port)
	{
		const FString ShownHost = Host.Contains(TEXT(":")) ? FString::Printf(TEXT("[%s]"), *Host) : Host;
		return Port > 0 ? FString::Printf(TEXT("%s://%s:%d"), *Scheme, *ShownHost, Port) : FString::Printf(TEXT("%s://%s"), *Scheme, *ShownHost);
	}

	/** The command's file name, without folder or extension, in lower case. */
	FString CommandFileName(const FString& Command)
	{
		FString Clean = Command.TrimStartAndEnd().TrimQuotes();
		Clean.ReplaceInline(TEXT("\\"), TEXT("/"));
		FString Name = FPaths::GetCleanFilename(Clean);
		const FString Extension = FPaths::GetExtension(Name).ToLower();
		if (Extension == TEXT("exe") || Extension == TEXT("cmd") || Extension == TEXT("bat") || Extension == TEXT("ps1")
			|| Extension == TEXT("sh") || Extension == TEXT("js") || Extension == TEXT("py"))
		{
			Name = FPaths::GetBaseFilename(Name);
		}
		return Name.ToLower();
	}

	/** Fills a server's address and words from its URL, command and arguments. The arguments themselves are not kept. */
	void ApplyEndpoint(FHyperAIStudioConfiguredMcpServer& Server, const FString& Url, const FString& Command, const TArray<FString>& Args)
	{
		FString Path;
		if (!Url.IsEmpty() && SplitUrl(Url, Server.Scheme, Server.Host, Server.Port, Path))
		{
			Server.bHasUrl = true;
			AddArgumentWords(Path, Server.Words);
		}
		if (!Command.IsEmpty())
		{
			Server.CommandName = CommandFileName(Command);
			AddNameWords(Server.CommandName, Server.Words);
		}
		for (const FString& Arg : Args)
		{
			AddArgumentWords(Arg, Server.Words);
			// A proxy such as mcp-remote http://localhost:5353/mcp: its loopback target is where it really goes.
			FString Scheme;
			FString Host;
			int32 Port = 0;
			FString ArgPath;
			if (!Server.bHasUrl && Arg.Contains(TEXT("://")) && SplitUrl(Arg, Scheme, Host, Port, ArgPath)
				&& HyperAIStudio::McpBypass::IsLoopbackHost(Host))
			{
				Server.Scheme = Scheme;
				Server.Host = Host;
				Server.Port = Port;
				Server.bHasUrl = true;
				AddArgumentWords(ArgPath, Server.Words);
			}
		}
		if (Server.bHasUrl && Server.CommandName.IsEmpty())
		{
			Server.Source.Address = FormatAddress(Server.Scheme, Server.Host, Server.Port);
		}
		else if (Server.bHasUrl)
		{
			Server.Source.Address = FString::Printf(TEXT("command %s -> %s"), *Server.CommandName, *FormatAddress(Server.Scheme, Server.Host, Server.Port));
		}
		else if (!Server.CommandName.IsEmpty())
		{
			Server.Source.Address = FString::Printf(TEXT("command %s"), *Server.CommandName);
		}
	}

	/** JSON with comments (VS Code's mcp.json allows them) without them; strings are kept as they are. */
	FString StripJsonComments(const FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		bool bInString = false;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Character = Text[Index];
			if (bInString)
			{
				Out.AppendChar(Character);
				if (Character == TEXT('\\') && Index + 1 < Text.Len())
				{
					Out.AppendChar(Text[++Index]);
				}
				else if (Character == TEXT('"'))
				{
					bInString = false;
				}
				continue;
			}
			if (Character == TEXT('"'))
			{
				bInString = true;
				Out.AppendChar(Character);
				continue;
			}
			if (Character == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('/'))
			{
				while (Index < Text.Len() && Text[Index] != TEXT('\n'))
				{
					++Index;
				}
				Out.AppendChar(TEXT('\n'));
				continue;
			}
			if (Character == TEXT('/') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('*'))
			{
				Index += 2;
				while (Index + 1 < Text.Len() && !(Text[Index] == TEXT('*') && Text[Index + 1] == TEXT('/')))
				{
					++Index;
				}
				++Index;
				Out.AppendChar(TEXT(' '));
				continue;
			}
			Out.AppendChar(Character);
		}
		return Out;
	}

	/** Drops a comma that only whitespace separates from a closing } or ] (after StripJsonComments). Strings are kept. */
	FString RemoveTrailingCommas(const FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		bool bInString = false;
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			const TCHAR Character = Text[Index];
			if (bInString)
			{
				Out.AppendChar(Character);
				if (Character == TEXT('\\') && Index + 1 < Text.Len())
				{
					Out.AppendChar(Text[++Index]);
				}
				else if (Character == TEXT('"'))
				{
					bInString = false;
				}
				continue;
			}
			if (Character == TEXT('"'))
			{
				bInString = true;
			}
			else if (Character == TEXT(','))
			{
				int32 Next = Index + 1;
				while (Next < Text.Len() && FChar::IsWhitespace(Text[Next]))
				{
					++Next;
				}
				if (Next < Text.Len() && (Text[Next] == TEXT('}') || Text[Next] == TEXT(']')))
				{
					continue;
				}
			}
			Out.AppendChar(Character);
		}
		return Out;
	}

	TSharedPtr<FJsonObject> ReadJsonObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Root;
		const FString Clean = RemoveTrailingCommas(StripJsonComments(Text));
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Clean);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return nullptr;
		}
		return Root;
	}

	/** One entry of a JSON config: its URL, command and arguments (for words only), and whether it is off. */
	FHyperAIStudioConfiguredMcpServer ReadJsonEntry(const FString& Name, const TSharedPtr<FJsonObject>& Entry, const FHyperAIStudioMcpServerSource& Template)
	{
		FHyperAIStudioConfiguredMcpServer Server;
		Server.Name = Name;
		Server.Source = Template;
		Server.Source.EntryName = Name;
		AddNameWords(Name, Server.Words);
		FString Url;
		for (const TCHAR* Field : {TEXT("url"), TEXT("httpUrl"), TEXT("serverUrl")})
		{
			if (Url.IsEmpty() && Entry->TryGetStringField(Field, Url))
			{
				break;
			}
		}
		FString Command;
		Entry->TryGetStringField(TEXT("command"), Command);
		TArray<FString> Args;
		const TArray<TSharedPtr<FJsonValue>>* ArgValues = nullptr;
		if (Entry->TryGetArrayField(TEXT("args"), ArgValues) && ArgValues)
		{
			for (const TSharedPtr<FJsonValue>& Value : *ArgValues)
			{
				FString Arg;
				if (Value.IsValid() && Value->TryGetString(Arg))
				{
					Args.Add(MoveTemp(Arg));
				}
			}
		}
		ApplyEndpoint(Server, Url, Command, Args);
		bool bFlag = false;
		if (Entry->TryGetBoolField(TEXT("disabled"), bFlag) && bFlag)
		{
			Server.bEnabled = false;
		}
		if (Entry->TryGetBoolField(TEXT("enabled"), bFlag) && !bFlag)
		{
			Server.bEnabled = false;
		}
		return Server;
	}

	void ReadJsonContainer(const TSharedPtr<FJsonObject>& Container, const FHyperAIStudioMcpServerSource& Template,
		TArray<FHyperAIStudioConfiguredMcpServer>& OutServers)
	{
		if (!Container.IsValid())
		{
			return;
		}
		int32 Count = 0;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Container->Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (!Pair.Value.IsValid() || !Pair.Value->TryGetObject(Entry) || !Entry || !Entry->IsValid())
			{
				continue;
			}
			if (++Count > MaxServersPerFile)
			{
				break;
			}
			OutServers.Add(ReadJsonEntry(Pair.Key, *Entry, Template));
		}
	}

	/** Project keys in ~/.claude.json are spelled with either slash and any case on Windows. */
	FString NormalizeProjectKey(const FString& Path)
	{
		FString Result = Path.TrimStartAndEnd();
		Result.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Result.Len() > 1 && Result.EndsWith(TEXT("/")))
		{
			Result.LeftChopInline(1);
		}
#if PLATFORM_WINDOWS
		Result.ToLowerInline();
#endif
		return Result;
	}

	// ---- A small TOML subset reader: table headers, dotted and quoted keys, strings, booleans, arrays, inline tables. ----

	struct FTomlStatement
	{
		bool bHeader = false;
		bool bArrayHeader = false;
		/** The header's path, or the assignment's key path. */
		TArray<FString> Path;
		/** The assignment's value, as written. */
		FString Value;
	};

	bool ParseTomlKeyPath(const FString& Text, TArray<FString>& OutPath)
	{
		OutPath.Reset();
		int32 Index = 0;
		auto SkipSpace = [&Text, &Index]()
		{
			while (Index < Text.Len() && (Text[Index] == TEXT(' ') || Text[Index] == TEXT('\t')))
			{
				++Index;
			}
		};
		while (true)
		{
			SkipSpace();
			if (Index >= Text.Len())
			{
				return false;
			}
			FString Segment;
			if (Text[Index] == TEXT('"'))
			{
				++Index;
				while (Index < Text.Len() && Text[Index] != TEXT('"'))
				{
					if (Text[Index] == TEXT('\\') && Index + 1 < Text.Len())
					{
						++Index;
					}
					Segment.AppendChar(Text[Index++]);
				}
				if (Index >= Text.Len())
				{
					return false;
				}
				++Index;
			}
			else if (Text[Index] == TEXT('\''))
			{
				++Index;
				while (Index < Text.Len() && Text[Index] != TEXT('\''))
				{
					Segment.AppendChar(Text[Index++]);
				}
				if (Index >= Text.Len())
				{
					return false;
				}
				++Index;
			}
			else
			{
				while (Index < Text.Len() && (IsAlnum(Text[Index]) || Text[Index] == TEXT('_') || Text[Index] == TEXT('-')))
				{
					Segment.AppendChar(Text[Index++]);
				}
				if (Segment.IsEmpty())
				{
					return false;
				}
			}
			OutPath.Add(MoveTemp(Segment));
			SkipSpace();
			if (Index >= Text.Len())
			{
				return true;
			}
			if (Text[Index] != TEXT('.'))
			{
				return false;
			}
			++Index;
		}
	}

	/**
	 * Splits TOML into statements. Strings (basic, literal, and both multi-line forms), comments, and arrays or inline
	 * tables spread over several lines are followed exactly enough to know where each statement ends.
	 */
	bool SplitToml(const FString& Text, TArray<FTomlStatement>& OutStatements, FString& OutError)
	{
		int32 Index = 0;
		const int32 Length = Text.Len();
		auto StartsAt = [&Text, Length](const int32 At, const TCHAR* Token)
		{
			const int32 TokenLength = FCString::Strlen(Token);
			return At + TokenLength <= Length && FCString::Strncmp(*Text + At, Token, TokenLength) == 0;
		};
		while (Index < Length)
		{
			const TCHAR Character = Text[Index];
			if (FChar::IsWhitespace(Character) || Character == 0xFEFF)
			{
				++Index;
				continue;
			}
			if (Character == TEXT('#'))
			{
				while (Index < Length && Text[Index] != TEXT('\n'))
				{
					++Index;
				}
				continue;
			}
			if (OutStatements.Num() >= MaxTomlStatements)
			{
				OutError = TEXT("too many statements");
				return false;
			}
			FTomlStatement Statement;
			if (Character == TEXT('['))
			{
				Statement.bHeader = true;
				Statement.bArrayHeader = StartsAt(Index, TEXT("[["));
				const int32 Open = Index + (Statement.bArrayHeader ? 2 : 1);
				int32 Close = Open;
				TCHAR Quote = 0;
				for (; Close < Length && Text[Close] != TEXT('\n'); ++Close)
				{
					if (Quote)
					{
						if (Text[Close] == TEXT('\\') && Quote == TEXT('"'))
						{
							++Close;
						}
						else if (Text[Close] == Quote)
						{
							Quote = 0;
						}
					}
					else if (Text[Close] == TEXT('"') || Text[Close] == TEXT('\''))
					{
						Quote = Text[Close];
					}
					else if (Text[Close] == TEXT(']'))
					{
						break;
					}
				}
				if (Close >= Length || Text[Close] != TEXT(']') || (Statement.bArrayHeader && !StartsAt(Close, TEXT("]]"))))
				{
					OutError = TEXT("unclosed table header");
					return false;
				}
				if (!ParseTomlKeyPath(Text.Mid(Open, Close - Open), Statement.Path))
				{
					OutError = TEXT("unreadable table header");
					return false;
				}
				Index = Close + (Statement.bArrayHeader ? 2 : 1);
				// The rest of the line may hold only a comment.
				while (Index < Length && Text[Index] != TEXT('\n'))
				{
					if (Text[Index] == TEXT('#'))
					{
						while (Index < Length && Text[Index] != TEXT('\n'))
						{
							++Index;
						}
						break;
					}
					if (!FChar::IsWhitespace(Text[Index]))
					{
						OutError = TEXT("text after a table header");
						return false;
					}
					++Index;
				}
				OutStatements.Add(MoveTemp(Statement));
				continue;
			}

			// key = value: the key runs to the first '=' outside quotes.
			const int32 KeyStart = Index;
			TCHAR Quote = 0;
			for (; Index < Length; ++Index)
			{
				const TCHAR Current = Text[Index];
				if (Current == TEXT('\n'))
				{
					OutError = TEXT("a key without a value");
					return false;
				}
				if (Quote)
				{
					if (Current == TEXT('\\') && Quote == TEXT('"'))
					{
						++Index;
					}
					else if (Current == Quote)
					{
						Quote = 0;
					}
				}
				else if (Current == TEXT('"') || Current == TEXT('\''))
				{
					Quote = Current;
				}
				else if (Current == TEXT('='))
				{
					break;
				}
			}
			if (Index >= Length || !ParseTomlKeyPath(Text.Mid(KeyStart, Index - KeyStart), Statement.Path))
			{
				OutError = TEXT("unreadable key");
				return false;
			}
			++Index;
			// The value runs to the end of its line, past any line breaks inside strings, arrays and inline tables.
			const int32 ValueStart = Index;
			int32 Depth = 0;
			int32 ValueEnd = Length;
			while (Index < Length)
			{
				const TCHAR Current = Text[Index];
				if (StartsAt(Index, TEXT("\"\"\"")) || StartsAt(Index, TEXT("'''")))
				{
					const TCHAR* Delimiter = Current == TEXT('"') ? TEXT("\"\"\"") : TEXT("'''");
					Index += 3;
					while (Index < Length && !StartsAt(Index, Delimiter))
					{
						if (Current == TEXT('"') && Text[Index] == TEXT('\\'))
						{
							++Index;
						}
						++Index;
					}
					if (Index >= Length)
					{
						OutError = TEXT("unclosed multi-line string");
						return false;
					}
					Index += 3;
					// A closing run may hold one or two more quotes, which belong to the string.
					while (Index < Length && Text[Index] == Current)
					{
						++Index;
					}
					continue;
				}
				if (Current == TEXT('"') || Current == TEXT('\''))
				{
					++Index;
					while (Index < Length && Text[Index] != Current && Text[Index] != TEXT('\n'))
					{
						if (Current == TEXT('"') && Text[Index] == TEXT('\\'))
						{
							++Index;
						}
						++Index;
					}
					if (Index >= Length || Text[Index] != Current)
					{
						OutError = TEXT("unclosed string");
						return false;
					}
					++Index;
					continue;
				}
				if (Current == TEXT('#'))
				{
					if (Depth == 0)
					{
						ValueEnd = Index;
					}
					while (Index < Length && Text[Index] != TEXT('\n'))
					{
						++Index;
					}
					if (Depth == 0)
					{
						break;
					}
					continue;
				}
				if (Current == TEXT('[') || Current == TEXT('{'))
				{
					++Depth;
				}
				else if (Current == TEXT(']') || Current == TEXT('}'))
				{
					--Depth;
					if (Depth < 0)
					{
						OutError = TEXT("unbalanced brackets");
						return false;
					}
				}
				else if (Current == TEXT('\n') && Depth == 0)
				{
					ValueEnd = Index;
					break;
				}
				++Index;
			}
			if (Depth != 0)
			{
				OutError = TEXT("unclosed array or inline table");
				return false;
			}
			Statement.Value = Text.Mid(ValueStart, FMath::Max(0, FMath::Min(ValueEnd, Length) - ValueStart)).TrimStartAndEnd();
			if (Statement.Value.IsEmpty())
			{
				OutError = TEXT("a key without a value");
				return false;
			}
			OutStatements.Add(MoveTemp(Statement));
		}
		return true;
	}

	/** A TOML string value ("basic", 'literal', or either multi-line form); false for anything else. */
	bool ReadTomlString(const FString& Value, FString& OutText)
	{
		OutText.Reset();
		const FString Trimmed = Value.TrimStartAndEnd();
		if (Trimmed.Len() >= 6 && (Trimmed.StartsWith(TEXT("\"\"\"")) || Trimmed.StartsWith(TEXT("'''"))))
		{
			OutText = Trimmed.Mid(3, Trimmed.Len() - 6).TrimStartAndEnd();
			return true;
		}
		if (Trimmed.Len() >= 2 && Trimmed.StartsWith(TEXT("'")) && Trimmed.EndsWith(TEXT("'")))
		{
			OutText = Trimmed.Mid(1, Trimmed.Len() - 2);
			return true;
		}
		if (Trimmed.Len() >= 2 && Trimmed.StartsWith(TEXT("\"")) && Trimmed.EndsWith(TEXT("\"")))
		{
			const FString Inner = Trimmed.Mid(1, Trimmed.Len() - 2);
			for (int32 Index = 0; Index < Inner.Len(); ++Index)
			{
				if (Inner[Index] == TEXT('\\') && Index + 1 < Inner.Len())
				{
					const TCHAR Next = Inner[++Index];
					switch (Next)
					{
					case TEXT('n'): OutText.AppendChar(TEXT('\n')); break;
					case TEXT('t'): OutText.AppendChar(TEXT('\t')); break;
					case TEXT('r'): OutText.AppendChar(TEXT('\r')); break;
					default: OutText.AppendChar(Next); break;
					}
					continue;
				}
				OutText.AppendChar(Inner[Index]);
			}
			return true;
		}
		return false;
	}

	/** Splits the inside of an array or inline table at top-level commas, leaving out comments (a multi-line array may hold them). */
	TArray<FString> SplitTomlItems(const FString& Inner)
	{
		TArray<FString> Items;
		FString Current;
		int32 Depth = 0;
		TCHAR Quote = 0;
		for (int32 Index = 0; Index < Inner.Len(); ++Index)
		{
			const TCHAR Character = Inner[Index];
			if (Quote)
			{
				Current.AppendChar(Character);
				if (Character == TEXT('\\') && Quote == TEXT('"') && Index + 1 < Inner.Len())
				{
					Current.AppendChar(Inner[++Index]);
				}
				else if (Character == Quote)
				{
					Quote = 0;
				}
				continue;
			}
			if (Character == TEXT('#'))
			{
				while (Index + 1 < Inner.Len() && Inner[Index + 1] != TEXT('\n'))
				{
					++Index;
				}
				continue;
			}
			if (Character == TEXT('"') || Character == TEXT('\''))
			{
				Quote = Character;
			}
			else if (Character == TEXT('[') || Character == TEXT('{'))
			{
				++Depth;
			}
			else if (Character == TEXT(']') || Character == TEXT('}'))
			{
				--Depth;
			}
			else if (Character == TEXT(',') && Depth == 0)
			{
				Items.Add(Current.TrimStartAndEnd());
				Current.Reset();
				continue;
			}
			Current.AppendChar(Character);
		}
		Items.Add(Current.TrimStartAndEnd());
		Items.RemoveAll([](const FString& Item) { return Item.IsEmpty(); });
		return Items;
	}

	TArray<FString> ReadTomlStringArray(const FString& Value)
	{
		TArray<FString> Result;
		const FString Trimmed = Value.TrimStartAndEnd();
		if (!Trimmed.StartsWith(TEXT("[")) || !Trimmed.EndsWith(TEXT("]")))
		{
			return Result;
		}
		for (const FString& Item : SplitTomlItems(Trimmed.Mid(1, Trimmed.Len() - 2)))
		{
			FString Text;
			if (ReadTomlString(Item, Text))
			{
				Result.Add(MoveTemp(Text));
			}
		}
		return Result;
	}

	/** What a Codex config says about one server, gathered from every statement that names it. */
	struct FCodexServerFields
	{
		FString Url;
		FString Command;
		TArray<FString> Args;
		TArray<FString> Tools;
		bool bEnabled = true;
		int32 Order = 0;
	};

	struct FCodexReader
	{
		TMap<FString, FCodexServerFields> Servers;
		int32 NextOrder = 0;

		FCodexServerFields& Server(const FString& Name)
		{
			FCodexServerFields* Found = Servers.Find(Name);
			if (!Found)
			{
				Found = &Servers.Add(Name);
				Found->Order = NextOrder++;
			}
			return *Found;
		}

		/** One key/value at an absolute path; inline tables are walked into, so every TOML shape lands here. */
		void Assign(const TArray<FString>& Path, const FString& Value, const int32 Depth = 0)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			if (Depth < 8 && Trimmed.StartsWith(TEXT("{")) && Trimmed.EndsWith(TEXT("}")))
			{
				for (const FString& Item : SplitTomlItems(Trimmed.Mid(1, Trimmed.Len() - 2)))
				{
					int32 Equals = INDEX_NONE;
					TCHAR Quote = 0;
					for (int32 Index = 0; Index < Item.Len(); ++Index)
					{
						if (Quote)
						{
							if (Item[Index] == Quote)
							{
								Quote = 0;
							}
						}
						else if (Item[Index] == TEXT('"') || Item[Index] == TEXT('\''))
						{
							Quote = Item[Index];
						}
						else if (Item[Index] == TEXT('='))
						{
							Equals = Index;
							break;
						}
					}
					TArray<FString> Key;
					if (Equals != INDEX_NONE && ParseTomlKeyPath(Item.Left(Equals), Key))
					{
						TArray<FString> Child = Path;
						Child.Append(Key);
						Assign(Child, Item.Mid(Equals + 1), Depth + 1);
					}
				}
				return;
			}
			if (Path.Num() < 2 || Path[0] != TEXT("mcp_servers"))
			{
				// Opening the table is enough to make a server: [mcp_servers.x] with nothing else in it is still listed.
				return;
			}
			FCodexServerFields& Fields = Server(Path[1]);
			if (Path.Num() == 3)
			{
				const FString& Field = Path[2];
				FString Text;
				if (Field == TEXT("url") && ReadTomlString(Trimmed, Text))
				{
					Fields.Url = Text;
				}
				else if (Field == TEXT("command") && ReadTomlString(Trimmed, Text))
				{
					Fields.Command = Text;
				}
				else if (Field == TEXT("args"))
				{
					Fields.Args = ReadTomlStringArray(Trimmed);
				}
				else if (Field == TEXT("enabled"))
				{
					Fields.bEnabled = !Trimmed.Equals(TEXT("false"));
				}
				else if (Field == TEXT("enabled_tools"))
				{
					for (const FString& Tool : ReadTomlStringArray(Trimmed))
					{
						Fields.Tools.AddUnique(Tool);
					}
				}
				// env, env_vars, http_headers, bearer_token_env_var and the rest are never read.
			}
			else if (Path.Num() >= 4 && Path[2] == TEXT("tools"))
			{
				Fields.Tools.AddUnique(Path[3]);
			}
		}

		void OpenTable(const TArray<FString>& Path)
		{
			if (Path.Num() >= 2 && Path[0] == TEXT("mcp_servers"))
			{
				FCodexServerFields& Fields = Server(Path[1]);
				if (Path.Num() >= 4 && Path[2] == TEXT("tools"))
				{
					Fields.Tools.AddUnique(Path[3]);
				}
			}
		}
	};

	/** One finding per family while classifying. */
	struct FFamilyBuilder
	{
		TArray<FHyperAIStudioMcpFinding> Findings;

		FHyperAIStudioMcpFinding& Get(const FString& Id, const FString& DisplayName)
		{
			for (FHyperAIStudioMcpFinding& Finding : Findings)
			{
				if (Finding.Id == Id)
				{
					return Finding;
				}
			}
			FHyperAIStudioMcpFinding& Finding = Findings.AddDefaulted_GetRef();
			Finding.Id = Id;
			Finding.DisplayName = DisplayName;
			Finding.Class = EHyperAIStudioMcpServerClass::OtherTools;
			Finding.Reason = EHyperAIStudioMcpReason::NotUnreal;
			return Finding;
		}

		/** Adds a source; a finding that changes the editor in any of its places changes it. */
		void Add(const FString& Id, const FString& DisplayName, const EHyperAIStudioMcpServerClass Class,
			const EHyperAIStudioMcpReason Reason, const FHyperAIStudioMcpServerSource& Source, const int32 Port = 0)
		{
			const bool bNew = !Findings.ContainsByPredicate([&Id](const FHyperAIStudioMcpFinding& Finding) { return Finding.Id == Id; });
			FHyperAIStudioMcpFinding& Finding = Get(Id, DisplayName);
			if (bNew || (Class == EHyperAIStudioMcpServerClass::ChangesEditor && Finding.Class != EHyperAIStudioMcpServerClass::ChangesEditor))
			{
				Finding.Class = Class;
				Finding.Reason = Reason;
				Finding.Port = Port;
			}
			Finding.Sources.AddUnique(Source);
		}
	};

	bool ContainsNoCase(const FString& Text, const TCHAR* Part)
	{
		return Text.Contains(Part, ESearchCase::IgnoreCase);
	}

	FText ScopeText(const FString& Scope)
	{
		if (Scope == TEXT("user"))
		{
			return LOCTEXT("ScopeUser", "user");
		}
		if (Scope == TEXT("local"))
		{
			return LOCTEXT("ScopeLocal", "local");
		}
		return LOCTEXT("ScopeProject", "project");
	}

	/** "nwiro", "nwiro and rider", "nwiro, rider and Python remote execution". */
	FText JoinNames(const TArray<FString>& Names)
	{
		if (Names.IsEmpty())
		{
			return FText::GetEmpty();
		}
		if (Names.Num() == 1)
		{
			return FText::FromString(Names[0]);
		}
		TArray<FText> Head;
		for (int32 Index = 0; Index < Names.Num() - 1; ++Index)
		{
			Head.Add(FText::FromString(Names[Index]));
		}
		return FText::Format(LOCTEXT("JoinAnd", "{0} and {1}"), FText::Join(LOCTEXT("JoinComma", ", "), Head), FText::FromString(Names.Last()));
	}

	/** Where a finding is, briefly: ".mcp.json, ~/.codex/config.toml, the Nwiro Integration Kit plugin". */
	FText Places(const FHyperAIStudioMcpFinding& Finding)
	{
		TArray<FText> Parts;
		TSet<FString> Seen;
		for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
		{
			FText Part;
			FString Key;
			switch (Source.Kind)
			{
			case EHyperAIStudioMcpSourceKind::Plugin:
				Key = TEXT("plugin:") + Source.EntryName;
				Part = FText::Format(LOCTEXT("PlacePlugin", "the {0} plugin"), FText::FromString(Source.EntryName));
				break;
			case EHyperAIStudioMcpSourceKind::EditorSetting:
				Key = TEXT("setting");
				Part = LOCTEXT("PlaceSetting", "Project Settings");
				break;
			default:
				Key = Source.ShortFile;
				Part = FText::FromString(Source.ShortFile);
				break;
			}
			if (!Seen.Contains(Key))
			{
				Seen.Add(Key);
				Parts.Add(Part);
			}
		}
		return FText::Join(LOCTEXT("JoinComma", ", "), Parts);
	}

	FString EscapeMarkdown(const FString& Text)
	{
		FString Out;
		for (const TCHAR Character : Text)
		{
			// As the report renderer reads them: an escaped mark stands for itself, so names and paths show as written.
			if (Character == TEXT('\\') || Character == TEXT('`') || Character == TEXT('*') || Character == TEXT('_') || Character == TEXT('|')
				|| Character == TEXT('[') || Character == TEXT(']'))
			{
				Out.AppendChar(TEXT('\\'));
			}
			Out.AppendChar(Character);
		}
		return Out;
	}
}

bool HyperAIStudio::McpBypass::ParseUrl(const FString& Url, FString& OutScheme, FString& OutHost, int32& OutPort)
{
	FString Path;
	return Private::SplitUrl(Url, OutScheme, OutHost, OutPort, Path);
}

bool HyperAIStudio::McpBypass::IsLoopbackHost(const FString& Host)
{
	const FString Lower = Host.ToLower().TrimStartAndEnd();
	return Lower == TEXT("localhost") || Lower.EndsWith(TEXT(".localhost")) || Lower.StartsWith(TEXT("127."))
		|| Lower == TEXT("::1") || Lower == TEXT("[::1]") || Lower == TEXT("0.0.0.0") || Lower == TEXT("::");
}

FString HyperAIStudio::McpBypass::ShortenPath(const FString& Path, const FString& ProjectRoot, const FString& Home)
{
	FString Clean = Path;
	Clean.ReplaceInline(TEXT("\\"), TEXT("/"));
	auto Under = [&Clean](FString Root, FString& OutRest)
	{
		Root.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Root.EndsWith(TEXT("/")))
		{
			Root.LeftChopInline(1);
		}
		if (Root.IsEmpty() || Clean.Len() <= Root.Len() + 1 || !Clean.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		OutRest = Clean.Mid(Root.Len() + 1);
		return true;
	};
	FString Rest;
	if (Under(ProjectRoot, Rest))
	{
		return Rest;
	}
	if (Under(Home, Rest))
	{
		return TEXT("~/") + Rest;
	}
	return Clean;
}

bool HyperAIStudio::McpBypass::ParseJsonServers(const FString& JsonText, const FString& Container,
	const FHyperAIStudioMcpServerSource& Template, TArray<FHyperAIStudioConfiguredMcpServer>& OutServers, FString& OutError)
{
	using namespace Private;
	if (JsonText.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Root = ReadJsonObject(JsonText);
	if (!Root.IsValid())
	{
		OutError = TEXT("not valid JSON");
		return false;
	}
	const TSharedPtr<FJsonObject>* Servers = nullptr;
	if (Root->TryGetObjectField(Container, Servers) && Servers)
	{
		FHyperAIStudioMcpServerSource Source = Template;
		Source.Container = Container;
		ReadJsonContainer(*Servers, Source, OutServers);
	}
	return true;
}

bool HyperAIStudio::McpBypass::ParseClaudeUserConfig(const FString& JsonText, const FString& ProjectRoot, const FString& File,
	const FString& ShortFile, TArray<FHyperAIStudioConfiguredMcpServer>& OutServers, TArray<FString>& OutDisabledProjectServers, FString& OutError)
{
	using namespace Private;
	if (JsonText.TrimStartAndEnd().IsEmpty())
	{
		return true;
	}
	const TSharedPtr<FJsonObject> Root = ReadJsonObject(JsonText);
	if (!Root.IsValid())
	{
		OutError = TEXT("not valid JSON");
		return false;
	}
	FHyperAIStudioMcpServerSource Template;
	Template.Kind = EHyperAIStudioMcpSourceKind::JsonEntry;
	Template.Client = ClaudeCode;
	Template.Scope = TEXT("user");
	Template.File = File;
	Template.ShortFile = ShortFile;
	Template.Container = TEXT("mcpServers");
	const TSharedPtr<FJsonObject>* UserServers = nullptr;
	if (Root->TryGetObjectField(TEXT("mcpServers"), UserServers) && UserServers)
	{
		ReadJsonContainer(*UserServers, Template, OutServers);
	}
	const TSharedPtr<FJsonObject>* Projects = nullptr;
	if (!Root->TryGetObjectField(TEXT("projects"), Projects) || !Projects || !Projects->IsValid())
	{
		return true;
	}
	const FString Wanted = NormalizeProjectKey(ProjectRoot);
	FHyperAIStudioMcpServerSource Local = Template;
	Local.Kind = EHyperAIStudioMcpSourceKind::ClaudeLocalEntry;
	Local.Scope = TEXT("local");
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Projects)->Values)
	{
		const TSharedPtr<FJsonObject>* Project = nullptr;
		if (NormalizeProjectKey(Pair.Key) != Wanted || !Pair.Value.IsValid() || !Pair.Value->TryGetObject(Project) || !Project)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>* LocalServers = nullptr;
		if ((*Project)->TryGetObjectField(TEXT("mcpServers"), LocalServers) && LocalServers)
		{
			TArray<FHyperAIStudioConfiguredMcpServer> Found;
			ReadJsonContainer(*LocalServers, Local, Found);
			for (FHyperAIStudioConfiguredMcpServer& Server : Found)
			{
				// The same project under both slash spellings is one entry to the user.
				const bool bSeen = OutServers.ContainsByPredicate([&Server](const FHyperAIStudioConfiguredMcpServer& Existing)
				{
					return Existing.Source == Server.Source;
				});
				if (!bSeen)
				{
					OutServers.Add(MoveTemp(Server));
				}
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* Disabled = nullptr;
		if ((*Project)->TryGetArrayField(TEXT("disabledMcpjsonServers"), Disabled) && Disabled)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Disabled)
			{
				FString Name;
				if (Value.IsValid() && Value->TryGetString(Name))
				{
					OutDisabledProjectServers.AddUnique(Name);
				}
			}
		}
	}
	return true;
}

void HyperAIStudio::McpBypass::ParseClaudeDisabledServers(const FString& JsonText, TArray<FString>& OutDisabled)
{
	const TSharedPtr<FJsonObject> Root = Private::ReadJsonObject(JsonText);
	const TArray<TSharedPtr<FJsonValue>>* Disabled = nullptr;
	if (Root.IsValid() && Root->TryGetArrayField(TEXT("disabledMcpjsonServers"), Disabled) && Disabled)
	{
		for (const TSharedPtr<FJsonValue>& Value : *Disabled)
		{
			FString Name;
			if (Value.IsValid() && Value->TryGetString(Name))
			{
				OutDisabled.AddUnique(Name);
			}
		}
	}
}

bool HyperAIStudio::McpBypass::ParseCodexToml(const FString& TomlText, const FHyperAIStudioMcpServerSource& Template,
	TArray<FHyperAIStudioConfiguredMcpServer>& OutServers, FString& OutError)
{
	using namespace Private;
	TArray<FTomlStatement> Statements;
	if (!SplitToml(TomlText, Statements, OutError))
	{
		return false;
	}
	FCodexReader Reader;
	TArray<FString> Table;
	bool bArrayTable = false;
	for (const FTomlStatement& Statement : Statements)
	{
		if (Statement.bHeader)
		{
			Table = Statement.Path;
			bArrayTable = Statement.bArrayHeader;
			if (!bArrayTable)
			{
				Reader.OpenTable(Table);
			}
			continue;
		}
		if (bArrayTable)
		{
			continue;
		}
		TArray<FString> Path = Table;
		Path.Append(Statement.Path);
		Reader.Assign(Path, Statement.Value);
	}
	TArray<TPair<FString, FCodexServerFields>> Ordered;
	for (const TPair<FString, FCodexServerFields>& Pair : Reader.Servers)
	{
		Ordered.Add(Pair);
	}
	Ordered.Sort([](const TPair<FString, FCodexServerFields>& A, const TPair<FString, FCodexServerFields>& B) { return A.Value.Order < B.Value.Order; });
	for (const TPair<FString, FCodexServerFields>& Pair : Ordered)
	{
		if (OutServers.Num() >= MaxServersPerFile)
		{
			break;
		}
		FHyperAIStudioConfiguredMcpServer Server;
		Server.Name = Pair.Key;
		Server.Source = Template;
		Server.Source.Kind = EHyperAIStudioMcpSourceKind::TomlTable;
		Server.Source.Container = TEXT("mcp_servers");
		Server.Source.EntryName = Pair.Key;
		Server.bEnabled = Pair.Value.bEnabled;
		Server.ToolNames = Pair.Value.Tools;
		AddNameWords(Pair.Key, Server.Words);
		for (const FString& Tool : Pair.Value.Tools)
		{
			AddToolWords(Tool, Server.Words);
		}
		ApplyEndpoint(Server, Pair.Value.Url, Pair.Value.Command, Pair.Value.Args);
		OutServers.Add(MoveTemp(Server));
	}
	return true;
}

TArray<FString> HyperAIStudio::McpBypass::GetConfigFiles(const FHyperAIStudioMcpScanPaths& Paths)
{
	TArray<FString> Files;
	auto Add = [&Files](const FString& Path)
	{
		if (!Path.IsEmpty())
		{
			Files.AddUnique(FPaths::ConvertRelativePathToFull(Path));
		}
	};
	if (!Paths.ProjectRoot.IsEmpty())
	{
		Add(Paths.ProjectRoot / TEXT(".mcp.json"));
		Add(Paths.ProjectRoot / TEXT(".codex/config.toml"));
		Add(Paths.ProjectRoot / TEXT(".gemini/settings.json"));
		Add(Paths.ProjectRoot / TEXT(".cursor/mcp.json"));
		Add(Paths.ProjectRoot / TEXT(".vscode/mcp.json"));
		Add(Paths.ProjectRoot / TEXT(".claude/settings.json"));
		Add(Paths.ProjectRoot / TEXT(".claude/settings.local.json"));
	}
	if (!Paths.ClaudeConfigDir.IsEmpty())
	{
		Add(Paths.ClaudeConfigDir / TEXT(".claude.json"));
	}
	if (!Paths.CodexHome.IsEmpty())
	{
		Add(Paths.CodexHome / TEXT("config.toml"));
	}
	if (!Paths.Home.IsEmpty())
	{
		Add(Paths.Home / TEXT(".gemini/settings.json"));
		Add(Paths.Home / TEXT(".cursor/mcp.json"));
	}
	if (!Paths.VSCodeUserDir.IsEmpty())
	{
		Add(Paths.VSCodeUserDir / TEXT("mcp.json"));
	}
	return Files;
}

TArray<FHyperAIStudioConfiguredMcpServer> HyperAIStudio::McpBypass::ReadConfiguredServers(const FHyperAIStudioMcpScanPaths& Paths, TArray<FString>& OutProblems)
{
	using namespace Private;
	TArray<FHyperAIStudioConfiguredMcpServer> Servers;
	TSet<FString> ReadFiles;
	auto Load = [&OutProblems, &Paths, &ReadFiles](const FString& RawPath, FString& OutText, FString& OutFull, FString& OutShort)
	{
		if (RawPath.IsEmpty())
		{
			return false;
		}
		OutFull = FPaths::ConvertRelativePathToFull(RawPath);
		OutShort = ShortenPath(OutFull, Paths.ProjectRoot, Paths.Home);
		// The project and the home folder can be the same; a file is read once.
		if (ReadFiles.Contains(OutFull.ToLower()) || !FPaths::FileExists(OutFull))
		{
			return false;
		}
		ReadFiles.Add(OutFull.ToLower());
		const int64 Size = IFileManager::Get().FileSize(*OutFull);
		if (Size > MaxConfigBytes)
		{
			OutProblems.Add(FString::Printf(TEXT("%s is too large to read (%lld bytes)."), *OutShort, Size));
			return false;
		}
		if (!FFileHelper::LoadFileToString(OutText, *OutFull))
		{
			OutProblems.Add(FString::Printf(TEXT("%s could not be opened."), *OutShort));
			return false;
		}
		return true;
	};
	auto ReadJson = [&](const FString& RawPath, const TCHAR* Client, const TCHAR* Scope, const TCHAR* Container)
	{
		FString Text;
		FHyperAIStudioMcpServerSource Template;
		if (!Load(RawPath, Text, Template.File, Template.ShortFile))
		{
			return;
		}
		Template.Kind = EHyperAIStudioMcpSourceKind::JsonEntry;
		Template.Client = Client;
		Template.Scope = Scope;
		FString Error;
		if (!ParseJsonServers(Text, Container, Template, Servers, Error))
		{
			OutProblems.Add(FString::Printf(TEXT("%s could not be read (%s)."), *Template.ShortFile, *Error));
		}
	};
	auto ReadToml = [&](const FString& RawPath, const TCHAR* Scope)
	{
		FString Text;
		FHyperAIStudioMcpServerSource Template;
		if (!Load(RawPath, Text, Template.File, Template.ShortFile))
		{
			return;
		}
		Template.Client = Codex;
		Template.Scope = Scope;
		FString Error;
		if (!ParseCodexToml(Text, Template, Servers, Error))
		{
			OutProblems.Add(FString::Printf(TEXT("%s could not be read (%s)."), *Template.ShortFile, *Error));
		}
	};

	// Claude Code: the project's .mcp.json, then the user's ~/.claude.json (user and local servers).
	const int32 ProjectJsonStart = Servers.Num();
	if (!Paths.ProjectRoot.IsEmpty())
	{
		ReadJson(Paths.ProjectRoot / TEXT(".mcp.json"), ClaudeCode, TEXT("project"), TEXT("mcpServers"));
	}
	const int32 ProjectJsonEnd = Servers.Num();
	TArray<FString> DisabledProjectServers;
	if (!Paths.ClaudeConfigDir.IsEmpty())
	{
		FString Text;
		FString Full;
		FString Short;
		if (Load(Paths.ClaudeConfigDir / TEXT(".claude.json"), Text, Full, Short))
		{
			FString Error;
			if (!ParseClaudeUserConfig(Text, Paths.ProjectRoot, Full, Short, Servers, DisabledProjectServers, Error))
			{
				OutProblems.Add(FString::Printf(TEXT("%s could not be read (%s)."), *Short, *Error));
			}
		}
	}
	if (!Paths.ProjectRoot.IsEmpty())
	{
		for (const TCHAR* Settings : {TEXT(".claude/settings.json"), TEXT(".claude/settings.local.json")})
		{
			FString Text;
			FString Full;
			FString Short;
			if (Load(Paths.ProjectRoot / Settings, Text, Full, Short))
			{
				ParseClaudeDisabledServers(Text, DisabledProjectServers);
			}
		}
	}
	for (int32 Index = ProjectJsonStart; Index < ProjectJsonEnd; ++Index)
	{
		if (DisabledProjectServers.Contains(Servers[Index].Name))
		{
			Servers[Index].bEnabled = false;
		}
	}

	// Codex: the project's .codex/config.toml, then CODEX_HOME's.
	if (!Paths.ProjectRoot.IsEmpty())
	{
		ReadToml(Paths.ProjectRoot / TEXT(".codex/config.toml"), TEXT("project"));
	}
	if (!Paths.CodexHome.IsEmpty())
	{
		ReadToml(Paths.CodexHome / TEXT("config.toml"), TEXT("user"));
	}

	// Gemini, Cursor and VS Code: the project's file, then the user's.
	if (!Paths.ProjectRoot.IsEmpty())
	{
		ReadJson(Paths.ProjectRoot / TEXT(".gemini/settings.json"), Gemini, TEXT("project"), TEXT("mcpServers"));
		ReadJson(Paths.ProjectRoot / TEXT(".cursor/mcp.json"), Cursor, TEXT("project"), TEXT("mcpServers"));
		ReadJson(Paths.ProjectRoot / TEXT(".vscode/mcp.json"), VSCode, TEXT("project"), TEXT("servers"));
	}
	if (!Paths.Home.IsEmpty())
	{
		ReadJson(Paths.Home / TEXT(".gemini/settings.json"), Gemini, TEXT("user"), TEXT("mcpServers"));
		ReadJson(Paths.Home / TEXT(".cursor/mcp.json"), Cursor, TEXT("user"), TEXT("mcpServers"));
	}
	if (!Paths.VSCodeUserDir.IsEmpty())
	{
		ReadJson(Paths.VSCodeUserDir / TEXT("mcp.json"), VSCode, TEXT("user"), TEXT("servers"));
	}
	return Servers;
}

TArray<FHyperAIStudioMcpFinding> HyperAIStudio::McpBypass::Classify(const TArray<FHyperAIStudioConfiguredMcpServer>& Servers,
	const FHyperAIStudioMcpEditorFacts& Facts, const TSet<FString>& Silenced)
{
	using namespace Private;
	using EClass = EHyperAIStudioMcpServerClass;
	using EReason = EHyperAIStudioMcpReason;

	const FHyperAIStudioMcpPluginFact* NwiroPlugin = nullptr;
	const FHyperAIStudioMcpPluginFact* RiderLinkPlugin = nullptr;
	TArray<const FHyperAIStudioMcpPluginFact*> McpPlugins;
	for (const FHyperAIStudioMcpPluginFact& Plugin : Facts.Plugins)
	{
		if (ContainsNoCase(Plugin.Name, TEXT("nwiro")) || ContainsNoCase(Plugin.FriendlyName, TEXT("nwiro")))
		{
			NwiroPlugin = NwiroPlugin ? NwiroPlugin : &Plugin;
		}
		else if (Plugin.Name.Equals(TEXT("RiderLink"), ESearchCase::IgnoreCase))
		{
			RiderLinkPlugin = &Plugin;
		}
		else if ((ContainsNoCase(Plugin.Name, TEXT("mcp")) || ContainsNoCase(Plugin.FriendlyName, TEXT("mcp")))
			&& !ContainsNoCase(Plugin.CreatedBy, TEXT("Epic Games")) && !Plugin.Name.StartsWith(TEXT("HyperAI"), ESearchCase::IgnoreCase))
		{
			McpPlugins.Add(&Plugin);
		}
	}
	auto Listening = [&Facts](const int32 Port) { return Port > 0 && Facts.ListeningPorts.Contains(Port); };
	int32 NwiroPort = 0;
	for (const int32 Port : NwiroPorts)
	{
		if (Listening(Port))
		{
			NwiroPort = Port;
			break;
		}
	}

	FFamilyBuilder Families;
	for (const FHyperAIStudioConfiguredMcpServer& Server : Servers)
	{
		if (!Server.bEnabled)
		{
			continue;
		}
		const bool bLoopback = Server.bHasUrl && IsLoopbackHost(Server.Host);
		if (bLoopback && Server.Port == Facts.HyperAIPort)
		{
			// HyperAI's own Unreal MCP endpoint: its calls reach the approval gate.
			continue;
		}
		const bool bNwiroPort = bLoopback && (Server.Port == NwiroPorts[0] || Server.Port == NwiroPorts[1]);
		if (Server.Words.Contains(TEXT("nwiro")) || bNwiroPort)
		{
			const bool bHere = NwiroPlugin != nullptr || (bLoopback && Listening(Server.Port));
			Families.Add(TEXT("nwiro"), TEXT("nwiro"), bHere ? EClass::ChangesEditor : EClass::OtherTools,
				bHere ? EReason::Nwiro : EReason::NwiroNotHere, Server.Source, NwiroPort);
		}
		else if (Server.Words.Contains(TEXT("rider")) || Server.Words.Contains(TEXT("jetbrains")))
		{
			const bool bLinked = RiderLinkPlugin != nullptr;
			Families.Add(TEXT("rider"), TEXT("rider"), bLinked ? EClass::ChangesEditor : EClass::OtherTools,
				bLinked ? EReason::Rider : EReason::RiderNoLink, Server.Source);
		}
		else if (Server.Words.Contains(TEXT("unreal")))
		{
			const bool bRemote = Server.bHasUrl && !bLoopback;
			Families.Add(Server.Name.ToLower(), Server.Name, bRemote ? EClass::OtherTools : EClass::ChangesEditor,
				bRemote ? EReason::UnrealRemote : EReason::UnrealName, Server.Source);
		}
		else if (bLoopback && Listening(Server.Port))
		{
			Families.Add(Server.Name.ToLower(), Server.Name, EClass::ChangesEditor, EReason::EditorPort, Server.Source, Server.Port);
		}
		else
		{
			Families.Add(Server.Name.ToLower(), Server.Name, EClass::OtherTools, EReason::NotUnreal, Server.Source);
		}
	}

	auto PluginSource = [](const FHyperAIStudioMcpPluginFact& Plugin, const int32 Port)
	{
		FHyperAIStudioMcpServerSource Source;
		Source.Kind = EHyperAIStudioMcpSourceKind::Plugin;
		Source.EntryName = Plugin.FriendlyName.IsEmpty() ? Plugin.Name : Plugin.FriendlyName;
		Source.File = Plugin.BaseDir;
		Source.ShortFile = Plugin.ShortBaseDir.IsEmpty() ? Plugin.BaseDir : Plugin.ShortBaseDir;
		Source.Address = Port > 0 ? FString::Printf(TEXT("port %d"), Port) : FString();
		return Source;
	};
	if (NwiroPlugin)
	{
		// Hosted here, so any program on this computer can reach it, configured or not.
		Families.Add(TEXT("nwiro"), TEXT("nwiro"), EClass::ChangesEditor, EReason::Nwiro, PluginSource(*NwiroPlugin, NwiroPort), NwiroPort);
	}
	if (RiderLinkPlugin)
	{
		const bool bRiderConfigured = Families.Findings.ContainsByPredicate([](const FHyperAIStudioMcpFinding& Finding) { return Finding.Id == TEXT("rider"); });
		if (bRiderConfigured)
		{
			Families.Add(TEXT("rider"), TEXT("rider"), EClass::ChangesEditor, EReason::Rider, PluginSource(*RiderLinkPlugin, 0));
		}
		else
		{
			Families.Add(TEXT("riderlink"), TEXT("RiderLink"), EClass::OtherTools, EReason::RiderLinkOnly, PluginSource(*RiderLinkPlugin, 0));
		}
	}
	for (const FHyperAIStudioMcpPluginFact* Plugin : McpPlugins)
	{
		Families.Add(Plugin->Name.ToLower(), Plugin->FriendlyName.IsEmpty() ? Plugin->Name : Plugin->FriendlyName,
			EClass::ChangesEditor, EReason::McpPlugin, PluginSource(*Plugin, 0));
	}
	if (Facts.bPythonRemoteExecution)
	{
		FHyperAIStudioMcpServerSource Source;
		Source.Kind = EHyperAIStudioMcpSourceKind::EditorSetting;
		Source.EntryName = TEXT("Enable Remote Execution?");
		Source.ShortFile = TEXT("Project Settings > Plugins > Python");
		Families.Add(TEXT("python-remote-execution"), TEXT("Python remote execution"), EClass::ChangesEditor,
			EReason::PythonRemoteExecution, Source);
	}

	TArray<FHyperAIStudioMcpFinding> Findings = MoveTemp(Families.Findings);
	for (FHyperAIStudioMcpFinding& Finding : Findings)
	{
		Finding.bSilenced = Silenced.Contains(Finding.Id);
		for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
		{
			const bool bConfig = Source.Kind != EHyperAIStudioMcpSourceKind::Plugin && Source.Kind != EHyperAIStudioMcpSourceKind::EditorSetting;
			if (bConfig && Facts.HyperAIExtraServerIds.Contains(Source.EntryName.ToLower()))
			{
				Finding.bWrittenByHyperAI = true;
			}
		}
	}
	Findings.Sort([](const FHyperAIStudioMcpFinding& A, const FHyperAIStudioMcpFinding& B)
	{
		if (A.ChangesEditor() != B.ChangesEditor())
		{
			return A.ChangesEditor();
		}
		return A.Id < B.Id;
	});
	return Findings;
}

TArray<FHyperAIStudioMcpFinding> HyperAIStudio::McpBypass::FilterBypass(const TArray<FHyperAIStudioMcpFinding>& Findings, const bool bUnsilencedOnly)
{
	return Findings.FilterByPredicate([bUnsilencedOnly](const FHyperAIStudioMcpFinding& Finding)
	{
		return Finding.ChangesEditor() && (!bUnsilencedOnly || !Finding.bSilenced);
	});
}

TArray<FString> HyperAIStudio::McpBypass::GetBypassNames(const TArray<FHyperAIStudioMcpFinding>& Findings)
{
	TArray<FString> Names;
	for (const FHyperAIStudioMcpFinding& Finding : Findings)
	{
		if (Finding.ChangesEditor())
		{
			Names.AddUnique(Finding.DisplayName);
		}
	}
	return Names;
}

TArray<FString> HyperAIStudio::McpBypass::BuildWarningKeys(const TArray<FHyperAIStudioMcpFinding>& Findings)
{
	TArray<FString> Keys;
	for (const FHyperAIStudioMcpFinding& Finding : FilterBypass(Findings, /*bUnsilencedOnly=*/true))
	{
		for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
		{
			Keys.AddUnique(FString::Printf(TEXT("%s=%d|%s|%s|%s"), *Finding.Id, static_cast<int32>(Source.Kind), *Source.File.ToLower(),
				*Source.Container, *Source.EntryName));
		}
	}
	Keys.Sort();
	return Keys;
}

FText HyperAIStudio::McpBypass::DescribeWhy(const FHyperAIStudioMcpFinding& Finding)
{
	switch (Finding.Reason)
	{
	case EHyperAIStudioMcpReason::Nwiro:
		return LOCTEXT("WhyNwiro", "The Nwiro Integration Kit runs this MCP server inside this editor; its tools include execute_python, which runs any editor Python.");
	case EHyperAIStudioMcpReason::NwiroNotHere:
		return LOCTEXT("WhyNwiroNotHere", "A Nwiro Integration Kit server, but that plugin is not enabled in this editor, so it cannot reach it.");
	case EHyperAIStudioMcpReason::Rider:
		return LOCTEXT("WhyRider", "JetBrains Rider's MCP server. Its Unreal tools (ue_execute_python, spawn_actor, ue_play) reach this editor through the RiderLink plugin.");
	case EHyperAIStudioMcpReason::RiderNoLink:
		return LOCTEXT("WhyRiderNoLink", "JetBrains Rider's MCP server. RiderLink is not enabled in this editor, so its Unreal tools cannot reach it.");
	case EHyperAIStudioMcpReason::RiderLinkOnly:
		return LOCTEXT("WhyRiderLinkOnly", "Rider's link to this editor. Agents reach it only through Rider's MCP server, which no agent config here lists.");
	case EHyperAIStudioMcpReason::UnrealName:
		return LOCTEXT("WhyUnrealName", "An Unreal server on this computer, so it can drive this editor directly.");
	case EHyperAIStudioMcpReason::UnrealRemote:
		return LOCTEXT("WhyUnrealRemote", "An Unreal server, but its address is on another computer, so it does not reach this editor.");
	case EHyperAIStudioMcpReason::EditorPort:
		return FText::Format(LOCTEXT("WhyEditorPort", "Its address, port {0}, is served by this editor itself."), FText::AsNumber(Finding.Port, &FNumberFormattingOptions::DefaultNoGrouping()));
	case EHyperAIStudioMcpReason::McpPlugin:
		return LOCTEXT("WhyMcpPlugin", "An enabled plugin that names itself an MCP server, running inside this editor.");
	case EHyperAIStudioMcpReason::PythonRemoteExecution:
		return LOCTEXT("WhyPythonRemote", "Python Remote Execution is on: any program on this computer, or on your network's multicast group, can run Python in this editor.");
	default:
		return LOCTEXT("WhyNotUnreal", "Not an Unreal server: its tools do not reach this editor, as far as HyperAI can tell.");
	}
}

FText HyperAIStudio::McpBypass::DescribeSource(const FHyperAIStudioMcpServerSource& Source)
{
	using namespace Private;
	switch (Source.Kind)
	{
	case EHyperAIStudioMcpSourceKind::Plugin:
		return Source.Address.IsEmpty()
			? FText::Format(LOCTEXT("SourcePlugin", "This editor: the {0} plugin ({1})"), FText::FromString(Source.EntryName), FText::FromString(Source.ShortFile))
			: FText::Format(LOCTEXT("SourcePluginPort", "This editor: the {0} plugin ({1}), listening on {2}"), FText::FromString(Source.EntryName),
				FText::FromString(Source.ShortFile), FText::FromString(Source.Address));
	case EHyperAIStudioMcpSourceKind::EditorSetting:
		return LOCTEXT("SourcePythonSetting", "This editor: Project Settings > Plugins > Python > Enable Remote Execution?");
	default:
		break;
	}
	FFormatNamedArguments Args;
	Args.Add(TEXT("Client"), FText::FromString(Source.Client));
	Args.Add(TEXT("Scope"), ScopeText(Source.Scope));
	Args.Add(TEXT("File"), FText::FromString(Source.ShortFile));
	Args.Add(TEXT("Entry"), FText::FromString(Source.EntryName));
	Args.Add(TEXT("Address"), FText::FromString(Source.Address));
	return Source.Address.IsEmpty()
		? FText::Format(LOCTEXT("SourceConfig", "{Client} ({Scope}): {File}, entry \"{Entry}\""), Args)
		: FText::Format(LOCTEXT("SourceConfigAddress", "{Client} ({Scope}): {File}, entry \"{Entry}\", {Address}"), Args);
}

FText HyperAIStudio::McpBypass::DescribeRemoval(const FHyperAIStudioMcpServerSource& Source, const bool bWrittenByHyperAI)
{
	using namespace Private;
	FFormatNamedArguments Args;
	Args.Add(TEXT("Entry"), FText::FromString(Source.EntryName));
	Args.Add(TEXT("File"), FText::FromString(Source.File.IsEmpty() ? Source.ShortFile : Source.File));
	Args.Add(TEXT("Container"), FText::FromString(Source.Container));
	FText Text;
	switch (Source.Kind)
	{
	case EHyperAIStudioMcpSourceKind::Plugin:
		return FText::Format(LOCTEXT("RemovePlugin", "Disable the {Entry} plugin in Edit > Plugins and restart the editor (it is in {File})."), Args);
	case EHyperAIStudioMcpSourceKind::EditorSetting:
		return LOCTEXT("RemovePythonSetting", "Untick Edit > Project Settings > Plugins > Python > Enable Remote Execution?");
	case EHyperAIStudioMcpSourceKind::ClaudeLocalEntry:
		Text = FText::Format(LOCTEXT("RemoveClaudeLocal", "In {File}, find this project under \"projects\" and delete \"{Entry}\" under its \"mcpServers\"; or run in the project folder: claude mcp remove {Entry} -s local"), Args);
		break;
	case EHyperAIStudioMcpSourceKind::TomlTable:
		Text = Source.Scope == TEXT("user")
			? FText::Format(LOCTEXT("RemoveCodexUser", "Delete the [mcp_servers.{Entry}] table, and any [mcp_servers.{Entry}.*] tables under it, in {File}; or set enabled = false in it; or run: codex mcp remove {Entry}"), Args)
			: FText::Format(LOCTEXT("RemoveCodexProject", "Delete the [mcp_servers.{Entry}] table, and any [mcp_servers.{Entry}.*] tables under it, in {File}; or set enabled = false in it."), Args);
		break;
	default:
		if (Source.Client == ClaudeCode && Source.Scope == TEXT("project"))
		{
			Text = FText::Format(LOCTEXT("RemoveClaudeProject", "Delete the \"{Entry}\" entry under \"mcpServers\" in {File} (HyperAI keeps entries it did not write, so it does not add it back); or run: claude mcp remove {Entry} -s project"), Args);
		}
		else if (Source.Client == ClaudeCode)
		{
			Text = FText::Format(LOCTEXT("RemoveClaudeUser", "Delete the \"{Entry}\" entry under the top-level \"mcpServers\" in {File}; or run: claude mcp remove {Entry} -s user"), Args);
		}
		else
		{
			Text = FText::Format(LOCTEXT("RemoveJson", "Delete the \"{Entry}\" entry under \"{Container}\" in {File}."), Args);
		}
		break;
	}
	if (bWrittenByHyperAI)
	{
		Text = FText::Format(LOCTEXT("RemoveWrittenByHyperAI", "{0} HyperAI writes this entry from Settings > MCP servers: turn it off or delete it there and press Regenerate Files, or it comes back."), Text);
	}
	return Text;
}

FText HyperAIStudio::McpBypass::DescribeConsequence()
{
	return LOCTEXT("Consequence", "Agents using it can edit assets and run Python in this editor with no HyperAI approval, no auto-approval, no undo grouping and no HyperAI checks.");
}

FText HyperAIStudio::McpBypass::BuildNotificationText(const TArray<FHyperAIStudioMcpFinding>& Findings)
{
	using namespace Private;
	TArray<FText> Items;
	for (const FHyperAIStudioMcpFinding& Finding : Findings)
	{
		if (Finding.ChangesEditor())
		{
			Items.Add(FText::Format(LOCTEXT("NotificationItem", "{0} ({1})"), FText::FromString(Finding.DisplayName), Places(Finding)));
		}
	}
	if (Items.IsEmpty())
	{
		return FText::GetEmpty();
	}
	return FText::Format(LOCTEXT("Notification",
		"Agents can change this editor without HyperAI's approval through {0}. There they can edit assets and run Python with no HyperAI approval, no auto-approval, no undo grouping and no HyperAI checks."),
		FText::Join(LOCTEXT("NotificationSeparator", "; "), Items));
}

FText HyperAIStudio::McpBypass::BuildAutoApprovalLine(const TArray<FHyperAIStudioMcpFinding>& Findings)
{
	const TArray<FString> Names = GetBypassNames(Findings);
	if (Names.IsEmpty())
	{
		return FText::GetEmpty();
	}
	return Names.Num() == 1
		? FText::Format(LOCTEXT("AutoApprovalOne", "Agents can also use {0}, which bypasses approvals entirely; auto-approval does not cover it."), Private::JoinNames(Names))
		: FText::Format(LOCTEXT("AutoApprovalMany", "Agents can also use {0}, which bypass approvals entirely; auto-approval does not cover them."), Private::JoinNames(Names));
}

FText HyperAIStudio::McpBypass::BuildActivityNote(const TArray<FHyperAIStudioMcpFinding>& Findings)
{
	const TArray<FString> Names = GetBypassNames(Findings);
	if (Names.IsEmpty())
	{
		return FText::GetEmpty();
	}
	return FText::Format(LOCTEXT("ActivityNote",
		"Not covered here: agents can also change this editor through {0}, with no HyperAI approval, auto-approval, undo grouping or checks."),
		Private::JoinNames(Names));
}

FString HyperAIStudio::McpBypass::BuildDetailsMarkdown(const TArray<FHyperAIStudioMcpFinding>& Findings, const TArray<FString>& Problems)
{
	using namespace Private;
	FString Markdown;
	auto AddFinding = [&Markdown](const FHyperAIStudioMcpFinding& Finding, const bool bRemoval)
	{
		Markdown += FString::Printf(TEXT("### %s\n\n%s\n\n"), *EscapeMarkdown(Finding.DisplayName), *DescribeWhy(Finding).ToString());
		for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
		{
			Markdown += FString::Printf(TEXT("- %s"), *EscapeMarkdown(DescribeSource(Source).ToString()));
			if (bRemoval)
			{
				Markdown += FString::Printf(TEXT("\n  - %s"), *EscapeMarkdown(FText::Format(LOCTEXT("ToRemove", "To remove: {0}"),
					DescribeRemoval(Source, Finding.bWrittenByHyperAI)).ToString()));
			}
			Markdown += TEXT("\n");
		}
		if (Finding.bSilenced)
		{
			Markdown += FString::Printf(TEXT("\n*%s*\n"), *LOCTEXT("DetailsSilenced", "You chose not to be warned about it in this project.").ToString());
		}
		Markdown += TEXT("\n");
	};
	const TArray<FHyperAIStudioMcpFinding> Bypass = FilterBypass(Findings, /*bUnsilencedOnly=*/false);
	if (Bypass.IsEmpty())
	{
		Markdown += FString::Printf(TEXT("%s\n\n"), *LOCTEXT("DetailsNoBypass", "No MCP server or editor plugin found here changes this editor outside HyperAI.").ToString());
	}
	else
	{
		Markdown += FString::Printf(TEXT("## %s\n\n%s\n\n"), *LOCTEXT("DetailsBypassHeading", "Can change this editor outside HyperAI").ToString(),
			*DescribeConsequence().ToString());
		for (const FHyperAIStudioMcpFinding& Finding : Bypass)
		{
			AddFinding(Finding, /*bRemoval=*/true);
		}
	}
	bool bOtherHeading = false;
	for (const FHyperAIStudioMcpFinding& Finding : Findings)
	{
		if (Finding.ChangesEditor())
		{
			continue;
		}
		if (!bOtherHeading)
		{
			bOtherHeading = true;
			Markdown += FString::Printf(TEXT("## %s\n\n%s\n\n"), *LOCTEXT("DetailsOtherHeading", "Other tools").ToString(),
				*LOCTEXT("DetailsOtherBody", "These do not reach this editor; they are listed so you know what else agents have.").ToString());
		}
		AddFinding(Finding, /*bRemoval=*/false);
	}
	if (!Problems.IsEmpty())
	{
		Markdown += FString::Printf(TEXT("## %s\n\n"), *LOCTEXT("DetailsProblemsHeading", "Not read").ToString());
		for (const FString& Problem : Problems)
		{
			Markdown += FString::Printf(TEXT("- %s\n"), *EscapeMarkdown(Problem));
		}
		Markdown += TEXT("\n");
	}
	Markdown += LOCTEXT("DetailsFooter",
		"HyperAI only reads these files and never changes them: remove an entry yourself if you do not want agents to have it. Names, addresses and file paths are shown; values from env, headers and arguments are never read out.").ToString();
	Markdown += TEXT("\n");
	return Markdown;
}

FString HyperAIStudio::McpBypass::BuildInstructionSection(const TArray<FHyperAIStudioMcpFinding>& Findings)
{
	TArray<FString> Names;
	for (const FString& Name : GetBypassNames(Findings))
	{
		Names.Add(Name.Contains(TEXT(" ")) ? Name : FString::Printf(TEXT("`%s`"), *Name));
	}
	if (Names.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("## Other MCP Servers\n\n- Use HyperAI's tools for changes to this editor. Other servers configured here (%s) bypass HyperAI's approvals, undo and checks; use them for edits only if the user asks."),
		*FString::Join(Names, TEXT(", ")));
}

FString HyperAIStudio::McpBypass::DescribeSourcePlain(const FHyperAIStudioMcpServerSource& Source)
{
	switch (Source.Kind)
	{
	case EHyperAIStudioMcpSourceKind::Plugin:
		return Source.Address.IsEmpty()
			? FString::Printf(TEXT("plugin %s (%s)"), *Source.EntryName, *Source.ShortFile)
			: FString::Printf(TEXT("plugin %s (%s, %s)"), *Source.EntryName, *Source.ShortFile, *Source.Address);
	case EHyperAIStudioMcpSourceKind::EditorSetting:
		return TEXT("setting Project Settings > Plugins > Python > Enable Remote Execution");
	default:
		return Source.Address.IsEmpty()
			? FString::Printf(TEXT("%s %s %s entry \"%s\""), *Source.Client, *Source.Scope, *Source.ShortFile, *Source.EntryName)
			: FString::Printf(TEXT("%s %s %s entry \"%s\" (%s)"), *Source.Client, *Source.Scope, *Source.ShortFile, *Source.EntryName, *Source.Address);
	}
}

FString HyperAIStudio::McpBypass::BuildLogLine(const TArray<FHyperAIStudioMcpFinding>& Findings)
{
	TArray<FString> Parts;
	for (const FHyperAIStudioMcpFinding& Finding : FilterBypass(Findings, /*bUnsilencedOnly=*/false))
	{
		TArray<FString> Places;
		for (const FHyperAIStudioMcpServerSource& Source : Finding.Sources)
		{
			Places.Add(DescribeSourcePlain(Source));
		}
		Parts.Add(FString::Printf(TEXT("%s [%s]%s"), *Finding.DisplayName, *FString::Join(Places, TEXT("; ")),
			Finding.bSilenced ? TEXT(" (not warned about in this project)") : TEXT("")));
	}
	if (Parts.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("Other MCP servers can change this editor outside HyperAI's approval gate (no HyperAI approval, auto-approval, undo grouping or checks): %s. Settings > Agents lists them and how to remove them."),
		*FString::Join(Parts, TEXT(", ")));
}

FString HyperAIStudio::McpBypass::ClassificationId(const EHyperAIStudioMcpServerClass Class)
{
	return Class == EHyperAIStudioMcpServerClass::ChangesEditor ? TEXT("changes_editor_outside_hyperai") : TEXT("other_tools");
}

#undef LOCTEXT_NAMESPACE
