#pragma once

#define EASE_NONE    0
#define EASE_IN      1
#define EASE_OUT     2
#define EASE_IN_OUT  3

#define EASING_LINEAR      0
#define EASING_QUADRATIC   1
#define EASING_CUBIC       2
#define EASING_QUARTIC     3
#define EASING_QUINTIC     4
#define EASING_SINE        5
#define EASING_CIRCULAR    6
#define EASING_EXPONENTIAL 7
#define EASING_ELASTIC     8
#define EASING_BOUNCE      9
#define EASING_BACK       10

float get_eased(float t, int easing_type, int easing_function);
