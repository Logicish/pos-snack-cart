#pragma once

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- The generic data-driven CYOA (choose-your-own-adventure) engine: nodes,
            choices, a small item/stat runtime, and an encounter-pool mechanism (see
            CyoaPool below) for stories that need "hit exactly N of these M rooms"
            randomized encounter selection. Story content (the actual writing) is
            separate data built against these types -- see story_test_data.h for a
            minimal smoke-test story, and snack_cart_pos.md's "Novelty mini-games"
            planning notes for the real stories planned on top of this (Elixir Run --
            formerly "Quest for Coffee" -- Jinx, Dragons), none of which are written yet.
  Notes---- Flags are deliberately NOT a separate array -- a stat holding 0 or 1 already
            covers "a flag," so CYOA_REQ_STAT_GTE/CYOA_EFF_ADJUST_STAT cover both without
            a third parallel array. Each choice carries at most ONE requirement and ONE
            effect (not a list) -- matches the original design note's singular wording
            ("an optional requirement... an optional effect") and keeps content-authoring
            simple; extend to arrays later if a real story ever needs to stack more than
            one condition/effect on a single choice.
*/

// Which physical button a choice is bound to. Deliberately excludes Back -- Back always
// exits the story outright (see screen_cyoa.cpp), never doubles as an in-story choice,
// so a story screen behaves the same safe way every other Extras screen does.
enum CyoaButton {
    CYOA_BTN_NONE,
    CYOA_BTN_UP,
    CYOA_BTN_DOWN,
    CYOA_BTN_LEFT,
    CYOA_BTN_RIGHT,
    CYOA_BTN_ENTER,
};

enum CyoaReqType {
    CYOA_REQ_NONE,
    CYOA_REQ_HAS_ITEM,    // index = item id
    CYOA_REQ_STAT_GTE,    // index = stat id, value = threshold
};

enum CyoaEffType {
    CYOA_EFF_NONE,
    CYOA_EFF_GRANT_ITEM,    // index = item id
    CYOA_EFF_CONSUME_ITEM,  // index = item id
    CYOA_EFF_ADJUST_STAT,   // index = stat id, value = delta (may be negative)
};

struct CyoaRequirement {
    CyoaReqType type;
    int index;
    int value;
};

struct CyoaEffect {
    CyoaEffType type;
    int index;
    int value;
};

// A run of rooms where the *engine*, not fixed story data, decides which specific rooms
// present a real encounter -- guarantees exactly target_hits hits across room_count
// rooms via the ratio formula (chance = hits_remaining / rooms_remaining, evaluated one
// room at a time as the player reaches it), matching the Hospital Town Games planning
// doc's "encounter selection" spec (see snack_cart_pos.md, Elixir Run's Act1/Act2
// 5-room-needing-3-hits pools). Each room in the pool has two node variants -- hit_node
// (a real encounter) and skip_node (a pass-through) -- authored as ordinary CyoaNodes
// like any other. See screen_cyoa.cpp's pool_advance()/choose() for the runtime walk.
struct CyoaPoolRoom {
    int hit_node;
    int skip_node;
};

struct CyoaPool {
    const CyoaPoolRoom *rooms;
    int                 room_count;
    int                 target_hits;
    int                 exit_node;   // loaded once every room in the pool is resolved
};

// A choice's next_node set to this sentinel means "let the active pool decide" -- used
// by the choice inside each pool room's hit/skip node that lets the player continue on
// to the next room (or, once the pool is exhausted, out to its exit_node). Meaningless
// outside an active pool.
#define CYOA_POOL_CONTINUE (-1)

#define CYOA_MAX_CHOICES 4

struct CyoaChoice {
    CyoaButton      button;   // CYOA_BTN_NONE = unused slot
    const char     *label;    // short button text, e.g. "Insert the coin"
    CyoaRequirement requirement;
    CyoaEffect      effect;
    int             next_node;   // index into the story's nodes[], or CYOA_POOL_CONTINUE
    const CyoaPool *enter_pool;  // set to start a pool instead of a direct jump; leave
                                  // this initializer off in story data (aggregate init
                                  // zero-fills it to nullptr) -- do NOT give it a default
                                  // member initializer, that turns CyoaChoice into a
                                  // non-aggregate under this toolchain's C++ standard and
                                  // breaks every existing positional brace-init call site
};

// A node with every choices[] slot at CYOA_BTN_NONE is an ending -- the engine doesn't
// need a separate "is this the end" flag, an empty choice list already means it (see
// screen_cyoa.cpp's render_node()).
struct CyoaNode {
    const char *body;
    CyoaChoice  choices[CYOA_MAX_CHOICES];
};

#define CYOA_MAX_ITEMS 8
#define CYOA_MAX_STATS 8

struct CyoaStory {
    const char     *title;
    const CyoaNode *nodes;
    int             node_count;
    int             start_node;
};
