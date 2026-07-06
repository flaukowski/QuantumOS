/**
 * QuantumOS Resonance Math Helpers
 *
 * Shared freestanding math approximations for the resonance subsystem.
 * The kernel links without libm, so these small helpers stand in for the
 * handful of transcendental / utility functions the resonance code needs.
 *
 * Previously each of resonant_scheduler.c, geometric_control.c,
 * consciousness_process.c and chiral_resources.c carried its own private
 * copy of these routines (see the note in the repo CLAUDE.md). They are
 * consolidated here so there is a single definition to reason about.
 *
 * Every helper is `static inline`: each translation unit gets its own copy,
 * so there is no multiple-definition conflict at link time and any helper a
 * given file does not use is dropped without a -Wunused-function warning.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef KERNEL_RESONANCE_MATH_HELPERS_H
#define KERNEL_RESONANCE_MATH_HELPERS_H

#ifndef PI
#define PI 3.14159265358979323846
#endif
#ifndef TWO_PI
#define TWO_PI (2.0 * PI)
#endif

/* |x| */
static inline double fast_abs(double x) {
    return x < 0.0 ? -x : x;
}

/* Newton's-method square root (10 iterations); returns 0 for x <= 0. */
static inline double fast_sqrt(double x) {
    if (x <= 0.0)
        return 0.0;
    double guess = x * 0.5;
    for (int i = 0; i < 10; i++) {
        guess = (guess + x / guess) * 0.5;
    }
    return guess;
}

/* Taylor-series sine with argument reduced to [-PI, PI]. */
static inline double fast_sin(double x) {
    while (x > PI)
        x -= TWO_PI;
    while (x < -PI)
        x += TWO_PI;
    double x2 = x * x;
    double x3 = x2 * x;
    double x5 = x3 * x2;
    double x7 = x5 * x2;
    return x - x3 / 6.0 + x5 / 120.0 - x7 / 5040.0;
}

/* cos(x) = sin(x + PI/2). */
static inline double fast_cos(double x) {
    return fast_sin(x + PI / 2.0);
}

/* Rational-polynomial asin: asin(x) ≈ x*(1 + x²*(1/6 + x²*(3/40 + x²*15/336))). */
static inline double fast_asin(double x) {
    if (x > 1.0)
        x = 1.0;
    if (x < -1.0)
        x = -1.0;
    double x2 = x * x;
    return x * (1.0 + x2 * (1.0 / 6.0 + x2 * (3.0 / 40.0 + x2 * 15.0 / 336.0)));
}

/* Quadrant-aware atan2 built on fast_asin. */
static inline double fast_atan2(double y, double x) {
    double r = fast_sqrt(x * x + y * y);
    if (r < 1e-15)
        return 0.0;
    double sin_val = y / r;
    if (x > 0)
        return fast_asin(sin_val);
    if (x < 0 && y >= 0)
        return PI - fast_asin(sin_val);
    if (x < 0 && y < 0)
        return -PI - fast_asin(sin_val);
    if (y > 0)
        return PI / 2.0;
    if (y < 0)
        return -PI / 2.0;
    return 0.0;
}

/* Clamp value to the closed interval [min, max]. */
static inline double clamp(double value, double min, double max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

#endif /* KERNEL_RESONANCE_MATH_HELPERS_H */
