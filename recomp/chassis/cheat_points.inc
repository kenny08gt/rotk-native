// Instructions that cheats may replace (see cheats.h). tools/add_hle_points.py places a
// ROTK_CHEAT_POINT at each; rebuild after adding one. Included by cheats.c as a C array.
0x80064DBC,  // Unlimited Health: the player's health -= damage
0x80064F08,  // Unlimited Secondary Weapon: the ammo count update
0x800B4E64,  // One Shot Kill Enemies: an enemy hit's store
