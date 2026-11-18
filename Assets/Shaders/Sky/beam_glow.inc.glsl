#ifndef BEAM_GLOW_INC_GLSL
#define BEAM_GLOW_INC_GLSL

// ONE BEAM'S GLOW IN THE AIR, in closed form - no march, no shadow ray, so no noise. Along the view ray the squared
// distance to the beam's line is a quadratic in t, and the two glow profiles integrate exactly over it: a tight CORE
// r^3 / (d^2 + r^2)^2 and a weak, wide HALO r / (d^2 + r^2) that ends at the light's range. The core is cut to the beam's
// length (the planes across its two ends), the halo to the range past them, both to the scene (tMax).
// Shared by the fog apply's columns (vol_apply.fs.glsl beamColumns: every pixel, the frame's first BEAM_COLUMNS beams)
// and the beam cards (beam_card.fs.glsl: one beam, on the pixels of its card only), so both draw the same glow.
// eye = the ray's origin, foot = the beam's start, axis = its unit direction, r = the core radius, range = the halo's
// reach, tipFade (m; 0 = none) = the fade toward the far end. Returns the glow (x the beam's colour); tp = the distance
// along the ray where it passes the beam.
#define BEAM_HALO 0.06
float beamGlow(vec3 eye, vec3 dir, float tMax, vec3 foot, vec3 axis, float len, float r, float range, float tipFade, out float tp)
{
    tp = 0.0;
    const vec3 w = eye - foot;

    // The part of the ray beside the beam: u(t) = dot(w, axis) + t * dot(dir, axis) within [0, len] for the CORE
    // (ta .. tb), and within the range past each end for the HALO (ha .. hb): its ends are ROUND (below).
    const float wa = dot(w, axis);
    const float da = dot(dir, axis);
    float ta = 0.0, tb = tMax;
    float ha = 0.0, hb = tMax;
    if (abs(da) > 1e-5)
    {
        const float t0 = -wa / da, t1 = (len - wa) / da;
        const float tr = range / abs(da);
        ta = max(ta, min(t0, t1));
        tb = min(tb, max(t0, t1));
        ha = max(ha, min(t0, t1) - tr);
        hb = min(hb, max(t0, t1) + tr);
    }
    else if (wa < -range || wa > len + range)
        return 0.0;
    else if (wa < 0.0 || wa > len)
        tb = ta; // beside an end: the halo only
    if (hb <= ha)
        return 0.0;

    // d^2(t) = s2 (t - tc)^2 + h2, across the axis.
    const vec3 wp = w - axis * wa;
    const vec3 dp = dir - axis * da;
    const float s2 = max(dot(dp, dp), 1e-8);
    const float tc = -dot(wp, dp) / s2;
    const float h2 = max(dot(wp, wp) - tc * tc * s2, 0.0);
    if (h2 >= range * range)
        return 0.0;

    const float q2 = h2 + r * r;
    const float q = sqrt(q2);
    const float s = sqrt(s2);
    // Core: the integral of r^3 / (q2 + x^2)^2 dx / s.
    float core = 0.0;
    if (tb > ta)
    {
        const float xa = s * (ta - tc), xb = s * (tb - tc);
        core = r * r * r * (xb / (q2 + xb * xb) - xa / (q2 + xa * xa) + (atan(xb / q) - atan(xa / q)) / q) / (2.0 * q2 * s);
    }
    // Halo: the integral of r / (q2 + x^2), faded out to the range by the distance to the beam as a CAPSULE - from the
    // point where the ray passes the beam to the SEGMENT, not the line - so the glow ends ROUND at the foot and the
    // tip. Cut on the end planes it showed a flat bottom and top.
    // PAST an end the profile's own distance is the capsule's too (qh: + past^2). With the line's distance alone, a
    // ray that passed the line beyond the tip still got the profile's peak: a thin dim beam going on past the tip.
    tp = clamp(tc, ha, hb);
    const float along = wa + tp * da;
    const float past = max(max(-along, along - len), 0.0);
    const float fade = max(1.0 - (h2 + s2 * (tp - tc) * (tp - tc) + past * past) / (range * range), 0.0);
    const float qh = sqrt(q2 + past * past);
    const float halo = BEAM_HALO * fade * fade * r * (atan(s * (hb - tc) / qh) - atan(s * (ha - tc) / qh)) / (qh * s);
    // THE TIP FADE: the glow fades out over that length toward the beam's far end, by where the ray passes the beam -
    // a short beam then ends soft, not as a cut.
    return (core + halo) * (tipFade > 0.0 ? smoothstep(0.0, tipFade, len - along) : 1.0);
}

#endif
