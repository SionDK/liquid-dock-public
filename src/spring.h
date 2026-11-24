#pragma once
#include <cmath>

// Critically-damped-ish spring used for every animation in the dock.
struct Spring {
    float x = 0, v = 0, target = 0;
    void step(float dt, float stiffness, float dampingRatio) {
        float c = 2.0f * dampingRatio * std::sqrt(stiffness);
        while (dt > 0) {
            float h = dt > (1.0f / 240.0f) ? (1.0f / 240.0f) : dt;
            float a = -stiffness * (x - target) - c * v;
            v += a * h;
            x += v * h;
            dt -= h;
        }
    }
    bool settled(float eps = 0.0015f) const { return std::fabs(x - target) < eps && std::fabs(v) < eps * 20; }
    void snap(float t) { x = target = t; v = 0; }
};
