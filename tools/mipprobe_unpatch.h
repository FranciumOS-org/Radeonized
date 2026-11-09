int rb_detour_state(void);   /* 1 detour present, 0 original prologue, -1 neither, -2 no row/driver */
int rb_detour_remove(void);  /* 0 removed, else a kern_return or -2 */
