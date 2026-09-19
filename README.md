# csgo-multi-appid

A **100% vibe coded** Valve server plugin that pins the CS:GO dedicated server's Steam appid, 
so the server behaves identically no matter what `csgo/steam.inf` says.

## Why

The legacy CS:GO build is distributed under appid **4465480**, but the depot was
re-published unchanged, so the shipped `csgo/steam.inf` still reads `AppID=730`.
That one line decides a surprising amount:

- which app the server logs on to Steam as (and therefore which GSLT is valid,
  and which clients' auth tickets validate instead of failing with
  `k_EBeginAuthSessionResultGameMismatch`)
- what `engine->GetAppID()` returns, which the workshop code compares against
  every item's `consumer_appid`
- the appid advertised in `A2S_INFO`, i.e. how the server is categorised in the
  server browser

So two otherwise identical installs behave differently depending on whether
someone remembered to edit `steam.inf` — and a `steamcmd ... validate` silently
reverts the edit. This plugin removes the file from the decision.

## What it does

On load, before the server logs on to Steam:

1. writes `steam_appid.txt` with the target appid, which is what `steamclient`
   reads at `SteamGameServer_Init`
2. exports `SteamAppId` into the environment for the same reason
3. overwrites the engine's parsed `steam.inf` value (`g_unSteamAppID`) so the
   advertised appid matches the one the server authenticates with, instead of
   the two disagreeing

Step 3 is a 4-byte write to a global the plugin locates by scanning the engine's
`steam_appid.txt` writer for the instruction that pushes it. The pattern must
match exactly once or the plugin leaves the global alone and says so. The
original value is restored on unload.

Nothing else about the engine is touched: no lobby behaviour, no netcode.

## host_workshop_map

Pinning the appid breaks the workshop, so the plugin fixes that too.

`DedicatedServerUGCFileInfo_t::BuildFromKV` in the game DLL rejects any item
whose `consumer_appid` is not `engine->GetAppID()`. Every CS:GO workshop item is
published under 730, because the legacy appid has no workshop of its own, so on
a server pinned to 4465480 every `host_workshop_map` and
`host_workshop_collection` fails with:

```
UGC file info consumer_appid 730 != engine 4465480
```

The check is only wrong in comparing against one appid when this build has two.
The plugin rewrites the half of it that calls `GetAppID`, so instead of

```asm
mov  ecx, dword_<engine>
mov  ebx, eax                 ; the item's consumer_appid
mov  edx, [ecx]
call [edx+19Ch]               ; IVEngineServer::GetAppID
cmp  ebx, eax
jz   ok
```

the site reads

```asm
mov  ebx, eax                 ; the item's consumer_appid
mov  eax, 4465480
cmp  ebx, eax
jz   +5                       ; falls through to the original cmp
mov  eax, 730
cmp  ebx, eax                 ; untouched
jz   ok                       ; untouched
```

An item published under either appid passes; anything else still fails, with the
engine's real appid in the warning, because the failure path re-reads it. The
comparison, the ban check and every other field check are untouched, and the
original bytes go back on unload.

The patched region is the same length as what it replaces (16 bytes on Windows,
21 on Linux, where the sequence also folds two calls' argument cleanup into one
`add esp, 10h` that the replacement keeps). The signature must match exactly once
or nothing is written.

## Cross-appid clients

Because the same build is distributed under two appids, clients launched under
the *other* one present auth tickets the server cannot judge: Steam answers
`k_EBeginAuthSessionResultGameMismatch`, and the engine rejects them. A game
server validates tickets only for the app it logged on as, and that is decided
by Valve's backend, so no amount of local configuration changes it.

Accepting those clients by treating the mismatch as success would be a bad
trade: the SteamID the engine uses comes from the client's own connect packet
and is only trustworthy once Steam has validated the ticket against it, so
anyone could claim any SteamID — breaking bans, admin-by-SteamID and stats.

Instead the plugin opens a **second Steam game server session inside the same
process**, logged on as the other appid, and validates the ticket there.
`BeginAuthSession` binds ticket to SteamID exactly as it would for a native
client, so a forged ID comes back invalid and the rejection stands.

`SteamGameServer_Init` infers its appid from `steam_appid.txt` and owns the one
session steam_api tracks — but the interface underneath takes the appid
explicitly:

```cpp
virtual bool InitGameServer( uint32 unIP, uint16 usGamePort, uint16 usQueryPort,
                             uint32 unFlags, AppId_t nGameAppId, const char *pchVersionString ) = 0;
```

so a second session is built straight from `ISteamClient`: its own pipe, its own
local user, its own game server interface. Its callbacks are pumped with
`Steam_BGetCallback` **on that pipe alone**, so the engine's own dispatch is
untouched. Nothing outlives the server: if it dies, the validator dies with it.

`InitGameServer` does bind a socket, so the session cannot be handed the real
server's ports:

```
CreateBoundSocket: ::bind couldn't find an open port between 27015 and 27015
```

It gets `k_unSteamGameServerQueryPortShared` for the query port, which tells
Steam not to stand up a query responder at all, and an ephemeral game port the
OS just confirmed was free. Neither is reachable or advertised — the session
never heartbeats and answers nothing.

There is nothing to configure and nothing to turn on: both appids are fixed
constants, and the validator validates whichever one the server is not pinned
to. It is not optional either, because a server that rejects every client from
the other appid is the problem this plugin exists to remove.

Only the mismatch case is diverted, and only on an affirmative pass. Invalid,
expired, duplicate and version mismatch tickets keep the engine's own answer.

**It does not block the server.** `BeginAuthSession` settles `InvalidTicket`,
`GameMismatch` and `ExpiredTicket` synchronously — that is ticket parsing and
the ticket-to-SteamID binding, which is the entire identity question, and it is
answered before the call returns. The `ValidateAuthTicketResponse_t` that
follows carries ban and licence status, not identity, so there is nothing worth
waiting for on the connect path (which runs on the main server thread).

The auth session is left open so that follow-up verdict can still arrive; it is
retired when it does, or after a minute if it never does.

### The follow-up verdict

When it arrives, it is not acted on here. It is handed to the engine's own
`CSteam3Server::OnValidateAuthTicketResponse`, the same function the engine's
Steam session calls, so a cross-appid client is dealt with by exactly the code
that deals with a native one: the ban check, the duplicate-SteamID check, the
`STEAM USERID validated` line, and `NetworkIDValidated` to both the plugin chain
and the game DLL. A VAC ban or missing licence therefore produces the engine's
own rejection, and an OK verdict makes the client count as fully authenticated
instead of leaving it in limbo.

The handler and the `CSteam3Server` singleton are located by signature. The
singleton candidate is only accepted if its `m_eServerMode` holds a valid
`EServerMode`; if either lookup fails the plugin says so and falls back to
logging the verdict, leaving everything else working.

Known limitations, worth understanding before relying on it:

- Anonymous game-server logon has to be permitted for the validated appid.
- Two game server sessions in one process is not a configuration Valve
  documents. `InitGameServer` taking an explicit appid is what makes it possible,
  and per-pipe dispatch (`Steam_BGetCallback` on our pipe only) is what keeps
  the two from interfering, but this is the part to watch first if something
  misbehaves.

## Usage

Drop both files in `csgo/addons/`:

```
csgo/addons/csgo-multi-appid.dll   (Windows)
csgo/addons/csgo-multi-appid.so    (Linux)
csgo/addons/csgo-multi-appid.vdf
```

Plugins in `addons/*.vdf` load automatically on a dedicated server. There are no
launch options: the server is pinned to **4465480**, and clients from **730**
are validated against a second session. Expected output on startup:

```
csgo-multi-appid: appid pinned to 4465480 (steam.inf said 730)
csgo-multi-appid: the Steam logon picks this up when the server activates; restart the server if it is already logged on
```

It only acts on a dedicated server — on a listen server it logs that it is doing
nothing, because writing `steam_appid.txt` into a client's game directory would
change what the client itself launches as.

## Building

The dedicated server is 32-bit on both platforms, so the plugin is too.

```sh
xmake f -p windows -a x86  -m release -y && xmake   # Windows
xmake f -p linux   -a i386 -m release -y && xmake   # Linux (needs a 32-bit toolchain)
```

On Linux, build inside the [Steam Runtime 3 (sniper) SDK](https://gitlab.steamos.cloud/steamrt/steamrt/-/blob/steamrt/sniper/README.md)
rather than whatever glibc the host happens to have:

```sh
docker run --rm -v "$PWD:/work" -w /work registry.gitlab.steamos.cloud/steamrt/sniper/sdk \
    bash -c 'xmake f -p linux -a i386 -m release -y && xmake'
```

The dedicated server itself runs on sniper, and its glibc (2.31) is old enough
that few hosts run anything older. A plain modern distro links in symbols the
server's glibc doesn't have — building on Ubuntu 24.04, for instance, produces
a `.so` that needs `GLIBC_2.34` (glibc 2.34 folded `libpthread` into `libc`,
and thread-safe-static guard variables pull that version in even when nothing
here touches threads) and fails to `dlopen` on almost everything currently
deployed. sniper also comes with i386 already enabled as a foreign
architecture and `-m32` already working, since CS:GO and the rest of Source 1
are 32-bit and Valve's own tooling has to cross-build for it too — no
`g++-multilib` install step needed.

No SDK checkout is required. The single engine interface the plugin implements
(`IServerPluginCallbacks`, version 004) is mirrored in `src/plugin.cpp`; it has
no virtual destructor, so the vtable layout is the same under MSVC and the
Itanium ABI, and only the declaration order matters. `tier0`'s `Msg`/`Warning`
are resolved at runtime rather than linked.

CI builds both platforms on every push — Linux inside the sniper SDK container,
same as above. Every push to `main` bumps the patch number of the latest `v*`
tag and publishes a release under the new tag; push a `v*` tag by hand to bump
the minor or major version. The version is baked into the plugin description
shown by `plugin_print`; any other build reports `dev`.
