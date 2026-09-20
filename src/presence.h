// Making the validator's clients visible in server queries.
//
// The engine tells Steam who is playing in exactly two places: BeginAuthSession
// on its own session, which is what registers a user at all, and
// BUpdateUserData from CGameServer::UpdateMasterServerPlayers, which returns
// false for anyone that session has no auth session for. The human count is
// never sent -- CSteam3Server::SendUpdatedServerDetails sends bots and max
// players and nothing else -- so Steam's idea of who is on the server is
// exactly the set of users it holds an auth session for.
//
// A cross-appid client is in no such set: the engine's BeginAuthSession really
// did fail, and authproxy only overrode the answer it returned. So Steam
// reports the server as empty of them, both in the master listing and in the
// A2S replies steamclient writes itself, which is every A2S reply once
// host_info_show / host_players_show are 2 and the engine hands the query
// straight to it.
//
// Each such client is therefore introduced to the engine's session separately,
// as an unauthenticated connection -- the documented way to list a player Steam
// did not vet -- and BUpdateUserData is rewritten to carry that SteamID instead
// of the real one. Count, name and score all come out right; only the identity
// Steam files them under is synthetic, which costs them nothing visible in a
// query and costs their friends the "join game" entry.
#pragma once

#include <cstdint>

namespace presence
{
// A client the validator vouched for. Nothing is created yet: Steam only needs
// to hear about a client the engine actually advertises, and the engine only
// advertises one that is in the game.
void Add( uint64_t steamID );

// The SteamID BUpdateUserData should carry for this client, or 0 to leave the
// call alone. Opens the Steam-side connection the first time it is asked for.
uint64_t Substitute( void *pEngineInterface, uint64_t steamID );

// The engine ending its own auth session for a client is the disconnect signal;
// it does that for every client that had a valid SteamID.
void Remove( void *pEngineInterface, uint64_t steamID );

// Retires everything this opened, so nothing stays listed on a server that no
// longer has it. A null interface just forgets, for when Steam is already gone.
void Clear( void *pEngineInterface );

// For when the hooks this depends on could not be installed. Listing a client
// without the signal that retires them again would leave the server advertising
// players who have left, so it does nothing at all instead.
void Disable();
}
