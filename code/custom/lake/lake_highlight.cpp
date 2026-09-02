//CUSTOM_ID(colors, lake_color_index_type);
//CUSTOM_ID(colors, lake_color_index_function);
//CUSTOM_ID(colors, lake_color_index_macro);
//CUSTOM_ID(colors, lake_color_index_command);
//CUSTOM_ID(colors, lake_color_index_namespace);
//CUSTOM_ID(colors, lake_color_member_variable);

#define LAKE_COLOR_INDEX_TYPE      0xFFA0D9AC
#define LAKE_COLOR_INDEX_FUNCTION  0xFF598061
#define LAKE_COLOR_INDEX_MACRO     0xFFF7A6C9
#define LAKE_COLOR_INDEX_COMMAND   0xFFF7A6C9
#define LAKE_COLOR_INDEX_NAMESPACE 0xFF67CC8E
#define LAKE_COLOR_MEMBER_VARIABLE 0xFF88CA8A

typedef struct Lake_Hashes_Notes
{
    u64 *hashes;
    Code_Index_Note **notes;
    i32 count;
} Lake_Hashes_Notes;

typedef struct Lake_Namespace_Member
{
    String_Const_u8 namespace_name;
    String_Const_u8 member_name;
} Lake_Namespace_Member;

typedef struct Lake_Namespace_Store
{
    String_Const_u8 *namespaces;
    i32 namespace_count;

    Lake_Namespace_Member *members;
    i32 member_count;
} Lake_Namespace_Store;

// Global application-wide namespace store cache
global Lake_Namespace_Store global_ns_store = {0};
global Arena global_ns_arena = {0};

// Namespace scanning needs actual tokens to exist, and tokenization is NOT
// guaranteed complete by the time HookID_BeginBuffer/HookID_SaveFile fire
// -- confirmed by [ Lake ] logging showing "(0 tokens)" for a freshly
// opened, clearly non-empty file. Code_Index-based coloring (types/
// functions/macros) doesn't hit this because it's consulted at RENDER
// time, well after tokenization finishes. So: begin_buffer/save_file just
// flip this flag, and the actual rescan happens lazily on the next render
// call, which is the earliest point tokens are reliably present.
global b32 global_ns_cache_dirty = true;

// ------------------------------------------------------------------------
// Include worklist state (see design note at top of file)
// ------------------------------------------------------------------------
#define LAKE_MAX_QUEUED_INCLUDES 512

global Arena global_lake_arena = {0};

// Absolute paths we've already resolved+scanned. Checked BEFORE resolving
// or creating a buffer, so a header included from ten different places
// only ever gets processed once.
global String_Const_u8 lake_scanned_paths[LAKE_MAX_QUEUED_INCLUDES];
global i32 lake_scanned_count = 0;

// Buffers still waiting to be scanned for their own #includes. Only
// project-local (quoted) includes ever get pushed here -- system headers
// are opened+indexed but treated as leaves (see design note).
global Buffer_ID lake_include_queue[LAKE_MAX_QUEUED_INCLUDES];
global i32 lake_queue_head = 0;
global i32 lake_queue_tail = 0;

function void
lake_ensure_arena(void)
{
    if (!global_lake_arena.base_allocator)
    {
        global_lake_arena = make_arena_system();
    }
}

function b32
lake_path_already_scanned(String_Const_u8 path)
{
    for (i32 i = 0; i < lake_scanned_count; i += 1)
    {
        if (string_match(lake_scanned_paths[i], path))
        {
            return true;
        }
    }
    return false;
}

function b32
lake_mark_path_scanned(String_Const_u8 path)
{
    if (lake_path_already_scanned(path)) return false;
    if (lake_scanned_count >= LAKE_MAX_QUEUED_INCLUDES) return false;

    lake_ensure_arena();
    lake_scanned_paths[lake_scanned_count] = push_string_copy(&global_lake_arena, path);
    lake_scanned_count += 1;
    return true;
}

function b32
lake_queue_push(Buffer_ID buffer)
{
    i32 next_tail = (lake_queue_tail + 1) % LAKE_MAX_QUEUED_INCLUDES;
    if (next_tail == lake_queue_head)
    {
        // Queue full -- backstop cap hit. Drop it rather than grow
        // unboundedly; see design note at top of file.
        return false;
    }
    lake_include_queue[lake_queue_tail] = buffer;
    lake_queue_tail = next_tail;
    return true;
}

function b32
lake_queue_pop(Buffer_ID *out_buffer)
{
    if (lake_queue_head == lake_queue_tail) return false; // empty
    *out_buffer = lake_include_queue[lake_queue_head];
    lake_queue_head = (lake_queue_head + 1) % LAKE_MAX_QUEUED_INCLUDES;
    return true;
}

function Lake_Namespace_Store
lake_gather_namespaces_and_members(Application_Links *app, Arena *arena);

function void
lake_update_namespace_cache(Application_Links *app)
{
    if (!global_ns_arena.base_allocator)
    {
        global_ns_arena = make_arena_system();
    }
    else
    {
        linalloc_clear(&global_ns_arena);
    }

    print_message(app, string_u8_litexpr("[ Lake ] rebuilding namespace cache...\n"));
    global_ns_store = lake_gather_namespaces_and_members(app, &global_ns_arena);
}

/* Hardcoded include directories to search on disk for <angle-bracket>
 * (system) includes. These are ONLY used to resolve+index system headers
 * as leaves -- see design note at top of file for why we never recurse
 * into what they themselves #include. */
global String_Const_u8 lake_system_include_paths[] = {
    string_u8_litexpr("C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\MSVC\\14.41.34120\\include"),
    string_u8_litexpr("C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt"),
};

function void
lake_quick_sort_hashes_notes(u64 *hashes, Code_Index_Note **notes, i64 start, i64 end)
{
    if (hashes && start < end)
    {
        u64 pivot_hash = hashes[(start + end) / 2];
        i64 i = start;
        i64 j = end;

        for (;;)
        {
            while (hashes[i] < pivot_hash) i++;
            while (hashes[j] > pivot_hash) j--;

            if (i <= j)
            {
                u64 hash_temp = hashes[i];
                hashes[i] = hashes[j];
                hashes[j] = hash_temp;

                Code_Index_Note *note_temp = notes[i];
                notes[i] = notes[j];
                notes[j] = note_temp;

                i++;
                j--;
            }

            if (i > j) break;
        }

        if (start < j) lake_quick_sort_hashes_notes(hashes, notes, start, j);
        if (i < end)   lake_quick_sort_hashes_notes(hashes, notes, i, end);
    }
}

function b32
lake_is_scope_operator(Application_Links *app, Arena *arena, Buffer_ID buffer, Token *token)
{
    if (token && token->kind == TokenBaseKind_Operator)
    {
        Temp_Memory temp = begin_temp(arena);
        String_Const_u8 lexeme = push_token_lexeme(app, arena, buffer, token);
        b32 result = string_match(lexeme, string_u8_litexpr("::"));
        end_temp(temp);
        return result;
    }
    return false;
}

//function b32
//lake_is_builtin_or_stl_type(String_Const_u8 name)
//{
//    global String_Const_u8 stl_types[] = {
//        string_u8_litexpr("vector"),
//        string_u8_litexpr("string"),
//        string_u8_litexpr("string_view"),
//        string_u8_litexpr("map"),
//        string_u8_litexpr("unordered_map"),
//        string_u8_litexpr("set"),
//        string_u8_litexpr("unique_ptr"),
//        string_u8_litexpr("shared_ptr"),
//        string_u8_litexpr("size_t"),
//        string_u8_litexpr("int8_t"),   string_u8_litexpr("uint8_t"),
//        string_u8_litexpr("int16_t"),  string_u8_litexpr("uint16_t"),
//        string_u8_litexpr("int32_t"),  string_u8_litexpr("uint32_t"),
//        string_u8_litexpr("int64_t"),  string_u8_litexpr("uint64_t"),
//        string_u8_litexpr("uintptr_t"),string_u8_litexpr("intptr_t"),
//    };

//    for (i32 i = 0; i < ArrayCount(stl_types); i += 1)
//    {
//        if (string_match(name, stl_types[i]))
//        {
//            return true;
//        }
//    }
//    return false;
//}

//function b32
//lake_is_builtin_c_function(String_Const_u8 name)
//{
//    global String_Const_u8 c_functions[] = {
//        string_u8_litexpr("printf"),  string_u8_litexpr("fprintf"),
//        string_u8_litexpr("sprintf"), string_u8_litexpr("snprintf"),
//        string_u8_litexpr("scanf"),   string_u8_litexpr("sscanf"),
//        string_u8_litexpr("malloc"),  string_u8_litexpr("calloc"),
//        string_u8_litexpr("realloc"), string_u8_litexpr("free"),
//        string_u8_litexpr("memcpy"),  string_u8_litexpr("memset"),
//        string_u8_litexpr("memmove"), string_u8_litexpr("strlen"),
//        string_u8_litexpr("strcmp"),  string_u8_litexpr("strncmp"),
//    };

//    for (i32 i = 0; i < ArrayCount(c_functions); i += 1)
//    {
//        if (string_match(name, c_functions[i]))
//        {
//            return true;
//        }
//    }
//    return false;
//}

/* Scans loaded buffers to collect namespaces and their declared members.
 * Unchanged from before -- this walks ALREADY-open buffers and doesn't
 * open new ones, so it was never part of the recursion problem.
 *
 * NOTE: matches the "namespace" lexeme regardless of whether this fork's
 * lexer classifies it as TokenBaseKind_Keyword or TokenBaseKind_Identifier
 * -- context-sensitive C++ keywords aren't always in a lexer's reserved
 * table, and checking Keyword-only was the actual reason locally-declared
 * namespaces (unlike the hardcoded "std"/"posix" literals above) never
 * got detected. */
function Lake_Namespace_Store
lake_gather_namespaces_and_members(Application_Links *app, Arena *arena)
{
    Lake_Namespace_Store result = {0};
    i32 max_namespaces = 128;
    i32 max_members = 512;

    result.namespaces = push_array(arena, String_Const_u8, max_namespaces);
    result.members    = push_array(arena, Lake_Namespace_Member, max_members);

    /* Standard built-in namespaces */
    result.namespaces[result.namespace_count++] = string_u8_litexpr("std");
    result.namespaces[result.namespace_count++] = string_u8_litexpr("posix");

    i32 buffers_scanned = 0;

    Buffer_ID buffer_it = get_buffer_next(app, 0, Access_Always);
    while (buffer_it != 0 && result.namespace_count < max_namespaces)
    {
      String_Const_u8 buf_name = push_buffer_file_name(app, arena, buffer_it);
      Temp_Memory name_temp = begin_temp(arena);
      Token_Array tokens = get_token_array_from_buffer(app, buffer_it);

        if (buf_name.size == 0)
        {
            // Internal/non-file-backed buffer (*scratch*, *messages*,
            // etc.) -- nothing useful to scan, and logging these just
            // adds noise every single rebuild.
            end_temp(name_temp);
            buffer_it = get_buffer_next(app, buffer_it, Access_Always);
            continue;
        }

        {
            String_Const_u8 msg = push_u8_stringf(arena,
                "[ Lake ] scanning buffer '%.*s' (%lld tokens)\n",
                string_expand(buf_name), (long long)tokens.count);
            print_message(app, msg);
        }
        end_temp(name_temp);

        buffers_scanned += 1;

        if (tokens.tokens != 0)
        {
            Token_Iterator_Array it = token_iterator_index(0, &tokens, 0);
            for (;;)
            {
                Token *token = token_it_read(&it);
                if (token == 0 || token->kind == TokenBaseKind_EOF) break;

                // NOTE: was `token->kind == TokenBaseKind_Keyword` only --
                // that's the bug. Match the lexeme itself instead of
                // gating on a token kind we can't be sure of.
                if (token->kind == TokenBaseKind_Keyword ||
                    token->kind == TokenBaseKind_Identifier)
                {
                    String_Const_u8 lexeme = push_token_lexeme(app, arena, buffer_it, token);
                    if (string_match(lexeme, string_u8_litexpr("namespace")))
                    {
                        if (token_it_inc_non_whitespace(&it))
                        {
                            Token *ns_token = token_it_read(&it);
                            if (ns_token && ns_token->kind == TokenBaseKind_Identifier)
                            {
                                String_Const_u8 ns_name = push_token_lexeme(app, arena, buffer_it, ns_token);

                                b32 exists = false;
                                for (i32 i = 0; i < result.namespace_count; i += 1)
                                {
                                    if (string_match(result.namespaces[i], ns_name))
                                    {
                                        exists = true;
                                        break;
                                    }
                                }

                                if (!exists && result.namespace_count < max_namespaces)
                                {
                                    result.namespaces[result.namespace_count++] = ns_name;

                                    Temp_Memory log_temp = begin_temp(arena);
                                    String_Const_u8 msg = push_u8_stringf(arena,
                                        "[ Lake ] found namespace '%.*s'\n",
                                        string_expand(ns_name));
                                    print_message(app, msg);
                                    end_temp(log_temp);
                                }

                                /* Parse namespace body { ... } to collect member identifiers */
                                if (token_it_inc_non_whitespace(&it))
                                {
                                    Token *brace_token = token_it_read(&it);
                                    if (brace_token && brace_token->kind == TokenBaseKind_ScopeOpen)
                                    {
                                        i32 scope_depth = 1;
                                        while (token_it_inc_all(&it))
                                        {
                                            Token *body_token = token_it_read(&it);
                                            if (body_token == 0 || body_token->kind == TokenBaseKind_EOF) break;

                                            if (body_token->kind == TokenBaseKind_ScopeOpen)
                                            {
                                                scope_depth += 1;
                                            }
                                            else if (body_token->kind == TokenBaseKind_ScopeClose)
                                            {
                                                scope_depth -= 1;
                                                if (scope_depth == 0) break;
                                            }
                                            else if (scope_depth == 1 && body_token->kind == TokenBaseKind_Identifier)
                                            {
                                                String_Const_u8 member_name = push_token_lexeme(app, arena, buffer_it, body_token);

                                                if (result.member_count < max_members)
                                                {
                                                    b32 m_exists = false;
                                                    for (i32 m = 0; m < result.member_count; m += 1)
                                                    {
                                                        if (string_match(result.members[m].namespace_name, ns_name) &&
                                                            string_match(result.members[m].member_name, member_name))
                                                        {
                                                            m_exists = true;
                                                            break;
                                                        }
                                                    }

                                                    if (!m_exists)
                                                    {
                                                        result.members[result.member_count].namespace_name = ns_name;
                                                        result.members[result.member_count].member_name = member_name;
                                                        result.member_count += 1;

                                                        Temp_Memory log_temp = begin_temp(arena);
                                                        String_Const_u8 msg = push_u8_stringf(arena,
                                                            "[ Lake ]   member '%.*s::%.*s'\n",
                                                            string_expand(ns_name), string_expand(member_name));
                                                        print_message(app, msg);
                                                        end_temp(log_temp);
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                if (!token_it_inc_all(&it)) break;
            }
        }
        buffer_it = get_buffer_next(app, buffer_it, Access_Always);
    }

    {
        Temp_Memory log_temp = begin_temp(arena);
        String_Const_u8 msg = push_u8_stringf(arena,
            "[ Lake ] scan complete: %d buffer(s), %d namespace(s), %d member(s)\n",
            buffers_scanned, result.namespace_count, result.member_count);
        print_message(app, msg);
        end_temp(log_temp);
    }

    return result;
}

// ------------------------------------------------------------------------
// Include resolution -- rewritten around the worklist design (see the
// big comment at the top of the file for why).
// ------------------------------------------------------------------------

/* Resolves a project-local ("quoted") include relative to the including
 * buffer's own directory. Recursion-safe: pushes the new buffer onto the
 * worklist queue ONLY if it hasn't been scanned before (checked by
 * resolved absolute path, before creating anything). */
function void
lake_resolve_local_include(Application_Links *app, Arena *arena,
                            Buffer_ID including_buffer, String_Const_u8 file_name)
{
    String_Const_u8 including_path = push_buffer_file_name(app, arena, including_buffer);
    String_Const_u8 dir = string_remove_last_folder(including_path);

    String_Const_u8 full_path = push_u8_stringf(arena, "%.*s%.*s",
                                                string_expand(dir),
                                                string_expand(file_name));

    if (!lake_mark_path_scanned(full_path))
    {
        // Already scanned (or we're at the cap) -- nothing to do.
        return;
    }

    Buffer_ID existing = get_buffer_by_name(app, full_path, Access_Always);
    Buffer_ID target = existing;

    if (target == 0)
    {
        target = create_buffer(app, full_path, BufferCreate_Background);
    }

    if (target != 0)
    {
        // Only LOCAL includes get queued for further scanning -- this is
        // the actual fix for the explosion. System headers never reach
        // this function (see lake_resolve_system_include).
        lake_queue_push(target);
    }
}

/* Resolves a <system> include against the hardcoded SDK paths and opens
 * it purely as an indexing LEAF -- it gets tokenized/indexed (so
 * jump-to-definition still works for things like printf) but is
 * deliberately never pushed onto the include-scanning queue, so its own
 * (huge, deeply nested) #includes are never followed. */
function void
lake_resolve_system_include(Application_Links *app, Arena *arena, String_Const_u8 file_name)
{
    for (i32 path_index = 0; path_index < ArrayCount(lake_system_include_paths); path_index += 1)
    {
        String_Const_u8 path = lake_system_include_paths[path_index];
        String_Const_u8 full_path = push_u8_stringf(arena, "%.*s\\%.*s",
                                                    string_expand(path),
                                                    string_expand(file_name));

        if (!lake_mark_path_scanned(full_path))
        {
            return; // already indexed this one
        }

        Buffer_ID existing = get_buffer_by_name(app, full_path, Access_Always);
        if (existing != 0)
        {
            return; // already open, and (by construction) never queued for scanning
        }

        Buffer_ID new_buf = create_buffer(app, full_path, BufferCreate_Background);
        if (new_buf != 0)
        {
            // Deliberately NOT pushed to lake_include_queue. Leaf only.
            return;
        }
    }

    // Not found in either SDK path -- fine, most system headers won't
    // resolve against just these two folders (e.g. anything under a
    // deeper WinSDK subfolder). Not an error worth logging per-header;
    // it would be extremely noisy.
}

/* Scans one buffer's tokens for #include directives and dispatches each
 * one to either the local or system resolver. Does NOT recurse and does
 * NOT rescan other buffers -- that's the caller's (worklist drain's) job. */
function void
lake_scan_buffer_includes(Application_Links *app, Arena *arena, Buffer_ID buffer)
{
    if (buffer == 0) return;
    if (!buffer_exists(app, buffer)) return;

    Token_Array tokens = get_token_array_from_buffer(app, buffer);
    if (tokens.tokens == 0 || tokens.count == 0) return;

    Token_Iterator_Array it = token_iterator_index(0, &tokens, 0);

    for (;;)
    {
        Token *token = token_it_read(&it);
        if (token == 0 || token->kind == TokenBaseKind_EOF) break;

        if (token->kind == TokenBaseKind_Preprocessor)
        {
            Temp_Memory temp = begin_temp(arena);
            String_Const_u8 lexeme = push_token_lexeme(app, arena, buffer, token);

            if (string_match(lexeme, string_u8_litexpr("#include")) ||
                string_match(lexeme, string_u8_litexpr("include")))
            {
                if (token_it_inc_non_whitespace(&it))
                {
                    Token *header_token = token_it_read(&it);
                    if (header_token &&
                       (header_token->kind == TokenBaseKind_LiteralString ||
                        header_token->kind == TokenBaseKind_LexError))
                    {
                        String_Const_u8 header_lexeme = push_token_lexeme(app, arena, buffer, header_token);

                        if (header_lexeme.size >= 2)
                        {
                            u8 first = header_lexeme.str[0];
                            u8 last  = header_lexeme.str[header_lexeme.size - 1];
                            b32 is_quoted = (first == '"' && last == '"');
                            b32 is_system = (first == '<' && last == '>');

                            if (is_quoted || is_system)
                            {
                                header_lexeme.str += 1;
                                header_lexeme.size -= 2;

                                if (is_quoted)
                                {
                                    lake_resolve_local_include(app, arena, buffer, header_lexeme);
                                }
                                else
                                {
                                    lake_resolve_system_include(app, arena, header_lexeme);
                                }
                            }
                        }
                    }
                }
            }
            end_temp(temp);
        }

        if (!token_it_inc_all(&it)) break;
    }
}

/* Drains the include worklist to exhaustion. Bounded by construction:
 * every push goes through lake_mark_path_scanned first, so the same file
 * can only ever enter the queue once, and the queue itself is capped at
 * LAKE_MAX_QUEUED_INCLUDES as a hard backstop. No full-buffer rescans. */
function void
lake_drain_include_queue(Application_Links *app, Arena *arena)
{
    Buffer_ID buffer;
    i32 processed = 0;

    while (lake_queue_pop(&buffer) && processed < LAKE_MAX_QUEUED_INCLUDES)
    {
        lake_scan_buffer_includes(app, arena, buffer);
        processed += 1;
    }
}

// ------------------------------------------------------------------------
// Code index note lookup (unchanged -- this part was never the problem)
// ------------------------------------------------------------------------

function Lake_Hashes_Notes
lake_create_big_note_array(Application_Links *app, Arena *arena)
{
    ProfileScope(app, "lake_create_big_note_array");

    Lake_Hashes_Notes hashes_notes = {0};
    Buffer_ID buffer_it = get_buffer_next(app, 0, Access_Always);

    while (buffer_it)
    {
        Code_Index_File *file = code_index_get_file(buffer_it);
        if (file)
        {
            hashes_notes.count += file->note_array.count;
        }
        buffer_it = get_buffer_next(app, buffer_it, Access_Always);
    }

    hashes_notes.hashes = push_array(arena, u64, hashes_notes.count);
    hashes_notes.notes  = push_array(arena, Code_Index_Note *, hashes_notes.count);

    i32 count = 0;
    {
        ProfileScope(app, "lake create hashes");

        buffer_it = get_buffer_next(app, 0, Access_Always);
        while (buffer_it)
        {
            Code_Index_File *file = code_index_get_file(buffer_it);
            if (file)
            {
                for (i32 i = 0; i < file->note_array.count; i += 1)
                {
                    hashes_notes.notes[count] = file->note_array.ptrs[i];
                    hashes_notes.hashes[count] =
                        table_hash_u8(hashes_notes.notes[count]->text.str,
                                      hashes_notes.notes[count]->text.size);
                    count += 1;
                }
            }
            buffer_it = get_buffer_next(app, buffer_it, Access_Always);
        }
    }

    if (count)
    {
        ProfileScope(app, "lake_quick_sort_hashes_notes");
        lake_quick_sort_hashes_notes(hashes_notes.hashes, hashes_notes.notes, 0, count - 1);
    }

    return hashes_notes;
}

/* Helper: Safety check for note kind weights */
function u8
lake_get_note_weight(Code_Index_Note_Kind kind)
{
    switch (kind)
    {
        case CodeIndexNote_4coderCommand: return 1;
        case CodeIndexNote_Function:      return 2;
        case CodeIndexNote_Type:          return 3;
        case CodeIndexNote_Macro:         return 4;
        default:                          return 0;
    }
}

function String_Const_u8
lake_push_string_copy(Arena *arena, String_Const_u8 src)
{
    String_Const_u8 result = {};
    if (src.size > 0 && arena != 0)
    {
        u8 *str = push_array(arena, u8, src.size + 1);
        block_copy(str, src.str, src.size);
        str[src.size] = 0;
        
        result.str = str;
        result.size = src.size;
    }
    return result;
}

function b32
lake_is_known_namespace(Lake_Namespace_Store *store, String_Const_u8 name)
{
    if (name.size == 0) return false;

    if (string_compare(name, string_u8_litexpr("std")) == 0 ||
        string_compare(name, string_u8_litexpr("std11")) == 0)
    {
        return true;
    }

    if (store == 0) return false;

    for (i32 i = 0; i < store->namespace_count; i += 1)
    {
        if (string_compare(store->namespaces[i], name) == 0)
        {
            return true;
        }
    }

    return false;
}

function b32
lake_is_known_namespace_member(Lake_Namespace_Store *store, String_Const_u8 ns_name, String_Const_u8 member_name)
{
    if (store == 0 || member_name.size == 0) return false;

    for (i32 i = 0; i < store->member_count; i += 1)
    {
        Lake_Namespace_Member *m = &store->members[i];

        if (ns_name.size > 0)
        {
            if (string_compare(m->namespace_name, ns_name) == 0 &&
                string_compare(m->member_name, member_name) == 0)
            {
                return true;
            }
        }
        else
        {
            if (string_compare(m->member_name, member_name) == 0)
            {
                return true;
            }
        }
    }

    return false;
}

function Code_Index_Note *
lake_get_note(Application_Links *app, Lake_Hashes_Notes *hashes_notes, String_Const_u8 name)
{
    ProfileScope(app, "lake_get_note");

    if (hashes_notes->count <= 0) return 0;

    Code_Index_Note *result = 0;
    u64 name_hash = table_hash_u8(name.str, name.size);

    i32 start = 0;
    i32 end   = hashes_notes->count - 1;

    while (start <= end)
    {
        i32 middle = (start + end) / 2;
        u64 note_hash = hashes_notes->hashes[middle];

        if (name_hash < note_hash)
        {
            end = middle - 1;
        }
        else if (name_hash > note_hash)
        {
            start = middle + 1;
        }
        else
        {
            while (middle - 1 >= start && hashes_notes->hashes[middle - 1] == name_hash)
            {
                middle -= 1;
            }

            u8 current_weight = 0;
            while (middle <= end && hashes_notes->hashes[middle] == name_hash)
            {
                Code_Index_Note *note = hashes_notes->notes[middle];
                u8 weight = lake_get_note_weight(note->note_kind);

                if (weight > current_weight)
                {
                    if (string_compare(name, note->text) == 0)
                    {
                        current_weight = weight;
                        result = note;
                        if (current_weight == 4) break;
                    }
                }

                middle += 1;
            }

            break;
        }
    }

    return result;
}
function void
lake_draw_cpp_token_colors(Application_Links *app, Text_Layout_ID text_layout_id,
                            Token_Array *array, Buffer_ID buffer)
{
    Range_i64 visible_range = text_layout_get_visible_range(app, text_layout_id);
    i64 first_index = token_index_from_pos(array, visible_range.first);
    Token_Iterator_Array it = token_iterator_index(0, array, first_index);

    Scratch_Block scratch(app);
    code_index_lock();

    if (global_ns_cache_dirty)
    {
        lake_update_namespace_cache(app);
        global_ns_cache_dirty = false;
    }

    Lake_Hashes_Notes hashes_notes = lake_create_big_note_array(app, scratch);
    Lake_Namespace_Store *ns_store = &global_ns_store;

    ProfileBlockNamed(app, "lake token loop", lake_token_loop);

    for (;;)
    {
        Token *token = token_it_read(&it);
        if (token == 0 || token->pos >= visible_range.one_past_last) break;

        FColor color = get_token_color_cpp(*token);

        if (token->kind == TokenBaseKind_Identifier)
        {
            Temp_Memory temp = begin_temp(scratch);
            String_Const_u8 lexeme = push_token_lexeme(app, scratch, buffer, token);

            Token_Iterator_Array prev_it = it;
            b32 has_prev = token_it_dec_non_whitespace(&prev_it);
            Token *prev_token = has_prev ? token_it_read(&prev_it) : 0;

            Token_Iterator_Array next_it = it;
            b32 has_next = token_it_inc_non_whitespace(&next_it);
            Token *next_token = has_next ? token_it_read(&next_it) : 0;

            b32 prev_is_dot_arrow = prev_token && prev_token->kind == TokenBaseKind_Operator &&
                (prev_token->sub_kind == TokenCppKind_Dot || prev_token->sub_kind == TokenCppKind_Arrow);

            b32 prev_is_scope = prev_token && lake_is_scope_operator(app, scratch, buffer, prev_token);
            b32 next_is_scope = next_token && lake_is_scope_operator(app, scratch, buffer, next_token);

            // Check if the next token is '(' to identify function calls
            b32 is_func_call = (next_token && next_token->kind == TokenBaseKind_ParentheticalOpen);

            if (prev_is_dot_arrow)
            {
                // Access via `.` or `->` (e.g., items.push_back vs buf->state)
                Code_Index_Note *note = lake_get_note(app, &hashes_notes, lexeme);
                if ((note && note->note_kind == CodeIndexNote_Function) || is_func_call)
                {
                    color = fcolor_argb(LAKE_COLOR_INDEX_FUNCTION); // Pink / Function
                }
                else
                {
                    color = fcolor_argb(LAKE_COLOR_MEMBER_VARIABLE); // Tan / Member variable
                }
            }
            else if (next_is_scope)
            {
                // LHS of :: (e.g. AppState in AppState::something)
                if (lake_is_known_namespace(ns_store, lexeme))
                {
                    color = fcolor_argb(LAKE_COLOR_INDEX_NAMESPACE); // Red
                }
                else
                {
                    Code_Index_Note *note = lake_get_note(app, &hashes_notes, lexeme);
                    if (note && note->note_kind == CodeIndexNote_Type)
                    {
                        color = fcolor_argb(LAKE_COLOR_INDEX_TYPE); // Green for types
                    }
                }
            }
            else if (prev_is_scope)
            {
                // RHS of :: (e.g. something or func() in AppState::something)
                String_Const_u8 lhs_lexeme = {};
                Token_Iterator_Array lhs_it = prev_it;
                if (token_it_dec_non_whitespace(&lhs_it))
                {
                    Token *lhs_token = token_it_read(&lhs_it);
                    if (lhs_token && lhs_token->kind == TokenBaseKind_Identifier)
                    {
                        lhs_lexeme = push_token_lexeme(app, scratch, buffer, lhs_token);
                    }
                }

                Code_Index_Note *note = lake_get_note(app, &hashes_notes, lexeme);

                if (note)
                {
                    switch (note->note_kind)
                    {
                        case CodeIndexNote_Type:          color = fcolor_argb(LAKE_COLOR_INDEX_TYPE); break;
                        case CodeIndexNote_Function:      color = fcolor_argb(LAKE_COLOR_INDEX_FUNCTION); break;
                        case CodeIndexNote_Macro:         color = fcolor_argb(LAKE_COLOR_INDEX_MACRO); break;
                        case CodeIndexNote_4coderCommand: color = fcolor_argb(LAKE_COLOR_INDEX_COMMAND); break;
                    }
                }
                else if (lake_is_known_namespace_member(ns_store, lhs_lexeme, lexeme))
                {
                    if (is_func_call)
                    {
                        color = fcolor_argb(LAKE_COLOR_INDEX_FUNCTION); // Pink
                    }
                    else
                    {
                        color = fcolor_argb(LAKE_COLOR_MEMBER_VARIABLE); // Tan / Member variable
                    }
                }
            }
            else
            {
                // Standalone identifier
                Code_Index_Note *note = lake_get_note(app, &hashes_notes, lexeme);
                if (note)
                {
                    switch (note->note_kind)
                    {
                        case CodeIndexNote_Type:          color = fcolor_argb(LAKE_COLOR_INDEX_TYPE); break;
                        case CodeIndexNote_Function:      color = fcolor_argb(LAKE_COLOR_INDEX_FUNCTION); break;
                        case CodeIndexNote_Macro:         color = fcolor_argb(LAKE_COLOR_INDEX_MACRO); break;
                        case CodeIndexNote_4coderCommand: color = fcolor_argb(LAKE_COLOR_INDEX_COMMAND); break;
                    }
                }
            }

            end_temp(temp);
        }

        ARGB_Color argb = fcolor_resolve(color);
        paint_text_color(app, text_layout_id, Ii64_size(token->pos, token->size), argb);

        if (!token_it_inc_all(&it)) break;
    }

    ProfileCloseNow(lake_token_loop);
    code_index_unlock();
}

CUSTOM_COMMAND_SIG(lake_jump_to_definition)
CUSTOM_DOC("Jumps to the definition of the symbol under the cursor using the code index.")
{
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
    i64 pos = view_get_cursor_pos(app, view);

    Scratch_Block scratch(app);

    // 1. Get the token under the cursor
    Token_Array tokens = get_token_array_from_buffer(app, buffer);
    if (tokens.tokens == 0) return;

    Token *token = token_from_pos(&tokens, pos);
    if (token == 0 || token->kind != TokenBaseKind_Identifier) return;

    String_Const_u8 symbol_name = push_token_lexeme(app, scratch, buffer, token);

    // 2. Query code index notes
    code_index_lock();
    Lake_Hashes_Notes hashes_notes = lake_create_big_note_array(app, scratch);

    // Look up the note using symbol_name
    Code_Index_Note *note = lake_get_note(app, &hashes_notes, symbol_name);
    if (note == 0)
    {
        note = code_index_note_from_string(symbol_name);
    }

    // 3. Check validity and perform jump
    if (note != 0 && note->file != 0)
    {
        Buffer_ID target_buffer = note->file->buffer;
        i64 target_pos = note->pos.first;
        code_index_unlock();

        jump_to_location(app, view, target_buffer, target_pos);
    }
    else
    {
        code_index_unlock();
        String_Const_u8 msg = push_u8_stringf(scratch, "[Lake Debug] Definition not found for: %.*s\n",
                                              string_expand(symbol_name));
        print_message(app, msg);
    }
}

CUSTOM_COMMAND_SIG(lake_reload_if_unloaded_changes)
CUSTOM_DOC("Reloads the current buffer from disk if it contains unloaded external changes.")
{
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);
    
    if (buffer == 0) return;

    Scratch_Block scratch(app);

    // Retrieve the buffer's dirty / modification state flags
    Dirty_State dirty_state = buffer_get_dirty_state(app, buffer);

    if (HasFlag(dirty_state, DirtyState_UnloadedChanges))
    {
        // Reopen the buffer using the correct 3-argument signature (passing 0 for default flags)
        Buffer_Reopen_Result result = buffer_reopen(app, buffer, 0);
        
        if (result != 0)
        {
            String_Const_u8 msg = push_u8_stringf(scratch, "[Lake] Successfully reloaded buffer with external changes.\n");
            print_message(app, msg);
        }
        else
        {
            String_Const_u8 msg = push_u8_stringf(scratch, "[Lake] Failed to reload buffer from disk.\n");
            print_message(app, msg);
        }
    }
    else
    {
        String_Const_u8 msg = push_u8_stringf(scratch, "[Lake] No unloaded changes detected for this buffer.\n");
        print_message(app, msg);
    }
}

CUSTOM_COMMAND_SIG(lake_reload_all_unloaded_changes)
CUSTOM_DOC("Iterates through all open buffers and reloads any that contain unloaded external changes.")
{
    Scratch_Block scratch(app);
    int reloaded_count = 0;

    // Iterate through all buffers using 4coder's buffer traversal API
    for (Buffer_ID buffer = get_buffer_next(app, 0, Access_Always); buffer != 0; buffer = get_buffer_next(app, buffer, Access_Always))
    {
        // Retrieve dirty/modification flags for the current buffer
        Dirty_State dirty_state = buffer_get_dirty_state(app, buffer);

        if (HasFlag(dirty_state, DirtyState_UnloadedChanges))
        {
            // Reopen the buffer from disk
            Buffer_Reopen_Result result = buffer_reopen(app, buffer, 0);
            
            if (result != 0)
            {
                reloaded_count += 1;
            }
        }
    }

    // Print a summary message to the 4coder message buffer
    String_Const_u8 msg = push_u8_stringf(scratch, "[Lake] Successfully reloaded %d buffer(s) with unloaded external changes.\n", reloaded_count);
    print_message(app, msg);
}
CUSTOM_COMMAND_SIG(lake_log_current_directory)
CUSTOM_DOC("Logs all files and directories in the current buffer's directory.")
{
    Scratch_Block scratch(app);

    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);

    String_Const_u8 file_name =
        push_buffer_file_name(app, scratch, buffer);

    if (file_name.size == 0)
    {
        print_message(
            app,
            string_u8_litexpr(
                "[Lake] Error: Could not determine current file path.\n"
            )
        );
        return;
    }

    String_Const_u8 directory =
        string_remove_last_folder(file_name);

    print_message(
        app,
        push_u8_stringf(
            scratch,
            "[Lake] Directory: %.*s\n",
            string_expand(directory)
        )
    );

    File_List file_list =
        system_get_file_list(scratch, directory);

    print_message(
        app,
        push_u8_stringf(
            scratch,
            "[Lake] Found %d entries:\n",
            file_list.count
        )
    );

    for (File_Info **node = file_list.infos;
         node < file_list.infos + file_list.count;
         ++node)
    {
        File_Info *info = *node;

        b32 is_directory =
            ((info->attributes.flags & FileAttribute_IsDirectory) != 0);

        if (is_directory)
        {
            print_message(
                app,
                push_u8_stringf(
                    scratch,
                    "  [DIR ] %.*s\n",
                    string_expand(info->file_name)
                )
            );
        }
        else
        {
            print_message(
                app,
                push_u8_stringf(
                    scratch,
                    "  [FILE] %.*s\n",
                    string_expand(info->file_name)
                )
            );
        }
    }
}
CUSTOM_COMMAND_SIG(lake_format_project_files)
CUSTOM_DOC("Finds the project-root .clang-format file and runs clang-format on all relevant source files.")
{
    Scratch_Block scratch(app);

    // Get the current file.
    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);

    String_Const_u8 file_name = push_buffer_file_name(app, scratch, buffer);

    if (file_name.size == 0)
    {
        print_message(
            app,
            string_u8_litexpr(
                "[Lake] Error: Could not determine current file path.\n"
            )
        );
        return;
    }

    // Start at the current file's directory.
    String_Const_u8 directory =
        string_remove_last_folder(file_name);

    // Search upward for .clang-format.
    String_Const_u8 clang_format_file = {};

    for (;;)
    {
        String_Const_u8 candidate =
            push_u8_stringf(
                scratch,
                "%.*s\\.clang-format",
                string_expand(directory)
            );

        if (file_exists(
                app, candidate))
        {
            clang_format_file = candidate;
            break;
        }

        String_Const_u8 parent =
            string_remove_last_folder(directory);

        if (parent.size == directory.size)
        {
            break;
        }

        directory = parent;
    }
    if (clang_format_file.size == 0)
    {
        print_message(
            app,
            string_u8_litexpr(
                "[Lake] Error: Could not find .clang-format in this "
                "directory or any parent directory.\n"
            )
        );
        return;
    }

    print_message(
        app,
        push_u8_stringf(
            scratch,
            "[Lake] Found .clang-format: %.*s\n",
            string_expand(clang_format_file)
        )
    );

    // Explicitly tell clang-format which config file to use.
    String_Const_u8 cmd =
        push_u8_stringf(
            scratch,
            "cd /d \"%.*s\" && "
            "for /r %%i in (*.cpp *.h *.c *.cc) do "
            "clang-format -i -style=\"file:%.*s\" \"%%i\"",
            string_expand(directory),
            string_expand(clang_format_file)
        );

    print_message(
        app,
        string_u8_litexpr(
            "[Lake] Running clang-format across project files...\n"
        )
    );

    Buffer_Identifier out_buffer =
        buffer_identifier(
            string_u8_litexpr("*clang-format*")
        );

    exec_system_command(
        app,
        view,
        out_buffer,
        directory,
        cmd,
        CLI_OverlapWithConflict | CLI_CursorAtEnd
    );

    Buffer_ID jump_buffer_id =
        get_buffer_by_name(
            app,
            string_u8_litexpr("*clang-format*"),
            Access_Always
        );

    if (jump_buffer_id != 0)
    {
        lock_jump_buffer(
            app,
            string_u8_litexpr("*clang-format*")
        );
    }
}
// Helper function to remove the path from a string (keeps only the filename)
String_Const_u8 string_remove_path(String_Const_u8 string)
{
    u64 last_slash = 0;
    for (u64 i = 0; i < string.size; i += 1)
    {
        if (string.str[i] == '/' || string.str[i] == '\\')
        {
            last_slash = i + 1;
        }
    }
    return string_skip(string, last_slash);
}

// Helper function to remove everything from the last period onwards (e.g., removing file extension)
String_Const_u8 string_remove_last_period(String_Const_u8 string)
{
    u64 last_dot = string.size;
    for (int i = (int)string.size - 1; i >= 0; i -= 1)
    {
        if (string.str[i] == '.')
        {
            last_dot = (int)i;
            break;
        }
    }
    return string_prefix(string, last_dot);
}

// Helper function to compare two String_Const_u8 strings for equality
b32 are_strings_equal(String_Const_u8 a, String_Const_u8 b)
{
    if (a.size != b.size)
    {
        return false;
    }
    for (u64 i = 0; i < a.size; i += 1)
    {
        if (a.str[i] != b.str[i])
        {
            return false;
        }
    }
    return true;
}

CUSTOM_COMMAND_SIG(lake_insert_header_boiler)
CUSTOM_DOC("Inserts a standard file header template for C/C++ source and header files based on the current filename.")
{
    Scratch_Block scratch(app);

    View_ID view = get_active_view(app, Access_Always);
    Buffer_ID buffer = view_get_buffer(app, view, Access_Always);

    String_Const_u8 file_name_str = push_buffer_file_name(app, scratch, buffer);
    if (file_name_str.size == 0)
    {
        print_message(app, string_u8_litexpr("[Lake] Error: Could not determine current filename.\n"));
        return;
    }

    // Extract file extension and base name
    String_Const_u8 ext = string_file_extension(file_name_str);
    String_Const_u8 base_name = string_remove_last_period(string_remove_path(file_name_str));

    // Get current date (YYYY.MM.DD)
    // Note: You can use system time functions or define a format string depending on your custom layer's date helper.
    // For simplicity, we construct the insertion text using scratch formatting.

    if (are_strings_equal(ext, string_u8_litexpr("cpp")) || are_strings_equal(ext, string_u8_litexpr("c")))
    {
        String_Const_u8 text_to_insert = push_u8_stringf(scratch,
            "\n"
            "/**\n"
            " * @file   %.*s.%*s\n"
            " * @author Jake Lee (lake.jee.tech@gmail.com)\n"
            " * @brief  \n"
            " * @date   2026.08.18\n"
            " */\n"
            "\n"
            "#include \"%.*s.h\"\n"
            "\n"
            "// End of %.*s.%*s\n",
            string_expand(base_name), string_expand(ext),
            string_expand(base_name),
            string_expand(base_name), string_expand(ext)
        );

        buffer_replace_range(app, buffer, Range_i64{0, 0}, text_to_insert);
    }
    else if (are_strings_equal(ext, string_u8_litexpr("h")))
    {
        // Convert base name to uppercase guard name if needed
        // (Assuming you have a helper or can write out uppercase macro guards)
        String_Const_u8 text_to_insert = push_u8_stringf(scratch,
            "\n"
            "/**\n"
            " * @file   %.*s.h\n"
            " * @author Jake Lee (lake.jee.tech@gmail.com)\n"
            " * @brief  \n"
            " * @date   2026.08.18\n"
            " */\n"
            "\n"
            "#ifndef %.*s_H\n"
            "#define %.*s_H\n"
            "\n"
            "\n"
            "#endif // %.*s_H\n",
            string_expand(base_name),
            string_expand(base_name),
            string_expand(base_name),
            string_expand(base_name)
        );

        buffer_replace_range(app, buffer, Range_i64{0, 0}, text_to_insert);
    }
}

function BUFFER_HOOK_SIG(lake_on_begin_buffer)
{
    ProfileScope(app, "begin buffer");
    default_begin_buffer(app, buffer_id);

    Scratch_Block scratch(app);
    lake_ensure_arena();

    // Push just the buffer that was actually opened, then drain the
    // worklist -- no rescanning of every other open buffer. Each header
    // discovered along the way gets scanned exactly once (enforced by
    // lake_mark_path_scanned), and system headers are leaves, so this
    // terminates quickly regardless of how large the project is.
    lake_queue_push(buffer_id);
    lake_drain_include_queue(app, scratch);

    // NOTE: was calling lake_update_namespace_cache(app) directly here --
    // that's what was scanning with 0 tokens. Tokenization isn't done yet
    // at begin_buffer time. Just flag dirty; lake_draw_cpp_token_colors
    // picks this up on the next render, once tokens actually exist.
    print_message(app, string_u8_litexpr("[ Lake ] begin_buffer: flagged dirty (rebuild deferred to render)\n"));
    global_ns_cache_dirty = true;

    return 0;
}

function BUFFER_HOOK_SIG(lake_on_save_file)
{
    clean_trailing_whitespace(app);

    Scratch_Block scratch(app);
    lake_ensure_arena();

    lake_queue_push(buffer_id);
    lake_drain_include_queue(app, scratch);
    print_message(app, string_u8_litexpr("[ Lake ] save_file: flagged dirty (rebuild deferred to render)\n"));
    global_ns_cache_dirty = true;

    return 0;
}

/* Registers lake's begin-buffer/save-file hooks. These were previously
 * defined but never wired up anywhere -- set_all_default_hooks(app) only
 * installs 4coder's OWN default_begin_buffer/default save hooks, it has
 * no idea lake_on_begin_buffer/lake_on_save_file exist. That's why
 * include scanning and namespace detection silently never ran: this call
 * was simply missing.
 *
 * Call this from custom_layer_init in 4coder_default_bindings.cpp, AFTER
 * set_all_default_hooks(app) (so lake's hooks take over from the
 * defaults for these two specifically -- they each call the
 * corresponding default_* function internally first, so nothing about
 * default behavior is lost, this only adds to it). e.g.:
 *
 *     set_all_default_hooks(app);
 *     lake_install_hooks(app);
 *     mapping_init(tctx, &framework_mapping);
 *     ...
 */
function void
lake_install_hooks(Application_Links *app)
{
    set_custom_hook(app, HookID_BeginBuffer, lake_on_begin_buffer);
    set_custom_hook(app, HookID_SaveFile, lake_on_save_file);
}