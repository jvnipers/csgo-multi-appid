#include "presence.h"
#include "appid.h"
#include "platform.h"
#include "steam_min.h"

namespace presence
{
namespace
{

// One per player slot. The engine will not have more clients than this, and a
// client that never reaches the game never takes one.
const int kMaxEntries = 64;

struct Entry
{
	uint64_t	steamID;		// what the client really is
	uint64_t	steamIDSteam;	// what Steam knows it as, 0 until it is asked for
	bool		bUsed;
};

Entry	s_Entries[ kMaxEntries ];
bool	s_bBroken;

Entry *Find( uint64_t steamID )
{
	for ( auto &e : s_Entries )
	{
		if ( e.bUsed && e.steamID == steamID )
			return &e;
	}
	return nullptr;
}

void Retire( Entry &e, steam::ISteamGameServer *pServer )
{
	if ( e.steamIDSteam && pServer )
		pServer->SendUserDisconnect( e.steamIDSteam );

	e = { 0, 0, false };
}

} // namespace

void Add( uint64_t steamID )
{
	if ( Find( steamID ) )
		return;

	for ( auto &e : s_Entries )
	{
		if ( !e.bUsed )
		{
			e = { steamID, 0, true };
			return;
		}
	}

	plat::Warn( "csgo-multi-appid: presence table full\n" );
}

uint64_t Substitute( void *pEngineInterface, uint64_t steamID )
{
	if ( s_bBroken || !pEngineInterface )
		return 0;

	Entry *pEntry = Find( steamID );
	if ( !pEntry )
		return 0;

	if ( !pEntry->steamIDSteam )
	{
		steam::ISteamGameServer *pServer = (steam::ISteamGameServer *)pEngineInterface;
		const uint64_t created = pServer->CreateUnauthenticatedUserConnection().m_ullValue;
		if ( !created )
		{
			// It is a deprecated call, so a Steam that no longer honours it is
			// a real possibility rather than a defensive one. Say so once and
			// then stay out of the way: everything else keeps working, and the
			// server is no worse off than it was before any of this existed.
			s_bBroken = true;
			plat::Warn( "csgo-multi-appid: Steam would not open a connection for %llu,"
						" so clients from appid %u stay invisible in server queries\n",
						(unsigned long long)steamID, appid::Other() );
			return 0;
		}

		pEntry->steamIDSteam = created;
		plat::Log( "csgo-multi-appid: %llu is listed to Steam as %llu\n",
				   (unsigned long long)steamID, (unsigned long long)created );
	}

	return pEntry->steamIDSteam;
}

void Remove( void *pEngineInterface, uint64_t steamID )
{
	if ( Entry *pEntry = Find( steamID ) )
		Retire( *pEntry, (steam::ISteamGameServer *)pEngineInterface );
}

void Disable()
{
	s_bBroken = true;
}

void Clear( void *pEngineInterface )
{
	for ( auto &e : s_Entries )
	{
		if ( e.bUsed )
			Retire( e, (steam::ISteamGameServer *)pEngineInterface );
	}
}

} // namespace presence
