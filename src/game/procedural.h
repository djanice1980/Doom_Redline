#pragma once
// Procedurally generated fallback art and sounds, used when no Doom WAD is
// available (and for the block-mode textures, which are ours regardless).
#include <map>
#include <string>
#include <vector>

#include "core/image.h"

namespace rl::proc {

// Textures for the block mode ------------------------------------------------
Image blockTexture(int size = 32);        // bevelled tile, white so it can be tinted
Image redBlockTexture(int size = 32);     // cracked, glowing "refusing" block
Image wallTexture(int size = 64);         // tech wall for the arena
Image floorTexture(int size = 64);
Image skyGradient(int w = 256, int h = 64);

// Sprites ---------------------------------------------------------------------
Image enemyFrame(int frame, int size = 64);      // 0 idle, 1 attack, 2 pain, 3..6 death
Image gunFrame(int frame, int w = 96, int h = 72); // 0 idle, 1 fire, 2 recoil
Image muzzleFlash(int size = 48);
Image explosionFrame(int frame, int size = 64);  // 0..4
Image fireball(int size = 16);
Image crosshair(int size = 15);
Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);

// 5x7 bitmap font scaled by `scale`. Keys are the ASCII chars.
std::map<char, Image> font(int scale = 2);

// Sounds (mono float samples) -------------------------------------------------
struct Sound { int rate = 22050; std::vector<float> samples; };
Sound sndShoot();
Sound sndExplode();
Sound sndHit();
Sound sndMove();
Sound sndRotate();
Sound sndLock();
Sound sndClear();
Sound sndRedLine();     // alarm
Sound sndPain();
Sound sndEnemyDie();
Sound sndLevelUp();
Sound sndFireball();
Sound sndGameOver();

}  // namespace rl::proc
