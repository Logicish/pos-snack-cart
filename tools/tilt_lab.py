"""
Tilt Lab -- offline puzzle generator + difficulty calculator for the Castle Defense
replacement (working name "tilt puzzle"; final name TBD).

Rules being modeled (owner's design, 2026-09-30):
  - N x N box with a border wall. One gap in the border is the EXIT.
  - Mixed-size rectangular blocks (1x1, 1x2, 2x1, 1x3, 3x1, 2x2). Block 0 is RED.
  - A press (U/D/L/R) slides EVERY movable block as far as it can go in that
    direction at once, like tilting the box. Blocks stop against walls, fixed wall
    cells, or other blocks.
  - Fixed wall cells ('#') inside the box never move.
  - Only red can pass through the exit (it's a wall to everything else). Red
    touching the outside through the exit = solved.
  - No move limit, no par. Score in the game = boards solved. The player can reset
    a board, because a tilt can leave it in a dead state that can't be solved.

What this tool measures for each board (search over every reachable state):
  min_moves     fewest presses to solve
  states        how many distinct layouts are reachable
  dead_ratio    share of reachable layouts that can no longer be solved (traps)
  rand_moves    expected presses for a player mashing random directions (resetting
                when stuck) -- a "how lost can you get" measure
  first_ok      share of the non-wasted opening moves that still leave the board
                solvable
  opt_paths     how many different fastest solutions exist (1 = one narrow path)
  near_paths    winning press sequences up to min_moves + NEAR_SLACK long -- "how
                many ways are there to win if you're a little sloppy"
  good_ratio    average share of presses that move you closer to solved
  human_moves   expected presses for a simulated person (Jarusek & Pelanek's model,
                see HM_B below) -- this IS the difficulty (v3)
  difficulty    = human_moves; orders boards easy -> hard

Usage:
  python tools/tilt_lab.py gen     [--n 6] [--count 400] [--seed 1] [--jobs N] [--out tools/puzzles.json]
  python tools/tilt_lab.py report  [tools/puzzles.json]
  python tools/tilt_lab.py show    <id|"line"> [tools/puzzles.json]   board + optimal solution
  python tools/tilt_lab.py play    <id|"line"> [tools/puzzles.json]   w/a/s/d, r=reset, q=quit
  python tools/tilt_lab.py export  [tools/puzzles.json] [--tiers 5] [--per-tier 100]
                                   -> tools/slidefree/tier1.txt.. (copy to SD /slidefree/)
  python tools/tilt_lab.py check   tools/slidefree/tier3.txt     validate + re-solve a tier file
  python tools/tilt_lab.py regrade [tools/puzzles.json]          re-score the pool (after tuning)
  python tools/tilt_lab.py calibrate log.csv                     fit the model to the cart's real play log
"""

import argparse
import json
import math
import multiprocessing as mp
import os
import random
import sys
from collections import deque

import numpy as np

try:
    import scipy.sparse as sp
    import scipy.sparse.linalg as spla
except ImportError:  # falls back to iteration
    sp = None

# ── tuning knobs ─────────────────────────────────────────────────────────────
BLOCK_SHAPES = [  # (h, w, weight)
    (1, 1, 3), (1, 2, 3), (2, 1, 3), (1, 3, 1), (3, 1, 1), (2, 2, 1),
]
RED_SHAPES = [(1, 2, 2), (2, 1, 1), (1, 1, 1)]
WALLS_RANGE = (1, 5)       # fixed wall cells per board
BLOCKS_RANGE = (3, 7)      # movable blocks besides red
MIN_MOVES_KEEP = 3         # reject boards solvable faster than this
STATE_CAP = 20000          # give up on a board whose state space is bigger

NEAR_SLACK = 2             # near_paths counts wins up to min_moves + this

# ── human model (difficulty v3, 2026-09-30) ──────────────────────────────────
# Jarusek & Pelanek, "What Determines Difficulty of Transport Puzzles?" (FLAIRS 2011):
# on one-way puzzles (Sokoban, their Replacement puzzle -- and Slide Free, since a tilt
# can't be undone) solution length predicts human difficulty poorly; a simulated person
# wandering the state space predicts it much better. Their model: from state s, each
# successor s' gets score d(s) + B if it's closer to solved, else d(s) (d = presses to
# solved), and is picked with probability score/sum -- mostly random when far from the
# goal, increasingly focused near it. B ~= 25 fit best across all three of their puzzles.
# Plus their hill-climbing extension: +H when s' *looks* closer (red nearer the exit).
# Stuck (dead) states: wander randomly, noticing and resetting with chance RESET each
# press. difficulty = expected presses for this simulated person, solved exactly as an
# absorbing Markov chain (no sampling noise). Calibrate B/H/RESET against real play with
# `calibrate` once the cart's /slidefree/log.csv has data.
# Slide Free addition (not in the paper -- their puzzles had no free reset): a person
# resets voluntarily once things look worse than the start, so in any layout more than
# HM_LOST_MARGIN presses further from solved than the start was, they reset with chance
# HM_LOST per press. Without this, a 3-move board with one wrong-turn trap rated as ~70
# presses, because the model just wandered.
HM_B = 25.0
HM_H = 10.0
HM_RESET = 0.3
HM_LOST = 0.15
HM_LOST_MARGIN = 2

DIRS = {"U": (-1, 0), "D": (1, 0), "L": (0, -1), "R": (0, 1)}
WIN = "WIN"


class Board:
    def __init__(self, n, walls, exit_side, exit_pos, dims):
        self.n = n
        self.walls = frozenset(map(tuple, walls))
        self.exit_side = exit_side
        self.exit_pos = exit_pos
        self.dims = [tuple(d) for d in dims]  # dims[0] is red
        rh, rw = self.dims[0]
        if exit_side in "LR":
            col = -1 if exit_side == "L" else n
            self.exit_cells = {(r, col) for r in range(exit_pos, exit_pos + rh)}
        else:
            row = -1 if exit_side == "U" else n
            self.exit_cells = {(row, c) for c in range(exit_pos, exit_pos + rw)}

    def cells(self, i, r, c):
        h, w = self.dims[i]
        return [(r + y, c + x) for y in range(h) for x in range(w)]

    def tilt(self, state, d):
        """One press. Returns the new state tuple, or WIN."""
        dr, dc = DIRS[d]
        pos = list(state)
        occ = {}
        for i, (r, c) in enumerate(pos):
            for cell in self.cells(i, r, c):
                occ[cell] = i
        n = self.n
        moved = True
        while moved:
            moved = False
            for i in range(len(pos)):
                r, c = pos[i]
                nr, nc = r + dr, c + dc
                new = self.cells(i, nr, nc)
                out = False
                ok = True
                for cell in new:
                    y, x = cell
                    if 0 <= y < n and 0 <= x < n:
                        if cell in self.walls or occ.get(cell, i) != i:
                            ok = False
                            break
                    elif i == 0 and cell in self.exit_cells:
                        out = True
                    else:
                        ok = False
                        break
                if not ok:
                    continue
                if out:
                    return WIN
                for cell in self.cells(i, r, c):
                    del occ[cell]
                for cell in new:
                    occ[cell] = i
                pos[i] = (nr, nc)
                moved = True
        return tuple(pos)


# ── analysis ─────────────────────────────────────────────────────────────────
def build_graph(board, start):
    """Every reachable layout + its non-no-op presses + presses-to-solved (dist).
    None if unsolvable or too big."""
    index = {start: 0}
    states = [start]
    succ = []            # per state: list of (dir, next_index or -1 for WIN)
    q = deque([start])
    while q:
        s = q.popleft()
        edges = []
        for d in "UDLR":
            t = board.tilt(s, d)
            if t == WIN:
                edges.append((d, -1))
            elif t != s:
                if t not in index:
                    if len(states) >= STATE_CAP:
                        return None
                    index[t] = len(states)
                    states.append(t)
                    q.append(t)
                edges.append((d, index[t]))
        succ.append(edges)

    k = len(states)
    # distance to WIN from every state (reverse BFS); unreachable = dead
    INF = 1 << 30
    rev = [[] for _ in range(k)]
    dist = [INF] * k
    dq = deque()
    for i, edges in enumerate(succ):
        for d, j in edges:
            if j == -1:
                if dist[i] == INF:
                    dist[i] = 1
                    dq.append(i)
            else:
                rev[j].append(i)
    while dq:
        j = dq.popleft()
        for i in rev[j]:
            if dist[i] == INF:
                dist[i] = dist[j] + 1
                dq.append(i)
    if dist[0] == INF:
        return None
    return states, succ, dist


def analyze(board, start, params=None):
    """Full search of the reachable state graph + every metric. None if unsolvable or
    too big."""
    g = build_graph(board, start)
    if g is None:
        return None
    states, succ, dist = g
    k = len(states)
    INF = 1 << 30
    live = [x < INF for x in dist]

    # shortest solution (BFS from start, parents)
    parent = {0: None}
    dq = deque([0])
    goal = None
    while dq and goal is None:
        i = dq.popleft()
        for d, j in succ[i]:
            if j == -1:
                goal = (i, d)
                break
            if j not in parent:
                parent[j] = (i, d)
                dq.append(j)
    path = [goal[1]]
    i = goal[0]
    while parent[i] is not None:
        i, d = parent[i]
        path.append(d)
    path.reverse()

    dead = k - sum(live)
    first = [j for d, j in succ[0]]
    first_ok = sum(1 for j in first if j == -1 or live[j]) / len(first)

    rand_moves = random_walk_expectation(k, succ, live)
    human_moves = human_model_expectation(board, states, succ, dist, *(params or (HM_B, HM_H, HM_RESET, HM_LOST)))
    opt_paths = count_optimal_paths(succ, dist)
    near_paths = count_winning_walks(k, succ, len(path) + NEAR_SLACK)

    # good_ratio: over every solvable layout, the share of presses that get you
    # closer to solved -- low means most presses lead you astray
    ratios = []
    for i in range(k):
        if not live[i]:
            continue
        edges = succ[i]
        good = sum(1 for d, j in edges if j == -1 or dist[j] < dist[i])
        ratios.append(good / len(edges))
    good_ratio = sum(ratios) / len(ratios)

    return {
        "min_moves": len(path),
        "opt_paths": opt_paths,
        "near_paths": near_paths,
        "good_ratio": round(good_ratio, 3),
        "solution": "".join(path),
        "states": k,
        "dead_states": dead,
        "dead_ratio": round(dead / k, 4),
        "first_ok": round(first_ok, 3),
        "rand_moves": round(rand_moves, 1),
        "human_moves": round(human_moves, 1),
    }


def random_walk_expectation(k, succ, live):
    """Expected presses for a random masher: each press picks uniformly among the
    presses that change something; a dead state costs one 'reset' press back to the
    start. Solves E = 1 + mean(E[next]) exactly."""
    rows, cols, vals = [], [], []
    b = np.ones(k)
    for i in range(k):
        rows.append(i); cols.append(i); vals.append(1.0)
        if not live[i]:
            if i != 0:
                rows.append(i); cols.append(0); vals.append(-1.0)
            continue
        edges = succ[i]
        p = 1.0 / len(edges)
        for d, j in edges:
            if j != -1:
                rows.append(i); cols.append(j); vals.append(-p)
    if sp is not None:
        A = sp.csr_matrix((vals, (rows, cols)), shape=(k, k))
        E = spla.spsolve(A.tocsc(), b)
    else:
        A = np.zeros((k, k))
        for r_, c_, v in zip(rows, cols, vals):
            A[r_, c_] += v
        E = np.linalg.solve(A, b)
    return float(E[0])


def count_optimal_paths(succ, dist):
    """Number of distinct fastest move sequences from the start (state 0)."""
    order = sorted((i for i in range(len(succ)) if dist[i] < (1 << 30)), key=lambda i: dist[i])
    cnt = {}
    for i in order:
        c = 0
        for d, j in succ[i]:
            if j == -1:
                c += 1 if dist[i] == 1 else 0
            elif dist[j] == dist[i] - 1:
                c += cnt[j]
        cnt[i] = c
    return cnt[0]


def count_winning_walks(k, succ, max_len):
    """Winning press sequences from the start of length <= max_len (no-op presses
    excluded) -- 'how many ways are there to win if you're a little sloppy'."""
    rows, cols = [], []
    w = np.zeros(k)
    for i, edges in enumerate(succ):
        for d, j in edges:
            if j == -1:
                w[i] += 1
            else:
                rows.append(i); cols.append(j)
    f = w.copy()
    total = f[0]
    if sp is not None:
        A = sp.csr_matrix((np.ones(len(rows)), (rows, cols)), shape=(k, k))
        for _ in range(max_len - 1):
            f = A @ f
            total += f[0]
    else:
        for _ in range(max_len - 1):
            g = np.zeros(k)
            np.add.at(g, rows, f[cols])
            f = g
            total += f[0]
    return int(total)


def red_distance(board, state):
    """How far red *looks* from the exit: cells off the exit's line + cells to travel
    to the wall. What a person eyeballs, ignoring every other block."""
    (r, c), (h, w), n, p = state[0], board.dims[0], board.n, board.exit_pos
    side = board.exit_side
    if side == "R":
        return abs(r - p) + (n - (c + w))
    if side == "L":
        return abs(r - p) + c
    if side == "D":
        return abs(c - p) + (n - (r + h))
    return abs(c - p) + r


def human_model_expectation(board, states, succ, dist, B, H, reset, lost):
    """Expected presses for the simulated person (see HM_B above), solved exactly."""
    k = len(states)
    INF = 1 << 30
    hd = [red_distance(board, st) for st in states]
    rows, cols, vals = [], [], []
    b = np.ones(k)
    for i in range(k):
        rows.append(i); cols.append(i); vals.append(1.0)
        edges = succ[i]
        if dist[i] == INF:
            # stuck: reset (1 press, then start over) with chance `reset`, else wander
            if not edges:
                rows.append(i); cols.append(0); vals.append(-1.0)
                b[i] = 2.0  # this press + the reset press
                continue
            rows.append(i); cols.append(0); vals.append(-reset)
            b[i] = 1.0 + reset
            p = (1 - reset) / len(edges)
            for d, j in edges:
                rows.append(i); cols.append(j); vals.append(-p)
            continue
        # clearly worse off than the start: maybe give up and reset
        give_up = lost if dist[i] > dist[0] + HM_LOST_MARGIN else 0.0
        if give_up:
            rows.append(i); cols.append(0); vals.append(-give_up)
            b[i] = 1.0 + give_up
        scores = []
        for d, j in edges:
            closer = j == -1 or dist[j] < dist[i]
            looks = (j == -1) or hd[j] < hd[i]
            scores.append(dist[i] + (B if closer else 0) + (H if looks else 0))
        tot = sum(scores)
        for (d, j), sc in zip(edges, scores):
            if j != -1:
                rows.append(i); cols.append(j); vals.append(-(1 - give_up) * sc / tot)
    if sp is not None:
        A = sp.csr_matrix((vals, (rows, cols)), shape=(k, k))
        E = spla.spsolve(A.tocsc(), b)
    else:
        A = np.zeros((k, k))
        for r_, c_, v in zip(rows, cols, vals):
            A[r_, c_] += v
        E = np.linalg.solve(A, b)
    return float(E[0])


def difficulty(m):
    """v3: expected presses for the simulated person -- 'a typical player needs about
    this many presses'. The v2 weighted mix is kept below as difficulty_v2 for
    comparison only."""
    return round(m["human_moves"], 1)


V2_WEIGHTS = {"min_moves": 1.0, "log2_near_paths": -1.0, "log2_rand_moves": 1.0,
              "astray": 6.0, "dead_ratio": 3.0}


def difficulty_v2(m):
    f = {
        "min_moves": m["min_moves"],
        "log2_near_paths": math.log2(max(1, m["near_paths"])),
        "log2_rand_moves": math.log2(max(1.0, m["rand_moves"])),
        "astray": 1 - m["good_ratio"],
        "dead_ratio": m["dead_ratio"],
    }
    return round(sum(V2_WEIGHTS[key] * f[key] for key in V2_WEIGHTS), 2)


# ── generation ───────────────────────────────────────────────────────────────
def weighted(rng, shapes):
    total = sum(s[2] for s in shapes)
    x = rng.uniform(0, total)
    for h, w, wt in shapes:
        x -= wt
        if x <= 0:
            return (h, w)
    return shapes[-1][:2]


def random_board(rng, n):
    red = weighted(rng, RED_SHAPES)
    side = rng.choice("UDLR")
    span = red[0] if side in "LR" else red[1]
    exit_pos = rng.randint(0, n - span)

    taken = set()
    walls = []
    for _ in range(rng.randint(*WALLS_RANGE)):
        cell = (rng.randrange(n), rng.randrange(n))
        if cell not in taken:
            taken.add(cell)
            walls.append(cell)

    dims, pos = [], []

    def place(h, w):
        for _ in range(40):
            r, c = rng.randint(0, n - h), rng.randint(0, n - w)
            cells = {(r + y, c + x) for y in range(h) for x in range(w)}
            if not cells & taken:
                taken.update(cells)
                return (r, c)
        return None

    p = place(*red)
    if p is None:
        return None
    dims.append(red); pos.append(p)
    for _ in range(rng.randint(*BLOCKS_RANGE)):
        shape = weighted(rng, BLOCK_SHAPES)
        p = place(*shape)
        if p is not None:
            dims.append(shape); pos.append(p)

    board = Board(n, walls, side, exit_pos, dims)
    return board, tuple(pos)


def board_to_json(board, start):
    return {
        "n": board.n,
        "walls": sorted(board.walls),
        "exit": [board.exit_side, board.exit_pos],
        "blocks": [[r, c, h, w] for (r, c), (h, w) in zip(start, board.dims)],
    }


def board_from_json(p):
    dims = [(b[2], b[3]) for b in p["blocks"]]
    start = tuple((b[0], b[1]) for b in p["blocks"])
    return Board(p["n"], p["walls"], p["exit"][0], p["exit"][1], dims), start


def gen_one(seed_n):
    """Worker: one random board from its own seed. Returns a kept board dict or None."""
    seed, n = seed_n
    made = random_board(random.Random(seed), n)
    if made is None:
        return None
    board, start = made
    m = analyze(board, start)
    if m is None or m["min_moves"] < MIN_MOVES_KEEP:
        return None
    m["difficulty"] = difficulty(m)
    m["difficulty_v2"] = difficulty_v2(m)
    return {**board_to_json(board, start), "metrics": m}


def cmd_gen(args):
    out, seen = [], set()
    tries = 0
    seeds = ((args.seed * 10_000_000 + i, args.n) for i in range(10**9))
    with mp.Pool(args.jobs) as pool:
        for p in pool.imap_unordered(gen_one, seeds, chunksize=4):
            tries += 1
            if p is None:
                continue
            key = json.dumps({k: p[k] for k in ("n", "walls", "exit", "blocks")}, sort_keys=True)
            if key in seen:
                continue
            seen.add(key)
            out.append(p)
            if len(out) % 50 == 0:
                print(f"  {len(out)}/{args.count} kept ({tries} tried)", file=sys.stderr, flush=True)
            if len(out) >= args.count:
                pool.terminate()
                break
    out.sort(key=lambda p: p["metrics"]["difficulty"])
    for i, p in enumerate(out):
        p["id"] = i
    with open(args.out, "w") as f:
        json.dump(out, f, indent=1)
    print(f"wrote {len(out)} boards to {args.out} ({tries} generated)")


# ── SD line format ───────────────────────────────────────────────────────────
# One board per line:   <min_moves> <exit> <row0>|<row1>|...|<rowN-1>
#   exit  = side U/D/L/R + index where the gap starts (row for L/R, column for U/D);
#           the gap is as wide as red
#   grid  = '.' empty, '#' fixed wall, 'R' red, any other letter = one block (its
#           cells must form a filled rectangle)
# min_moves is informational (the game ignores it) and may be omitted in a
# hand-drawn line. Lines starting with ';' are comments.
BLOCK_LETTERS = "abcdefghijklmnopqstuvwxyz"  # no 'r', too easy to misread next to 'R'


def board_to_line(board, start, min_moves=None):
    n = board.n
    grid = [["." for _ in range(n)] for _ in range(n)]
    for (r, c) in board.walls:
        grid[r][c] = "#"
    for i, (r, c) in enumerate(start):
        ch = "R" if i == 0 else BLOCK_LETTERS[i - 1]
        for (y, x) in board.cells(i, r, c):
            grid[y][x] = ch
    rows = "|".join("".join(row) for row in grid)
    head = f"{min_moves} " if min_moves is not None else ""
    return f"{head}{board.exit_side}{board.exit_pos} {rows}"


def board_from_line(line):
    """Parses one SD line. Returns (board, start, min_moves or None); raises ValueError."""
    parts = line.split()
    if len(parts) == 3:
        min_moves, ex, grid = int(parts[0]), parts[1], parts[2]
    elif len(parts) == 2:
        min_moves, (ex, grid) = None, parts
    else:
        raise ValueError("expected '<min> <exit> <grid>' or '<exit> <grid>'")
    rows = grid.split("|")
    n = len(rows)
    if any(len(r) != n for r in rows):
        raise ValueError(f"grid must be {n}x{n}")
    side, pos = ex[0].upper(), int(ex[1:])
    if side not in "UDLR":
        raise ValueError(f"bad exit side {ex[0]!r}")
    walls, cells = [], {}
    for r, row in enumerate(rows):
        for c, ch in enumerate(row):
            if ch == "#":
                walls.append((r, c))
            elif ch != ".":
                cells.setdefault(ch, []).append((r, c))
    if "R" not in cells:
        raise ValueError("no red block 'R'")
    dims, start = [], []
    for ch in ["R"] + sorted(k for k in cells if k != "R"):
        pts = cells[ch]
        r0, r1 = min(q[0] for q in pts), max(q[0] for q in pts)
        c0, c1 = min(q[1] for q in pts), max(q[1] for q in pts)
        h, w = r1 - r0 + 1, c1 - c0 + 1
        if len(pts) != h * w:
            raise ValueError(f"block {ch!r} isn't a filled rectangle")
        dims.append((h, w))
        start.append((r0, c0))
    span = dims[0][0] if side in "LR" else dims[0][1]
    if pos < 0 or pos + span > n:
        raise ValueError("exit gap runs off the edge")
    return Board(n, walls, side, pos, dims), tuple(start), min_moves


# ── display ──────────────────────────────────────────────────────────────────
def render(board, state):
    n = board.n
    grid = [["." for _ in range(n)] for _ in range(n)]
    for (r, c) in board.walls:
        grid[r][c] = "#"
    for i, (r, c) in enumerate(state):
        ch = "R" if i == 0 else chr(ord("a") + (i - 1) % 26)
        for (y, x) in board.cells(i, r, c):
            grid[y][x] = ch
    lines = []
    ex = board.exit_cells
    top = "+" + "".join("  " if (-1, c) in ex else "--" for c in range(n)) + "+"
    bot = "+" + "".join("  " if (n, c) in ex else "--" for c in range(n)) + "+"
    lines.append(top)
    for r in range(n):
        left = " " if (r, -1) in ex else "|"
        right = "  <- exit" if (r, n) in ex else "|"
        right = " " + right if (r, n) in ex else right
        lines.append(left + "".join(f" {ch}" for ch in grid[r]) + right)
    lines.append(bot)
    return "\n".join(lines)


def load(path):
    with open(path) as f:
        return json.load(f)


def cmd_report(args):
    ps = load(args.file)
    ms = [p["metrics"] for p in ps]

    def col(key):
        return np.array([m[key] for m in ms], dtype=float)

    print(f"{len(ps)} boards\n")
    print(f"{'metric':<12}{'min':>8}{'p25':>8}{'median':>8}{'p75':>8}{'max':>9}")
    for key in ("min_moves", "opt_paths", "near_paths", "good_ratio", "states", "dead_ratio", "rand_moves", "human_moves"):
        v = col(key)
        print(f"{key:<12}" + "".join(f"{x:>8.2f}" for x in np.percentile(v, [0, 25, 50, 75])) + f"{v.max():>9.2f}")

    print("\nmin_moves histogram:")
    mm = col("min_moves").astype(int)
    for k in range(mm.min(), mm.max() + 1):
        c = int((mm == k).sum())
        print(f"  {k:>2} moves: {c:>4} {'#' * min(60, c)}")

    d = col("difficulty")
    print("\ncorrelation with difficulty:")
    for key in ("min_moves", "opt_paths", "near_paths", "good_ratio", "rand_moves", "dead_ratio", "difficulty_v2"):
        print(f"  {key:<11} {np.corrcoef(col(key), d)[0, 1]:+.2f}")

    # The owner's point, checked: same min_moves, very different difficulty
    print("\nsame min_moves, easiest vs hardest by difficulty:")
    for target in (6, 10):
        group = [p for p in ps if p["metrics"]["min_moves"] == target]
        if len(group) < 2:
            continue
        for label, p in (("easiest", group[0]), ("hardest", group[-1])):
            m = p["metrics"]
            print(f"  {target} moves {label:<8}#{p['id']:<5} difficulty {m['difficulty']:>6}  "
                  f"opt_paths {m['opt_paths']:<4} near_paths {m['near_paths']:<6} good {m['good_ratio']}")

    print("\nsamples, easy -> hard (id: difficulty):")
    for q in (0, 0.25, 0.5, 0.75, 1.0):
        p = ps[min(len(ps) - 1, int(q * (len(ps) - 1)))]
        board, start = board_from_json(p)
        m = p["metrics"]
        print(f"\n#{p['id']}  difficulty {m['difficulty']}  min {m['min_moves']}  "
              f"states {m['states']}  dead {m['dead_ratio']:.0%}  random {m['rand_moves']}")
        print(render(board, start))


def resolve(args):
    """A pool id, or a board line pasted in quotes. Returns (board, start, metrics, label)."""
    if args.board.strip().isdigit():
        p = load(args.file)[int(args.board)]
        board, start = board_from_json(p)
        return board, start, p["metrics"], f"#{p['id']}"
    board, start, _ = board_from_line(args.board)
    m = analyze(board, start)
    if m is None:
        sys.exit("that board is unsolvable (or too big to search)")
    m["difficulty"] = difficulty(m)
    return board, start, m, "pasted board"


def cmd_show(args):
    board, start, m, label = resolve(args)
    print(label)
    print(board_to_line(board, start, m["min_moves"]))
    print(json.dumps(m, indent=1))
    p = {"metrics": m}
    s = start
    print("\nstart")
    print(render(board, s))
    for d in p["metrics"]["solution"]:
        s = board.tilt(s, d)
        print(f"\n{d}")
        print("SOLVED" if s == WIN else render(board, s))


def cmd_play(args):
    board, start, m, label = resolve(args)
    p = {"metrics": m}
    keys = {"w": "U", "s": "D", "a": "L", "d": "R"}
    s, moves = start, 0
    print(f"{label}  (optimal: {m['min_moves']} moves)  w/a/s/d, r=reset, q=quit")
    while True:
        print(render(board, s))
        k = input(f"moves {moves}> ").strip().lower()[:1]
        if k == "q":
            return
        if k == "r":
            s = start
            continue
        if k in keys:
            t = board.tilt(s, keys[k])
            if t == s:
                continue
            moves += 1
            if t == WIN:
                print(f"SOLVED in {moves} (optimal {p['metrics']['min_moves']})")
                return
            s = t


def cmd_export(args):
    """Pool -> tier files for the SD card. Drops the hardest TRIM of the pool (the
    30-45 move monsters), splits the rest into equal-count difficulty bands, and takes
    per_tier boards spread evenly across each band."""
    ps = sorted(load(args.file), key=lambda p: p["metrics"]["difficulty"])
    ps = ps[: int(len(ps) * (1 - args.trim))]
    band = len(ps) // args.tiers
    if band < args.per_tier:
        sys.exit(f"pool too small: {len(ps)} usable boards, need {args.tiers * args.per_tier}"
                 f" -- run gen with a bigger --count")
    os.makedirs(args.outdir, exist_ok=True)
    for t in range(args.tiers):
        chunk = ps[t * band:(t + 1) * band]
        step = len(chunk) / args.per_tier
        pick = [chunk[int(i * step)] for i in range(args.per_tier)]
        mm = [p["metrics"]["min_moves"] for p in pick]
        dd = [p["metrics"]["difficulty"] for p in pick]
        path = os.path.join(args.outdir, f"tier{t + 1}.txt")
        with open(path, "w", newline="\n") as f:
            f.write(f"; Slide Free - tier {t + 1} of {args.tiers}, {len(pick)} boards\n")
            f.write(f"; difficulty {min(dd)}-{max(dd)}, min moves {min(mm)}-{max(mm)}\n")
            f.write("; format: <min_moves> <exit side+pos> <rows top to bottom, '|' separated>\n")
            f.write("; '.' empty  '#' wall  'R' red  other letters = blocks. Made by tools/tilt_lab.py\n")
            for p in pick:
                board, start = board_from_json(p)
                f.write(board_to_line(board, start, p["metrics"]["min_moves"]) + "\n")
        print(f"tier {t + 1}: {len(pick)} boards  difficulty {min(dd):>6}-{max(dd):<6} "
              f"min moves {min(mm)}-{max(mm)} (median {sorted(mm)[len(mm) // 2]})  -> {path}")


def cmd_check(args):
    """Validates a tier file the way the cart will read it, and re-solves every board."""
    bad = ok = 0
    diffs = []
    for no, raw in enumerate(open(args.file), 1):
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        try:
            board, start, mm = board_from_line(line)
        except ValueError as e:
            print(f"line {no}: {e}")
            bad += 1
            continue
        m = analyze(board, start)
        if m is None:
            print(f"line {no}: unsolvable or too big")
            bad += 1
            continue
        if mm is not None and mm != m["min_moves"]:
            print(f"line {no}: says {mm} moves, actually {m['min_moves']}")
        ok += 1
        diffs.append(difficulty(m))
    print(f"{ok} good, {bad} bad" + (f", difficulty {min(diffs)}-{max(diffs)}" if diffs else ""))


def regrade_one(p):
    board, start = board_from_json(p)
    m = analyze(board, start)
    if m is None:
        return None
    m["difficulty"] = difficulty(m)
    m["difficulty_v2"] = difficulty_v2(m)
    return {**{k: p[k] for k in ("n", "walls", "exit", "blocks")}, "metrics": m}


def cmd_regrade(args):
    """Re-scores every board in the pool with the current model settings."""
    ps = load(args.file)
    with mp.Pool(args.jobs) as pool:
        out = [p for p in pool.imap(regrade_one, ps, chunksize=8) if p]
    out.sort(key=lambda p: p["metrics"]["difficulty"])
    for i, p in enumerate(out):
        p["id"] = i
    with open(args.file, "w") as f:
        json.dump(out, f, indent=1)
    v2 = np.array([p["metrics"]["difficulty_v2"] for p in out])
    v3 = np.array([p["metrics"]["difficulty"] for p in out])
    mm = np.array([p["metrics"]["min_moves"] for p in out])
    from scipy.stats import spearmanr
    print(f"regraded {len(out)} boards")
    print(f"rank agreement with old v2 score:   {spearmanr(v2, v3)[0]:+.2f}")
    print(f"rank agreement with min_moves:      {spearmanr(mm, v3)[0]:+.2f}")


def calib_one(item):
    line, grid = item
    board, start, _ = board_from_line(line)
    g = build_graph(board, start)
    if g is None:
        return None
    states, succ, dist = g
    return [human_model_expectation(board, states, succ, dist, B, H, R, L) for (B, H, R, L) in grid]


def cmd_calibrate(args):
    """Fits the human model's B/H/RESET to real play logged by the cart.
    Log line (SD /slidefree/log.csv): tier,presses,resets,seconds,solved,<board line>"""
    from scipy.stats import spearmanr
    per = {}
    for raw in open(args.log):
        parts = raw.strip().split(",", 5)
        if len(parts) != 6 or not parts[0].isdigit():
            continue
        tier, presses, resets, secs, solved, line = parts
        per.setdefault(line, []).append((int(presses), int(resets), float(secs), int(solved)))
    boards = [(line, runs) for line, runs in per.items() if any(r[3] for r in runs)]
    if len(boards) < 8:
        sys.exit(f"only {len(boards)} solved boards in the log -- play more first (8+ needed, 30+ is better)")
    # human difficulty per board: mean seconds over solves, and mean presses
    secs = np.array([np.mean([r[2] for r in runs if r[3]]) for _, runs in boards])
    press = np.array([np.mean([r[0] for r in runs if r[3]]) for _, runs in boards])
    grid = [(B, H, R, L) for B in (0, 5, 10, 25, 50, 100) for H in (0, 5, 10, 25, 50)
            for R in (0.1, 0.3, 0.6) for L in (0.0, 0.05, 0.15, 0.3)]
    with mp.Pool(args.jobs) as pool:
        res = list(pool.imap(calib_one, [(line, grid) for line, _ in boards]))
    keep = [i for i, r in enumerate(res) if r is not None]
    M = np.array([res[i] for i in keep])
    secs, press = secs[keep], press[keep]
    mins = np.array([analyze(*board_from_line(boards[i][0])[:2])["min_moves"] for i in keep])
    print(f"{len(keep)} boards with real solves\n")
    print(f"baseline  min_moves vs seconds: {spearmanr(mins, secs)[0]:+.2f}   vs presses: {spearmanr(mins, press)[0]:+.2f}")
    scored = sorted(((spearmanr(M[:, g], secs)[0], spearmanr(M[:, g], press)[0], grid[g]) for g in range(len(grid))),
                    reverse=True)
    print("\nbest model settings (rank correlation with real seconds / presses):")
    for rs, rp, (B, H, R, L) in scored[:8]:
        print(f"  B={B:<4} H={H:<3} RESET={R:<4} LOST={L:<5} seconds {rs:+.2f}   presses {rp:+.2f}")
    B, H, R, L = scored[0][2]
    print(f"\nset HM_B = {B}, HM_H = {H}, HM_RESET = {R}, HM_LOST = {L} at the top of tilt_lab.py, then run regrade + export")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gen")
    g.add_argument("--n", type=int, default=6)
    g.add_argument("--count", type=int, default=400)
    g.add_argument("--seed", type=int, default=1)
    g.add_argument("--out", default="tools/puzzles.json")
    g.add_argument("--jobs", type=int, default=os.cpu_count())
    r = sub.add_parser("report")
    r.add_argument("file", nargs="?", default="tools/puzzles.json")
    for name in ("show", "play"):
        s = sub.add_parser(name)
        s.add_argument("board", help="pool id, or a board line in quotes")
        s.add_argument("file", nargs="?", default="tools/puzzles.json")
    e = sub.add_parser("export")
    e.add_argument("file", nargs="?", default="tools/puzzles.json")
    e.add_argument("--tiers", type=int, default=5)
    e.add_argument("--per-tier", type=int, default=100)
    e.add_argument("--trim", type=float, default=0.02)
    e.add_argument("--outdir", default="tools/slidefree")
    c = sub.add_parser("check")
    c.add_argument("file")
    rg = sub.add_parser("regrade")
    rg.add_argument("file", nargs="?", default="tools/puzzles.json")
    rg.add_argument("--jobs", type=int, default=os.cpu_count())
    cb = sub.add_parser("calibrate")
    cb.add_argument("log", help="log.csv copied off the cart's SD card")
    cb.add_argument("--jobs", type=int, default=os.cpu_count())
    args = ap.parse_args()
    {"gen": cmd_gen, "report": cmd_report, "show": cmd_show, "play": cmd_play,
     "export": cmd_export, "check": cmd_check, "regrade": cmd_regrade,
     "calibrate": cmd_calibrate}[args.cmd](args)


if __name__ == "__main__":
    main()
