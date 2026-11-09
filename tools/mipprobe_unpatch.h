/* mipprobe_unpatch.h - this process's copy of the swizzle detour and cave. */
int rb_detour_state(void);   /* 1 detour present, 0 original prologue, -1 neither, -2 no row/driver */
int rb_detour_remove(void);  /* 0 removed, else a kern_return or -2 */
int rb_cave_state(void);     /* 1 the cave THIS BUILD's row carries, 0 some other bytes, -2 no row/driver */
int rb_cave_install(void);   /* write this build's cave and detour here; 0 ok, else kern_return or -2/-3 */
