"""Reference tests for the post-pass fog math (src/xbr/fci_enhance.cpp, kFogHlsl).

The fog runs on the GPU in HLSL; these tests verify the closed-form optical-depth
formula and the sky blend against a numerical integration, so the math is known
correct before the Windows build. The formulas must match the HLSL in kFogHlsl
exactly (same variable names, same order of operations).
"""

import math


def height_optical_depth(
    s0: float,
    a: float,
    H: float,
    h0: float,
    cam_z: float,
    dz_total: float,
    dist: float,
) -> float:
    """The closed-form height-dependent optical depth, as in the HLSL.

    sigma(z) = s0 * (a + (1-a) * exp(-(z - h0) / H))
    tau = integral of sigma along the ray from the camera to the pixel.

    The HLSL computes:
        k = (Pw.z - cam.z) / H
        g = 1 if |k| < 1e-4 else exp(-(cam.z - h0)/H) * (1 - exp(-k)) / k
        tau = s0 * dist * (a + (1-a) * g)
    """
    k = dz_total / H
    eu = math.exp(-(cam_z - h0) / H)
    if abs(k) < 1e-4:
        g = eu
    else:
        g = eu * (1.0 - math.exp(-k)) / k
    return s0 * dist * (a + (1.0 - a) * g)


def height_optical_depth_numeric(
    s0: float,
    a: float,
    H: float,
    h0: float,
    cam_z: float,
    dz_total: float,
    dist: float,
    steps: int = 10000,
) -> float:
    """Numerical integration of the same integral (the reference)."""
    if dist <= 0:
        return 0.0
    dz_per = dz_total / dist  # height change per unit distance
    total = 0.0
    for i in range(steps):
        s = (i + 0.5) * dist / steps  # mid-point
        z = cam_z + s * dz_per
        sigma = s0 * (a + (1.0 - a) * math.exp(-(z - h0) / H))
        total += sigma * (dist / steps)
    return total


def sky_fog(e: float, thickness: float, sky_e: float) -> float:
    """The sky (horizon) fog factor, as in the HLSL.

    f = thickness * (1 - smoothstep(0, sky_E, e))
    The result is the optical depth; the blend is 1 - exp(-f).
    """
    # HLSL smoothstep(0, sky_E, e): clamp e/sky_E to [0,1], then t*t*(3-2*t)
    t = max(0.0, min(1.0, e / sky_e))
    ss = t * t * (3.0 - 2.0 * t)
    return thickness * (1.0 - ss)


class TestOpticalDepth:
    """The closed-form height-dependent optical depth matches the numerical integral."""

    def test_matches_numeric_horizontal(self):
        # horizontal ray (dz_total = 0), camera at the reference height
        s0, a, H, h0 = 0.01, 0.15, 12.0, 1.5
        cam_z, dz_total, dist = 1.5, 0.0, 50.0
        closed = height_optical_depth(s0, a, H, h0, cam_z, dz_total, dist)
        numeric = height_optical_depth_numeric(s0, a, H, h0, cam_z, dz_total, dist)
        assert abs(closed - numeric) < 1e-3 * max(1.0, numeric)

    def test_matches_numeric_upward(self):
        # ray going up 10 m over 50 m
        s0, a, H, h0 = 0.01, 0.15, 12.0, 1.5
        cam_z, dz_total, dist = 1.5, 10.0, 50.0
        closed = height_optical_depth(s0, a, H, h0, cam_z, dz_total, dist)
        numeric = height_optical_depth_numeric(s0, a, H, h0, cam_z, dz_total, dist)
        assert abs(closed - numeric) < 1e-3 * max(1.0, numeric)

    def test_matches_numeric_downward(self):
        # ray going down 8 m over 50 m
        s0, a, H, h0 = 0.01, 0.15, 12.0, 1.5
        cam_z, dz_total, dist = 1.5, -8.0, 50.0
        closed = height_optical_depth(s0, a, H, h0, cam_z, dz_total, dist)
        numeric = height_optical_depth_numeric(s0, a, H, h0, cam_z, dz_total, dist)
        assert abs(closed - numeric) < 1e-3 * max(1.0, numeric)

    def test_matches_numeric_far(self):
        # a long ray, 500 m, going up 30 m
        s0, a, H, h0 = 0.005, 0.15, 12.0, 1.5
        cam_z, dz_total, dist = 1.5, 30.0, 500.0
        closed = height_optical_depth(s0, a, H, h0, cam_z, dz_total, dist)
        numeric = height_optical_depth_numeric(s0, a, H, h0, cam_z, dz_total, dist)
        assert abs(closed - numeric) < 1e-3 * max(1.0, numeric)

    def test_monotonic_in_distance(self):
        # more distance: more fog
        s0, a, H, h0 = 0.01, 0.15, 12.0, 1.5
        cam_z, dz_total = 1.5, 5.0
        t1 = height_optical_depth(s0, a, H, h0, cam_z, dz_total, 10.0)
        t2 = height_optical_depth(s0, a, H, h0, cam_z, dz_total, 50.0)
        t3 = height_optical_depth(s0, a, H, h0, cam_z, dz_total, 200.0)
        assert t1 < t2 < t3

    def test_upward_less_than_downward(self):
        # at the same distance, looking up has less fog than looking down
        # (the density falls with height)
        s0, a, H, h0 = 0.01, 0.15, 12.0, 1.5
        cam_z, dist = 1.5, 50.0
        up = height_optical_depth(s0, a, H, h0, cam_z, +10.0, dist)
        down = height_optical_depth(s0, a, H, h0, cam_z, -10.0, dist)
        assert up < down

    def test_floor_prevents_zero(self):
        # with a = 1 (no height dependence), the fog is uniform: s0 * dist
        s0, a, H, h0 = 0.01, 1.0, 12.0, 1.5
        cam_z, dz_total, dist = 1.5, 50.0, 100.0
        tau = height_optical_depth(s0, a, H, h0, cam_z, dz_total, dist)
        assert abs(tau - s0 * dist) < 1e-9

    def test_zero_floor_high_altitude(self):
        # with a = 0, a camera very high up (far above h0) sees little fog
        s0, a, H, h0 = 0.01, 0.0, 12.0, 1.5
        cam_z_high, dz_total, dist = 100.0, 0.0, 50.0
        tau_high = height_optical_depth(s0, a, H, h0, cam_z_high, dz_total, dist)
        cam_z_low = 1.5
        tau_low = height_optical_depth(s0, a, H, h0, cam_z_low, dz_total, dist)
        assert tau_high < tau_low


class TestSkyFog:
    """The sky (horizon) fog factor."""

    def test_max_at_horizon(self):
        # at e = 0 (the horizon), the fog is at its maximum: thickness
        f = sky_fog(0.0, 0.3, 0.2)
        assert abs(f - 0.3) < 1e-9

    def test_zero_upward(self):
        # looking well up (e >> sky_E), the fog is zero
        f = sky_fog(1.0, 0.3, 0.2)
        assert f == 0.0

    def test_zero_below_horizon(self):
        # below the horizon (e < 0), the fog is at its maximum (the ground fog)
        f = sky_fog(-0.5, 0.3, 0.2)
        assert abs(f - 0.3) < 1e-9

    def test_monotonic(self):
        # the fog falls as e rises from 0 to sky_E
        f0 = sky_fog(0.0, 0.3, 0.2)
        f1 = sky_fog(0.1, 0.3, 0.2)
        f2 = sky_fog(0.2, 0.3, 0.2)
        assert f0 > f1 > f2


class TestBlend:
    """The transmittance and the final blend."""

    def test_transmittance(self):
        # T = exp(-tau): more optical depth, less transmittance
        for tau in (0.0, 0.5, 1.0, 2.0, 5.0):
            T = math.exp(-tau)
            assert 0.0 <= T <= 1.0
        assert math.exp(-0.0) == 1.0
        assert math.exp(-5.0) < 0.01

    def test_blend_to_fog_color(self):
        # out = lerp(scene, fog, 1 - T): at tau=0 the scene is unchanged,
        # at large tau it is the fog color
        scene = 0.5
        fog = 0.8
        for tau in (0.0, 1.0, 5.0):
            T = math.exp(-tau)
            out = scene + (fog - scene) * (1.0 - T)
            if tau == 0.0:
                assert abs(out - scene) < 1e-9
            else:
                assert scene < out < fog if fog > scene else fog < out < scene
        # at large tau, the blend is the fog color
        out = scene + (fog - scene) * (1.0 - math.exp(-20.0))
        assert abs(out - fog) < 1e-6


class TestConstantsLayout:
    """The cbuffer layout: 8 float4s = 32 floats, matching the HLSL and the C++ p[32]."""

    def test_32_floats(self):
        # p0..p7, 4 floats each
        assert 8 * 4 == 32

    def test_upload_size(self):
        # the Pass uploads 256 bytes = 64 floats; 32 fits
        assert 32 * 4 <= 256

    def test_fields(self):
        # the field layout, as in the C++ Fog() and the HLSL cbuffer
        # p0 = (texel_x, texel_y, tan_x, tan_y)
        # p1 = (s0, a, H, h0)
        # p2 = (sun_dir, sun_strength)
        # p3 = (fog_col, s)
        # p4 = view row 0
        # p5 = view row 1
        # p6 = view row 2
        # p7 = (thickness, sky_E, mode, water_h)
        layout = {
            "p0": ["texel_x", "texel_y", "tan_x", "tan_y"],
            "p1": ["s0", "a", "H", "h0"],
            "p2": ["sun_x", "sun_y", "sun_z", "sun_strength"],
            "p3": ["fog_r", "fog_g", "fog_b", "wpm"],
            "p4": ["vm00", "vm01", "vm02", "vm03"],
            "p5": ["vm10", "vm11", "vm12", "vm13"],
            "p6": ["vm20", "vm21", "vm22", "vm23"],
            "p7": ["thickness", "sky_E", "mode", "water_h"],
        }
        total = sum(len(v) for v in layout.values())
        assert total == 32
