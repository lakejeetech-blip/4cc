/*
 * main.cpp
 * ------------------------------------------------------------------------
 * Test file for verifying semantic token coloring in 4coder.
 *
 * TESTS:
 * 1. Standard headers (<stdio.h>, <vector>) -> tests lake_resolve_and_load_include
 * 2. Types / Enums / Structs -> tests lake_color_index_type
 * 3. Functions -> tests lake_color_index_function
 * 4. Preprocessor Macros -> tests lake_color_index_macro
 * 5. Member access (. and ->) -> verifies type coloring bypass on fields
 * 6. Local variables vs Macros -> tests name collision behavior (e.g. 'X')
 * ------------------------------------------------------------------------
 */

#include <stdio.h>
#include <vector>

// Macro definitions
#define MAX_BUFFER_SIZE 1024
#define CALC_SQUARE(x) ((x) * (x))

// Typedefs and Enums
typedef unsigned long long u64;

enum App_State
{
  AppState_Uninitialized,
  AppState_Running,
  AppState_Shutdown
};

// Custom Struct
struct Texture_Buffer
{
  u64 id;
  u64 width;
  u64 height;
  App_State state;
};

// Function Definitions
static Texture_Buffer
create_texture_buffer(u64 width, u64 height)
{
  Texture_Buffer buf = {};
  buf.id = 1;
  buf.width = width;   // 'width' here is member access (dot), should not color as type
  buf.height = height;
  buf.state = AppState_Running;
  return buf;
}

static void
update_texture_state(Texture_Buffer *buf, App_State new_state)
{
  if (buf)
  {
    buf->state = new_state; // 'state' here is member access (arrow)
  }
}


namespace AppState {
  int something;
  
};

namespace SomeNameSpace {
  typedef struct ass ass;
  struct ass {
    int fuck;
  };
};

int main(int argc, char **argv)
{
  // 1. Type & Function Highlighting
  Texture_Buffer main_texture = create_texture_buffer(1920, 1080);
  App_State current_state = AppState_Running;
  
  // 2. Member Access Recoloring Check
  update_texture_state(&main_texture, AppState_Shutdown);
  
  // 3. Macro Highlighting
  u64 buffer_capacity = MAX_BUFFER_SIZE;
  u64 area = CALC_SQUARE(12);
  
  // 4. Local Variable Highlighting (testing single-letter loop index vs macro)
  for (u64 X = 0; X < 10; X += 1)
  {
    u64 val = X * 2;
    printf("Value: %llu\n", val);
  }
  
  // 5. Standard Library Include Resolution Check
  std::vector<int> items;
  items.push_back((int)buffer_capacity);
  AppState::something = 2;
  
  auto thing = SomeNameSpace::ass{};
  this_name_space_doesnt_exist::something;
  
  
  return 0;
}