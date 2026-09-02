/*
 * lake.cpp
 * ------------------------------------------------------------------------
 */

#include "../4coder_default_include.cpp"


#include "lake_jelly_cursor.cpp"
#include "lake_highlight.cpp"

/* Custom render hook that replaces default_render_buffer */
function void
lake_render_buffer(Application_Links *app, View_ID view_id, Face_ID face_id,
                   Buffer_ID buffer, Text_Layout_ID text_layout_id,
                   Rect_f32 rect)
{
    ProfileScope(app, "lake render buffer");

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
    set_custom_hook(app, HookID_Tick, lake_tick);
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
