#include <assert.h>
static void gradient_sampler_contracts(void)
{
    svg_gradient_stop_t stops[] = {{0, 0xffff0000}, {1, 0xff0000ff}};
    os64_geometry_map_t map = {1, 0, 0, 1, 0, 0};
    svg_linear_gradient_t g = {.x2 = 100, .transform = map, .stops = stops, .count = 2};
    svg_linear_sampler_t s;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, -20, 0) == 0xffff0000);
    assert(svg_linear_gradient_sample(&s, 50, 999) == 0xff800080);
    assert(svg_linear_gradient_sample(&s, 120, 0) == 0xff0000ff);
    g.spread = SVG_SPREAD_REPEAT;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, 125, 0) == 0xffbf0040);
    assert(svg_linear_gradient_sample(&s, -25, 0) == 0xff4000bf);
    g.spread = SVG_SPREAD_REFLECT;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, 125, 0) == 0xff4000bf);
    assert(svg_linear_gradient_sample(&s, -25, 0) == 0xffbf0040);
    stops[0].argb = 0x00ff0000;
    g.spread = SVG_SPREAD_PAD;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, 50, 0) == 0x80800080);
    svg_gradient_stop_t edge[] = {
        {0, 0xffff0000}, {.5, 0xffff0000}, {.5, 0xff0000ff}, {1, 0xff0000ff}};
    g.stops = edge;
    g.count = 4;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, 49, 0) == 0xffff0000);
    assert(svg_linear_gradient_sample(&s, 50, 0) == 0xff0000ff);
    g.x2 = 0;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, -1, 0) == 0xff0000ff);
    g.x2 = 1;
    g.object_bbox = true;
    g.stops = stops;
    g.count = 2;
    stops[0].argb = 0xffff0000;
    svg_bounds_t box = {.min_x = 10, .min_y = 20, .max_x = 110, .max_y = 60};
    g.transform = (os64_geometry_map_t){0, 1, -1, 0, 1, 0};
    // Gradient rotation inside a 100x40 box: its midpoint is y=40.
    assert(svg_linear_gradient_prepare(&g, &map, &box, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, 60, 40) == 0xff800080);
    map = (os64_geometry_map_t){2, 0, 1, 3, 5, 7};
    assert(svg_linear_gradient_prepare(&g, &map, &box, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, 165, 127) == 0xff800080);
    box.max_y = box.min_y;
    assert(svg_linear_gradient_prepare(&g, &map, &box, &s) == SVG_OK && !s.count);
    g.object_bbox = false;
    map = (os64_geometry_map_t){0};
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK && !s.count);
    map = (os64_geometry_map_t){1, 0, 0, 1, 0, 0};
    g.count = 0;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK && !s.count);
    g.count = 1;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_OK);
    assert(svg_linear_gradient_sample(&s, 999, 999) == 0xffff0000);
    g.count = SVG_GRADIENT_STOPS_MAX + 1;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_LIMIT && !s.count);
    g.count = 2;
    stops[0].offset = .9;
    stops[1].offset = .1;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_BAD_ARGUMENT && !s.count);
    stops[0].offset = 0;
    stops[1].offset = NAN;
    assert(svg_linear_gradient_prepare(&g, &map, NULL, &s) == SVG_BAD_ARGUMENT && !s.count);
    printf("SVG numeric gradients: transforms, alpha, spread, discontinuities and limits pass\n");
}
