#include "steam_min.h"
#include "platform.h"

#include <cstring>

#if defined( _WIN32 )
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace steam
{
namespace
{

typedef void *( *CreateInterfaceFn )( const char *pName, int *pReturnCode );

#if defined( _WIN32 )
const char *const kSteamApiModule = "steam_api.dll";
const char *const kSteamClientModule = "steamclient.dll";
#else
const char *const kSteamApiModule = "libsteam_api.so";
const char *const kSteamClientModule = "steamclient.so";
#endif

void *s_hSteamApi;
void *s_hSteamClient;

#if defined( _WIN32 )
const char *const kEngineModule = "engine.dll";
#else
const char *const kEngineModule = "engine.so";
#endif

// Only a version whose BeginAuthSession slot has been read out of a binary
// belongs here, and only for as long as a build that asks for it is actually
// supported. A version that is not in this table gets no hook, because the
// alternative is writing over whichever method happens to live at a guessed
// index. Adding a row means checking the slot first -- the layout mirrored in
// steam_min.h has to match it too.
struct GameServerVersion_t
{
	const char	*pszVersion;
	int			nBeginAuthSessionSlot;
	int			nLogOffSlot;
	int			nEndAuthSessionSlot;
	int			nBUpdateUserDataSlot;
};

const GameServerVersion_t kGameServerVersions[] = {
	{ "SteamGameServer014", 26, 7, 27, 41 },
};

const GameServerVersion_t *s_pVersion;
bool s_bVersionResolved;

// Whole-word search: the terminator is part of the needle so that a longer
// version string cannot match on its prefix.
bool ImageContains( const unsigned char *pStart, size_t nSize, const char *pszNeedle )
{
	const size_t nLen = strlen( pszNeedle ) + 1;
	if ( nLen > nSize )
		return false;

	const unsigned char nFirst = (unsigned char)pszNeedle[ 0 ];
	for ( size_t i = 0; i + nLen <= nSize; ++i )
	{
		if ( pStart[ i ] == nFirst && memcmp( pStart + i, pszNeedle, nLen ) == 0 )
			return true;
	}
	return false;
}

void ResolveVersion()
{
	if ( s_bVersionResolved )
		return;
	s_bVersionResolved = true;

	const unsigned char *pImage = nullptr;
	size_t nSize = 0;
	if ( !plat::ModuleImageRange( kEngineModule, &pImage, &nSize ) )
		return;

	for ( const GameServerVersion_t &v : kGameServerVersions )
	{
		if ( !ImageContains( pImage, nSize, v.pszVersion ) )
			continue;

		s_pVersion = &v;
		plat::Log( "csgo-multi-appid: the engine talks %s (BeginAuthSession is slot %d)\n",
				   v.pszVersion, v.nBeginAuthSessionSlot );
		return;
	}

	plat::Warn( "csgo-multi-appid: %s asks for an ISteamGameServer version this build does not know;"
				" cross-appid clients cannot be validated\n", kEngineModule );
}

void *OpenLoaded( const char *pszName )
{
#if defined( _WIN32 )
	return (void *)GetModuleHandleA( pszName );
#else
	return dlopen( pszName, RTLD_NOW | RTLD_NOLOAD );
#endif
}

void *Symbol( void *hModule, const char *pszName )
{
	if ( !hModule )
		return nullptr;
#if defined( _WIN32 )
	return (void *)GetProcAddress( (HMODULE)hModule, pszName );
#else
	return dlsym( hModule, pszName );
#endif
}

// Steam_BGetCallback and friends live in steamclient on Windows and in both
// modules on Linux, so try each.
void *DispatchSymbol( const char *pszName )
{
	if ( void *p = Symbol( s_hSteamApi, pszName ) )
		return p;
	return Symbol( s_hSteamClient, pszName );
}

} // namespace

bool ModulesReady()
{
	Api probe;
	return probe.Load() && probe.BGetCallback && probe.FreeLastCallback;
}

const char *GameServerVersion()
{
	ResolveVersion();
	return s_pVersion ? s_pVersion->pszVersion : nullptr;
}

int BeginAuthSessionSlot()
{
	ResolveVersion();
	return s_pVersion ? s_pVersion->nBeginAuthSessionSlot : -1;
}

int LogOffSlot()
{
	ResolveVersion();
	return s_pVersion ? s_pVersion->nLogOffSlot : -1;
}

int EndAuthSessionSlot()
{
	ResolveVersion();
	return s_pVersion ? s_pVersion->nEndAuthSessionSlot : -1;
}

int BUpdateUserDataSlot()
{
	ResolveVersion();
	return s_pVersion ? s_pVersion->nBUpdateUserDataSlot : -1;
}

bool Api::Load()
{
	if ( !s_hSteamApi )
		s_hSteamApi = OpenLoaded( kSteamApiModule );
	if ( !s_hSteamClient )
		s_hSteamClient = OpenLoaded( kSteamClientModule );

	if ( !s_hSteamApi && !s_hSteamClient )
		return false; // Steam is not up in this process yet

	GetHSteamUser = (HSteamUser( * )())Symbol( s_hSteamApi, "SteamGameServer_GetHSteamUser" );
	GetHSteamPipe = (HSteamPipe( * )())Symbol( s_hSteamApi, "SteamGameServer_GetHSteamPipe" );
	BGetCallback = (bool ( * )( HSteamPipe, CallbackMsg_t * ))DispatchSymbol( "Steam_BGetCallback" );
	FreeLastCallback = (void ( * )( HSteamPipe ))DispatchSymbol( "Steam_FreeLastCallback" );

	return GetHSteamUser && GetHSteamPipe;
}

ISteamClient *Api::Client()
{
	if ( !s_hSteamClient )
		s_hSteamClient = OpenLoaded( kSteamClientModule );
	if ( !s_hSteamClient )
		return nullptr;

	CreateInterfaceFn pfnCreate = (CreateInterfaceFn)Symbol( s_hSteamClient, "CreateInterface" );
	if ( !pfnCreate )
		return nullptr;

	// Asking for the exact version keeps this vtable matching the mirror above.
	ISteamClient *pClient = (ISteamClient *)pfnCreate( kSteamClientVersion, nullptr );
	if ( !pClient )
		plat::Warn( "steam: %s not available\n", kSteamClientVersion );

	return pClient;
}

ISteamGameServer *Api::GameServer( HSteamUser hUser, HSteamPipe hPipe )
{
	if ( !hUser || !hPipe )
		return nullptr;

	const char *pszVersion = GameServerVersion();
	if ( !pszVersion )
		return nullptr;

	ISteamClient *pClient = Client();
	return pClient ? pClient->GetISteamGameServer( hUser, hPipe, pszVersion ) : nullptr;
}

ISteamGameServer *Api::EngineGameServer()
{
	if ( !GetHSteamUser || !GetHSteamPipe )
		return nullptr;
	return GameServer( GetHSteamUser(), GetHSteamPipe() );
}

} // namespace steam
