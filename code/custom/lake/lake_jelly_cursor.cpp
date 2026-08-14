/*
 * lake_jelly_cursor.cpp
 * ------------------------------------------------------------------------
 * Smooth, frame-gated Neovide-style jelly cursor for 4coder.
 */

#include <math.h>

CUSTOM_ID(colors, lake_color_jelly_cursor);

#define LAKE_TRAIL_COUNT 3  // Reduced trail length for a tighter, faster feel

// Motion tuning: Extreme stiffness for instant snap and minimal lag
global f32 lake_jelly_stiffness    = 1500.0f;  // Increased heavily for instant catch-up
global f32 lake_jelly_damping      = 90.0f;    // Balanced higher damping to prevent wild shaking
global f32 lake_jelly_max_stretch  = 60.0f;    
global f32 lake_jelly_roundness    = 4.0f;    

global f32 lake_last_dt = 1.0f / 60.0f;
global u64 lake_current_frame_count = 1;

function void
lake_tick(Application_Links *app, Frame_Info frame_info)
{
    default_tick(app, frame_info);
    lake_last_dt = (f32)frame_info.animation_dt;
    lake_current_frame_count += 1;
}

#define LAKE_MAX_JELLY_VIEWS 32

typedef struct Lake_Cursor_Spring
{
    View_ID view;
    b32 initialized;

    f32 visual_x, visual_y;
    f32 velocity_x, velocity_y;

    f32 rest_w, rest_h;
    u64 last_updated_frame;

    // Trail history buffer
    Vec2_f32 trail_pos[LAKE_TRAIL_COUNT];
    Vec2_f32 trail_size[LAKE_TRAIL_COUNT];
} Lake_Cursor_Spring;

global Lake_Cursor_Spring lake_jelly_springs[LAKE_MAX_JELLY_VIEWS];
global i32 lake_jelly_spring_count = 0;

function Lake_Cursor_Spring *
lake_get_cursor_spring(View_ID view)
{
    for (i32 i = 0; i < lake_jelly_spring_count; i += 1)
    {
        if (lake_jelly_springs[i].view == view)
        {
            return &lake_jelly_springs[i];
        }
    }

    if (lake_jelly_spring_count < LAKE_MAX_JELLY_VIEWS)
    {
        Lake_Cursor_Spring *spring = &lake_jelly_springs[lake_jelly_spring_count];
        *spring = {};
        spring->view = view;
        lake_jelly_spring_count += 1;
        return spring;
    }

    return &lake_jelly_springs[0];
}

function void
lake_draw_jelly_cursor(Application_Links *app, View_ID view, Buffer_ID buffer,
                        Text_Layout_ID text_layout_id, i64 cursor_pos, b32 is_active_view)
{
    if (!is_active_view) return;

    // Get current target cursor rect from text layout
    Rect_f32 target_rect = text_layout_character_on_screen(app, text_layout_id, cursor_pos);

    f32 target_x = target_rect.x0;
    f32 target_y = target_rect.y0;
    f32 target_w = target_rect.x1 - target_rect.x0;
    f32 target_h = target_rect.y1 - target_rect.y0;

    if (target_w < 1.0f) target_w = 2.0f;
    if (target_h < 1.0f) target_h = 16.0f;

    Lake_Cursor_Spring *spring = lake_get_cursor_spring(view);

    if (!spring->initialized)
    {
        spring->visual_x = target_x;
        spring->visual_y = target_y;
        spring->velocity_x = 0.0f;
        spring->velocity_y = 0.0f;
        spring->rest_w = target_w;
        spring->rest_h = target_h;
        spring->last_updated_frame = lake_current_frame_count;

        for (i32 i = 0; i < LAKE_TRAIL_COUNT; i += 1)
        {
            spring->trail_pos[i] = V2f32(target_x, target_y);
            spring->trail_size[i] = V2f32(target_w, target_h);
        }
        spring->initialized = true;
    }

    spring->rest_w = target_w;
    spring->rest_h = target_h;

    // Frame-gate: Only advance physics ONCE per frame tick
    if (spring->last_updated_frame != lake_current_frame_count)
    {
        spring->last_updated_frame = lake_current_frame_count;

        f32 dt = lake_last_dt;
        if (dt > 0.05f) dt = 0.05f;
        if (dt <= 0.0f) dt = 1.0f / 60.0f;

        f32 dist_x = target_x - spring->visual_x;
        f32 dist_y = target_y - spring->visual_y;
        f32 total_dist = sqrtf(dist_x * dist_x + dist_y * dist_y);

        // Instant snap threshold for massive jumps (buffer switch, long page jump)
        if (total_dist > 2000.0f)
        {
            spring->visual_x = target_x;
            spring->visual_y = target_y;
            spring->velocity_x = 0.0f;
            spring->velocity_y = 0.0f;
            for (i32 i = 0; i < LAKE_TRAIL_COUNT; i += 1)
            {
                spring->trail_pos[i] = V2f32(target_x, target_y);
                spring->trail_size[i] = V2f32(target_w, target_h);
            }
        }
        else
        {
            // Substepped spring integration with high frequency updates
            const f32 k_substep = 1.0f / 400.0f;
            i32 substeps = (i32)(dt / k_substep) + 1;
            f32 substep_dt = dt / (f32)substeps;

            for (i32 s = 0; s < substeps; s += 1)
            {
                f32 force_x = (target_x - spring->visual_x) * lake_jelly_stiffness;
                spring->velocity_x += (force_x - spring->velocity_x * lake_jelly_damping) * substep_dt;
                spring->visual_x += spring->velocity_x * substep_dt;

                f32 force_y = (target_y - spring->visual_y) * lake_jelly_stiffness;
                spring->velocity_y += (force_y - spring->velocity_y * lake_jelly_damping) * substep_dt;
                spring->visual_y += spring->velocity_y * substep_dt;
            }
        }

        // Shift trail history buffer only when physics updates
        for (i32 i = LAKE_TRAIL_COUNT - 1; i > 0; i -= 1)
        {
            spring->trail_pos[i] = spring->trail_pos[i - 1];
            spring->trail_size[i] = spring->trail_size[i - 1];
        }
        spring->trail_pos[0] = V2f32(spring->visual_x, spring->visual_y);
        spring->trail_size[0] = V2f32(spring->rest_w, spring->rest_h);
    }

    // Velocity-based stretch along movement direction
    f32 stretch_x = fminf(fabsf(spring->velocity_x) * 0.035f, lake_jelly_max_stretch);
    f32 stretch_y = fminf(fabsf(spring->velocity_y) * 0.035f, lake_jelly_max_stretch);

    f32 draw_w = spring->rest_w + stretch_x;
    f32 draw_h = spring->rest_h + stretch_y;

    f32 draw_x = spring->visual_x;
    f32 draw_y = spring->visual_y;
    if (spring->velocity_x < 0.0f) draw_x -= stretch_x;
    if (spring->velocity_y < 0.0f) draw_y -= stretch_y;

    i32 cursor_sub_id = default_cursor_sub_id();
    FColor cursor_fcolor = fcolor_id(defcolor_cursor, cursor_sub_id);

    // 1. Draw trail segments
    for (i32 i = LAKE_TRAIL_COUNT - 1; i >= 1; i -= 1)
    {
        f32 progress = (f32)i / (f32)LAKE_TRAIL_COUNT;
        f32 shrink = 1.0f - (progress * 0.15f);

        Vec2_f32 tpos = spring->trail_pos[i];
        Vec2_f32 tsize = spring->trail_size[i];

        f32 w = tsize.x * shrink;
        f32 h = tsize.y * shrink;
        f32 x_off = (tsize.x - w) * 0.5f;
        f32 y_off = (tsize.y - h) * 0.5f;

        Rect_f32 trail_rect = Rf32(tpos.x + x_off, tpos.y + y_off, tpos.x + x_off + w, tpos.y + y_off + h);
        draw_rectangle_fcolor(app, trail_rect, lake_jelly_roundness, cursor_fcolor);
    }

    // 2. Draw main active cursor
    Rect_f32 cursor_rect = Rf32(draw_x, draw_y, draw_x + draw_w, draw_y + draw_h);
    draw_rectangle_fcolor(app, cursor_rect, lake_jelly_roundness, cursor_fcolor);

    // Keep frame loop active until motion settles
    f32 rem_x = target_x - spring->visual_x;
    f32 rem_y = target_y - spring->visual_y;
    f32 rem_dist = sqrtf(rem_x * rem_x + rem_y * rem_y);

    b32 still_animating = (rem_dist > 0.2f) ||
                          (fabsf(spring->velocity_x) > 0.5f) ||
                          (fabsf(spring->velocity_y) > 0.5f);

    if (still_animating)
    {
        animate_in_n_milliseconds(app, 0);
    }
}