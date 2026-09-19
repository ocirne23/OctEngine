module;

#include <immintrin.h>
#include <cstddef>

export module Animation:BoneMath;

import Core;
import Core.glm;
import Core.Transform;

// The numeric kernels of the rigid-bone animators (Entity's SceneAnimatorComponent and
// HumanoidAnimatorComponent). EVERYTHING here is an inline function or a plain struct of registers: it
// compiles into its caller, there is no call and no indirection to pay for the sharing.

// The SSE paths load a Transform as two registers: (pos.xyz, scale) and quat (x, y, z, w).
static_assert(sizeof(Transform) == 32 && offsetof(Transform, pos) == 0 && offsetof(Transform, scale) == 12 && offsetof(Transform, quat) == 16);
static_assert(sizeof(glm::quat) == 16 && offsetof(glm::quat, x) == 0 && offsetof(glm::quat, w) == 12);

export namespace BoneMath
{
    // sin / cos by polynomial, |x| <= pi/2. Within 3e-7 for |x| <= pi/4; 2e-4 at the limit, where a
    // quaternion built from it wants a normalize.
    inline void polyTrig(float x, float& outSin, float& outCos)
    {
        const float x2 = x * x;
        outSin = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f))));
        outCos = 1.0f + x2 * (-0.5f + x2 * (1.0f / 24.0f + x2 * (-1.0f / 720.0f + x2 * (1.0f / 40320.0f))));
    }

    // The same, four lanes.
    inline void polyTrig4(__m128 x, __m128& outSin, __m128& outCos)
    {
        const __m128 x2 = _mm_mul_ps(x, x);
        __m128 s = _mm_fmadd_ps(x2, _mm_set1_ps(-1.0f / 5040.0f), _mm_set1_ps(1.0f / 120.0f));
        s = _mm_fmadd_ps(x2, s, _mm_set1_ps(-1.0f / 6.0f));
        outSin = _mm_mul_ps(_mm_fmadd_ps(x2, s, _mm_set1_ps(1.0f)), x);
        __m128 c = _mm_fmadd_ps(x2, _mm_set1_ps(1.0f / 40320.0f), _mm_set1_ps(-1.0f / 720.0f));
        c = _mm_fmadd_ps(x2, c, _mm_set1_ps(1.0f / 24.0f));
        c = _mm_fmadd_ps(x2, c, _mm_set1_ps(-0.5f));
        outCos = _mm_fmadd_ps(x2, c, _mm_set1_ps(1.0f));
    }

    // sin / cos of a whole-turn phase in 0..1, without the CRT: fold onto the nearest quadrant axis, so
    // the polynomial only ever sees |x| <= pi/4 (error < 4e-7), then rotate by the quadrant.
    inline void phaseTrig(float phase, float& outSin, float& outCos)
    {
        constexpr float HalfPi = 1.57079632679f;
        const float turns4 = phase * 4.0f;
        const int quadrant = int(turns4 + 0.5f); // 0..4
        float s, c;
        polyTrig((turns4 - float(quadrant)) * HalfPi, s, c);
        switch (quadrant & 3)
        {
        case 0:  outSin = s;  outCos = c;  break;
        case 1:  outSin = c;  outCos = -s; break;
        case 2:  outSin = -s; outCos = -c; break;
        default: outSin = -c; outCos = s;  break;
        }
    }

    // sin(2 pi phase) ALONE, phase in 0..1, with NO branch: phaseTrig's quadrant switch depends on the
    // data (the walk phases of a crowd are random), so it mispredicts; this folds with min / sign bits
    // instead. sin(2 pi p) = -sin(2 pi t) for t = p - 0.5, and |t| > 0.25 mirrors onto 0.5 - |t|, so the
    // polynomial sees 0..pi/2 (degree 9: error < 4e-6).
    inline float phaseSin(float phase)
    {
        constexpr float TwoPi = 6.28318530718f;
        const __m128 signBit = _mm_set_ss(-0.0f);
        const __m128 t = _mm_set_ss(phase - 0.5f);
        const __m128 a = _mm_andnot_ps(signBit, t); // |t|
        const float x = _mm_cvtss_f32(_mm_min_ss(a, _mm_sub_ss(_mm_set_ss(0.5f), a))) * TwoPi;
        const float x2 = x * x;
        const float p = x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
        // the result's sign is the OPPOSITE of t's: p >= 0, so xor in the flipped sign bit of t
        return _mm_cvtss_f32(_mm_xor_ps(_mm_set_ss(p), _mm_andnot_ps(t, signBit)));
    }

    // composeTransform(parent, local) for MANY locals under ONE parent: the parent is prepared once - its
    // rotation as three matrix columns (x the scale, w lane 0), its quaternion as the four sign-folded
    // broadcast rows of a quaternion product. Per local: two loads, six shuffles, seven FMAs, two stores.
    struct ParentCompose
    {
        __m128 col0, col1, col2, origin, scaleLane, qW, qX, qY, qZ;

        explicit ParentCompose(const Transform& parent)
        {
            const glm::quat& q = parent.quat;
            const float s = parent.scale;
            const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
            const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
            const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
            col0 = _mm_setr_ps(s * (1.0f - 2.0f * (yy + zz)), s * 2.0f * (xy + wz), s * 2.0f * (xz - wy), 0.0f);
            col1 = _mm_setr_ps(s * 2.0f * (xy - wz), s * (1.0f - 2.0f * (xx + zz)), s * 2.0f * (yz + wx), 0.0f);
            col2 = _mm_setr_ps(s * 2.0f * (xz + wy), s * 2.0f * (yz - wx), s * (1.0f - 2.0f * (xx + yy)), 0.0f);
            origin = _mm_setr_ps(parent.pos.x, parent.pos.y, parent.pos.z, 0.0f);
            scaleLane = _mm_setr_ps(0.0f, 0.0f, 0.0f, s);
            qW = _mm_set1_ps(q.w);
            qX = _mm_setr_ps(q.x, -q.x, q.x, -q.x);
            qY = _mm_setr_ps(q.y, q.y, -q.y, -q.y);
            qZ = _mm_setr_ps(-q.z, q.z, q.z, -q.z);
        }

        inline void apply(const Transform& local, Transform& out) const
        {
            const __m128 l = _mm_loadu_ps(&local.pos.x); // (x, y, z, scale)
            __m128 posScale = _mm_fmadd_ps(col0, _mm_shuffle_ps(l, l, _MM_SHUFFLE(0, 0, 0, 0)), origin);
            posScale = _mm_fmadd_ps(col1, _mm_shuffle_ps(l, l, _MM_SHUFFLE(1, 1, 1, 1)), posScale);
            posScale = _mm_fmadd_ps(col2, _mm_shuffle_ps(l, l, _MM_SHUFFLE(2, 2, 2, 2)), posScale);
            posScale = _mm_fmadd_ps(scaleLane, l, posScale); // w lane: parent.scale * local.scale

            const __m128 b = _mm_loadu_ps(&local.quat.x); // parent.quat * b
            __m128 rot = _mm_mul_ps(qW, b);
            rot = _mm_fmadd_ps(qX, _mm_shuffle_ps(b, b, _MM_SHUFFLE(0, 1, 2, 3)), rot); // b.wzyx
            rot = _mm_fmadd_ps(qY, _mm_shuffle_ps(b, b, _MM_SHUFFLE(1, 0, 3, 2)), rot); // b.zwxy
            rot = _mm_fmadd_ps(qZ, _mm_shuffle_ps(b, b, _MM_SHUFFLE(2, 3, 0, 1)), rot); // b.yxwz

            _mm_storeu_ps(&out.pos.x, posScale);
            _mm_storeu_ps(&out.quat.x, rot);
        }
    };
}
