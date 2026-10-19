/* Bomba: multiplayer sessions over Steam (see bomba_steamnet.h). */

/* (The tunnel waits on more sockets than Windows' 64.) */
#define FD_SETSIZE 256
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
#include "../command.h"
#include "../configuration.h"
#include "../core_info.h"
#include "../paths.h"
#include "../runahead.h"
#include "../runloop.h"
#include "../tasks/tasks_internal.h"
#include "../verbosity.h"
#ifdef HAVE_NETWORKING
#include "../network/netplay/netplay.h"
#endif

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#define STEAM_APPID 1118310u

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
   uint64_t game;
   uint32_t ip;
   uint16_t port, query_port;
   steam_id_t lobby;
} friend_game_t;
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

/* Joined only through an invite (no Join Game in friends lists). */
#define LOBBY_PRIVATE            0
#define IDENTITY_STEAMID         16
#define RESULT_OK                1
#define ENTER_SUCCESS            1
#define CHAT_LEFT_MASK           (2 | 4 | 8 | 16)
#define RESULT_LIMIT_EXCEEDED    25
/* Unreliable, no Nagle (the emulator's protocol does its own
 * reliability), and reopen a session that broke. */
#define SEND_FLAGS               (1 | 32)
/* Netplay's connections (TCP) as a stream of messages: reliable and in
 * order, no Nagle, reopening. */
#define SEND_STREAM_FLAGS        (8 | 1 | 32)

/* Channels: a launcher core's room, netplay's connections, connections to
 * the host's shared ports. Each netplay or shared message is a byte saying
 * what, the connection's number (the guest counts them: what's left of one
 * before isn't taken for the next), then the data; a shared connection's
 * OPEN has the port (2 bytes, high first). */
#define CH_ROOM                  0
#define CH_NETPLAY               1
#define CH_SHARED                2
#define STREAM_OPEN              'O'
#define STREAM_DATA              'D'
#define STREAM_CLOSE             'C'
#define STREAM_HEADER            2

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
   int      (*GetLargeFriendAvatar)(void*, steam_id_t);
   int      (*GetMediumFriendAvatar)(void*, steam_id_t);
   int      (*GetFriendCount)(void*, int);
   steam_id_t (*GetFriendByIndex)(void*, int, int);
   int      (*GetFriendPersonaState)(void*, steam_id_t);
   bool     (*GetFriendGamePlayed)(void*, steam_id_t, friend_game_t*);
   bool     (*InviteUserToLobby)(void*, steam_id_t, steam_id_t);

   void*    (*SteamUtils)(void);
   bool     (*GetImageSize)(void*, int, uint32_t*, uint32_t*);
   bool     (*GetImageRGBA)(void*, int, uint8_t*, int);

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
/* Connections to shared ports at once (all guests), and ports shared. */
#define MAX_SHARED  64
#define MAX_SHARED_PORTS 8
/* A guest the host hasn't heard from this long has gone. */
#define PEER_IDLE_USEC (30 * 1000000LL)
/* Steam not making a session: asked again after this long. */
#define SN_RETRY_USEC  (30 * 1000000LL)
/* How often the session's players are looked at. */
#define SN_PLAYERS_USEC (250000LL)
/* Host: how long a guest's netplay connection waits for netplay to start
 * (the guest's game may start first), trying this often. */
#define STREAM_CONNECT_USEC (20 * 1000000LL)
#define STREAM_RETRY_USEC   (250000LL)
/* Host: a guest's connection outlives the guest in the lobby this long at
 * most (it may arrive before the lobby shows the guest). */
#define STREAM_GRACE_USEC   (5 * 1000000LL)

typedef struct
{
   steam_id_t id;
   SOCKET s;
   int64_t last;
} peer_t;

/* A netplay or shared connection through the tunnel. */
typedef struct
{
   steam_id_t id;      /* the other end: host, a guest; guest, the host */
   SOCKET s;           /* INVALID_SOCKET: none (host: maybe trying again) */
   uint8_t num;
   uint8_t ch;         /* CH_NETPLAY, CH_SHARED */
   uint16_t port;      /* host: where it's passed on to */
   bool live;          /* host: wanted, connected or not */
   bool connecting;    /* host: connect() under way */
   int64_t opened, retry_at;
   /* From Steam, not taken by netplay yet. */
   char *out;
   size_t out_len, out_cap;
   /* For Steam, which had no room for it yet: a whole message. */
   char *held;
   uint32_t held_len;
} stream_t;

static struct
{
   bool ok;
   int32_t pipe;
   void *mm, *friends, *msgs, *utils;
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

   /* Guest: the room's stand-in, and netplay's (listening). */
   SOCKET local;
   unsigned local_port;
   SOCKET listen;
   unsigned netplay_port;

   /* TCP ports the host shares (bomba_shared_tcp_ports; guest: from the
    * lobby), and guest: the stand-ins listening on the same ports here. */
   uint16_t shared_ports[MAX_SHARED_PORTS];
   unsigned nshared_ports;
   SOCKET shared_listen[MAX_SHARED_PORTS];

   /* Host: netplay for the running game (sn_host_netplay): which game it
    * was tried for, and whether it runs for the session. */
   char np_content[MAX_PATH];
   bool np_tried, np_started;

   /* Host: who is in the lobby, for the tunnel (under members_cs). */
   CRITICAL_SECTION members_cs;
   steam_id_t members[BOMBA_STEAMNET_MAX_PLAYERS];
   unsigned nmembers;

   /* The others in the session, as last looked at (players_known: once
    * looked at, so who is there on arrival isn't news). avatar_of: whose
    * picture each shows. */
   bomba_steamnet_player_t players[BOMBA_STEAMNET_MAX_PLAYERS];
   steam_id_t avatar_of[BOMBA_STEAMNET_MAX_PLAYERS];
   unsigned nplayers;
   bool players_known;
   int64_t players_at;

   /* Friends who can be invited (online), and those invited to this
    * session lately (until when they show as such). */
   bomba_steamnet_friend_t friends_list[BOMBA_STEAMNET_MAX_FRIENDS];
   unsigned nfriends;
   steam_id_t invited[BOMBA_STEAMNET_MAX_FRIENDS];
   int64_t invited_until[BOMBA_STEAMNET_MAX_FRIENDS];
   unsigned ninvited;

   /* Host: players kicked who haven't left the lobby yet (read by the
    * tunnel too). */
   volatile steam_id_t kicked[BOMBA_STEAMNET_MAX_PLAYERS];
   volatile unsigned nkicked;

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

static bool sn_is_kicked(steam_id_t id)
{
   unsigned i;
   for (i = 0; i < sn.nkicked; i++)
      if (sn.kicked[i] == id)
         return true;
   return false;
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

/* Netplay's connections ------------------------------------------------- */

static SOCKET sn_tcp_socket(void)
{
   u_long nonblock = 1;
   BOOL nodelay    = TRUE;
   SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (s == INVALID_SOCKET)
      return s;
   ioctlsocket(s, FIONBIO, &nonblock);
   setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay,
         sizeof(nodelay));
   return s;
}

/* On 127.0.0.1:port (0: any free one). */
static SOCKET sn_tcp_listener(uint16_t port)
{
   struct sockaddr_in addr;
   SOCKET s = sn_tcp_socket();
   if (s == INVALID_SOCKET)
      return s;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family      = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port        = htons(port);
   if (     bind(s, (struct sockaddr*)&addr, sizeof(addr)) != 0
         || listen(s, port ? SOMAXCONN : 1) != 0)
   {
      closesocket(s);
      return INVALID_SOCKET;
   }
   return s;
}

static bool sn_is_member_now(steam_id_t id)
{
   unsigned i;
   bool found = false;
   EnterCriticalSection(&sn.members_cs);
   for (i = 0; i < sn.nmembers && !found; i++)
      found = sn.members[i] == id;
   LeaveCriticalSection(&sn.members_cs);
   return found;
}

/* A message of the stream's: what, its number, data. RESULT_LIMIT_EXCEEDED:
 * Steam has no room for it now. */
static int sn_stream_send(stream_t *st, char what, char *msg, uint32_t len)
{
   net_identity_t id;
   msg[0] = what;
   msg[1] = (char)st->num;
   sn_identity(&id, st->id);
   return api.SendMessageToUser(sn.msgs, &id, msg, STREAM_HEADER + len,
         SEND_STREAM_FLAGS, st->ch);
}

static void sn_stream_tell(stream_t *st, char what)
{
   char msg[STREAM_HEADER];
   sn_stream_send(st, what, msg, 0);
}

/* Ends the connection (tell: the other end hears of it). */
static void sn_stream_close(stream_t *st, bool tell)
{
   if (st->s != INVALID_SOCKET)
      closesocket(st->s);
   if (tell && st->live)
      sn_stream_tell(st, STREAM_CLOSE);
   st->s          = INVALID_SOCKET;
   st->live       = false;
   st->connecting = false;
   st->out_len    = 0;
   st->held_len   = 0;
}

static void sn_stream_free(stream_t *st)
{
   free(st->out);
   free(st->held);
   st->out     = st->held = NULL;
   st->out_cap = 0;
}

static void sn_stream_take(stream_t *st, const char *data, size_t len)
{
   if (st->out_len + len > st->out_cap)
   {
      size_t cap = st->out_cap ? st->out_cap : 65536;
      char *out;
      while (cap < st->out_len + len)
         cap *= 2;
      if (!(out = (char*)realloc(st->out, cap)))
         return;
      st->out     = out;
      st->out_cap = cap;
   }
   memcpy(st->out + st->out_len, data, len);
   st->out_len += len;
}

/* Host: (again) to the host's netplay, or the shared port. */
static void sn_stream_connect(stream_t *st)
{
   struct sockaddr_in addr;

   if ((st->s = sn_tcp_socket()) == INVALID_SOCKET)
      return;
   memset(&addr, 0, sizeof(addr));
   addr.sin_family      = AF_INET;
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   addr.sin_port        = htons(st->port);
   st->connecting = true;
   if (     connect(st->s, (struct sockaddr*)&addr, sizeof(addr)) == 0)
      st->connecting = false;
   else if (WSAGetLastError() != WSAEWOULDBLOCK)
   {
      closesocket(st->s);
      st->s = INVALID_SOCKET;
   }
}

/* Host: a connect() under way, done yet? */
static void sn_stream_check_connect(stream_t *st, int64_t now)
{
   fd_set w, e;
   struct timeval tv = { 0, 0 };

   if (st->s != INVALID_SOCKET)
   {
      FD_ZERO(&w);
      FD_ZERO(&e);
      FD_SET(st->s, &w);
      FD_SET(st->s, &e);
      if (select(0, NULL, &w, &e, &tv) <= 0)
         return;
      if (FD_ISSET(st->s, &w) && !FD_ISSET(st->s, &e))
      {
         st->connecting = false;
         RARCH_LOG("[Bomba] Guest %llu reached port %u.\n",
               (unsigned long long)st->id, st->port);
         return;
      }
      /* (Netplay, or what's on the shared port, not started yet.) */
      closesocket(st->s);
      st->s        = INVALID_SOCKET;
      st->retry_at = now + STREAM_RETRY_USEC;
   }
   if (now - st->opened > STREAM_CONNECT_USEC)
   {
      RARCH_WARN("[Bomba] Nothing on port %u for guest %llu.\n", st->port,
            (unsigned long long)st->id);
      sn_stream_close(st, true);
   }
   else if (now >= st->retry_at)
      sn_stream_connect(st);
}

/* Hands netplay what came from Steam, as much as it takes; false: the
 * connection broke. */
static bool sn_stream_flush(stream_t *st)
{
   while (st->out_len)
   {
      int r = send(st->s, st->out, (int)st->out_len, 0);
      if (r <= 0)
         return WSAGetLastError() == WSAEWOULDBLOCK;
      memmove(st->out, st->out + r, st->out_len - r);
      st->out_len -= r;
   }
   return true;
}

/* Moves what there is between netplay and Steam. */
static void sn_stream_pump(stream_t *st, char *buf, size_t size)
{
   int r;

   if (!st->live || st->connecting || st->s == INVALID_SOCKET)
      return;

   if (!sn_stream_flush(st))
   {
      sn_stream_close(st, true);
      return;
   }

   /* Steam takes in order: nothing new until the held one went. */
   if (st->held_len)
   {
      if (sn_stream_send(st, STREAM_DATA, st->held,
            st->held_len - STREAM_HEADER) == RESULT_LIMIT_EXCEEDED)
         return;
      st->held_len = 0;
   }

   while ((r = recv(st->s, buf + STREAM_HEADER,
         (int)(size - STREAM_HEADER), 0)) > 0)
   {
      if (sn_stream_send(st, STREAM_DATA, buf, (uint32_t)r)
            == RESULT_LIMIT_EXCEEDED)
      {
         if (!st->held)
            st->held = (char*)malloc(size);
         if (st->held)
         {
            memcpy(st->held, buf, STREAM_HEADER + r);
            st->held_len = STREAM_HEADER + r;
         }
         return;
      }
   }
   if (r == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
      sn_stream_close(st, true);
}

/* Host: the guest's connection, made if new. */
static stream_t *sn_stream_of(stream_t *streams, size_t *count, steam_id_t id)
{
   size_t i;
   for (i = 0; i < *count; i++)
      if (streams[i].id == id)
         return &streams[i];
   if (*count >= MAX_PEERS)
      return NULL;
   memset(&streams[*count], 0, sizeof(streams[0]));
   streams[*count].id   = id;
   streams[*count].s    = INVALID_SOCKET;
   streams[*count].ch   = CH_NETPLAY;
   streams[*count].port = BOMBA_STEAMNET_NETPLAY_PORT;
   return &streams[(*count)++];
}

/* A netplay message from the other end. */
static void sn_stream_message(stream_t *streams, size_t *count, bool host,
      steam_id_t from, const char *data, int size)
{
   stream_t *st;
   char what;
   uint8_t num;

   if (size < STREAM_HEADER)
      return;
   what = data[0];
   num  = (uint8_t)data[1];

   if (!host)
   {
      st = &streams[0];
      if (from != st->id || num != st->num || !st->live)
         return;
      if (what == STREAM_DATA)
         sn_stream_take(st, data + STREAM_HEADER, size - STREAM_HEADER);
      else if (what == STREAM_CLOSE)
      {
         sn_stream_flush(st);
         sn_stream_close(st, false);
      }
      return;
   }

   if (!(st = sn_stream_of(streams, count, from)))
      return;
   if (what == STREAM_OPEN)
   {
      sn_stream_close(st, false);
      st->num      = num;
      st->live     = true;
      st->opened   = sn_now();
      st->retry_at = 0;
      sn_stream_connect(st);
      RARCH_LOG("[Bomba] Guest %llu opened a netplay connection.\n",
            (unsigned long long)from);
   }
   else if (num != st->num || !st->live)
      ; /* (an earlier connection's) */
   else if (what == STREAM_DATA)
      sn_stream_take(st, data + STREAM_HEADER, size - STREAM_HEADER);
   else if (what == STREAM_CLOSE)
   {
      if (!st->connecting && st->s != INVALID_SOCKET)
         sn_stream_flush(st);
      sn_stream_close(st, false);
   }
}

/* Shared ports' connections ---------------------------------------------- */

static bool sn_is_shared_port(uint16_t port)
{
   unsigned i;
   for (i = 0; i < sn.nshared_ports; i++)
      if (sn.shared_ports[i] == port)
         return true;
   return false;
}

/* The live connection with id and num; with new, else a free one made. */
static stream_t *sn_shared_of(stream_t *shared, size_t *count, steam_id_t id,
      uint8_t num, bool new_one)
{
   size_t i;
   for (i = 0; i < *count; i++)
      if (shared[i].live && shared[i].id == id && shared[i].num == num)
         return &shared[i];
   if (!new_one)
      return NULL;
   for (i = 0; i < *count; i++)
      if (!shared[i].live)
         break;
   if (i == *count)
   {
      if (*count >= MAX_SHARED)
         return NULL;
      memset(&shared[i], 0, sizeof(shared[0]));
      shared[i].s = INVALID_SOCKET;
      (*count)++;
   }
   shared[i].id = id;
   shared[i].ch = CH_SHARED;
   return &shared[i];
}

/* A shared connection's message from the other end. */
static void sn_shared_message(stream_t *shared, size_t *count, bool host,
      steam_id_t from, const char *data, int size)
{
   stream_t *st;
   char what;
   uint8_t num;

   if (size < STREAM_HEADER)
      return;
   what = data[0];
   num  = (uint8_t)data[1];

   if (host && what == STREAM_OPEN)
   {
      uint16_t port;
      if (size < STREAM_HEADER + 2)
         return;
      port = (uint16_t)(((uint8_t)data[2] << 8) | (uint8_t)data[3]);
      if ((st = sn_shared_of(shared, count, from, num, false)))
         sn_stream_close(st, false);
      if (!(st = sn_shared_of(shared, count, from, num, true)))
         return;
      st->num      = num;
      st->port     = port;
      st->live     = true;
      st->opened   = sn_now();
      st->retry_at = 0;
      /* Only to what the host shares. */
      if (!sn_is_shared_port(port))
      {
         RARCH_WARN("[Bomba] Guest %llu asked for port %u, not shared.\n",
               (unsigned long long)from, port);
         sn_stream_close(st, true);
         return;
      }
      sn_stream_connect(st);
      RARCH_LOG("[Bomba] Guest %llu opened a connection to port %u.\n",
            (unsigned long long)from, port);
      return;
   }
   if (!host && from != sn.host)
      return;
   if (!(st = sn_shared_of(shared, count, from, num, false)))
      return; /* (an earlier connection's) */
   if (what == STREAM_DATA)
      sn_stream_take(st, data + STREAM_HEADER, size - STREAM_HEADER);
   else if (what == STREAM_CLOSE)
   {
      if (!st->connecting && st->s != INVALID_SOCKET)
         sn_stream_flush(st);
      sn_stream_close(st, false);
   }
}

/* Guest: a connection to a shared port here, passed on to the host's. */
static void sn_shared_accept(stream_t *shared, size_t *count, unsigned which,
      uint8_t *num)
{
   char msg[STREAM_HEADER + 2];
   u_long nonblock = 1;
   BOOL nodelay    = TRUE;
   uint16_t port   = sn.shared_ports[which];
   stream_t *st;
   SOCKET s        = accept(sn.shared_listen[which], NULL, NULL);

   if (s == INVALID_SOCKET)
      return;
   (*num)++;
   if (!(st = sn_shared_of(shared, count, sn.host, *num, true)))
   {
      closesocket(s);
      return;
   }
   ioctlsocket(s, FIONBIO, &nonblock);
   setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay,
         sizeof(nodelay));
   st->num  = *num;
   st->port = port;
   st->s    = s;
   st->live = true;
   msg[STREAM_HEADER]     = (char)(port >> 8);
   msg[STREAM_HEADER + 1] = (char)(port & 0xff);
   sn_stream_send(st, STREAM_OPEN, msg, 2);
   RARCH_LOG("[Bomba] Connection to the host's port %u.\n", port);
}

static void sn_tunnel(void *data)
{
   static char buf[65536];
   peer_t peers[MAX_PEERS];
   /* Netplay's connections: host, one per guest; guest, [0] (the host). */
   stream_t streams[MAX_PEERS];
   /* Connections to shared ports (guest: numbered by shared_num). */
   static stream_t shared[MAX_SHARED];
   size_t nshared = 0;
   uint8_t shared_num = 0;
   size_t npeers = 0, nstreams = 0, i;
   bool host     = sn.state == BOMBA_STEAMNET_HOSTING;
   steam_id_t to = sn.host;
   net_identity_t host_id;
   struct sockaddr_in app;
   int app_len   = 0;
   bool heard_host = false;
   int last_res    = RESULT_OK;

   sn_identity(&host_id, to);
   if (!host)
   {
      memset(&streams[0], 0, sizeof(streams[0]));
      streams[0].id   = to;
      streams[0].s    = INVALID_SOCKET;
      streams[0].ch   = CH_NETPLAY;
      streams[0].port = BOMBA_STEAMNET_NETPLAY_PORT;
      nstreams        = 1;
   }

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
      {
         FD_SET(sn.local, &set);
         if (sn.listen != INVALID_SOCKET)
            FD_SET(sn.listen, &set);
         for (i = 0; i < sn.nshared_ports; i++)
            if (sn.shared_listen[i] != INVALID_SOCKET)
               FD_SET(sn.shared_listen[i], &set);
      }
      for (i = 0; i < nstreams; i++)
         if (     streams[i].live && !streams[i].connecting
               && streams[i].s != INVALID_SOCKET)
            FD_SET(streams[i].s, &set);
      for (i = 0; i < nshared; i++)
         if (     shared[i].live && !shared[i].connecting
               && shared[i].s != INVALID_SOCKET)
            FD_SET(shared[i].s, &set);
      tv.tv_sec  = 0;
      tv.tv_usec = 1000;
      if (set.fd_count)
         select(0, &set, NULL, NULL, &tv);
      else
         Sleep(1);

      /* Steam -> netplay */
      n = api.ReceiveMessages(sn.msgs, CH_NETPLAY, msgs, 32);
      for (k = 0; k < n; k++)
      {
         net_msg_t *m    = msgs[k];
         steam_id_t from = m->peer.type == IDENTITY_STEAMID
            ? m->peer.u.steam_id : 0;
         if (from && !(host && sn_is_kicked(from)))
            sn_stream_message(streams, &nstreams, host, from,
                  (const char*)m->data, m->size);
         api.ReleaseMessage(m);
      }

      /* Netplay's connections: new (guest), started, pumped, ended. */
      if (!host && sn.listen != INVALID_SOCKET)
      {
         SOCKET s = accept(sn.listen, NULL, NULL);
         if (s != INVALID_SOCKET)
         {
            u_long nonblock = 1;
            BOOL nodelay    = TRUE;
            stream_t *st    = &streams[0];
            ioctlsocket(s, FIONBIO, &nonblock);
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay,
                  sizeof(nodelay));
            sn_stream_close(st, true);
            st->num++;
            st->s    = s;
            st->live = true;
            sn_stream_tell(st, STREAM_OPEN);
            RARCH_LOG("[Bomba] Netplay connecting to the host.\n");
         }
      }
      now = sn_now();
      for (i = 0; i < nstreams; i++)
      {
         stream_t *st = &streams[i];
         if (!st->live)
            continue;
         /* Kicked, or gone from the lobby. */
         if (host && (sn_is_kicked(st->id)
               || (now - st->opened > STREAM_GRACE_USEC
                  && !sn_is_member_now(st->id))))
         {
            RARCH_LOG("[Bomba] Guest %llu's netplay connection ended.\n",
                  (unsigned long long)st->id);
            sn_stream_close(st, true);
            continue;
         }
         if (st->connecting)
            sn_stream_check_connect(st, now);
         sn_stream_pump(st, buf, sizeof(buf));
      }

      /* Shared ports: the same. */
      n = api.ReceiveMessages(sn.msgs, CH_SHARED, msgs, 32);
      for (k = 0; k < n; k++)
      {
         net_msg_t *m    = msgs[k];
         steam_id_t from = m->peer.type == IDENTITY_STEAMID
            ? m->peer.u.steam_id : 0;
         if (from && !(host && sn_is_kicked(from)))
            sn_shared_message(shared, &nshared, host, from,
                  (const char*)m->data, m->size);
         api.ReleaseMessage(m);
      }
      if (!host)
         for (i = 0; i < sn.nshared_ports; i++)
            if (sn.shared_listen[i] != INVALID_SOCKET)
               sn_shared_accept(shared, &nshared, (unsigned)i, &shared_num);
      now = sn_now();
      for (i = 0; i < nshared; i++)
      {
         stream_t *st = &shared[i];
         if (!st->live)
            continue;
         if (host && (sn_is_kicked(st->id)
               || (now - st->opened > STREAM_GRACE_USEC
                  && !sn_is_member_now(st->id))))
         {
            sn_stream_close(st, true);
            continue;
         }
         if (st->connecting)
            sn_stream_check_connect(st, now);
         sn_stream_pump(st, buf, sizeof(buf));
      }

      /* Steam -> emulator */
      n = api.ReceiveMessages(sn.msgs, CH_ROOM, msgs, 32);
      for (k = 0; k < n; k++)
      {
         net_msg_t *m    = msgs[k];
         steam_id_t from = m->peer.type == IDENTITY_STEAMID
            ? m->peer.u.steam_id : 0;
         if (host && from && sn_is_kicked(from))
            ; /* sent out of the session */
         else if (host && from)
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

   for (i = 0; i < nstreams; i++)
   {
      sn_stream_close(&streams[i], true);
      sn_stream_free(&streams[i]);
   }
   for (i = 0; i < nshared; i++)
   {
      sn_stream_close(&shared[i], true);
      sn_stream_free(&shared[i]);
   }
   for (i = 0; host && i < nshared; i++)
   {
      net_identity_t id;
      sn_identity(&id, shared[i].id);
      api.CloseSession(sn.msgs, &id);
   }
   for (i = 0; i < npeers; i++)
   {
      net_identity_t id;
      closesocket(peers[i].s);
      sn_identity(&id, peers[i].id);
      api.CloseSession(sn.msgs, &id);
   }
   /* (A guest's netplay connection but no room.) */
   for (i = 0; host && i < nstreams; i++)
   {
      net_identity_t id;
      sn_identity(&id, streams[i].id);
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
   unsigned i;
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
   if (sn.listen != INVALID_SOCKET)
   {
      closesocket(sn.listen);
      sn.listen = INVALID_SOCKET;
   }
   for (i = 0; i < sn.nshared_ports; i++)
      if (sn.shared_listen[i] != INVALID_SOCKET)
         closesocket(sn.shared_listen[i]);
   sn.nshared_ports = 0;
   sn.local_port    = 0;
   sn.netplay_port  = 0;
}

/* Ports from a list like "1027, 7777" (at most MAX_SHARED_PORTS). */
static void sn_read_ports(const char *s)
{
   sn.nshared_ports = 0;
   while (*s && sn.nshared_ports < MAX_SHARED_PORTS)
   {
      char *end;
      unsigned long port = strtoul(s, &end, 10);
      if (end == s)
      {
         s++;
         continue;
      }
      if (port > 0 && port < 65536 && !sn_is_shared_port((uint16_t)port))
      {
         sn.shared_listen[sn.nshared_ports]  = INVALID_SOCKET;
         sn.shared_ports[sn.nshared_ports++] = (uint16_t)port;
      }
      s = end;
   }
}

/* The ports as a list, "1027 7777". */
static void sn_ports_list(char *s, size_t len)
{
   size_t n = 0;
   unsigned i;
   s[0] = '\0';
   for (i = 0; i < sn.nshared_ports && n < len; i++)
      n += snprintf(s + n, len - n, "%s%u", i ? " " : "",
            sn.shared_ports[i]);
}

/* ---------------------------------------------------------------------- */
/* Session                                                                */
/* ---------------------------------------------------------------------- */

#ifdef HAVE_NETWORKING
static void sn_stop_netplay(void);
#endif

static void sn_end(void)
{
#ifdef HAVE_NETWORKING
   sn_stop_netplay();
#endif
   sn_tunnel_stop();
   if (sn.lobby)
      api.LeaveLobby(sn.mm, sn.lobby);
   sn.state          = BOMBA_STEAMNET_OFF;
   sn.lobby          = 0;
   sn.host           = 0;
   sn.host_name[0]   = '\0';
   sn.pub_system[0]  = '\0';
   sn.pub_title[0]   = '\0';
   sn.host_system[0] = '\0';
   sn.host_title[0]  = '\0';
   sn.nplayers       = 0;
   sn.players_known  = false;
   sn.nmembers       = 0;
   sn.nkicked        = 0;
   sn.ninvited       = 0;
}

/* This player's own session (see bomba_steamnet_poll). */
static void sn_host(void)
{
   sn_end();
   sn.state = BOMBA_STEAMNET_CREATING;
   api.CreateLobby(sn.mm, LOBBY_PRIVATE, BOMBA_STEAMNET_MAX_PLAYERS);
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
   {
      char ports[MAX_SHARED_PORTS * 6 + 1];
      sn_read_ports(config_get_ptr()->arrays.bomba_shared_tcp_ports);
      sn_ports_list(ports, sizeof(ports));
      api.SetLobbyData(sn.mm, sn.lobby, "ports", ports);
      if (*ports)
         RARCH_LOG("[Bomba] Sharing TCP ports %s with the session.\n", ports);
   }
   sn.state = BOMBA_STEAMNET_HOSTING;
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

   if (     (sn.local  = sn_udp_socket())   == INVALID_SOCKET
         || (sn.listen = sn_tcp_listener(0)) == INVALID_SOCKET)
   {
      sn_message("Multiplayer: couldn't open the session's local port.");
      sn_back_home(0);
      return;
   }
   /* The host's shared ports, on the same ports here. */
   {
      unsigned i;
      sn_read_ports(sn_lobby_data(sn.lobby, "ports"));
      for (i = 0; i < sn.nshared_ports; i++)
      {
         if ((sn.shared_listen[i] = sn_tcp_listener(sn.shared_ports[i]))
               == INVALID_SOCKET)
         {
            char msg[128];
            snprintf(msg, sizeof(msg), "Multiplayer: port %u is in use "
                  "here; the host's can't be reached on it.",
                  sn.shared_ports[i]);
            sn_message(msg);
         }
         else
            RARCH_LOG("[Bomba] The host's port %u is on 127.0.0.1:%u.\n",
                  sn.shared_ports[i], sn.shared_ports[i]);
      }
   }
   {
      struct sockaddr_in addr;
      int len = sizeof(addr);
      getsockname(sn.local, (struct sockaddr*)&addr, &len);
      sn.local_port = ntohs(addr.sin_port);
      len = sizeof(addr);
      getsockname(sn.listen, (struct sockaddr*)&addr, &len);
      sn.netplay_port = ntohs(addr.sin_port);
   }
   sn.state = BOMBA_STEAMNET_JOINED;
   sn_tunnel_start();
   RARCH_LOG("[Bomba] Joined Steam lobby %llu (host %llu), local ports %u "
         "(room), %u (netplay).\n", (unsigned long long)sn.lobby,
         (unsigned long long)sn.host, sn.local_port, sn.netplay_port);
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

/* Whether the lobby's "kick" list (Steam IDs, space-separated) has id. */
static bool sn_kick_lists(steam_id_t id)
{
   const char *s = sn_lobby_data(sn.lobby, "kick");
   while (*s)
   {
      char *end;
      steam_id_t k = strtoull(s, &end, 10);
      if (end == s)
         break;
      if (k == id)
         return true;
      s = end;
   }
   return false;
}

static void sn_on_lobby_data(const lobby_data_t *d)
{
   if (d->lobby != sn.lobby || sn.state != BOMBA_STEAMNET_JOINED)
      return;
   if (sn_kick_lists(sn.me))
   {
      char msg[128];
      snprintf(msg, sizeof(msg), "Multiplayer: %s removed you from the "
            "session.", sn.host_name);
      sn_back_home(0);
      sn_message(msg);
      return;
   }
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
               && sn_is_member(who->u.steam_id)
               && !sn_is_kicked(who->u.steam_id))
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

   sn.local  = INVALID_SOCKET;
   sn.listen = INVALID_SOCKET;
   InitializeCriticalSection(&sn.members_cs);

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
   SN_SYM(GetLargeFriendAvatar,  "SteamAPI_ISteamFriends_GetLargeFriendAvatar");
   SN_SYM(GetMediumFriendAvatar, "SteamAPI_ISteamFriends_GetMediumFriendAvatar");
   SN_SYM(GetFriendCount,        "SteamAPI_ISteamFriends_GetFriendCount");
   SN_SYM(GetFriendByIndex,      "SteamAPI_ISteamFriends_GetFriendByIndex");
   SN_SYM(GetFriendPersonaState, "SteamAPI_ISteamFriends_GetFriendPersonaState");
   SN_SYM(GetFriendGamePlayed,   "SteamAPI_ISteamFriends_GetFriendGamePlayed");
   SN_SYM(InviteUserToLobby,     "SteamAPI_ISteamMatchmaking_InviteUserToLobby");
   SN_SYM(SteamUtils,            "SteamAPI_SteamUtils_v010");
   SN_SYM(GetImageSize,          "SteamAPI_ISteamUtils_GetImageSize");
   SN_SYM(GetImageRGBA,          "SteamAPI_ISteamUtils_GetImageRGBA");
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
   sn.utils   = api.SteamUtils();
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

static bomba_steamnet_player_t *sn_find_player(
      bomba_steamnet_player_t *list, unsigned n, steam_id_t id)
{
   unsigned i;
   for (i = 0; i < n; i++)
      if (list[i].id == id)
         return &list[i];
   return NULL;
}

/* Host: the lobby's kick list from sn.kicked. */
static void sn_publish_kicked(void)
{
   char list[BOMBA_STEAMNET_MAX_PLAYERS * 21 + 1];
   size_t len = 0;
   unsigned i;
   list[0] = '\0';
   for (i = 0; i < sn.nkicked; i++)
      len += snprintf(list + len, sizeof(list) - len, "%s%llu",
            i ? " " : "", (unsigned long long)sn.kicked[i]);
   api.SetLobbyData(sn.mm, sn.lobby, "kick", list);
}

/* Who else is in the session (the host first), telling of arrivals and
 * departures. */
static void sn_refresh_players(void)
{
   bomba_steamnet_player_t now[BOMBA_STEAMNET_MAX_PLAYERS];
   steam_id_t avatar_of[BOMBA_STEAMNET_MAX_PLAYERS];
   bool hosting     = sn.state == BOMBA_STEAMNET_HOSTING;
   steam_id_t owner = api.GetLobbyOwner(sn.mm, sn.lobby);
   int members      = api.GetNumLobbyMembers(sn.mm, sn.lobby);
   unsigned n = 0, i;
   int m;

   for (m = 0; m < members && n < BOMBA_STEAMNET_MAX_PLAYERS; m++)
   {
      steam_id_t id = api.GetLobbyMemberByIndex(sn.mm, sn.lobby, m);
      const char *name;
      /* (Kicked players linger until their side sees it.) */
      if (!id || id == sn.me || (hosting && sn_is_kicked(id)))
         continue;
      name = api.GetFriendPersonaName(sn.friends, id);
      memset(&now[n], 0, sizeof(now[n]));
      now[n].id   = id;
      now[n].host = id == owner;
      strlcpy(now[n].name, name ? name : "", sizeof(now[n].name));
      avatar_of[n++] = id;
   }

   /* The host first. */
   for (i = 1; i < n; i++)
      if (now[i].host)
      {
         bomba_steamnet_player_t p = now[i];
         steam_id_t a              = avatar_of[i];
         memmove(&now[1], &now[0], i * sizeof(now[0]));
         memmove(&avatar_of[1], &avatar_of[0], i * sizeof(avatar_of[0]));
         now[0]       = p;
         avatar_of[0] = a;
         break;
      }

   for (i = 0; i < n; i++)
   {
      /* Steam fetches pictures on demand: the large one once it's in,
       * the medium one meanwhile. */
      int a = api.GetLargeFriendAvatar(sn.friends, avatar_of[i]);
      if (a <= 0)
         a = api.GetMediumFriendAvatar(sn.friends, avatar_of[i]);
      now[i].avatar = a > 0 ? a : 0;
   }

   if (sn.players_known)
   {
      char msg[160];
      for (i = 0; i < n; i++)
         if (!sn_find_player(sn.players, sn.nplayers, now[i].id))
         {
            snprintf(msg, sizeof(msg), "Multiplayer: %s joined %s.",
                  now[i].name, hosting ? "your session" : "the session");
            sn_message(msg);
         }
      for (i = 0; i < sn.nplayers; i++)
         if (     !sn_find_player(now, n, sn.players[i].id)
               && !sn_is_kicked(sn.players[i].id)
               && !sn.players[i].host) /* (sn_on_lobby_chat tells) */
         {
            snprintf(msg, sizeof(msg), "Multiplayer: %s left %s.",
                  sn.players[i].name,
                  hosting ? "your session" : "the session");
            sn_message(msg);
         }
   }

   /* Kicked players gone: free to be invited again. */
   if (hosting)
   {
      unsigned before = sn.nkicked;
      for (i = 0; i < sn.nkicked; )
      {
         if (!sn_is_member(sn.kicked[i]))
            sn.kicked[i] = sn.kicked[--sn.nkicked];
         else
            i++;
      }
      if (sn.nkicked != before)
         sn_publish_kicked();
   }

   memcpy(sn.players, now, n * sizeof(now[0]));
   memcpy(sn.avatar_of, avatar_of, n * sizeof(avatar_of[0]));
   sn.nplayers      = n;
   sn.players_known = true;

   EnterCriticalSection(&sn.members_cs);
   for (i = 0; i < n; i++)
      sn.members[i] = now[i].id;
   sn.nmembers = n;
   LeaveCriticalSection(&sn.members_cs);
}

#ifdef HAVE_NETWORKING
/* Whether the running game's core can do netplay (launcher cores can't:
 * theirs is the room), without netplay saying so on screen. */
static bool sn_core_does_netplay(void)
{
   struct retro_core_t *core = &runloop_state_get_ptr()->current_core;
   return   core_info_current_supports_netplay()
         && core->retro_serialize_size
         && core->retro_serialize_size() > 0
         && !(core_serialization_quirks()
            & (RETRO_SERIALIZATION_QUIRK_INCOMPLETE
             | RETRO_SERIALIZATION_QUIRK_SINGLE_SESSION));
}

static void sn_stop_netplay(void)
{
   if (!sn.np_started)
      return;
   sn.np_started = false;
   sn.np_tried   = false;
   if (netplay_driver_ctl(RARCH_NETPLAY_CTL_IS_DATA_INITED, NULL))
      command_event(CMD_EVENT_NETPLAY_DISCONNECT, NULL);
   RARCH_LOG("[Bomba] Netplay for the session stopped.\n");
}

/* Host: netplay for the running game while guests are in the session,
 * started on it as it runs (once per game: a core that can't stays
 * without), stopped when the last guest leaves. */
static void sn_host_netplay(void)
{
   runloop_state_t *rs = runloop_state_get_ptr();
   const char *content = path_get(RARCH_PATH_CONTENT);
   bool running        = (rs->current_core.flags & RETRO_CORE_FLAG_GAME_LOADED)
      ? true : false;
   bool on             = netplay_driver_ctl(RARCH_NETPLAY_CTL_IS_DATA_INITED,
         NULL);

   if (!running)
      content = "";
   if (!string_is_equal(content, sn.np_content))
   {
      strlcpy(sn.np_content, content, sizeof(sn.np_content));
      sn.np_tried = false;
   }
   /* (Ends with the game.) */
   if (!on)
      sn.np_started = false;

   if (sn.nplayers && running && !on && !sn.np_tried)
   {
      sn.np_tried = true;
      if (!sn_core_does_netplay())
         return;
      netplay_driver_ctl(RARCH_NETPLAY_CTL_ENABLE_SERVER, NULL);
      if (command_event(CMD_EVENT_NETPLAY_INIT, NULL))
      {
         sn.np_started = true;
#if HAVE_RUNAHEAD
         /* (Not with netplay.) */
         preempt_deinit(rs);
#endif
         RARCH_LOG("[Bomba] Netplay hosted for the session.\n");
      }
      else
         RARCH_ERR("[Bomba] Netplay not hosted for the session.\n");
   }
   else if (!sn.nplayers && sn.np_started)
      sn_stop_netplay();
}
#endif

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

   if (     (sn.state == BOMBA_STEAMNET_HOSTING
            || sn.state == BOMBA_STEAMNET_JOINED)
         && sn_now() >= sn.players_at)
   {
      sn.players_at = sn_now() + SN_PLAYERS_USEC;
      sn_refresh_players();
   }

   if (sn.state == BOMBA_STEAMNET_HOSTING)
   {
      sn_publish_game();
#ifdef HAVE_NETWORKING
      sn_host_netplay();
#endif
   }
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
unsigned bomba_steamnet_netplay_port(void)   { return sn.netplay_port; }

bool bomba_steamnet_netplay(void)
{
   return sn.state == BOMBA_STEAMNET_HOSTING
       || sn.state == BOMBA_STEAMNET_JOINED;
}
const char *bomba_steamnet_my_name(void)     { return sn.my_name; }
const char *bomba_steamnet_host_system(void) { return sn.host_system; }
const char *bomba_steamnet_host_title(void)  { return sn.host_title; }

unsigned bomba_steamnet_players(const bomba_steamnet_player_t **players)
{
   *players = sn.players;
   return sn.nplayers;
}

uint8_t *bomba_steamnet_avatar_rgba(int avatar, unsigned *w, unsigned *h)
{
   uint32_t iw = 0, ih = 0;
   uint8_t *rgba;

   if (     !sn.ok || avatar <= 0 || !sn.utils
         || !api.GetImageSize(sn.utils, avatar, &iw, &ih) || !iw || !ih)
      return NULL;
   if (!(rgba = (uint8_t*)malloc((size_t)iw * ih * 4)))
      return NULL;
   if (!api.GetImageRGBA(sn.utils, avatar, rgba, (int)(iw * ih * 4)))
   {
      free(rgba);
      return NULL;
   }
   *w = iw;
   *h = ih;
   return rgba;
}

void bomba_steamnet_kick(uint64_t id)
{
   bomba_steamnet_player_t *p;
   char msg[128];

   if (     sn.state != BOMBA_STEAMNET_HOSTING
         || !(p = sn_find_player(sn.players, sn.nplayers, id)))
      return;
   snprintf(msg, sizeof(msg), "Multiplayer: %s was kicked.", p->name);

   if (!sn_is_kicked(id) && sn.nkicked < BOMBA_STEAMNET_MAX_PLAYERS)
   {
      net_identity_t who;
      /* Its traffic stops at once; the guest leaves on seeing the list. */
      sn.kicked[sn.nkicked] = id;
      sn.nkicked++;
      sn_publish_kicked();
      sn_identity(&who, id);
      api.CloseSession(sn.msgs, &who);
   }
   RARCH_LOG("[Bomba] Kicked %llu.\n", (unsigned long long)id);

   /* Off the list now, not on the next look. */
   {
      size_t i    = (size_t)(p - sn.players);
      size_t tail = sn.nplayers - i - 1;
      memmove(&sn.players[i], &sn.players[i + 1], tail * sizeof(sn.players[0]));
      memmove(&sn.avatar_of[i], &sn.avatar_of[i + 1],
            tail * sizeof(sn.avatar_of[0]));
      sn.nplayers--;
   }
   sn_message(msg);
}

bool bomba_steamnet_can_invite(void)
{
   return sn.ok && sn.lobby && (sn.state == BOMBA_STEAMNET_HOSTING
         || sn.state == BOMBA_STEAMNET_JOINED);
}

/* An invite shows as such for a few seconds, then the friend can be
 * invited again (missed it, declined, or joined and left). */
#define SN_INVITED_USEC (3 * 1000000LL)

/* Seconds until the friend can be invited again, rounded up; 0 = now. */
static unsigned sn_was_invited(steam_id_t id)
{
   unsigned i;
   int64_t now = sn_now();
   for (i = 0; i < sn.ninvited; )
   {
      if (now >= sn.invited_until[i])
      {
         sn.ninvited--;
         sn.invited[i]       = sn.invited[sn.ninvited];
         sn.invited_until[i] = sn.invited_until[sn.ninvited];
         continue;
      }
      if (sn.invited[i] == id)
         return (unsigned)((sn.invited_until[i] - now + 999999) / 1000000);
      i++;
   }
   return 0;
}

/* Names of the Steam games friends play, from the Steam store (the
 * Steamworks API has none), fetched once each. */
#define SN_MAX_APPS 64
#define SN_APP_URL  "https://store.steampowered.com/api/appdetails?filters=basic&appids="

static struct
{
   uint32_t appid;
   bool done;          /* fetched (name may still be empty: unknown) */
   char name[96];
} sn_apps[SN_MAX_APPS];
static unsigned sn_napps;

/* JSON string at s (after its opening quote) into out, as UTF-8. */
static void sn_json_str(const char *s, char *out, size_t len)
{
   size_t o = 0;
   while (*s && *s != '"' && o + 4 < len)
   {
      if (*s != '\\')
      {
         out[o++] = *s++;
         continue;
      }
      s++;
      if (*s == 'u')
      {
         char hex[5] = { 0 };
         unsigned c, k;
         for (k = 0; k < 4 && s[1 + k]; k++)
            hex[k] = s[1 + k];
         c  = (unsigned)strtoul(hex, NULL, 16);
         s += 1 + k;
         if (c < 0x80)
            out[o++] = (char)c;
         else if (c < 0x800)
         {
            out[o++] = (char)(0xc0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3f));
         }
         else
         {
            out[o++] = (char)(0xe0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3f));
            out[o++] = (char)(0x80 | (c & 0x3f));
         }
         continue;
      }
      if (*s)
         out[o++] = *s == 'n' ? ' ' : *s == 't' ? ' ' : *s;
      if (*s)
         s++;
   }
   out[o] = '\0';
}

static void sn_app_cb(retro_task_t *task, void *task_data, void *user_data,
      const char *error)
{
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   unsigned i                 = (unsigned)(uintptr_t)user_data;
   char *body, *name;

   if (i >= sn_napps)
      return;
   sn_apps[i].done = true;
   if (!data || data->status != 200 || !data->data || !data->len)
      return;
   if (!(body = (char*)malloc(data->len + 1)))
      return;
   memcpy(body, data->data, data->len);
   body[data->len] = '\0';
   if ((name = strstr(body, "\"name\":\"")))
      sn_json_str(name + 8, sn_apps[i].name, sizeof(sn_apps[i].name));
   free(body);
}

/* The game's name, or "" while it's being fetched (or unknown). */
static const char *sn_app_name(uint32_t appid)
{
   char url[160];
   unsigned i;

   for (i = 0; i < sn_napps; i++)
      if (sn_apps[i].appid == appid)
         return sn_apps[i].name;
   if (sn_napps >= SN_MAX_APPS)
      return "";
   i = sn_napps++;
   sn_apps[i].appid   = appid;
   sn_apps[i].done    = false;
   sn_apps[i].name[0] = '\0';
   snprintf(url, sizeof(url), SN_APP_URL "%u", appid);
   task_push_http_transfer(url, true, "GET", sn_app_cb,
         (void*)(uintptr_t)i);
   return "";
}

static int sn_friend_cmp(const void *pa, const void *pb)
{
   const bomba_steamnet_friend_t *a = (const bomba_steamnet_friend_t*)pa;
   const bomba_steamnet_friend_t *b = (const bomba_steamnet_friend_t*)pb;
   if (a->status != b->status)
      return a->status > b->status ? -1 : 1;
   return _stricmp(a->name, b->name);
}

/* Steam's "immediate" friends: the friends list proper. */
#define FRIEND_FLAG_IMMEDIATE 4
/* A CGameID's type (bits 24-31): 2 is a non-Steam shortcut. */
#define GAME_TYPE_SHORTCUT    2

unsigned bomba_steamnet_friends(const bomba_steamnet_friend_t **list)
{
   int i, count;
   unsigned n = 0;

   *list = sn.friends_list;
   if (!bomba_steamnet_can_invite())
      return sn.nfriends = 0;

   /* Friends not in the session already: playing Bomba (RetroArch), in
    * another game, online, offline; by name within each. */
   count = api.GetFriendCount(sn.friends, FRIEND_FLAG_IMMEDIATE);
   for (i = 0; i < count && n < BOMBA_STEAMNET_MAX_FRIENDS; i++)
   {
      steam_id_t id = api.GetFriendByIndex(sn.friends, i,
            FRIEND_FLAG_IMMEDIATE);
      bomba_steamnet_friend_t *f = &sn.friends_list[n];
      friend_game_t game;
      const char *name;
      int a;

      if (!id || sn_is_member(id))
         continue;
      memset(f, 0, sizeof(*f));
      f->id = id;
      name  = api.GetFriendPersonaName(sn.friends, id);
      strlcpy(f->name, name ? name : "", sizeof(f->name));
      memset(&game, 0, sizeof(game));
      if (api.GetFriendPersonaState(sn.friends, id) == 0)
         f->status = BOMBA_FRIEND_OFFLINE;
      else if (!api.GetFriendGamePlayed(sn.friends, id, &game) || !game.game)
         f->status = BOMBA_FRIEND_ONLINE;
      else if ((uint32_t)(game.game & 0xffffff) == STEAM_APPID)
         f->status = BOMBA_FRIEND_PLAYING;
      else
      {
         f->status = BOMBA_FRIEND_IN_GAME;
         if (((game.game >> 24) & 0xff) == GAME_TYPE_SHORTCUT)
            strlcpy(f->game, "a non-Steam game", sizeof(f->game));
         else
            strlcpy(f->game, sn_app_name((uint32_t)(game.game & 0xffffff)),
                  sizeof(f->game));
      }
      f->invited = sn_was_invited(id);
      a = api.GetMediumFriendAvatar(sn.friends, id);
      f->avatar  = a > 0 ? a : 0;
      n++;
   }
   qsort(sn.friends_list, n, sizeof(sn.friends_list[0]), sn_friend_cmp);
   return sn.nfriends = n;
}

bool bomba_steamnet_invite(uint64_t id)
{
   unsigned i;
   char msg[128];

   if (!bomba_steamnet_can_invite() || !api.InviteUserToLobby(sn.mm, sn.lobby, id))
   {
      sn_message("Multiplayer: couldn't send the invite.");
      return false;
   }
   if (!sn_was_invited(id) && sn.ninvited < BOMBA_STEAMNET_MAX_FRIENDS)
   {
      sn.invited[sn.ninvited]         = id;
      sn.invited_until[sn.ninvited++] = sn_now() + SN_INVITED_USEC;
   }
   for (i = 0; i < sn.nfriends; i++)
      if (sn.friends_list[i].id == id)
      {
         sn.friends_list[i].invited = SN_INVITED_USEC / 1000000;
         snprintf(msg, sizeof(msg), "Multiplayer: invited %s.",
               sn.friends_list[i].name);
         sn_message(msg);
         break;
      }
   RARCH_LOG("[Bomba] Invited %llu.\n", (unsigned long long)id);
   return true;
}

void bomba_steamnet_leave(void)
{
   char msg[128];

   if (sn.state != BOMBA_STEAMNET_JOINED && sn.state != BOMBA_STEAMNET_JOINING)
      return;
   if (sn.host_name[0])
      snprintf(msg, sizeof(msg), "Multiplayer: left %s's session.",
            sn.host_name);
   else
      strlcpy(msg, "Multiplayer: left the session.", sizeof(msg));
   sn_back_home(0);
   sn_message(msg);
}

void bomba_steamnet_set_game(const char *system, const char *title)
{
   strlcpy(sn.want_system, system ? system : "", sizeof(sn.want_system));
   strlcpy(sn.want_title,  title  ? title  : "", sizeof(sn.want_title));
}
