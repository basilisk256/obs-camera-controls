#include "easing.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static float linear(float t) { return t; }

static float quad_in(float t) { return t * t; }
static float quad_out(float t) { return t * (2.0f - t); }
static float quad_inout(float t) {
	if (t < 0.5f) return 2.0f * t * t;
	return -1.0f + (4.0f - 2.0f * t) * t;
}

static float cubic_in(float t) { return t * t * t; }
static float cubic_out(float t) { float f = t - 1.0f; return f * f * f + 1.0f; }
static float cubic_inout(float t) {
	if (t < 0.5f) return 4.0f * t * t * t;
	float f = 2.0f * t - 2.0f;
	return 0.5f * f * f * f + 1.0f;
}

static float quart_in(float t) { return t * t * t * t; }
static float quart_out(float t) { float f = t - 1.0f; return 1.0f - f * f * f * f; }
static float quart_inout(float t) {
	if (t < 0.5f) return 8.0f * t * t * t * t;
	float f = t - 1.0f;
	return 1.0f - 8.0f * f * f * f * f;
}

static float quint_in(float t) { return t * t * t * t * t; }
static float quint_out(float t) { float f = t - 1.0f; return f * f * f * f * f + 1.0f; }
static float quint_inout(float t) {
	if (t < 0.5f) return 16.0f * t * t * t * t * t;
	float f = 2.0f * t - 2.0f;
	return 0.5f * f * f * f * f * f + 1.0f;
}

static float sine_in(float t) { return 1.0f - cosf(t * (float)M_PI * 0.5f); }
static float sine_out(float t) { return sinf(t * (float)M_PI * 0.5f); }
static float sine_inout(float t) { return 0.5f * (1.0f - cosf(t * (float)M_PI)); }

static float circ_in(float t) { return 1.0f - sqrtf(1.0f - t * t); }
static float circ_out(float t) { float f = t - 1.0f; return sqrtf(1.0f - f * f); }
static float circ_inout(float t) {
	if (t < 0.5f) return 0.5f * (1.0f - sqrtf(1.0f - 4.0f * t * t));
	float f = 2.0f * t - 2.0f;
	return 0.5f * (sqrtf(1.0f - f * f) + 1.0f);
}

static float expo_in(float t) { return (t == 0.0f) ? 0.0f : powf(2.0f, 10.0f * (t - 1.0f)); }
static float expo_out(float t) { return (t == 1.0f) ? 1.0f : 1.0f - powf(2.0f, -10.0f * t); }
static float expo_inout(float t) {
	if (t == 0.0f) return 0.0f;
	if (t == 1.0f) return 1.0f;
	if (t < 0.5f) return 0.5f * powf(2.0f, 20.0f * t - 10.0f);
	return 1.0f - 0.5f * powf(2.0f, -20.0f * t + 10.0f);
}

static float elastic_in(float t) {
	return sinf(13.0f * (float)M_PI * 0.5f * t) * powf(2.0f, 10.0f * (t - 1.0f));
}
static float elastic_out(float t) {
	return sinf(-13.0f * (float)M_PI * 0.5f * (t + 1.0f)) * powf(2.0f, -10.0f * t) + 1.0f;
}
static float elastic_inout(float t) {
	if (t < 0.5f)
		return 0.5f * sinf(13.0f * (float)M_PI * t) * powf(2.0f, 10.0f * (2.0f * t - 1.0f));
	return 0.5f * (sinf(-13.0f * (float)M_PI * t) * powf(2.0f, -10.0f * (2.0f * t - 1.0f)) + 2.0f);
}

static float bounce_out(float t) {
	if (t < 1.0f / 2.75f) return 7.5625f * t * t;
	if (t < 2.0f / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
	if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
	t -= 2.625f / 2.75f;
	return 7.5625f * t * t + 0.984375f;
}
static float bounce_in(float t) { return 1.0f - bounce_out(1.0f - t); }
static float bounce_inout(float t) {
	if (t < 0.5f) return 0.5f * bounce_in(2.0f * t);
	return 0.5f * bounce_out(2.0f * t - 1.0f) + 0.5f;
}

static float back_in(float t) { return t * t * (2.70158f * t - 1.70158f); }
static float back_out(float t) { float f = t - 1.0f; return f * f * (2.70158f * f + 1.70158f) + 1.0f; }
static float back_inout(float t) {
	if (t < 0.5f) {
		float f = 2.0f * t;
		return 0.5f * f * f * (3.59491f * f - 2.59491f);
	}
	float f = 2.0f * t - 2.0f;
	return 0.5f * (f * f * (3.59491f * f + 2.59491f) + 2.0f);
}

typedef float (*ease_fn)(float);

static const ease_fn ease_table[][3] = {
	{ linear,      linear,       linear },
	{ quad_in,     quad_out,     quad_inout },
	{ cubic_in,    cubic_out,    cubic_inout },
	{ quart_in,    quart_out,    quart_inout },
	{ quint_in,    quint_out,    quint_inout },
	{ sine_in,     sine_out,     sine_inout },
	{ circ_in,     circ_out,     circ_inout },
	{ expo_in,     expo_out,     expo_inout },
	{ elastic_in,  elastic_out,  elastic_inout },
	{ bounce_in,   bounce_out,   bounce_inout },
	{ back_in,     back_out,     back_inout },
};

float get_eased(float t, int easing_type, int easing_function)
{
	if (easing_type == EASE_NONE || easing_function == EASING_LINEAR)
		return t;
	if (easing_function < 0 || easing_function > EASING_BACK)
		return t;

	int col;
	switch (easing_type) {
	case EASE_IN:     col = 0; break;
	case EASE_OUT:    col = 1; break;
	case EASE_IN_OUT: col = 2; break;
	default:          return t;
	}
	return ease_table[easing_function][col](t);
}
