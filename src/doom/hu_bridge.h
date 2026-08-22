//
// Pushes game state out to the JavaScript layer.
//

#ifndef __HU_BRIDGE__
#define __HU_BRIDGE__

#include "doomtype.h"

// The map's line segments, sent once per level for the minimap.
void HU_BridgeLevel(void);

// Scores and the local player's position. Throttled internally.
void HU_BridgeState(void);

// One kill, for the feed. Indices into players[].
void HU_BridgeKill(int victim, int killer);

// Final table when the level ends.
void HU_BridgeFinish(void);

#endif
