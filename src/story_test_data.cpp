#include "story_test_data.h"

/*
  Author--- LogicishDesigns
  Date----- September 2026
  Function- Data for the CYOA engine smoke-test story declared in story_test_data.h.
            Exercises every engine feature (item grant/consume, a stat requirement/
            effect, multiple choice buttons on one node, two distinct endings, and --
            added 2026-09-15 -- a CyoaPool encounter run) in as few nodes as that takes.
            Not meant to be good writing, just coverage.
*/

// Item/stat ids -- indices into the engine's runtime arrays, named here for readability.
#define ITEM_COIN   0
#define STAT_SNACKS 0

// A tiny 3-room/2-hit encounter pool, proving CyoaPool (cyoa_engine.h) actually works
// end to end before any real story (Elixir Run's own 5-room/3-hit pools) gets built
// against it. Declared ahead of NODES so &POOL_TEST is valid inside it; room ids point
// forward into NODES (7-12, added below the original 7 smoke-test nodes) -- fine since
// those are plain ints, not addresses, so no ordering issue there.
static const CyoaPoolRoom POOL_TEST_ROOMS[] = {
    { 7,  8  },
    { 9,  10 },
    { 11, 12 },
};
static const CyoaPool POOL_TEST = { POOL_TEST_ROOMS, 3, 2, 4 };

static const CyoaNode NODES[] = {
    // 0: start
    {
        "You spot a vending machine\nhumming quietly in the break\nroom.",
        {
            { CYOA_BTN_ENTER, "Check your pocket",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_GRANT_ITEM, ITEM_COIN, 0 }, 1 },
            { CYOA_BTN_LEFT, "Walk away, not hungry",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, 5 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    // 1: found a coin
    {
        "You found a coin!",
        {
            { CYOA_BTN_ENTER, "Insert it",
              { CYOA_REQ_HAS_ITEM, ITEM_COIN, 0 }, { CYOA_EFF_CONSUME_ITEM, ITEM_COIN, 0 }, 2 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    // 2: machine drops a snack
    {
        "The machine whirs and\ndrops a snack!",
        {
            { CYOA_BTN_ENTER, "Grab it",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_ADJUST_STAT, STAT_SNACKS, 1 }, 3 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    // 3: try for another? -- tests a stat-gated choice (trivially true here, but a real
    // requirement check, not a hardcoded branch). UP now also tests CyoaPool: enters
    // POOL_TEST instead of jumping straight to node 4 -- next_node (4) is set but unused
    // once enter_pool is set, kept only because it happens to be POOL_TEST's own exit_node.
    {
        "Feeling snackier already.\nTry for another?",
        {
            { CYOA_BTN_UP, "Search for more change",
              { CYOA_REQ_STAT_GTE, STAT_SNACKS, 1 }, { CYOA_EFF_NONE, 0, 0 }, 4, &POOL_TEST },
            { CYOA_BTN_DOWN, "That's enough",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, 6 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    // 4: no more coins
    {
        "No more coins in sight.",
        {
            { CYOA_BTN_ENTER, "Head back to work",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, 6 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    // 5: ending -- walked away
    {
        "You walk away, stomach\ngrumbling.\n\nTHE END.",
        {
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    // 6: ending -- snack acquired
    {
        "Snack acquired. Back to\nthe grind.\n\nTHE END.",
        {
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    // 7-12: POOL_TEST's three rooms (hit/skip pair each) -- reached only via node 3's UP
    // choice. Each "Keep looking" choice hands control back to the active pool via
    // CYOA_POOL_CONTINUE rather than naming a fixed next node.
    { // 7: room A, hit
        "A coin winks up at you from\na crack in the tile.",
        {
            { CYOA_BTN_ENTER, "Keep looking",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, CYOA_POOL_CONTINUE },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    { // 8: room A, skip
        "Nothing under this vent.",
        {
            { CYOA_BTN_ENTER, "Keep looking",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, CYOA_POOL_CONTINUE },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    { // 9: room B, hit
        "Another coin, wedged by\nthe wall.",
        {
            { CYOA_BTN_ENTER, "Keep looking",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, CYOA_POOL_CONTINUE },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    { // 10: room B, skip
        "Just dust bunnies here.",
        {
            { CYOA_BTN_ENTER, "Keep looking",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, CYOA_POOL_CONTINUE },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    { // 11: room C, hit
        "One more coin, tucked in\nthe corner.",
        {
            { CYOA_BTN_ENTER, "Keep looking",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, CYOA_POOL_CONTINUE },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
    { // 12: room C, skip
        "Empty-handed again.",
        {
            { CYOA_BTN_ENTER, "Keep looking",
              { CYOA_REQ_NONE, 0, 0 }, { CYOA_EFF_NONE, 0, 0 }, CYOA_POOL_CONTINUE },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
            { CYOA_BTN_NONE, nullptr, {}, {}, 0 },
        },
    },
};

const CyoaStory STORY_TEST = {
    "VENDING MACHINE (TEST)",
    NODES,
    sizeof(NODES) / sizeof(NODES[0]),
    0,
};
