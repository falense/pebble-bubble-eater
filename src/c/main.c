#include <pebble.h>

// ---------------------------------------------------------------------------
// BubbleEater — an Agar.io-style game for Pebble Time 2 (emery), IMU-controlled
// ---------------------------------------------------------------------------

#define TICK_MS 33  // ~30 fps

// Testing aid: start at the upgrade screen instead of in the game.
// Set to 0 for release builds.
#define DEBUG_START_AT_UPGRADE 0

// World is larger than the screen; the camera follows the player.
#define WORLD_W 600
#define WORLD_H 600

// Positions and velocities use 24.8 fixed point (1 px = 256 units).
#define FP_SHIFT 8
#define FP_ONE (1 << FP_SHIFT)
#define TO_FP(v) ((v) << FP_SHIFT)
#define TO_PX(v) ((v) >> FP_SHIFT)

#define FOOD_COUNT 36
#define FOOD_RADIUS 2
// Food is worth more area than its drawn size (as if r=5) so growth is
// snappy early and tapers naturally as the bubble gets bigger.
#define FOOD_VALUE 25
#define ENEMY_COUNT 6

#define PLAYER_START_RADIUS 8
#define MAX_RADIUS 70

// Tilt (milli-G) is clamped to this before mapping to velocity.
#define TILT_MAX 500
// Dead zone so the bubble rests when the watch is held roughly level.
#define TILT_DEADZONE 60

// Speed: v_fp = tilt * SPEED_COEF / (SPEED_BASE + radius)
#define SPEED_COEF 28
#define SPEED_BASE 10

// An eater must be at least 10% larger (in radius) to eat another cell.
#define EAT_RATIO_NUM 11
#define EAT_RATIO_DEN 10

typedef enum {
  STATE_RUNNING,
  STATE_PAUSED,
  STATE_UPGRADE,  // player hit MAX_RADIUS; picking an upgrade for the next round
  STATE_REPLACE,  // all 3 slots full; choosing which owned upgrade to drop
  STATE_GAME_OVER,
} GameState;

// The upgrade pool. Actives are triggered with UP/DOWN during play, so at
// most 2 can be owned at once; passives just work.
typedef enum {
  UPG_DASH,     // active: burst of speed, costs mass
  UPG_GHOST,    // active: briefly untouchable (and can't eat enemies)
  UPG_FREEZE,   // active: enemies stop moving for a moment
  UPG_MAGNET,   // passive: nearby food drifts toward you
  UPG_VAMPIRE,  // passive: grazing a bigger enemy siphons its mass
  UPG_CAMO,     // passive: enemies can't see you while you hold still
  UPG_TRAIL,    // passive: you leave a trail that slows enemies crossing it
  UPG_MITOSIS,  // passive: eaten enemies burst into food pellets
  UPG_VIRUS,    // passive: spiky viruses roam that pop enemies, not you
  UPG_COUNT,
} UpgradeId;

#define UPG_NONE (-1)
#define MAX_OWNED 3
#define MAX_UPG_LEVEL 3

typedef struct {
  int32_t x, y;   // fixed point, world coordinates
  int32_t mass;   // area in px²; source of truth for size
  int16_t r;      // radius in px, derived from mass
  bool alive;
} Cell;

typedef struct {
  int32_t x, y;
  bool alive;
} Food;

static Window *s_window;
static Layer *s_game_layer;
static AppTimer *s_timer;

static GameState s_state;
static Cell s_player;
static Cell s_enemies[ENEMY_COUNT];
static Food s_food[FOOD_COUNT];
static int s_score;
static int32_t s_cam_x, s_cam_y;  // top-left of camera, px

// Roguelite loop: reaching MAX_RADIUS ends the round; the player picks one
// upgrade (max 3 owned, repeat picks level up), shrinks back to start size
// and plays on against faster enemies.
static int s_level;  // current round, 1-based
static int8_t s_owned[MAX_OWNED];
static int8_t s_owned_lvl[MAX_OWNED];
static int8_t s_offers[3];  // UpgradeId per button: UP, SELECT, DOWN
static int8_t s_pending;    // new upgrade awaiting a slot in STATE_REPLACE

static const char *UPG_NAMES[UPG_COUNT] = {
    "Dash", "Ghost", "Freeze", "Magnet", "Vampire", "Camo", "Trail", "Mitosis", "Virus",
};
static const bool UPG_IS_ACTIVE[UPG_COUNT] = {true, true, true, false, false,
                                              false, false, false, false};

// Active-ability timers, in ticks. *_ticks = time left while in effect,
// *_cd = cooldown until usable again.
static int16_t s_dash_ticks, s_dash_cd;
static int16_t s_ghost_ticks, s_ghost_cd;
static int16_t s_freeze_ticks, s_freeze_cd;
static bool s_camo_hidden;
static bool s_vamp_link[ENEMY_COUNT];  // enemy being siphoned this tick

// Extra food spawned by Mitosis; does not respawn when eaten.
#define MITOSIS_MAX 10
static Food s_mfood[MITOSIS_MAX];

// Viruses pop enemies that touch them; the player (who owns the upgrade)
// is immune.
#define VIRUS_MAX 4
#define VIRUS_RADIUS 7
static Food s_virus[VIRUS_MAX];

// Trail: recent player positions in world px, ring buffer. Recorded every
// 2 ticks; the level caps how many points persist (60/90/120 ≈ 4/6/8 s).
#define TRAIL_MAX 120
static GPoint s_trail[TRAIL_MAX];
static uint8_t s_trail_len, s_trail_head, s_trail_tick;

// --- helpers ---------------------------------------------------------------

static int32_t isqrt32(int32_t v) {
  if (v <= 0) {
    return 0;
  }
  int32_t res = 0;
  int32_t bit = 1 << 30;
  while (bit > v) {
    bit >>= 2;
  }
  while (bit != 0) {
    if (v >= res + bit) {
      v -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return res;
}

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

static int rand_range(int lo, int hi) {
  return lo + rand() % (hi - lo + 1);
}

static bool within_dist(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t max_dist_px) {
  if (max_dist_px <= 0) {
    return false;
  }
  int32_t dx = TO_PX(ax - bx);
  int32_t dy = TO_PX(ay - by);
  return dx * dx + dy * dy <= max_dist_px * max_dist_px;
}

// Food is eaten on any touch; a cell must be mostly engulfed (Agar.io rule:
// the eater's edge must reach past all but a third of the prey's radius).
static bool touches_food(const Cell *eater, const Food *f) {
  return within_dist(eater->x, eater->y, f->x, f->y, eater->r + FOOD_RADIUS);
}

static bool engulfs(const Cell *eater, const Cell *prey) {
  return within_dist(eater->x, eater->y, prey->x, prey->y, eater->r - prey->r / 3);
}

// Growth accumulates in mass (area) so small meals aren't lost to integer
// radius truncation; the px radius is derived on every change.
static void grow(Cell *c, int32_t eaten_mass) {
  c->mass = clamp32(c->mass + eaten_mass, 1, MAX_RADIUS * MAX_RADIUS);
  c->r = (int16_t)isqrt32(c->mass);
}

// --- upgrades ----------------------------------------------------------------

static int owned_index(int id) {
  for (int i = 0; i < MAX_OWNED; i++) {
    if (s_owned[i] == id) {
      return i;
    }
  }
  return -1;
}

// 0 if not owned, else 1..MAX_UPG_LEVEL.
static int upg_level(int id) {
  int idx = owned_index(id);
  return idx < 0 ? 0 : s_owned_lvl[idx];
}

static int actives_owned(void) {
  int n = 0;
  for (int i = 0; i < MAX_OWNED; i++) {
    if (s_owned[i] != UPG_NONE && UPG_IS_ACTIVE[(int)s_owned[i]]) {
      n++;
    }
  }
  return n;
}

// UP triggers the first owned active (slot 0), DOWN the second (slot 1).
static int active_in_slot(int slot) {
  int seen = 0;
  for (int i = 0; i < MAX_OWNED; i++) {
    int id = s_owned[i];
    if (id != UPG_NONE && UPG_IS_ACTIVE[id]) {
      if (seen == slot) {
        return id;
      }
      seen++;
    }
  }
  return UPG_NONE;
}

static bool player_hidden(void) {
  return s_ghost_ticks > 0 || s_camo_hidden;
}

static void trigger_active(int id) {
  int lvl = upg_level(id);
  switch (id) {
    case UPG_DASH:
      if (s_dash_cd == 0) {
        // Costs 10/8/6% of current mass, but never below start size.
        int32_t cost = s_player.mass * (12 - 2 * lvl) / 100;
        int32_t floor_mass = PLAYER_START_RADIUS * PLAYER_START_RADIUS;
        if (s_player.mass - cost < floor_mass) {
          cost = s_player.mass - floor_mass;
        }
        if (cost > 0) {
          grow(&s_player, -cost);
        }
        s_dash_ticks = 8;
        s_dash_cd = 105 - 15 * lvl;
      }
      break;
    case UPG_GHOST:
      if (s_ghost_cd == 0) {
        s_ghost_ticks = 30 + 15 * lvl;
        s_ghost_cd = 240 - 30 * lvl;
      }
      break;
    case UPG_FREEZE:
      if (s_freeze_cd == 0) {
        s_freeze_ticks = 45 + 15 * lvl;
        s_freeze_cd = 300 - 30 * lvl;
      }
      break;
    default:
      break;
  }
}

static bool active_ready(int id) {
  switch (id) {
    case UPG_DASH:
      return s_dash_cd == 0;
    case UPG_GHOST:
      return s_ghost_cd == 0;
    case UPG_FREEZE:
      return s_freeze_cd == 0;
    default:
      return false;
  }
}

// --- spawning ---------------------------------------------------------------

static void spawn_food(Food *f) {
  f->x = TO_FP(rand_range(FOOD_RADIUS + 2, WORLD_W - FOOD_RADIUS - 2));
  f->y = TO_FP(rand_range(FOOD_RADIUS + 2, WORLD_H - FOOD_RADIUS - 2));
  f->alive = true;
}

static void spawn_enemy(Cell *e) {
  // Size relative to the player keeps the game competitive as you grow.
  int lo = (s_player.r * 4) / 10;
  int hi = (s_player.r * 12) / 10;
  e->r = (int16_t)clamp32(rand_range(lo, hi), 4, MAX_RADIUS);
  e->mass = (int32_t)e->r * e->r;

  // Spawn away from the player so nothing materialises on top of them.
  int px = TO_PX(s_player.x);
  int py = TO_PX(s_player.y);
  for (int attempt = 0; attempt < 10; attempt++) {
    int x = rand_range(e->r, WORLD_W - e->r);
    int y = rand_range(e->r, WORLD_H - e->r);
    int dx = x - px;
    int dy = y - py;
    if (dx * dx + dy * dy > 180 * 180) {
      e->x = TO_FP(x);
      e->y = TO_FP(y);
      e->alive = true;
      return;
    }
  }
  // Fallback: opposite corner from the player.
  e->x = TO_FP(px < WORLD_W / 2 ? WORLD_W - 40 : 40);
  e->y = TO_FP(py < WORLD_H / 2 ? WORLD_H - 40 : 40);
  e->alive = true;
}

// Starts a round: player back to start size, fresh food and enemies.
// Score and upgrades persist; game_reset() clears those for a new game.
static void round_reset(void) {
  s_player.x = TO_FP(WORLD_W / 2);
  s_player.y = TO_FP(WORLD_H / 2);
  s_player.r = PLAYER_START_RADIUS;
  s_player.mass = PLAYER_START_RADIUS * PLAYER_START_RADIUS;
  s_player.alive = true;

  for (int i = 0; i < FOOD_COUNT; i++) {
    spawn_food(&s_food[i]);
  }
  for (int i = 0; i < ENEMY_COUNT; i++) {
    spawn_enemy(&s_enemies[i]);
  }

  s_dash_ticks = s_dash_cd = 0;
  s_ghost_ticks = s_ghost_cd = 0;
  s_freeze_ticks = s_freeze_cd = 0;
  s_camo_hidden = false;
  s_trail_len = s_trail_head = s_trail_tick = 0;
  for (int i = 0; i < MITOSIS_MAX; i++) {
    s_mfood[i].alive = false;
  }
  int viruses = upg_level(UPG_VIRUS) > 0 ? upg_level(UPG_VIRUS) + 1 : 0;
  for (int i = 0; i < VIRUS_MAX; i++) {
    if (i < viruses) {
      spawn_food(&s_virus[i]);
    } else {
      s_virus[i].alive = false;
    }
  }

  s_state = STATE_RUNNING;
}

static void game_reset(void) {
  s_score = 0;
  s_level = 1;
  for (int i = 0; i < MAX_OWNED; i++) {
    s_owned[i] = UPG_NONE;
    s_owned_lvl[i] = 0;
  }
  s_pending = UPG_NONE;
  round_reset();
}

// Build 3 distinct offers: level-ups of owned upgrades plus unowned ones.
// A new active is only offered while fewer than 2 are owned, since only
// UP and DOWN can trigger them.
static void make_offers(void) {
  int cand[UPG_COUNT];
  int n = 0;
  for (int id = 0; id < UPG_COUNT; id++) {
    int idx = owned_index(id);
    if (idx >= 0) {
      if (s_owned_lvl[idx] < MAX_UPG_LEVEL) {
        cand[n++] = id;
      }
    } else if (!UPG_IS_ACTIVE[id] || actives_owned() < 2) {
      cand[n++] = id;
    }
  }
  for (int k = 0; k < 3; k++) {
    if (n > 0) {
      int j = rand() % n;
      s_offers[k] = (int8_t)cand[j];
      cand[j] = cand[--n];
    } else {
      s_offers[k] = UPG_NONE;
    }
  }
}

static void next_round(void) {
  s_level++;
  round_reset();
}

static void pick_offer(int k) {
  int id = s_offers[k];
  if (id == UPG_NONE) {
    return;
  }
  int idx = owned_index(id);
  if (idx >= 0) {
    s_owned_lvl[idx]++;
    next_round();
    return;
  }
  for (int i = 0; i < MAX_OWNED; i++) {
    if (s_owned[i] == UPG_NONE) {
      s_owned[i] = (int8_t)id;
      s_owned_lvl[i] = 1;
      next_round();
      return;
    }
  }
  // All 3 slots taken: pick which owned upgrade to drop.
  s_pending = (int8_t)id;
  s_state = STATE_REPLACE;
}

static void replace_slot(int k) {
  if (s_pending == UPG_NONE) {
    return;
  }
  s_owned[k] = s_pending;
  s_owned_lvl[k] = 1;
  s_pending = UPG_NONE;
  next_round();
}

// --- simulation --------------------------------------------------------------

static void move_cell(Cell *c, int32_t vx, int32_t vy) {
  c->x = clamp32(c->x + vx, TO_FP(c->r), TO_FP(WORLD_W - c->r));
  c->y = clamp32(c->y + vy, TO_FP(c->r), TO_FP(WORLD_H - c->r));
}

static bool can_eat(int16_t eater_r, int16_t prey_r) {
  return (int32_t)eater_r * EAT_RATIO_DEN >= (int32_t)prey_r * EAT_RATIO_NUM;
}

static void update_player(void) {
  AccelData accel;
  if (accel_service_peek(&accel) < 0) {
    return;
  }
  int32_t tx = clamp32(accel.x, -TILT_MAX, TILT_MAX);
  int32_t ty = clamp32(-accel.y, -TILT_MAX, TILT_MAX);  // screen y grows downward
  if (tx > -TILT_DEADZONE && tx < TILT_DEADZONE) {
    tx = 0;
  }
  if (ty > -TILT_DEADZONE && ty < TILT_DEADZONE) {
    ty = 0;
  }

  // Camo: below a stillness threshold (looser with level) enemies lose you.
  int camo_lvl = upg_level(UPG_CAMO);
  int32_t atx = tx < 0 ? -tx : tx;
  int32_t aty = ty < 0 ? -ty : ty;
  s_camo_hidden = camo_lvl > 0 && atx < 60 * camo_lvl && aty < 60 * camo_lvl;

  int32_t vx = tx * SPEED_COEF / (SPEED_BASE + s_player.r);
  int32_t vy = ty * SPEED_COEF / (SPEED_BASE + s_player.r);
  if (s_dash_ticks > 0) {
    vx *= 3;
    vy *= 3;
  }
  move_cell(&s_player, vx, vy);
}

static bool on_trail(const Cell *c) {
  int32_t cx = TO_PX(c->x);
  int32_t cy = TO_PX(c->y);
  for (int i = 0; i < s_trail_len; i++) {
    int32_t dx = cx - s_trail[i].x;
    int32_t dy = cy - s_trail[i].y;
    if (dx * dx + dy * dy <= 8 * 8) {
      return true;
    }
  }
  return false;
}

static void update_enemy(Cell *e) {
  // Find the most attractive target: nearest edible cell. Flee the player
  // if they can eat us and are close.
  int32_t ex = e->x, ey = e->y;
  int32_t best_d2 = INT32_MAX;
  int32_t tgt_x = 0, tgt_y = 0;
  bool has_target = false;

  for (int i = 0; i < FOOD_COUNT; i++) {
    if (!s_food[i].alive) {
      continue;
    }
    int32_t dx = TO_PX(s_food[i].x - ex);
    int32_t dy = TO_PX(s_food[i].y - ey);
    int32_t d2 = dx * dx + dy * dy;
    if (d2 < best_d2) {
      best_d2 = d2;
      tgt_x = s_food[i].x;
      tgt_y = s_food[i].y;
      has_target = true;
    }
  }

  // Chasing the player beats food if the player is edible and nearby.
  if (s_player.alive && !player_hidden() && can_eat(e->r, s_player.r)) {
    int32_t dx = TO_PX(s_player.x - ex);
    int32_t dy = TO_PX(s_player.y - ey);
    int32_t d2 = dx * dx + dy * dy;
    if (d2 < 70 * 70) {
      tgt_x = s_player.x;
      tgt_y = s_player.y;
      has_target = true;
      best_d2 = d2;
    }
  }

  int32_t dir_x = 0, dir_y = 0;
  if (has_target) {
    dir_x = tgt_x - ex;
    dir_y = tgt_y - ey;
  }

  // Fleeing a dangerous player overrides everything.
  if (s_player.alive && !player_hidden() && can_eat(s_player.r, e->r)) {
    int32_t dx = TO_PX(ex - s_player.x);
    int32_t dy = TO_PX(ey - s_player.y);
    if (dx * dx + dy * dy < 90 * 90) {
      dir_x = ex - s_player.x;
      dir_y = ey - s_player.y;
    }
  }

  int32_t adx = dir_x < 0 ? -dir_x : dir_x;
  int32_t ady = dir_y < 0 ? -dir_y : dir_y;
  int32_t mag = adx > ady ? adx + ady / 2 : ady + adx / 2;  // fast approx length
  if (mag == 0) {
    return;
  }

  // Enemies run at half the player's full-tilt speed for the same size,
  // gaining 5% per level up to 85%.
  int32_t fac = 9 + s_level;
  if (fac > 17) {
    fac = 17;
  }
  int32_t speed = TILT_MAX * SPEED_COEF * fac / (20 * (SPEED_BASE + e->r));

  // Crossing the player's slime trail is like wading through glue.
  int trail_lvl = upg_level(UPG_TRAIL);
  if (trail_lvl > 0 && on_trail(e)) {
    speed = speed * (6 - trail_lvl) / 10;
  }
  int32_t vx = dir_x * speed / mag;
  int32_t vy = dir_y * speed / mag;
  move_cell(e, vx, vy);
}

// Mitosis: an eaten enemy bursts into extra one-shot pellets.
static void mitosis_burst(int32_t x, int32_t y) {
  int lvl = upg_level(UPG_MITOSIS);
  if (lvl == 0) {
    return;
  }
  int want = 1 + lvl;
  for (int i = 0; i < MITOSIS_MAX && want > 0; i++) {
    if (s_mfood[i].alive) {
      continue;
    }
    s_mfood[i].x = clamp32(x + TO_FP(rand_range(-24, 24)), TO_FP(4), TO_FP(WORLD_W - 4));
    s_mfood[i].y = clamp32(y + TO_FP(rand_range(-24, 24)), TO_FP(4), TO_FP(WORLD_H - 4));
    s_mfood[i].alive = true;
    want--;
  }
}

static void magnet_pull(Food *f) {
  f->x += clamp32((s_player.x - f->x) / 16, -TO_FP(2), TO_FP(2));
  f->y += clamp32((s_player.y - f->y) / 16, -TO_FP(2), TO_FP(2));
}

static void apply_magnet(void) {
  int lvl = upg_level(UPG_MAGNET);
  if (lvl == 0) {
    return;
  }
  int32_t radius = 25 + 15 * lvl;
  for (int i = 0; i < FOOD_COUNT; i++) {
    if (s_food[i].alive && within_dist(s_food[i].x, s_food[i].y, s_player.x, s_player.y, radius)) {
      magnet_pull(&s_food[i]);
    }
  }
  for (int i = 0; i < MITOSIS_MAX; i++) {
    if (s_mfood[i].alive && within_dist(s_mfood[i].x, s_mfood[i].y, s_player.x, s_player.y, radius)) {
      magnet_pull(&s_mfood[i]);
    }
  }
}

static void resolve_eating(void) {
  for (int e = 0; e < ENEMY_COUNT; e++) {
    s_vamp_link[e] = false;
  }

  // Player eats food.
  for (int i = 0; i < FOOD_COUNT; i++) {
    if (!s_food[i].alive) {
      continue;
    }
    if (touches_food(&s_player, &s_food[i])) {
      s_food[i].alive = false;
      grow(&s_player, FOOD_VALUE);
      s_score += 1;
      spawn_food(&s_food[i]);
    }
  }

  // Enemies eat food.
  for (int e = 0; e < ENEMY_COUNT; e++) {
    if (!s_enemies[e].alive) {
      continue;
    }
    for (int i = 0; i < FOOD_COUNT; i++) {
      if (!s_food[i].alive) {
        continue;
      }
      if (touches_food(&s_enemies[e], &s_food[i])) {
        s_food[i].alive = false;
        grow(&s_enemies[e], FOOD_VALUE);
        spawn_food(&s_food[i]);
      }
    }
  }

  // Mitosis pellets are one-shot: anyone can eat them, they don't respawn.
  for (int i = 0; i < MITOSIS_MAX; i++) {
    if (!s_mfood[i].alive) {
      continue;
    }
    if (touches_food(&s_player, &s_mfood[i])) {
      s_mfood[i].alive = false;
      grow(&s_player, FOOD_VALUE);
      s_score += 1;
      continue;
    }
    for (int e = 0; e < ENEMY_COUNT; e++) {
      if (s_enemies[e].alive && touches_food(&s_enemies[e], &s_mfood[i])) {
        s_mfood[i].alive = false;
        grow(&s_enemies[e], FOOD_VALUE);
        break;
      }
    }
  }

  // Player vs enemies. While ghosting the player is out of phase: nothing
  // can eat them and they can't eat (food is still fine).
  if (s_ghost_ticks == 0) {
    for (int e = 0; e < ENEMY_COUNT; e++) {
      Cell *en = &s_enemies[e];
      if (!en->alive) {
        continue;
      }
      if (can_eat(s_player.r, en->r) && engulfs(&s_player, en)) {
        s_score += en->r;
        grow(&s_player, en->mass);
        mitosis_burst(en->x, en->y);
        spawn_enemy(en);
      } else if (can_eat(en->r, s_player.r) && engulfs(en, &s_player)) {
        s_player.alive = false;
        s_state = STATE_GAME_OVER;
        light_enable(false);
        vibes_double_pulse();
        return;
      } else if (upg_level(UPG_VAMPIRE) > 0 && can_eat(en->r, s_player.r) &&
                 within_dist(en->x, en->y, s_player.x, s_player.y, en->r + s_player.r)) {
        // Grazing (touching without being engulfed by) a bigger enemy
        // siphons its mass, tick by tick.
        int32_t sip = 6 * upg_level(UPG_VAMPIRE);
        grow(en, -sip);
        grow(&s_player, sip);
        s_vamp_link[e] = true;
      }
    }
  }

  // Viruses pop enemies that touch them; the player is immune.
  for (int v = 0; v < VIRUS_MAX; v++) {
    if (!s_virus[v].alive) {
      continue;
    }
    for (int e = 0; e < ENEMY_COUNT; e++) {
      Cell *en = &s_enemies[e];
      if (!en->alive) {
        continue;
      }
      if (within_dist(s_virus[v].x, s_virus[v].y, en->x, en->y, en->r + VIRUS_RADIUS)) {
        int32_t floor_mass = 16;
        int32_t loss = en->mass / 2;
        if (en->mass - loss < floor_mass) {
          loss = en->mass - floor_mass;
        }
        if (loss > 0) {
          grow(en, -loss);
        }
        spawn_food(&s_virus[v]);  // relocate so it can't camp on one enemy
        break;
      }
    }
  }

  // Enemies eat each other.
  for (int a = 0; a < ENEMY_COUNT; a++) {
    if (!s_enemies[a].alive) {
      continue;
    }
    for (int b = 0; b < ENEMY_COUNT; b++) {
      if (a == b || !s_enemies[b].alive) {
        continue;
      }
      if (can_eat(s_enemies[a].r, s_enemies[b].r) && engulfs(&s_enemies[a], &s_enemies[b])) {
        grow(&s_enemies[a], s_enemies[b].mass);
        spawn_enemy(&s_enemies[b]);
      }
    }
  }
}

static void game_tick(void *context) {
  s_timer = app_timer_register(TICK_MS, game_tick, NULL);
  if (s_state != STATE_RUNNING) {
    return;
  }
  if (s_dash_ticks > 0) s_dash_ticks--;
  if (s_dash_cd > 0) s_dash_cd--;
  if (s_ghost_ticks > 0) s_ghost_ticks--;
  if (s_ghost_cd > 0) s_ghost_cd--;
  if (s_freeze_ticks > 0) s_freeze_ticks--;
  if (s_freeze_cd > 0) s_freeze_cd--;

  update_player();
  if (s_freeze_ticks == 0) {
    for (int i = 0; i < ENEMY_COUNT; i++) {
      if (s_enemies[i].alive) {
        update_enemy(&s_enemies[i]);
      }
    }
  }
  apply_magnet();
  resolve_eating();

  // Record the slime trail every couple of ticks. The per-level cap is
  // stable within a round (upgrades only change between rounds, and
  // round_reset clears the buffer), so the ring stays consistent.
  int trail_lvl = upg_level(UPG_TRAIL);
  if (trail_lvl > 0 && ++s_trail_tick >= 2) {
    int cap = 30 * (1 + trail_lvl);
    s_trail_tick = 0;
    s_trail[s_trail_head] = GPoint(TO_PX(s_player.x), TO_PX(s_player.y));
    s_trail_head = (s_trail_head + 1) % cap;
    if (s_trail_len < cap) {
      s_trail_len++;
    }
  }

  // Round won: hit max size, pick an upgrade for the next round.
  if (s_state == STATE_RUNNING && s_player.mass >= MAX_RADIUS * MAX_RADIUS) {
    make_offers();
    s_state = STATE_UPGRADE;
    vibes_short_pulse();
  }

  // Camera follows the player. On round displays it stays centered on the
  // player unconditionally — clamping to the world edge would push the
  // player into a corner the circular bezel clips away.
#ifdef PBL_ROUND
  s_cam_x = TO_PX(s_player.x) - PBL_DISPLAY_WIDTH / 2;
  s_cam_y = TO_PX(s_player.y) - PBL_DISPLAY_HEIGHT / 2;
#else
  s_cam_x = clamp32(TO_PX(s_player.x) - PBL_DISPLAY_WIDTH / 2, 0, WORLD_W - PBL_DISPLAY_WIDTH);
  s_cam_y = clamp32(TO_PX(s_player.y) - PBL_DISPLAY_HEIGHT / 2, 0, WORLD_H - PBL_DISPLAY_HEIGHT);
#endif

  layer_mark_dirty(s_game_layer);
}

// --- rendering ----------------------------------------------------------------

static void draw_hud(GContext *ctx) {
  static char buf[24];
  snprintf(buf, sizeof(buf), "L%d Score: %d", s_level, s_score);
  graphics_context_set_text_color(ctx, GColorWhite);
  // Centered on round displays so the circular bezel doesn't clip it.
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 0, PBL_DISPLAY_WIDTH - 8, 22), GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
}

// Centered text on a black pill so menus stay readable over the busy world.
static void draw_pill_line(GContext *ctx, const char *text, GFont font, int top) {
  GSize sz = graphics_text_layout_get_content_size(
      text, font, GRect(0, 0, PBL_DISPLAY_WIDTH, 100), GTextOverflowModeTrailingEllipsis,
      GTextAlignmentCenter);
  int w = sz.w + 22;
  int h = sz.h + 4;
  GRect pill = GRect((PBL_DISPLAY_WIDTH - w) / 2, top, w, h);
  int rad = h / 2 > 14 ? 14 : h / 2;
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, pill, rad, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_round_rect(ctx, pill, rad);
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, text, font, GRect(0, top - 2, PBL_DISPLAY_WIDTH, h + 4),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void draw_center_text(GContext *ctx, const char *line1, const char *line2) {
  draw_pill_line(ctx, line1, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                 PBL_DISPLAY_HEIGHT / 2 - 46);
  draw_pill_line(ctx, line2, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                 PBL_DISPLAY_HEIGHT / 2 - 6);
}

// Title plus three option pills; top/middle/bottom map to UP/SELECT/DOWN.
static void draw_menu(GContext *ctx, const char *title, const char *o0, const char *o1,
                      const char *o2) {
  GFont of = fonts_get_system_font(FONT_KEY_GOTHIC_18);
  draw_pill_line(ctx, title, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                 PBL_DISPLAY_HEIGHT / 2 - 56);
  draw_pill_line(ctx, o0, of, PBL_DISPLAY_HEIGHT / 2 - 18);
  draw_pill_line(ctx, o1, of, PBL_DISPLAY_HEIGHT / 2 + 12);
  draw_pill_line(ctx, o2, of, PBL_DISPLAY_HEIGHT / 2 + 42);
}

static void game_layer_update(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // Grid for a sense of motion.
  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite));
  for (int gx = -(s_cam_x % 50); gx < PBL_DISPLAY_WIDTH; gx += 50) {
    graphics_draw_line(ctx, GPoint(gx, 0), GPoint(gx, PBL_DISPLAY_HEIGHT));
  }
  for (int gy = -(s_cam_y % 50); gy < PBL_DISPLAY_HEIGHT; gy += 50) {
    graphics_draw_line(ctx, GPoint(0, gy), GPoint(PBL_DISPLAY_WIDTH, gy));
  }

  // World border.
  graphics_context_set_stroke_color(ctx, GColorLightGray);
  graphics_draw_rect(ctx, GRect(-s_cam_x, -s_cam_y, WORLD_W, WORLD_H));

  // Slime trail.
  if (upg_level(UPG_TRAIL) > 0) {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorMayGreen, GColorWhite));
    for (int i = 0; i < s_trail_len; i++) {
      GPoint p = GPoint(s_trail[i].x - s_cam_x, s_trail[i].y - s_cam_y);
      if (p.x < -2 || p.x > PBL_DISPLAY_WIDTH + 2 || p.y < -2 || p.y > PBL_DISPLAY_HEIGHT + 2) {
        continue;
      }
      graphics_fill_circle(ctx, p, 1);
    }
  }

  // Food, plus one-shot mitosis pellets in the same style.
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite));
  for (int i = 0; i < FOOD_COUNT + MITOSIS_MAX; i++) {
    const Food *f = i < FOOD_COUNT ? &s_food[i] : &s_mfood[i - FOOD_COUNT];
    if (!f->alive) {
      continue;
    }
    GPoint p = GPoint(TO_PX(f->x) - s_cam_x, TO_PX(f->y) - s_cam_y);
    if (p.x < -4 || p.x > PBL_DISPLAY_WIDTH + 4 || p.y < -4 || p.y > PBL_DISPLAY_HEIGHT + 4) {
      continue;
    }
    graphics_fill_circle(ctx, p, FOOD_RADIUS);
  }

  // Viruses: spiky rings, harmless to the player.
  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorPurple, GColorWhite));
  for (int i = 0; i < VIRUS_MAX; i++) {
    if (!s_virus[i].alive) {
      continue;
    }
    GPoint p = GPoint(TO_PX(s_virus[i].x) - s_cam_x, TO_PX(s_virus[i].y) - s_cam_y);
    if (p.x < -12 || p.x > PBL_DISPLAY_WIDTH + 12 || p.y < -12 || p.y > PBL_DISPLAY_HEIGHT + 12) {
      continue;
    }
    graphics_draw_circle(ctx, p, VIRUS_RADIUS);
    graphics_draw_line(ctx, GPoint(p.x - VIRUS_RADIUS, p.y), GPoint(p.x - VIRUS_RADIUS - 3, p.y));
    graphics_draw_line(ctx, GPoint(p.x + VIRUS_RADIUS, p.y), GPoint(p.x + VIRUS_RADIUS + 3, p.y));
    graphics_draw_line(ctx, GPoint(p.x, p.y - VIRUS_RADIUS), GPoint(p.x, p.y - VIRUS_RADIUS - 3));
    graphics_draw_line(ctx, GPoint(p.x, p.y + VIRUS_RADIUS), GPoint(p.x, p.y + VIRUS_RADIUS + 3));
  }

  // Enemies, colored by threat: red can eat you, green you can eat.
  for (int i = 0; i < ENEMY_COUNT; i++) {
    Cell *e = &s_enemies[i];
    if (!e->alive) {
      continue;
    }
    GPoint p = GPoint(TO_PX(e->x) - s_cam_x, TO_PX(e->y) - s_cam_y);
    if (p.x < -e->r || p.x > PBL_DISPLAY_WIDTH + e->r || p.y < -e->r ||
        p.y > PBL_DISPLAY_HEIGHT + e->r) {
      continue;
    }
#ifdef PBL_COLOR
    GColor fill;
    if (can_eat(e->r, s_player.r)) {
      fill = GColorRed;
    } else if (can_eat(s_player.r, e->r)) {
      fill = GColorIslamicGreen;
    } else {
      fill = GColorLightGray;
    }
    graphics_context_set_fill_color(ctx, fill);
    graphics_fill_circle(ctx, p, e->r);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_circle(ctx, p, e->r);
#else
    // BW threat language: dangerous = solid, edible = hollow,
    // near-equal = hollow with a center dot.
    graphics_context_set_stroke_color(ctx, GColorWhite);
    if (can_eat(e->r, s_player.r)) {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_circle(ctx, p, e->r);
    } else if (can_eat(s_player.r, e->r)) {
      graphics_draw_circle(ctx, p, e->r);
    } else {
      graphics_draw_circle(ctx, p, e->r);
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_circle(ctx, p, e->r > 6 ? 3 : 1);
    }
#endif
  }

  // Vampire siphon beams from grazed enemies into the player.
  graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorRed, GColorWhite));
  graphics_context_set_stroke_width(ctx, 3);
  for (int i = 0; i < ENEMY_COUNT; i++) {
    if (!s_vamp_link[i] || !s_enemies[i].alive) {
      continue;
    }
    graphics_draw_line(
        ctx, GPoint(TO_PX(s_player.x) - s_cam_x, TO_PX(s_player.y) - s_cam_y),
        GPoint(TO_PX(s_enemies[i].x) - s_cam_x, TO_PX(s_enemies[i].y) - s_cam_y));
  }
  graphics_context_set_stroke_width(ctx, 1);

  // Player: on BW a black core distinguishes it from solid (dangerous)
  // enemies. While ghosting or hidden, just an outline.
  if (s_player.alive) {
    GPoint p = GPoint(TO_PX(s_player.x) - s_cam_x, TO_PX(s_player.y) - s_cam_y);
    if (player_hidden()) {
      graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorCyan, GColorWhite));
      graphics_draw_circle(ctx, p, s_player.r);
      graphics_draw_circle(ctx, p, s_player.r > 4 ? s_player.r - 3 : 1);
    } else {
      graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorCyan, GColorWhite));
      graphics_fill_circle(ctx, p, s_player.r);
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_draw_circle(ctx, p, s_player.r);
#ifndef PBL_COLOR
      graphics_context_set_fill_color(ctx, GColorBlack);
      graphics_fill_circle(ctx, p, s_player.r / 3);
#endif
    }
  }

  draw_hud(ctx);

  // Owned actives sit next to their button: slot 0 by UP (top right),
  // slot 1 by DOWN (bottom right). Filled = ready, hollow = cooling down.
  for (int slot = 0; slot < 2; slot++) {
    int id = active_in_slot(slot);
    if (id == UPG_NONE) {
      continue;
    }
    int x = PBL_DISPLAY_WIDTH - PBL_IF_ROUND_ELSE(26, 13);
    int y = slot == 0 ? 44 : PBL_DISPLAY_HEIGHT - 44;
    char letter[2] = {UPG_NAMES[id][0], '\0'};
    if (active_ready(id)) {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_circle(ctx, GPoint(x, y), 9);
      graphics_context_set_text_color(ctx, GColorBlack);
    } else {
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_draw_circle(ctx, GPoint(x, y), 9);
      graphics_context_set_text_color(ctx, GColorWhite);
    }
    graphics_draw_text(ctx, letter, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(x - 8, y - 10, 16, 16), GTextOverflowModeTrailingEllipsis,
                       GTextAlignmentCenter, NULL);
  }

  if (s_state == STATE_PAUSED) {
    draw_center_text(ctx, "Paused", "SELECT to resume");
  } else if (s_state == STATE_UPGRADE) {
    // Level-ups show the level you'd reach, e.g. "Magnet 2".
    static char o[3][16];
    for (int k = 0; k < 3; k++) {
      int id = s_offers[k];
      if (id == UPG_NONE) {
        snprintf(o[k], sizeof(o[k]), "-");
      } else if (owned_index(id) >= 0) {
        snprintf(o[k], sizeof(o[k]), "%s %d", UPG_NAMES[id], upg_level(id) + 1);
      } else {
        snprintf(o[k], sizeof(o[k]), "%s", UPG_NAMES[id]);
      }
    }
    draw_menu(ctx, "Max size!", o[0], o[1], o[2]);
  } else if (s_state == STATE_REPLACE) {
    draw_menu(ctx, "Drop one:", UPG_NAMES[(int)s_owned[0]], UPG_NAMES[(int)s_owned[1]],
              UPG_NAMES[(int)s_owned[2]]);
  } else if (s_state == STATE_GAME_OVER) {
    static char over[40];
    snprintf(over, sizeof(over), "Score: %d\nSELECT to restart", s_score);
    draw_center_text(ctx, "Eaten!", over);
  }
}

// --- input --------------------------------------------------------------------

static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  switch (s_state) {
    case STATE_RUNNING:
      s_state = STATE_PAUSED;
      break;
    case STATE_PAUSED:
      s_state = STATE_RUNNING;
      break;
    case STATE_UPGRADE:
      pick_offer(1);
      break;
    case STATE_REPLACE:
      replace_slot(1);
      break;
    case STATE_GAME_OVER:
      game_reset();
      break;
  }
  // Keep the backlight on while playing; give it back to the system otherwise.
  light_enable(s_state == STATE_RUNNING);
  layer_mark_dirty(s_game_layer);
}

// UP and DOWN trigger active abilities during play and pick menu entries
// on the upgrade/replace screens.
static void updown_click_handler(ClickRecognizerRef recognizer, void *context) {
  int slot = click_recognizer_get_button_id(recognizer) == BUTTON_ID_UP ? 0 : 1;
  if (s_state == STATE_RUNNING) {
    int id = active_in_slot(slot);
    if (id != UPG_NONE) {
      trigger_active(id);
    }
    return;  // no redraw needed; the next tick draws the effect
  }
  if (s_state == STATE_UPGRADE) {
    pick_offer(slot == 0 ? 0 : 2);
  } else if (s_state == STATE_REPLACE) {
    replace_slot(slot == 0 ? 0 : 2);
  }
  light_enable(s_state == STATE_RUNNING);
  layer_mark_dirty(s_game_layer);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_UP, updown_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, updown_click_handler);
}

// --- app lifecycle --------------------------------------------------------------

static void window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);
  s_game_layer = layer_create(bounds);
  layer_set_update_proc(s_game_layer, game_layer_update);
  layer_add_child(window_layer, s_game_layer);
}

static void window_unload(Window *window) {
  layer_destroy(s_game_layer);
}

static void init(void) {
  srand(time(NULL));

  s_window = window_create();
  window_set_click_config_provider(s_window, click_config_provider);
  window_set_window_handlers(s_window, (WindowHandlers){
                                           .load = window_load,
                                           .unload = window_unload,
                                       });
  window_stack_push(s_window, true);

  // Subscribe with no handler so accel_service_peek() works.
  accel_data_service_subscribe(0, NULL);
  accel_service_set_sampling_rate(ACCEL_SAMPLING_25HZ);

  game_reset();
#if DEBUG_START_AT_UPGRADE
  make_offers();
  s_state = STATE_UPGRADE;
#endif
  light_enable(true);
  s_timer = app_timer_register(TICK_MS, game_tick, NULL);
}

static void deinit(void) {
  if (s_timer) {
    app_timer_cancel(s_timer);
  }
  light_enable(false);
  accel_data_service_unsubscribe();
  window_destroy(s_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
  return 0;
}
