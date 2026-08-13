/*
 * lake.cpp
 * ------------------------------------------------------------------------
 * Entry point for the "lake" custom layer.
 *
 * This starts from 4coder's default custom layer (same shape as
 * 4coder_default_bindings.cpp in the 4cc source tree) and adds semantic
 * identifier coloring via lake_highlight.cpp.
 *
 * BUILD
 *   This file replaces 4coder_default_bindings.cpp as the thing you pass
 *   to the build script, e.g. (4coder-community/4cc build scripts):
 *
 *       ./build.sh --mode=release lake.cpp
 *       .\build.bat -m=release lake.cpp
 *
 *   The resulting shared library goes wherever your 4ed binary loads the
 *   custom layer from (same folder as the default one it replaces).
 *
 * FILES THIS EXPECTS NEXT TO IT
 *   - lake_highlight.cpp   (the semantic coloring module)
 *
 * ONE-TIME CORE EDIT (see lake_highlight.cpp's big comment for why this
 * has to live in the core source rather than purely in this file):
 *   In 4coder_default_hooks.cpp, inside default_render_buffer, swap the
 *   call to draw_cpp_token_colors(...) for lake_draw_cpp_token_colors(...).
 * ------------------------------------------------------------------------
 */

#include "../4coder_default_include.cpp"

// lake: semantic identifier coloring (types/functions/macros via the
// built-in Code_Index). Must come after 4coder_default_include.cpp -- it
// depends on core types that file pulls in.
#include "lake_highlight.cpp"

/*
 * If you want lake to eventually diverge further from the defaults --
 * custom bindings, extra languages, extra render hooks, whatever --
 * this is the natural place to start pulling in more modules the same
 * way 4coder_fleury does (see 4coder_fleury.cpp's big #include block for
 * the pattern: headers first, then .cpp's, then wire it all up in
 * custom_layer_init below).
 */

/* Custom render hook that replaces default_render_buffer */
function void
lake_render_buffer(Application_Links *app, View_ID view_id, Face_ID face_id,
                   Buffer_ID buffer, Text_Layout_ID text_layout_id,
                   Rect_f32 rect)
{
    ProfileScope(app, "lake render buffer");

    // 1. Draw buffer background and line highlights
    //draw_buffer_range_background(app, view_id, text_layout_id);

    // 2. Render tokens using custom semantic coloring
    Token_Array token_array = get_token_array_from_buffer(app, buffer);
    if (token_array.tokens != 0)
    {
        lake_draw_cpp_token_colors(app, text_layout_id, &token_array, buffer);
    }
}

void
custom_layer_init(Application_Links *app)
{
    Thread_Context *tctx = get_thread_context(app);

    default_framework_init(app);
    set_all_default_hooks(app);
    lake_install_hooks(app);

    mapping_init(tctx, &framework_mapping);
    String_ID global_map_id = vars_save_string_lit("keys_global");
    String_ID file_map_id = vars_save_string_lit("keys_file");
    String_ID code_map_id = vars_save_string_lit("keys_code");
#if OS_MAC
    setup_mac_mapping(&framework_mapping, global_map_id, file_map_id, code_map_id);
#else
    setup_default_mapping(&framework_mapping, global_map_id, file_map_id, code_map_id);
#endif
    setup_essential_mapping(&framework_mapping, global_map_id, file_map_id, code_map_id);
}
