/* Bomba: multiplayer sessions over Steam (see bomba_steamnet.h). */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>
#include <rthreads/rthreads.h>

#include "bomba_steamnet.h"
#include "../runloop.h"
#include "../verbosity.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define STEAM_APPID 1118310u

/* steam/steam.c: the rich presence friends join through. */
void steam_set_connect(const char *connect);

/* Flat Steamworks API (SDK 1.57) as steam_api64.dll exports it; the
 * structures as the SDK lays them out on Windows (8-byte packing). */
typedef uint64_t steam_id_t;
typedef uint64_t steam_call_t;

#pragma pack(push, 8)
typedef struct { int32_t user; int32_t id; uint8_t *param; int32_t size; } cb_msg_t;
typedef struct { steam_call_t call; int32_t id; uint32_t size; } call_done_t;
typedef struct { int32_t result; steam_id_t lobby; } lobby_created_t;
typedef struct { steam_id_t lobby; uint32_t perms; uint8_t locked; uint32_t response; } lobby_enter_t;
typedef struct { steam_id_t lobby; steam_id_t member; uint8_t success; } lobby_data_t;
typedef struct { steam_id_t lobby; steam_id_t changed; steam_id_t by; uint32_t state; } lobby_chat_t;
typedef struct { steam_id_t lobby; steam_id_t friend_id; } join_requested_t;
typedef struct { steam_id_t friend_id; char connect[256]; } rp_join_requested_t;
typedef struct
{
   int32_t type;
   int32_t size;
   union { steam_id_t steam_id; uint32_t reserved[32]; } u;
} net_identity_t;
typedef struct
{
   void *data;
   int32_t size;
   uint32_t conn;
   net_identity_t peer;
   /* (more the tunnel doesn't read) */
} net_msg_t;
#pragma pack(pop)

enum
{
   CB_GAME_LOBBY_JOIN_REQUESTED = 333,
   CB_RICH_PRESENCE_JOIN        = 337,
   CB_LOBBY_ENTER               = 504,
   CB_LOBBY_DATA_UPDATE         = 505,
   CB_LOBBY_CHAT_UPDATE         = 506,
   CB_LOBBY_CREATED             = 513,
   CB_CALL_COMPLETED            = 703,
   CB_NET_SESSION_REQUEST       = 1251
};

#define LOBBY_FRIENDS_ONLY       1
#define IDENTITY_STEAMID         16
#define RESULT_OK                1
#define ENTER_SUCCESS            1
#define CHAT_LEFT_MASK           (2 | 4 | 8 | 16)
/* Unreliable, no Nagle (the emulator's protocol does its own
 * reliability), and reopen a session that broke. */
#define SEND_FLAGS               (1 | 32)

static struct
{
   bool     (*Init)(void);
   void     (*Shutdown)(void);
   int32_t  (*GetHSteamPipe)(void);
   void     (*DispatchInit)(void);
   void     (*RunFrame)(int32_t);
   bool     (*NextCallback)(int32_t, cb_msg_t*);
   void     (*FreeLastCallback)(int32_t);
   bool     (*CallResult)(int32_t, steam_call_t, void*, int, int, bool*);

   void*    (*Matchmaking)(void);
   void*    (*Friends)(void);
   void*    (*User)(void);
   void*    (*Messages)(void);
   void*    (*Utils)(void);

   steam_call_t (*CreateLobby)(void*, int, int);
   steam_call_t (*JoinLobby)(void*, steam_id_t);
   void     (*LeaveLobby)(void*, steam_id_t);
   bool     (*SetLobbyData)(void*, steam_id_t, const char*, const char*);
   const char* (*GetLobbyData)(void*, steam_id_t, const char*);
   steam_id_t (*GetLobbyOwner)(void*, steam_id_t);
   int      (*GetNumLobbyMembers)(void*, steam_id_t);
   steam_id_t (*GetLobbyMemberByIndex)(void*, steam_id_t, int);

   const char* (*GetFriendPersonaName)(void*, steam_id_t);
   const char* (*GetPersonaName)(void*);

   steam_id_t (*GetSteamID)(void*);

   int      (*SendMessageToUser)(void*, const net_identity_t*, const void*, uint32_t, int, int);
   int      (*ReceiveMessages)(void*, int, net_msg_t**, int);
   bool     (*AcceptSession)(void*, const net_identity_t*);
   bool     (*CloseSession)(void*, const net_identity_t*);
   void     (*ReleaseMessage)(net_msg_t*);
   void     (*SetSteamID64)(net_identity_t*, steam_id_t);

   void     (*InitRelay)(void*);
} api;

#define MAX_PEERS   16
/* A guest the host hasn't heard from this long has gone. */
#define PEER_IDLE_USEC (30 * 1000000LL)
/* Steam not making a session: asked again after this long. */
#define SN_RETRY_USEC  (30 * 1000000LL)

typedef struct
{
   steam_id_t id;
   SOCKET s;
   int64_t last;
} peer_t;

static struct
{
   bool ok;
   int32_t pipe;
   void *mm, *friends, *msgs;
   steam_id_t me;

   enum bomba_steamnet_state state;
   steam_id_t lobby;
   steam_id_t host;
   /* When to open this player's own session again (while off). */
   int64_t host_at;
   char host_name[64];
   char my_name[64];

   /* Host: the game being started, and what the lobby says now. */
   char want_system[40], want_title[256];
   char pub_system[40],  pub_title[256];
   /* Guest: the host's game. */
   char host_system[40], host_title[256];

   /* Guest: the room's stand-in. */
   SOCKET local;
   unsigned local_port;

   sthread_t *thread;
   volatile bool run;
} sn;

static void sn_message(const char *msg)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 240, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
}

static int64_t sn_now(void)
{
   static LARGE_INTEGER freq;
   LARGE_INTEGER t;
   if (!freq.QuadPart)
      QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&t);
   return (int64_t)(t.QuadPart / (double)freq.QuadPart * 1000000.0);
}

static void sn_identity(net_identity_t *id, steam_id_t steam_id)
{
   memset(id, 0, sizeof(*id));
   api.SetSteamID64(id, steam_id);
}

static const char *sn_lobby_data(steam_id_t lobby, const char *key)
{
   const char *v = api.GetLobbyData(sn.mm, lobby, key);
   return v ? v : "";
}

/* ---------------------------------------------------------------------- */
/* Tunnel                                                                 */
/* ---------------------------------------------------------------------- */

static SOCKET sn_udp_socket(void)
{
   struct sockaddr_in addr;
   u_long nonblock = 1;
   BOOL report     = FALSE;
   DWORD bytes;
   SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

   if (s == INVALID_SOCKET)
      return s;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family      = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0)
   {
      closesocket(s);
      return INVALID_SOCKET;
   }
   ioctlsocket(s, FIONBIO, &nonblock);
   /* A port nobody listens on yet (the room still starting) must not
    * break the socket. */
   WSAIoctl(s, SIO_UDP_CONNRESET, &report, sizeof(report),
         NULL, 0, &bytes, NULL, NULL);
   return s;
}

static peer_t *sn_peer(peer_t *peers, size_t *count, steam_id_t id)
{
   struct sockaddr_in room;
   size_t i;
   SOCKET s;

   for (i = 0; i < *count; i++)
      if (peers[i].id == id)
         return &peers[i];
   if (*count >= MAX_PEERS || (s = sn_udp_socket()) == INVALID_SOCKET)
      return NULL;

   /* The room sees each guest as its own address. */
   memset(&room, 0, sizeof(room));
   room.sin_family      = AF_INET;
   room.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   room.sin_port        = htons(BOMBA_STEAMNET_ROOM_PORT);
   connect(s, (struct sockaddr*)&room, sizeof(room));

   peers[*count].id   = id;
   peers[*count].s    = s;
   peers[*count].last = sn_now();
   RARCH_LOG("[Bomba] Guest %llu reached the room.\n", (unsigned long long)id);
   return &peers[(*count)++];
}

static void sn_tunnel(void *data)
{
   static char buf[65536];
   peer_t peers[MAX_PEERS];
   size_t npeers = 0, i;
   bool host     = sn.state == BOMBA_STEAMNET_HOSTING;
   steam_id_t to = sn.host;
   net_identity_t host_id;
   struct sockaddr_in app;
   int app_len   = 0;
   bool heard_host = false;
   int last_res    = RESULT_OK;

   sn_identity(&host_id, to);

   while (sn.run)
   {
      net_msg_t *msgs[32];
      int n, k;
      fd_set set;
      struct timeval tv;
      int64_t now;

      /* Wait for the emulator's packets, a millisecond at most (Steam's
       * side has nothing to wait on). */
      FD_ZERO(&set);
      if (host)
         for (i = 0; i < npeers; i++)
            FD_SET(peers[i].s, &set);
      else
         FD_SET(sn.local, &set);
      tv.tv_sec  = 0;
      tv.tv_usec = 1000;
      if (set.fd_count)
         select(0, &set, NULL, NULL, &tv);
      else
         Sleep(1);

      /* Steam -> emulator */
      n = api.ReceiveMessages(sn.msgs, 0, msgs, 32);
      for (k = 0; k < n; k++)
      {
         net_msg_t *m    = msgs[k];
         steam_id_t from = m->peer.type == IDENTITY_STEAMID
            ? m->peer.u.steam_id : 0;
         if (host && from)
         {
            peer_t *p = sn_peer(peers, &npeers, from);
            if (p)
            {
               send(p->s, (const char*)m->data, m->size, 0);
               p->last = sn_now();
            }
         }
         else if (!host && from == to)
         {
            if (!heard_host)
            {
               heard_host = true;
               RARCH_LOG("[Bomba] First data from the host%s.\n",
                     app_len ? "" : " (the emulator hasn't sent yet)");
            }
            if (app_len)
               sendto(sn.local, (const char*)m->data, m->size, 0,
                     (struct sockaddr*)&app, app_len);
         }
         else
            RARCH_WARN("[Bomba] Data from %llu, not in this session; ignored.\n",
                  (unsigned long long)from);
         api.ReleaseMessage(m);
      }

      /* Emulator -> Steam */
      if (host)
      {
         for (i = 0; i < npeers; i++)
         {
            net_identity_t id;
            int r;
            sn_identity(&id, peers[i].id);
            while ((r = recv(peers[i].s, buf, sizeof(buf), 0)) > 0)
               api.SendMessageToUser(sn.msgs, &id, buf, (uint32_t)r,
                     SEND_FLAGS, 0);
         }

         now = sn_now();
         for (i = 0; i < npeers; )
         {
            if (now - peers[i].last > PEER_IDLE_USEC)
            {
               RARCH_LOG("[Bomba] Guest %llu went quiet; dropped.\n",
                     (unsigned long long)peers[i].id);
               closesocket(peers[i].s);
               peers[i] = peers[--npeers];
            }
            else
               i++;
         }
      }
      else
      {
         struct sockaddr_in from;
         int from_len = sizeof(from), r;
         while ((r = recvfrom(sn.local, buf, sizeof(buf), 0,
               (struct sockaddr*)&from, &from_len)) > 0)
         {
            int res;
            /* Replies go to whoever last sent (the emulator). */
            if (!app_len)
               RARCH_LOG("[Bomba] The emulator reached the tunnel; sending to the host.\n");
            app     = from;
            app_len = from_len;
            res = api.SendMessageToUser(sn.msgs, &host_id, buf, (uint32_t)r,
                  SEND_FLAGS, 0);
            if (res != RESULT_OK && res != last_res)
               RARCH_WARN("[Bomba] Sending to the host failed (%d).\n", res);
            last_res = res;
            from_len = sizeof(from);
         }
      }
   }

   for (i = 0; i < npeers; i++)
   {
      net_identity_t id;
      closesocket(peers[i].s);
      sn_identity(&id, peers[i].id);
      api.CloseSession(sn.msgs, &id);
   }
   if (!host)
      api.CloseSession(sn.msgs, &host_id);
}

static void sn_tunnel_start(void)
{
   sn.run    = true;
   sn.thread = sthread_create(sn_tunnel, NULL);
}

static void sn_tunnel_stop(void)
{
   if (sn.thread)
   {
      sn.run = false;
      sthread_join(sn.thread);
      sn.thread = NULL;
   }
   if (sn.local != INVALID_SOCKET)
   {
      closesocket(sn.local);
      sn.local = INVALID_SOCKET;
   }
   sn.local_port = 0;
}

/* ---------------------------------------------------------------------- */
/* Session                                                                */
/* ---------------------------------------------------------------------- */

/* Friends join through the session's lobby: Steam's "Invite to Game" and
 * "Join Game" hand them this, as a callback or as RetroArch's command
 * line (see launch.ps1). */
static void sn_advertise(void)
{
   char connect[64];
   snprintf(connect, sizeof(connect), "+connect_lobby %llu",
         (unsigned long long)sn.lobby);
   steam_set_connect(connect);
}

static void sn_end(void)
{
   sn_tunnel_stop();
   if (sn.lobby)
      api.LeaveLobby(sn.mm, sn.lobby);
   if (sn.state == BOMBA_STEAMNET_HOSTING || sn.state == BOMBA_STEAMNET_JOINED)
      steam_set_connect("");
   sn.state          = BOMBA_STEAMNET_OFF;
   sn.lobby          = 0;
   sn.host           = 0;
   sn.host_name[0]   = '\0';
   sn.pub_system[0]  = '\0';
   sn.pub_title[0]   = '\0';
   sn.host_system[0] = '\0';
   sn.host_title[0]  = '\0';
}

/* This player's own session (see bomba_steamnet_poll). */
static void sn_host(void)
{
   sn_end();
   sn.state = BOMBA_STEAMNET_CREATING;
   api.CreateLobby(sn.mm, LOBBY_FRIENDS_ONLY, BOMBA_STEAMNET_MAX_PLAYERS);
}

/* A friend's session, from an invite or Join Game. */
static void sn_join(steam_id_t lobby)
{
   if (!lobby || lobby == sn.lobby)
      return;
   sn_end();
   sn.state = BOMBA_STEAMNET_JOINING;
   sn.lobby = lobby;
   api.JoinLobby(sn.mm, lobby);
   RARCH_LOG("[Bomba] Joining Steam lobby %llu.\n",
         (unsigned long long)lobby);
}

/* Back to one's own session soon (not at once: Steam refusing to make
 * one shouldn't be asked again every frame). */
static void sn_back_home(int64_t delay)
{
   sn_end();
   sn.host_at = sn_now() + delay;
}

static void sn_on_lobby_created(const lobby_created_t *c)
{
   if (sn.state != BOMBA_STEAMNET_CREATING)
   {
      /* Ended while it was being made. (Not the second copy, as callback
       * and call result, of the one hosted now: leaving that ends it for
       * Steam while this side still thinks it's hosting.) */
      if (c->result == RESULT_OK && c->lobby != sn.lobby)
         api.LeaveLobby(sn.mm, c->lobby);
      return;
   }
   if (c->result != RESULT_OK)
   {
      RARCH_ERR("[Bomba] Steam lobby not created (%d).\n", c->result);
      sn_back_home(SN_RETRY_USEC);
      return;
   }

   sn.lobby = c->lobby;
   sn.host  = sn.me;
   api.SetLobbyData(sn.mm, sn.lobby, "bomba", "1");
   api.SetLobbyData(sn.mm, sn.lobby, "name", sn.my_name);
   sn.state = BOMBA_STEAMNET_HOSTING;
   sn_advertise();
   sn_tunnel_start();
   RARCH_LOG("[Bomba] Hosting Steam lobby %llu.\n",
         (unsigned long long)sn.lobby);
}

static void sn_read_host_game(void)
{
   strlcpy(sn.host_system, sn_lobby_data(sn.lobby, "system"),
         sizeof(sn.host_system));
   strlcpy(sn.host_title, sn_lobby_data(sn.lobby, "title"),
         sizeof(sn.host_title));
}

static void sn_on_lobby_enter(const lobby_enter_t *e)
{
   const char *name;

   /* (Also the host entering its own lobby, and the second copy of a
    * join, as callback and call result.) */
   if (sn.state != BOMBA_STEAMNET_JOINING || e->lobby != sn.lobby)
      return;
   if (e->response != ENTER_SUCCESS)
   {
      RARCH_ERR("[Bomba] Steam lobby %llu not joined (response %u).\n",
            (unsigned long long)e->lobby, e->response);
      sn_message("Multiplayer: couldn't join the session.");
      sn.lobby = 0;
      sn_back_home(0);
      return;
   }
   if (!string_is_equal(sn_lobby_data(sn.lobby, "bomba"), "1"))
   {
      sn_message("Multiplayer: that isn't a Bomba session.");
      sn_back_home(0);
      return;
   }

   sn.host = api.GetLobbyOwner(sn.mm, sn.lobby);
   name    = api.GetFriendPersonaName(sn.friends, sn.host);
   strlcpy(sn.host_name, name ? name : "", sizeof(sn.host_name));
   sn_read_host_game();

   if ((sn.local = sn_udp_socket()) == INVALID_SOCKET)
   {
      sn_message("Multiplayer: couldn't open the session's local port.");
      sn_back_home(0);
      return;
   }
   {
      struct sockaddr_in addr;
      int len = sizeof(addr);
      getsockname(sn.local, (struct sockaddr*)&addr, &len);
      sn.local_port = ntohs(addr.sin_port);
   }
   sn.state = BOMBA_STEAMNET_JOINED;
   sn_advertise();
   sn_tunnel_start();
   RARCH_LOG("[Bomba] Joined Steam lobby %llu (host %llu), local port %u.\n",
         (unsigned long long)sn.lobby, (unsigned long long)sn.host,
         sn.local_port);
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "Multiplayer: joined %s's session.",
            sn.host_name);
      sn_message(msg);
   }
}

static bool sn_is_member(steam_id_t id)
{
   int i, n = api.GetNumLobbyMembers(sn.mm, sn.lobby);
   for (i = 0; i < n; i++)
      if (api.GetLobbyMemberByIndex(sn.mm, sn.lobby, i) == id)
         return true;
   return false;
}

static void sn_on_lobby_data(const lobby_data_t *d)
{
   if (d->lobby == sn.lobby && sn.state == BOMBA_STEAMNET_JOINED)
      sn_read_host_game();
}

static void sn_on_lobby_chat(const lobby_chat_t *c)
{
   if (     c->lobby == sn.lobby && sn.state == BOMBA_STEAMNET_JOINED
         && c->changed == sn.host && (c->state & CHAT_LEFT_MASK))
   {
      sn_back_home(0);
      sn_message("Multiplayer: the host left the session.");
   }
}

static void sn_dispatch(int id, const void *param)
{
   switch (id)
   {
      case CB_LOBBY_CREATED:
         sn_on_lobby_created((const lobby_created_t*)param);
         break;
      case CB_LOBBY_ENTER:
         sn_on_lobby_enter((const lobby_enter_t*)param);
         break;
      case CB_LOBBY_DATA_UPDATE:
         sn_on_lobby_data((const lobby_data_t*)param);
         break;
      case CB_LOBBY_CHAT_UPDATE:
         sn_on_lobby_chat((const lobby_chat_t*)param);
         break;
      case CB_GAME_LOBBY_JOIN_REQUESTED:
         /* An invite accepted, or Join Game in Steam. */
         sn_join(((const join_requested_t*)param)->lobby);
         break;
      case CB_RICH_PRESENCE_JOIN:
      {
         /* The same through rich presence: "+connect_lobby <id>". */
         const char *c = ((const rp_join_requested_t*)param)->connect;
         const char *id = strstr(c, "+connect_lobby ");
         if (id)
            sn_join(strtoull(id + 15, NULL, 10));
         break;
      }
      case CB_NET_SESSION_REQUEST:
      {
         const net_identity_t *who = (const net_identity_t*)param;
         /* Only players in this session reach the room. */
         if (     sn.state == BOMBA_STEAMNET_HOSTING
               && who->type == IDENTITY_STEAMID
               && sn_is_member(who->u.steam_id))
         {
            RARCH_LOG("[Bomba] Connection from %llu accepted (%d).\n",
                  (unsigned long long)who->u.steam_id,
                  api.AcceptSession(sn.msgs, who));
         }
         else
            RARCH_WARN("[Bomba] Connection from %llu refused (state %d, "
                  "type %d).\n", (unsigned long long)who->u.steam_id,
                  sn.state, who->type);
         break;
      }
      default:
         break;
   }
}

/* ---------------------------------------------------------------------- */
/* Lifetime                                                               */
/* ---------------------------------------------------------------------- */

void bomba_steamnet_init(void)
{
   char path[MAX_PATH], *slash;
   HMODULE dll;
   WSADATA wsa;
   char lobby_env[32];
   const char *name;

   sn.local = INVALID_SOCKET;

   /* Only when Steam started RetroArch (as for its overlay). */
   if (!GetEnvironmentVariableA("SteamAppId", path, sizeof(path)))
      return;

   GetModuleFileNameA(NULL, path, sizeof(path));
   if (!(slash = strrchr(path, '\\')))
      return;
   strlcpy(slash + 1, "mist\\steam_api64.dll",
         sizeof(path) - (size_t)(slash + 1 - path));
   if (!(dll = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)))
   {
      RARCH_ERR("[Bomba] Can't load \"%s\" (%lu).\n", path, GetLastError());
      return;
   }

#define SN_SYM(field, name) \
   { \
      FARPROC proc = GetProcAddress(dll, name); \
      if (!proc) \
      { \
         RARCH_ERR("[Bomba] steam_api64.dll lacks %s.\n", name); \
         return; \
      } \
      memcpy(&api.field, &proc, sizeof(proc)); \
   }
   SN_SYM(Init,                  "SteamAPI_Init");
   SN_SYM(Shutdown,              "SteamAPI_Shutdown");
   SN_SYM(GetHSteamPipe,         "SteamAPI_GetHSteamPipe");
   SN_SYM(DispatchInit,          "SteamAPI_ManualDispatch_Init");
   SN_SYM(RunFrame,              "SteamAPI_ManualDispatch_RunFrame");
   SN_SYM(NextCallback,          "SteamAPI_ManualDispatch_GetNextCallback");
   SN_SYM(FreeLastCallback,      "SteamAPI_ManualDispatch_FreeLastCallback");
   SN_SYM(CallResult,            "SteamAPI_ManualDispatch_GetAPICallResult");
   SN_SYM(Matchmaking,           "SteamAPI_SteamMatchmaking_v009");
   SN_SYM(Friends,               "SteamAPI_SteamFriends_v017");
   SN_SYM(User,                  "SteamAPI_SteamUser_v021");
   SN_SYM(Messages,              "SteamAPI_SteamNetworkingMessages_SteamAPI_v002");
   SN_SYM(Utils,                 "SteamAPI_SteamNetworkingUtils_SteamAPI_v004");
   SN_SYM(CreateLobby,           "SteamAPI_ISteamMatchmaking_CreateLobby");
   SN_SYM(JoinLobby,             "SteamAPI_ISteamMatchmaking_JoinLobby");
   SN_SYM(LeaveLobby,            "SteamAPI_ISteamMatchmaking_LeaveLobby");
   SN_SYM(SetLobbyData,          "SteamAPI_ISteamMatchmaking_SetLobbyData");
   SN_SYM(GetLobbyData,          "SteamAPI_ISteamMatchmaking_GetLobbyData");
   SN_SYM(GetLobbyOwner,         "SteamAPI_ISteamMatchmaking_GetLobbyOwner");
   SN_SYM(GetNumLobbyMembers,    "SteamAPI_ISteamMatchmaking_GetNumLobbyMembers");
   SN_SYM(GetLobbyMemberByIndex, "SteamAPI_ISteamMatchmaking_GetLobbyMemberByIndex");
   SN_SYM(GetFriendPersonaName,  "SteamAPI_ISteamFriends_GetFriendPersonaName");
   SN_SYM(GetPersonaName,        "SteamAPI_ISteamFriends_GetPersonaName");
   SN_SYM(GetSteamID,            "SteamAPI_ISteamUser_GetSteamID");
   SN_SYM(SendMessageToUser,     "SteamAPI_ISteamNetworkingMessages_SendMessageToUser");
   SN_SYM(ReceiveMessages,       "SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel");
   SN_SYM(AcceptSession,         "SteamAPI_ISteamNetworkingMessages_AcceptSessionWithUser");
   SN_SYM(CloseSession,          "SteamAPI_ISteamNetworkingMessages_CloseSessionWithUser");
   SN_SYM(ReleaseMessage,        "SteamAPI_SteamNetworkingMessage_t_Release");
   SN_SYM(SetSteamID64,          "SteamAPI_SteamNetworkingIdentity_SetSteamID64");
   SN_SYM(InitRelay,             "SteamAPI_ISteamNetworkingUtils_InitRelayNetworkAccess");
#undef SN_SYM

   /* Lobbies are per app: started some other way than as RetroArch (a
    * non-Steam shortcut), sessions would be invisible to friends. */
   {
      char appid[32];
      GetEnvironmentVariableA("SteamAppId", appid, sizeof(appid));
      if (strtoul(appid, NULL, 10) != STEAM_APPID)
      {
         RARCH_WARN("[Bomba] Started by Steam as app %s; using RetroArch's "
               "(%u) for multiplayer.\n", appid, STEAM_APPID);
         snprintf(appid, sizeof(appid), "%u", STEAM_APPID);
         SetEnvironmentVariableA("SteamAppId", appid);
      }
   }
   if (!api.Init())
   {
      RARCH_ERR("[Bomba] SteamAPI_Init failed; no Steam multiplayer.\n");
      return;
   }
   api.DispatchInit();
   sn.pipe    = api.GetHSteamPipe();
   sn.mm      = api.Matchmaking();
   sn.friends = api.Friends();
   sn.msgs    = api.Messages();
   sn.me      = api.GetSteamID(api.User());
   if (!sn.mm || !sn.friends || !sn.msgs)
   {
      api.Shutdown();
      return;
   }
   /* Relays ready before the first session needs them. */
   api.InitRelay(api.Utils());
   WSAStartup(MAKEWORD(2, 2), &wsa);

   name = api.GetPersonaName(sn.friends);
   strlcpy(sn.my_name, name ? name : "", sizeof(sn.my_name));
   sn.ok = true;
   RARCH_LOG("[Bomba] Steam multiplayer ready.\n");

   /* Started by Steam to join a session (Join Game / an invite while
    * RetroArch was closed): launch.ps1 passes the lobby on. Otherwise
    * this player's own session opens on the first poll. */
   if (GetEnvironmentVariableA("BOMBA_CONNECT_LOBBY", lobby_env,
         sizeof(lobby_env)))
   {
      SetEnvironmentVariableA("BOMBA_CONNECT_LOBBY", NULL);
      sn_join(strtoull(lobby_env, NULL, 10));
   }
}

/* Host: the lobby names the game running now, the one started from the
 * grid, or none. */
static void sn_publish_game(void)
{
   bool running = (runloop_state_get_ptr()->current_core.flags
         & RETRO_CORE_FLAG_GAME_LOADED) ? true : false;
   const char *system = running ? sn.want_system : "";
   const char *title  = running ? sn.want_title  : "";

   if (     string_is_equal(system, sn.pub_system)
         && string_is_equal(title, sn.pub_title))
      return;
   strlcpy(sn.pub_system, system, sizeof(sn.pub_system));
   strlcpy(sn.pub_title,  title,  sizeof(sn.pub_title));
   api.SetLobbyData(sn.mm, sn.lobby, "system", system);
   api.SetLobbyData(sn.mm, sn.lobby, "title",  title);
}

void bomba_steamnet_poll(void)
{
   cb_msg_t msg;

   if (!sn.ok)
      return;

   api.RunFrame(sn.pipe);
   while (api.NextCallback(sn.pipe, &msg))
   {
      if (msg.id == CB_CALL_COMPLETED)
      {
         const call_done_t *done = (const call_done_t*)msg.param;
         void *result            = malloc(done->size ? done->size : 1);
         bool failed             = true;
         if (     result
               && api.CallResult(sn.pipe, done->call, result,
                  (int)done->size, done->id, &failed)
               && !failed)
            sn_dispatch(done->id, result);
         else if (done->id == CB_LOBBY_CREATED
               && sn.state == BOMBA_STEAMNET_CREATING)
            sn_back_home(SN_RETRY_USEC);
         else if (done->id == CB_LOBBY_ENTER
               && sn.state == BOMBA_STEAMNET_JOINING)
         {
            RARCH_ERR("[Bomba] Steam lobby %llu not joined (call failed).\n",
                  (unsigned long long)sn.lobby);
            sn_message("Multiplayer: couldn't join the session.");
            sn.lobby = 0;
            sn_back_home(0);
         }
         free(result);
      }
      else
         sn_dispatch(msg.id, msg.param);
      api.FreeLastCallback(sn.pipe);
   }

   if (sn.state == BOMBA_STEAMNET_HOSTING)
      sn_publish_game();
   /* In no session: this player's own, so friends can be invited. */
   else if (sn.state == BOMBA_STEAMNET_OFF && sn_now() >= sn.host_at)
      sn_host();
}

void bomba_steamnet_deinit(void)
{
   if (!sn.ok)
      return;
   sn_end();
   api.Shutdown();
   WSACleanup();
   sn.ok = false;
}

/* ---------------------------------------------------------------------- */
/* State                                                                  */
/* ---------------------------------------------------------------------- */

enum bomba_steamnet_state bomba_steamnet_state(void) { return sn.state; }
unsigned bomba_steamnet_local_port(void)     { return sn.local_port; }
const char *bomba_steamnet_my_name(void)     { return sn.my_name; }
const char *bomba_steamnet_host_system(void) { return sn.host_system; }
const char *bomba_steamnet_host_title(void)  { return sn.host_title; }

void bomba_steamnet_set_game(const char *system, const char *title)
{
   strlcpy(sn.want_system, system ? system : "", sizeof(sn.want_system));
   strlcpy(sn.want_title,  title  ? title  : "", sizeof(sn.want_title));
}
