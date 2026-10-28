static void radial_double_root_contracts(void)
{
    const double radii[][2] = {{.5, .2},
                               {.2, .5},
                               {.7, .4},
                               {.4, .7},
                               {.3344016906625016, .23941596018595857},
                               {.5230502317000981, .07086288409434749}};
    const double scales[] = {1e-12, 1, 1e12};
    svg_gradient_stop_t stops[] = {{0, 0xffff0000}, {1, 0xff0000ff}};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    for (size_t i = 0; i < sizeof(radii) / sizeof(radii[0]); ++i)
        for (size_t j = 0; j < sizeof(scales) / sizeof(scales[0]); ++j) {
            double scale = scales[j];
            svg_radial_gradient_t g = {.cx = .5 * scale,
                                       .cy = .5 * scale,
                                       .fx = .5 * scale,
                                       .fy = .5 * scale,
                                       .r = radii[i][0] * scale,
                                       .fr = radii[i][1] * scale,
                                       .transform = map,
                                       .stops = stops,
                                       .count = 2};
            svg_radial_sampler_t s;
            double t;
            assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
            assert(svg_radial_gradient_position(&s, g.cx, g.cy, &t));
            assert(fabs(t + g.fr / (g.r - g.fr)) < 1e-12);
            uint32_t expected = g.r > g.fr ? 0xffff0000 : 0xff0000ff;
            assert(svg_radial_gradient_sample(&s, g.cx, g.cy) == expected);
        }
    svg_radial_gradient_t g = {
        .cx = 1, .r = .5, .fr = .2, .transform = map, .stops = stops, .count = 2};
    svg_radial_sampler_t s;
    double t;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    double edge = (.4 + 2.0 / 3) * .3 / sqrt(.91);
    assert(svg_radial_gradient_position(&s, .4, edge, &t));
    assert(fabs(t - .46 / .91) < 1e-7);
    assert(svg_radial_gradient_position(&s, .4, edge - 1e-8, &t));
    assert(!svg_radial_gradient_position(&s, .4, edge + 1e-8, &t) && t == 0);
    assert(svg_radial_gradient_sample(&s, .4, edge + 1e-8) == 0);
    // A clearly negative-radius double root remains uncovered.
    double back = (-1 + 2.0 / 3) * .3 / sqrt(.91);
    assert(!svg_radial_gradient_position(&s, -1, back, &t));
    assert(svg_radial_gradient_sample(&s, -1, back) == 0);
    puts("SVG radial double roots: concentric centers, scales, reversed radii and cone boundary "
         "pass");
}
static void radial_sampler_contracts(void)
{
    radial_double_root_contracts();
    svg_gradient_stop_t stops[] = {{0, 0xffff0000}, {1, 0xff0000ff}};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    svg_radial_gradient_t g = {.r = 100, .transform = map, .stops = stops, .count = 2};
    svg_radial_sampler_t s;
    double t;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 0, 0) == 0xffff0000);
    assert(svg_radial_gradient_sample(&s, 30, 40) == 0xff800080);
    assert(svg_radial_gradient_position(&s, 30, 40, &t) && fabs(t - .5) < 1e-12);
    assert(svg_radial_gradient_sample(&s, 100, 0) == 0xff0000ff);
    assert(svg_radial_gradient_sample(&s, 200, 0) == 0xff0000ff);
    g.fr = 25;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 0, 0) == 0xffff0000);
    assert(svg_radial_gradient_sample(&s, 62.5, 0) == 0xff800080);
    g.spread = SVG_SPREAD_REPEAT;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 6.25, 0) == 0xff4000bf);
    g.spread = SVG_SPREAD_REFLECT;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 6.25, 0) == 0xffbf0040);
    g.spread = SVG_SPREAD_PAD;
    g.fr = 0;
    g.fx = 25;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 62.5, 0) == 0xff800080);
    g.fx = 0;
    g.r = 25;
    g.fr = 100;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 62.5, 0) == 0xff800080);
    g.r = 100;
    g.fr = 0;
    g.fx = 100;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 0, 0) == 0xff800080);
    assert(svg_radial_gradient_sample(&s, 150, 0) == 0xff0000ff);
    assert(!svg_radial_gradient_position(&s, 150, 0, &t) && t == 0);
    g.spread = SVG_SPREAD_REPEAT;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 150, 0) == 0xff800080);
    assert(svg_radial_gradient_sample(&s, 100, 0) == 0xff800080);
    g.cx = .5;
    g.cy = .5;
    g.fx = .8;
    g.fy = .5;
    g.r = .5;
    g.fr = .2;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK && s.tangent);
    g.cx = g.cy = g.fy = 0;
    g.r = 100;
    g.fr = 0;
    g.fx = 200;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 100, 200) == 0);
    g.fx = 0;
    g.fr = g.r;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_OK && !s.count);
    g.fr = -1;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_BAD_ARGUMENT && !s.count);
    g.fr = 0;
    g.cx = NAN;
    assert(svg_radial_gradient_prepare(&g, &map, NULL, &s) == SVG_BAD_ARGUMENT && !s.count);
    g.cx = 0;
    g.r = 1;
    g.object_bbox = true;
    g.transform = (os64_geometry_map_t){0, 1, -1, 0, 0, 0};
    svg_bounds_t box = {.min_x = 10, .min_y = 20, .max_x = 110, .max_y = 60};
    map = (os64_geometry_map_t){2, 0, 1, 3, 5, 7};
    assert(svg_radial_gradient_prepare(&g, &map, &box, &s) == SVG_OK);
    assert(svg_radial_gradient_sample(&s, 65, 127) == 0xff800080);
    box.max_y = box.min_y;
    assert(svg_radial_gradient_prepare(&g, &map, &box, &s) == SVG_OK && !s.count);
    puts("SVG numeric radial gradients: circle geometry, spread, cone, transforms and limits pass");
}
