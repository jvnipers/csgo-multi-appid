#include "presence.h"
#include "appid.h"
#include "platform.h"
#include "steam_min.h"

#include <ctime>

namespace presence
{
namespace
{

// One per player slot. The engine will not have more clients than this.
const int kMaxEntries = 64;

// How long an entry may go unlisted before it is taken to be a connect that
// never became a player. The engine advertises its clients every two seconds
// (MASTER_SERVER_UPDATE_INTERVAL), so this is thirty cycles of slack.
const int kUnlistedSeconds = 60;

struct Entry
{
	uint64_t	steamID;		// what the client really is
	uint64_t	steamIDSteam;	// what Steam knows it as, 0 until it is asked for
	time_t		tAdded;
	int			nConnects;		// zero means the slot is free
};

Entry	s_Entries[ kMaxEntries ];
bool	s_bBroken;

Entry *Find( uint64_t steamID )
{
	for ( auto &e : s_Entries )
	{
		if ( e.nConnects && e.steamID == steamID )
			return &e;
	}
	return nullptr;
}

void Retire( Entry &e, steam::ISteamGameServer *pServer )
{
	if ( e.steamIDSteam && pServer )
		pServer->SendUserDisconnect( e.steamIDSteam );

	e = { 0, 0, 0, 0 };
}

} // namespace

void Add( uint64_t steamID )
{
	// An account can be here twice over: a client that dropped without the
	// engine noticing yet is still connected as far as the engine is concerned
	// when the replacement arrives, and the engine ends one auth session per
	// connection. Counting them keeps the first EndAuthSession from taking the
	// survivor's listing away with it.
	if ( Entry *pEntry = Find( steamID ) )
	{
		++pEntry->nConnects;
		return;
	}

	for ( auto &e : s_Entries )
	{
		if ( !e.nConnects )
		{
			e = { steamID, 0, time( nullptr ), 1 };
			return;
		}
	}

	plat::Warn( "csgo-multi-appid: presence table full\n" );
}

uint64_t Substitute( void *pEngineInterface, uint64_t steamID )
{
	Entry *pEntry = Find( steamID );
	if ( !pEntry )
		return 0;

	if ( !pEntry->steamIDSteam )
	{
		// Only opening a new one is off limits once this has failed. A client
		// already listed keeps its listing, or it would sit in the server
		// browser with whatever name and score it had at the time.
		if ( s_bBroken || !pEngineInterface )
			return 0;

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
	Entry *pEntry = Find( steamID );
	if ( !pEntry )
		return;

	if ( --pEntry->nConnects > 0 )
		return;

	Retire( *pEntry, (steam::ISteamGameServer *)pEngineInterface );
}

void Expire()
{
	// A connect can be turned away after the engine has already asked Steam to
	// validate it: the ban filter and ClientConnectionValidatePreNetChan both
	// reject at that point, and neither disconnects the client, so no
	// EndAuthSession ever follows and the entry would sit here for the life of
	// the process. Somebody banned who keeps retrying would fill the table.
	//
	// Nothing was opened for those, since the engine never advertised them, so
	// an entry that is still unlisted this long after arriving is one of them.
	const time_t now = time( nullptr );
	for ( auto &e : s_Entries )
	{
		if ( e.nConnects && !e.steamIDSteam && now - e.tAdded > kUnlistedSeconds )
			e = { 0, 0, 0, 0 };
	}
}

void Disable( void *pEngineInterface )
{
	s_bBroken = true;
	Clear( pEngineInterface );
}

void Clear( void *pEngineInterface )
{
	for ( auto &e : s_Entries )
	{
		if ( e.nConnects )
			Retire( e, (steam::ISteamGameServer *)pEngineInterface );
	}
}

} // namespace presence
