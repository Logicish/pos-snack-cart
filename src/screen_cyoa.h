#pragma once

#include "cyoa_engine.h"

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Runs any CyoaStory (see cyoa_engine.h) -- the one shared screen every CYOA
            story uses. Story-specific content lives entirely in the CyoaStory's own
            data, never here.
*/

void screen_cyoa_push(const CyoaStory *story);
