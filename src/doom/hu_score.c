//
// Deathmatch scoreboard and in-world player name tags.
//
// Vanilla DOOM shows a frag total on the status bar and a full table only at the
// intermission between levels. This adds:
//
//   * a compact score line across the top of the screen, drawn only when no
//     HTML overlay is doing the same job (see hu_bridge.c),
//   * a full scoreboard while the ` / ~ key is held,
//   * a name floating above each other player in the world.
//
// All of it is drawn straight onto the 320x200 framebuffer with the standard HUD
// font, so it needs no new graphics and works with or without the browser UI.
//

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "deh_str.h"
#include "doomdef.h"
#include "doomstat.h"
#include "g_game.h"
#include "hu_score.h"
#include "i_swap.h"   // SHORT
#include "i_video.h"  // SCREENWIDTH / SCREENHEIGHT
#include "m_misc.h"
#include "v_video.h"
#include "w_wad.h"
#include "z_zone.h"

// The HUD font, loaded by hu_stuff.c. Uppercase only, starting at '!'.
extern patch_t *hu_font[HU_FONTSIZE];

// DOOM's own colour names, used when the launcher passed no roster.
extern const char *player_names[];

#define SCORE_MAX_NAME 15

// Names supplied with -playernames, in player order. Empty until then, in which
// case we fall back to DOOM's own colour names.
static char score_names[MAXPLAYERS][SCORE_MAX_NAME + 1];
static boolean score_have_names = false;

// Tags collected during the sprite pass and drawn once the world is finished.
// Bounded by the player count, since only players get a tag.
typedef struct
{
    int player;
    int x;      // screen centre of the sprite
    int y;      // screen y of the top of the sprite
} scoretag_t;

static scoretag_t score_tags[MAXPLAYERS];
static int score_numtags;

// Whether to draw the top score line. Defaults to on so a build with no HTML
// overlay in front of it -- a plain page, or a browser that failed to load the
// UI -- still shows the frags somewhere. hu_bridge.c turns it off as soon as it
// finds a live handler on the page.
static boolean score_show_topline = true;

void HU_SetShowTopLine(boolean show)
{
    score_show_topline = show;
}

void HU_SetPlayerNames(const char *csv)
{
    int player = 0;
    int len = 0;

    memset(score_names, 0, sizeof(score_names));

    while (*csv && player < MAXPLAYERS)
    {
        if (*csv == ',')
        {
            player++;
            len = 0;
        }
        else if (len < SCORE_MAX_NAME)
        {
            score_names[player][len++] = *csv;
        }

        csv++;
    }

    score_have_names = true;
}

const char *HU_ScoreName(int player)
{
    if (player < 0 || player >= MAXPLAYERS)
        return "?";

    if (score_have_names && score_names[player][0])
        return score_names[player];

    // Falls back to GREEN / INDIGO / BROWN / RED when the launcher did not pass
    // names, so a tag is never blank.
    return player_names[player];
}

//
// Text drawing.
//
// m_menu.c has M_WriteText, but its forward declaration is static, so it has
// internal linkage and cannot be called from here. This is the same idea, kept
// small: uppercase-only, clipped to the screen.
//

//
// Recolouring the font to white.
//
// Freedoom's HUD font is red, which is fine for the score line but wrong for a
// name floating over a player. There is no white font in the IWAD and no
// translated-draw helper in v_video, so build a lookup that maps each palette
// entry to the neutral grey of the same brightness, and draw the glyphs through
// it. Shading is preserved; only the hue is dropped.
//

static byte white_map[256];
static boolean white_map_ready = false;

static void HU_BuildWhiteMap(void)
{
    byte *pal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_STATIC);
    int i;

    for (i = 0; i < 256; i++)
    {
        int want = (pal[i * 3] * 77 + pal[i * 3 + 1] * 150 + pal[i * 3 + 2] * 29) >> 8;
        int best = i;
        int bestScore = INT_MAX;
        int j;

        for (j = 0; j < 256; j++)
        {
            int r = pal[j * 3];
            int g = pal[j * 3 + 1];
            int b = pal[j * 3 + 2];
            int lum = (r * 77 + g * 150 + b * 29) >> 8;
            // Penalise any colour cast, then prefer the closest brightness.
            int cast = abs(r - g) + abs(g - b) + abs(r - b);
            int score = cast * 8 + abs(lum - want);

            if (score < bestScore)
            {
                bestScore = score;
                best = j;
            }
        }

        white_map[i] = best;
    }

    white_map_ready = true;
}

//
// Green tint for a protected player.
//
// Same trick as the white font map: for each palette entry find the closest
// green of matching brightness. A sprite drawn through this reads as clearly
// "not solid yet" without losing its shading.
//

static byte green_map[256];
static boolean green_map_ready = false;

byte *HU_GreenColormap(void)
{
    byte *pal;
    int i;

    if (green_map_ready)
        return green_map;

    pal = W_CacheLumpName(DEH_String("PLAYPAL"), PU_STATIC);

    for (i = 0; i < 256; i++)
    {
        int want = (pal[i * 3] * 77 + pal[i * 3 + 1] * 150 + pal[i * 3 + 2] * 29) >> 8;
        int best = i;
        int bestScore = INT_MAX;
        int j;

        for (j = 0; j < 256; j++)
        {
            int r = pal[j * 3];
            int g = pal[j * 3 + 1];
            int b = pal[j * 3 + 2];
            int lum = (r * 77 + g * 150 + b * 29) >> 8;
            // Reward green dominance, then match brightness.
            int greenness = (g * 2) - r - b;
            int score = abs(lum - want) - greenness;

            if (score < bestScore)
            {
                bestScore = score;
                best = j;
            }
        }

        green_map[i] = best;
    }

    green_map_ready = true;
    return green_map;
}

/** V_DrawPatch, but every pixel passes through the white lookup. */
static void HU_DrawPatchWhite(int x, int y, patch_t *patch)
{
    int col;
    int w = SHORT(patch->width);

    y -= SHORT(patch->topoffset);
    x -= SHORT(patch->leftoffset);

    if (y < 0 || y + SHORT(patch->height) > SCREENHEIGHT)
        return;

    for (col = 0; col < w; x++, col++)
    {
        column_t *column;

        if (x < 0 || x >= SCREENWIDTH)
            continue;

        column = (column_t *)((byte *)patch + LONG(patch->columnofs[col]));

        while (column->topdelta != 0xff)
        {
            byte *source = (byte *)column + 3;
            pixel_t *dest = I_VideoBuffer + (y + column->topdelta) * SCREENWIDTH + x;
            int count = column->length;

            while (count--)
            {
                *dest = white_map[*source++];
                dest += SCREENWIDTH;
            }

            column = (column_t *)((byte *)column + column->length + 4);
        }
    }
}

int HU_TextWidth(const char *text)
{
    int width = 0;
    int i;

    for (i = 0; text[i]; i++)
    {
        int c = toupper(text[i]) - HU_FONTSTART;

        if (c < 0 || c >= HU_FONTSIZE)
            width += 4; // space
        else
            width += SHORT(hu_font[c]->width);
    }

    return width;
}

void HU_WriteText(int x, int y, const char *text)
{
    int cx = x;
    int i;

    if (y < 0 || y > SCREENHEIGHT - 8)
        return;

    for (i = 0; text[i]; i++)
    {
        int c = toupper(text[i]) - HU_FONTSTART;
        int w;

        if (c < 0 || c >= HU_FONTSIZE)
        {
            cx += 4; // space
            continue;
        }

        w = SHORT(hu_font[c]->width);

        // Clip whole glyphs rather than letting V_DrawPatch run off the edge.
        if (cx < 0)
        {
            cx += w;
            continue;
        }
        if (cx + w > SCREENWIDTH)
            break;

        V_DrawPatch(cx, y, hu_font[c]);
        cx += w;
    }
}

/** HU_WriteText, recoloured to white. Used for the in-world name tags. */
void HU_WriteTextWhite(int x, int y, const char *text)
{
    int cx = x;
    int i;

    if (!white_map_ready)
        HU_BuildWhiteMap();

    if (y < 0 || y > SCREENHEIGHT - 8)
        return;

    for (i = 0; text[i]; i++)
    {
        int c = toupper(text[i]) - HU_FONTSTART;
        int w;

        if (c < 0 || c >= HU_FONTSIZE)
        {
            cx += 4;
            continue;
        }

        w = SHORT(hu_font[c]->width);

        if (cx < 0)
        {
            cx += w;
            continue;
        }
        if (cx + w > SCREENWIDTH)
            break;

        HU_DrawPatchWhite(cx, y, hu_font[c]);
        cx += w;
    }
}

//
// Name tags.
//

void HU_ClearNameTags(void)
{
    score_numtags = 0;
}

void HU_AddNameTag(int player, int x, int y)
{
    if (score_numtags >= MAXPLAYERS)
        return;

    score_tags[score_numtags].player = player;
    score_tags[score_numtags].x = x;
    score_tags[score_numtags].y = y;
    score_numtags++;
}

void HU_DrawNameTags(void)
{
    int i;

    for (i = 0; i < score_numtags; i++)
    {
        const char *name = HU_ScoreName(score_tags[i].player);
        int width = HU_TextWidth(name);
        int x = score_tags[i].x - width / 2;
        int y = score_tags[i].y - 10;

        // Tags live in screen space, so they always face the viewer and need no
        // billboarding of their own.
        if (y < 0)
            y = 0;

        HU_WriteTextWhite(x, y, name);
    }
}

//
// Scoreboard.
//

static void HU_DrawTopLine(void)
{
    char line[80];
    int i;

    line[0] = '\0';

    for (i = 0; i < MAXPLAYERS; i++)
    {
        char entry[32];

        if (!playeringame[i])
            continue;

        M_snprintf(entry, sizeof(entry), "%s %d  ",
                   HU_ScoreName(i), G_PlayerFrags(i));
        M_StringConcat(line, entry, sizeof(line));
    }

    HU_WriteText((SCREENWIDTH - HU_TextWidth(line)) / 2, 2, line);
}

static void HU_DrawFullBoard(void)
{
    char line[80];
    int y = 40;
    int i;

    HU_WriteText((SCREENWIDTH - HU_TextWidth("SCOREBOARD")) / 2, y, "SCOREBOARD");
    y += 16;

    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (!playeringame[i])
            continue;

        M_snprintf(line, sizeof(line), "%-16s %3d",
                   HU_ScoreName(i), G_PlayerFrags(i));
        HU_WriteText(60, y, line);
        y += 10;
    }

    if (fraglimit > 0)
    {
        M_snprintf(line, sizeof(line), "FIRST TO %d WINS", fraglimit);
        HU_WriteText((SCREENWIDTH - HU_TextWidth(line)) / 2, y + 6, line);
    }
}

void HU_DrawScore(void)
{
    if (!deathmatch)
        return;

    HU_DrawNameTags();

    // The always-on score line this used to draw across the top is now an HTML
    // panel fed by hu_bridge.c, which can use a real font at the display's own
    // resolution instead of a 320x200 bitmap one. Drawing both put two copies of
    // the same numbers on screen, overlapping.
    if (score_show_topline)
        HU_DrawTopLine();

    // Held, not toggled: release hides it again. '`' is unbound in DOOM, unlike
    // shift (run) or tab (automap), so it costs nothing to take.
    if (G_KeyIsDown('`'))
        HU_DrawFullBoard();
}
