#include "inputmap.h"

namespace InputMap {

uint32_t assign[F_COUNT] = { BTN_A, BTN_Y, BTN_R, BTN_L };
uint32_t pressedNow = 0;

void resetDefaults() { assign[F_ACTION] = BTN_A; assign[F_INVENTORY] = BTN_Y; assign[F_SCROLL_UP] = BTN_R; assign[F_SCROLL_DOWN] = BTN_L; }

uint32_t takePressed() { uint32_t p = pressedNow; pressedNow = 0; return p; }

const char *buttonName(uint32_t bit) {
	switch (bit) {
	case BTN_A: return "A";
	case BTN_B: return "B";
	case BTN_X: return "X";
	case BTN_Y: return "Y";
	case BTN_L: return "L";
	case BTN_R: return "R";
	default: return "?";
	}
}

void set(int f, uint32_t bit) {
	if (f < 0 || f >= F_COUNT) return;
	if (bit != BTN_A && bit != BTN_B && bit != BTN_X && bit != BTN_Y && bit != BTN_L && bit != BTN_R) return;
	for (int i = 0; i < F_COUNT; i++)
		if (i != f && assign[i] == bit) assign[i] = assign[f];
	assign[f] = bit;
}

}
