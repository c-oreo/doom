//
// Pushes game state out to the JavaScript layer.
//
// The leaderboard, kill feed, minimap and end screen are drawn as an HTML
// overlay rather than into DOOM's 320x200 framebuffer. That is not a stylistic
// preference: the UI is specified in a TrueType font, and this renderer can only
// blit paletted bitmap patches, so a TTF would have to be rasterised into lumps
// first and would still be locked to 320x200.
//
// So the engine's job here is only to report. Everything is serialised as JSON
// and handed to window.__doom.*; if that object is missing (the page has not
// mounted the overlay yet) the calls are simply ignored, so the engine never
// depends on the UI being there.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <emscripten.h>

#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "hu_bridge.h"
#include "hu_score.h"
#include "m_misc.h"
#include "p_local.h"
#include "r_state.h"

// State is sent at about 10 Hz. Every tic would be 35 JSON encodes a second for
// a panel that a human reads a few times a match.
#define BRIDGE_INTERVAL (TICRATE / 10)

static int bridge_lasttic;

/**
 * Sends one JSON payload to a named handler, if the page provides one.
 *
 * Returns whether a handler was actually there, which is how the engine knows
 * an overlay is drawing the score and it need not draw its own.
 */
static int HU_BridgeSend(const char *fn, const char *json)
{
    return EM_ASM_INT(
        {
            var name = UTF8ToString($0);
            var payload = UTF8ToString($1);
            if (window.__doom && typeof window.__doom[name] === "function")
            {
                try { window.__doom[name](JSON.parse(payload)); }
                catch (e) { /* never let the UI take the game down */ }
                return 1;
            }
            return 0;
        },
        fn, json);
}

/** Deaths are not stored by DOOM, but frags[killer][victim] gives them. */
static int HU_BridgeDeaths(int player)
{
    int killer;
    int deaths = 0;

    for (killer = 0; killer < MAXPLAYERS; killer++)
    {
        if (playeringame[killer])
            deaths += players[killer].frags[player];
    }

    return deaths;
}

/** Escapes the few characters a player name could carry into JSON. */
static void HU_BridgeName(char *out, size_t len, const char *in)
{
    size_t o = 0;
    size_t i;

    for (i = 0; in[i] && o + 2 < len; i++)
    {
        if (in[i] == '"' || in[i] == '\\')
            out[o++] = '\\';
        else if (in[i] < 32)
            continue;

        out[o++] = in[i];
    }

    out[o] = '\0';
}

void HU_BridgeLevel(void)
{
    // Two ints per line plus punctuation; generous so a large map cannot
    // overflow it.
    size_t cap = (size_t)numlines * 48 + 256;
    char *json = malloc(cap);
    size_t at;
    int i;

    if (json == NULL)
        return;

    M_StringCopy(json, "{\"lines\":[", cap);
    at = strlen(json);

    for (i = 0; i < numlines; i++)
    {
        char seg[64];

        M_snprintf(seg, sizeof(seg), "%s[%d,%d,%d,%d]",
                   i ? "," : "",
                   lines[i].v1->x >> FRACBITS, lines[i].v1->y >> FRACBITS,
                   lines[i].v2->x >> FRACBITS, lines[i].v2->y >> FRACBITS);

        if (at + strlen(seg) + 4 >= cap)
            break;

        strcpy(json + at, seg);
        at += strlen(seg);
    }

    M_StringConcat(json, "]}", cap);
    HU_BridgeSend("level", json);
    free(json);

    bridge_lasttic = 0;
}

void HU_BridgeState(void)
{
    char json[1024];
    char entry[192];
    char name[32];
    player_t *self;
    int i;

    if (gamestate != GS_LEVEL)
        return;

    if (leveltime - bridge_lasttic < BRIDGE_INTERVAL && leveltime > bridge_lasttic)
        return;

    bridge_lasttic = leveltime;
    self = &players[consoleplayer];

    M_StringCopy(json, "{\"players\":[", sizeof(json));

    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (!playeringame[i])
            continue;

        HU_BridgeName(name, sizeof(name), HU_ScoreName(i));
        M_snprintf(entry, sizeof(entry),
                   "%s{\"i\":%d,\"name\":\"%s\",\"frags\":%d,\"deaths\":%d,\"alive\":%s,\"self\":%s}",
                   json[strlen(json) - 1] == '[' ? "" : ",",
                   i, name, G_PlayerFrags(i), HU_BridgeDeaths(i),
                   players[i].playerstate == PST_DEAD ? "false" : "true",
                   i == consoleplayer ? "true" : "false");
        M_StringConcat(json, entry, sizeof(json));
    }

    // deathcount counts up from the moment of death; the overlay turns it into
    // the "respawning in N" line, so the wait and the auto-respawn in
    // P_DeathThink cannot drift apart.
    M_snprintf(entry, sizeof(entry),
               "],\"x\":%d,\"y\":%d,\"angle\":%u,\"invuln\":%d,\"dead\":%d,\"limit\":%d}",
               self->mo ? self->mo->x >> FRACBITS : 0,
               self->mo ? self->mo->y >> FRACBITS : 0,
               self->mo ? (unsigned)(self->mo->angle >> 24) : 0,
               self->powers[pw_invulnerability],
               self->playerstate == PST_DEAD ? self->deathcount : -1,
               fraglimit);
    M_StringConcat(json, entry, sizeof(json));

    // Checked every push rather than once, so an overlay that mounts late (or
    // is torn down) hands the score line back and forth without a restart.
    HU_SetShowTopLine(!HU_BridgeSend("state", json));
}

void HU_BridgeKill(int victim, int killer)
{
    char json[192];
    char victimName[32];
    char killerName[32];

    HU_BridgeName(victimName, sizeof(victimName), HU_ScoreName(victim));

    if (killer >= 0)
    {
        HU_BridgeName(killerName, sizeof(killerName), HU_ScoreName(killer));
        M_snprintf(json, sizeof(json),
                   "{\"victim\":\"%s\",\"killer\":\"%s\"}", victimName, killerName);
    }
    else
    {
        // No source: the map killed them, or they killed themselves.
        M_snprintf(json, sizeof(json),
                   "{\"victim\":\"%s\",\"killer\":null}", victimName);
    }

    HU_BridgeSend("kill", json);
}

void HU_BridgeFinish(void)
{
    char json[1024];
    char entry[192];
    char name[32];
    int i;

    M_StringCopy(json, "{\"rows\":[", sizeof(json));

    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (!playeringame[i])
            continue;

        HU_BridgeName(name, sizeof(name), HU_ScoreName(i));
        M_snprintf(entry, sizeof(entry),
                   "%s{\"name\":\"%s\",\"frags\":%d,\"deaths\":%d}",
                   json[strlen(json) - 1] == '[' ? "" : ",",
                   name, G_PlayerFrags(i), HU_BridgeDeaths(i));
        M_StringConcat(json, entry, sizeof(json));
    }

    M_StringConcat(json, "]}", sizeof(json));
    HU_BridgeSend("finish", json);
}
