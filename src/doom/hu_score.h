//
// Deathmatch scoreboard and in-world player name tags.
//

#ifndef __HU_SCORE__
#define __HU_SCORE__

#include "doomtype.h"
#include "hu_stuff.h"

// Comma-separated names in player order, from -playernames.
void HU_SetPlayerNames(const char *csv);

// The name for a player, falling back to DOOM's colour names.
const char *HU_ScoreName(int player);

// Small text helpers on the 320x200 framebuffer, using the HUD font.
int HU_TextWidth(const char *text);
void HU_WriteText(int x, int y, const char *text);
void HU_WriteTextWhite(int x, int y, const char *text);

// 256-byte colormap that tints a sprite green, for spawn protection.
byte *HU_GreenColormap(void);

// Suppresses the built-in top score line when something else is drawing it.
void HU_SetShowTopLine(boolean show);

// Name tags: cleared each frame, filled during the sprite pass, drawn with the
// rest of the score overlay.
void HU_ClearNameTags(void);
void HU_AddNameTag(int player, int x, int y);
void HU_DrawNameTags(void);

// Draws the top score line, the tags, and the full board while ` is held.
void HU_DrawScore(void);

#endif
