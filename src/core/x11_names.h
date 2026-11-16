// X11's headers, which plume's Vulkan interface brings in on Linux, define plain words as macros. Include this after
// them and before an SDK header that uses one of those words as a name (rex/ui/window.h's CursorShape).
// No include guard: it has to work each time it is included, wherever X11's headers came in.
#ifdef CursorShape
#undef CursorShape
#endif
