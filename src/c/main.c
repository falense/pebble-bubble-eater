#include <pebble.h>

// ---------------------------------------------------------------------------
// BubbleEater — an Agar.io-style game for Pebble Time 2 (emery), IMU-controlled
// ---------------------------------------------------------------------------

#define TICK_MS 33  // ~30 fps

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
  STATE_GAME_OVER,
} GameState;

typedef struct {
  int32_t x, y;  // fixed point, world coordinates
  int16_t r;     // radius in px
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

static int16_t grow_radius(int16_t r, int16_t eaten_r) {
  int32_t r2 = (int32_t)r * r + (int32_t)eaten_r * eaten_r;
  int32_t nr = isqrt32(r2);
  return (int16_t)clamp32(nr, 1, MAX_RADIUS);
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

static void game_reset(void) {
  s_player.x = TO_FP(WORLD_W / 2);
  s_player.y = TO_FP(WORLD_H / 2);
  s_player.r = PLAYER_START_RADIUS;
  s_player.alive = true;
  s_score = 0;

  for (int i = 0; i < FOOD_COUNT; i++) {
    spawn_food(&s_food[i]);
  }
  for (int i = 0; i < ENEMY_COUNT; i++) {
    spawn_enemy(&s_enemies[i]);
  }
  s_state = STATE_RUNNING;
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
  int32_t vx = tx * SPEED_COEF / (SPEED_BASE + s_player.r);
  int32_t vy = ty * SPEED_COEF / (SPEED_BASE + s_player.r);
  move_cell(&s_player, vx, vy);
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
  if (s_player.alive && can_eat(e->r, s_player.r)) {
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
  if (s_player.alive && can_eat(s_player.r, e->r)) {
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

  // Enemies run at half the player's full-tilt speed for the same size.
  int32_t speed = TILT_MAX * SPEED_COEF / (2 * (SPEED_BASE + e->r));
  int32_t vx = dir_x * speed / mag;
  int32_t vy = dir_y * speed / mag;
  move_cell(e, vx, vy);
}

static void resolve_eating(void) {
  // Player eats food.
  for (int i = 0; i < FOOD_COUNT; i++) {
    if (!s_food[i].alive) {
      continue;
    }
    if (touches_food(&s_player, &s_food[i])) {
      s_food[i].alive = false;
      s_player.r = grow_radius(s_player.r, FOOD_RADIUS);
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
        s_enemies[e].r = grow_radius(s_enemies[e].r, FOOD_RADIUS);
        spawn_food(&s_food[i]);
      }
    }
  }

  // Player vs enemies.
  for (int e = 0; e < ENEMY_COUNT; e++) {
    Cell *en = &s_enemies[e];
    if (!en->alive) {
      continue;
    }
    if (can_eat(s_player.r, en->r) && engulfs(&s_player, en)) {
      s_score += en->r;
      s_player.r = grow_radius(s_player.r, en->r);
      spawn_enemy(en);
    } else if (can_eat(en->r, s_player.r) && engulfs(en, &s_player)) {
      s_player.alive = false;
      s_state = STATE_GAME_OVER;
      light_enable(false);
      vibes_double_pulse();
      return;
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
        s_enemies[a].r = grow_radius(s_enemies[a].r, s_enemies[b].r);
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
  update_player();
  for (int i = 0; i < ENEMY_COUNT; i++) {
    if (s_enemies[i].alive) {
      update_enemy(&s_enemies[i]);
    }
  }
  resolve_eating();

  // Camera follows the player, clamped to the world.
  s_cam_x = clamp32(TO_PX(s_player.x) - PBL_DISPLAY_WIDTH / 2, 0, WORLD_W - PBL_DISPLAY_WIDTH);
  s_cam_y = clamp32(TO_PX(s_player.y) - PBL_DISPLAY_HEIGHT / 2, 0, WORLD_H - PBL_DISPLAY_HEIGHT);

  layer_mark_dirty(s_game_layer);
}

// --- rendering ----------------------------------------------------------------

static void draw_hud(GContext *ctx) {
  static char buf[24];
  snprintf(buf, sizeof(buf), "Score: %d", s_score);
  graphics_context_set_text_color(ctx, GColorWhite);
  // Centered on round displays so the circular bezel doesn't clip it.
  graphics_draw_text(ctx, buf, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
                     GRect(4, 0, PBL_DISPLAY_WIDTH - 8, 22), GTextOverflowModeTrailingEllipsis,
                     PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
}

static void draw_center_text(GContext *ctx, const char *line1, const char *line2) {
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, line1, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(0, PBL_DISPLAY_HEIGHT / 2 - 40, PBL_DISPLAY_WIDTH, 32),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  graphics_draw_text(ctx, line2, fonts_get_system_font(FONT_KEY_GOTHIC_18),
                     GRect(0, PBL_DISPLAY_HEIGHT / 2 - 4, PBL_DISPLAY_WIDTH, 44),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
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

  // Food.
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorYellow, GColorWhite));
  for (int i = 0; i < FOOD_COUNT; i++) {
    if (!s_food[i].alive) {
      continue;
    }
    GPoint p = GPoint(TO_PX(s_food[i].x) - s_cam_x, TO_PX(s_food[i].y) - s_cam_y);
    if (p.x < -4 || p.x > PBL_DISPLAY_WIDTH + 4 || p.y < -4 || p.y > PBL_DISPLAY_HEIGHT + 4) {
      continue;
    }
    graphics_fill_circle(ctx, p, FOOD_RADIUS);
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

  // Player: on BW a black core distinguishes it from solid (dangerous) enemies.
  if (s_player.alive) {
    GPoint p = GPoint(TO_PX(s_player.x) - s_cam_x, TO_PX(s_player.y) - s_cam_y);
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorCyan, GColorWhite));
    graphics_fill_circle(ctx, p, s_player.r);
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_draw_circle(ctx, p, s_player.r);
#ifndef PBL_COLOR
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, p, s_player.r / 3);
#endif
  }

  draw_hud(ctx);

  if (s_state == STATE_PAUSED) {
    draw_center_text(ctx, "Paused", "SELECT to resume");
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
    case STATE_GAME_OVER:
      game_reset();
      break;
  }
  // Keep the backlight on while playing; give it back to the system otherwise.
  light_enable(s_state == STATE_RUNNING);
  layer_mark_dirty(s_game_layer);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
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
