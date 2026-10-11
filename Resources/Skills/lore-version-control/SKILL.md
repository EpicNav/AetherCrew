---
name: lore-version-control
description: Work in an Unreal project kept in Epic's Lore version control (lore.org) through HyperAIStudio - read whether the project's Lore repository is local or on a server, what agents changed, and submit it once the user approves. Use when the user mentions Lore, version control, source control, committing, submitting, pushing, locks, or setting a project up for Lore, or when an edit is refused because a file is locked.
---

# Lore version control

Lore is Epic's open-source version control (https://lore.org, https://github.com/EpicGames/lore), pre-1.0. HyperAI
drives it through the `lore` command. Both tools below are on toolset
`HyperAIStudio.HyperAIStudioSourceControlToolset`, through `call_tool`.

## Read where the project stands

`hyper_source_control_status` with `{"bRefresh": true}`. Read `Lore`:

- `Mode` `local`: the repository lives in the project's `.lore` folder on this computer. No server, so no locks and
  no push. Agent edits are marked dirty once saved; a submit commits here.
- `Mode` `remote`: the repository is on `Server`. Agent edits lock each asset there first; a teammate's lock turns the
  edit away, naming them. A submit stages, commits, pushes and releases the locks.
- `Mode` `none`: the project is in no Lore workspace. `bCliFound` false means the `lore` command is not installed.
- `SignedInAs` lists who the `lore` command is signed in as (names only). `Next` says what the user can do next.

If an Unreal source control provider (Perforce, Git, Plastic) is set up, agent edits go through it, not Lore.

## Submit what agents changed

1. `hyper_source_control_submit` with `{"description": "...", "files": [], "bDryRun": true}`: show the user the list.
2. Only after the user says yes: the same with `"bDryRun": false` and a fresh `operationId`, then
   `hyper_operation_wait` with `bWaitThroughApproval` true. The user approves it in HyperAI Chat or Activity.
3. Submit only files agents changed this session; the user's own work is theirs to submit.

## Setting Lore up, and signing in, are the user's

- Setup changes the project folder (it adds `.lore` and `.loreignore`), so the user runs it in
  **HyperAI Studio > Settings > Version control**, which lists every step and asks before it runs. Never run
  `lore repository create`, `lore clone`, `lore repository delete` or `lore reset` yourself; point the user there.
- A server that needs sign-in (Epic's hosted Lore signs in with an Epic account) is signed in to by the user, in a
  terminal: `lore login <server>`. Never run `lore login` or `lore auth`, never pass `--token`, `--identity-token` or
  `--access-token`, and never ask the user for a token or password.
- Lore sets a repository's server when it is made: a local repository cannot be pointed at a server later. Say so
  if the user asks to "move to the server".

## When an edit is refused

- "locked by <name>": someone else holds the asset. Ask them to submit it, or work on another asset.
- "The Lore server did not answer": the server in `Lore.Server` is down or unreachable. Tell the user; do not retry
  in a loop, and do not turn off Check Out Before Agent Edits for them.
