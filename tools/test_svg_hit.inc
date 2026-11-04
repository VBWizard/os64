static void fill_hit_tests(void)
{
    Ledger l={0};svg_options_t o=options(&l);svg_context_t *c=NULL;
    CHECK(svg_create(&o,&c)==SVG_OK);
    const struct {const char *d;double x,y;bool odd,hit;} cases[]={
        {"M0 0H10V10H0Z",5,5,false,true},
        {"M0 0H10V10H0Z",10,5,false,true},
        {"M0 0H10V10H0Z",10.001,5,false,false},
        {"M0 0L10 0L0 10",2,2,false,true},
        {"M0 0L10 0L0 10",8,8,false,false},
        {"M0 0H10V10H0Z M2 2H8V8H2Z",5,5,false,true},
        {"M0 0H10V10H0Z M2 2H8V8H2Z",5,5,true,false},
        {"M0 0H10V10H0Z M2 2V8H8V2Z",5,5,false,false},
        {"M0 0H10V10H0Z M2 2V8H8V2Z",2,5,true,true},
        {"M0 0 Q5 10 10 0Z",5,4,false,true},
        {"M0 0 Q5 10 10 0Z",5,5,false,true},
        {"M0 0 Q5 10 10 0Z",5,5.001,false,false},
        {"M0 0 C0 10 10 10 10 0Z",5,7.5,false,true},
        {"M0 0 C0 10 10 10 10 0Z",5,7.501,false,false},
        {"M10 0A10 10 0 1 1 -10 0A10 10 0 1 1 10 0Z",0,0,false,true},
        {"M10 0A10 10 0 1 1 -10 0A10 10 0 1 1 10 0Z",0,10,false,true},
        {"M10 0A10 10 0 1 1 -10 0A10 10 0 1 1 10 0Z",0,10.001,false,false},
        {"M1 1",1,1,false,false},
        {"M1 1L1 1",1,1,false,true},
        {"M0 0L10 0",5,0,false,true},
        {"M0 0H10V10H0Z",NAN,5,false,false},
        {"M0 0H10V10H0Z",5,INFINITY,false,false},
        {"M0 0H1e300V1e300H0Z",5e299,5e299,false,true},
        {"M0 0H1e-200V1e-200H0Z",5e-201,5e-201,false,true},
    };
    for(size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        svg_path_t *p=parse(c,cases[i].d,SVG_OK);bool inside=false;
        size_t allocations=l.allocations, before=l.polls;
        CHECK(svg_path_contains(c,p,(os64_geometry_point_t){cases[i].x,cases[i].y},cases[i].odd,&inside)==SVG_OK);
        if(inside!=cases[i].hit)fprintf(stderr,"hit case %zu\n",i);
        CHECK(inside==cases[i].hit && allocations==l.allocations);
        size_t polls=l.polls-before;
        for(size_t j=1;j<=polls;j++) {
            l.cancel=l.polls+j;inside=true;
            CHECK(svg_path_contains(c,p,(os64_geometry_point_t){cases[i].x,cases[i].y},false,&inside)==SVG_CANCELLED && !inside);
            l.cancel=0;
        }
        CHECK(svg_path_release(c,p)==SVG_OK);
    }
    /* Analytic circle membership across both sweep directions, away from its
     * boundary; this catches missed extrema and incorrect endpoint counting. */
    for(unsigned reverse=0;reverse<2;reverse++) {
        svg_path_t *p=parse(c,reverse?"M10 0A10 10 0 1 0 -10 0A10 10 0 1 0 10 0Z":
            "M10 0A10 10 0 1 1 -10 0A10 10 0 1 1 10 0Z",SVG_OK);
        for(int y=-22;y<=22;y++)for(int x=-22;x<=22;x++) {
            bool inside=false;double px=x*.5,py=y*.5;
            CHECK(svg_path_contains(c,p,(os64_geometry_point_t){px,py},false,&inside)==SVG_OK);
            if(inside!=(px*px+py*py<=100))fprintf(stderr,"circle reverse %u x %g y %g hit %d\n",reverse,px,py,inside);
            CHECK(inside==(px*px+py*py<=100));
        }
        CHECK(svg_path_release(c,p)==SVG_OK);
    }
    const char *curves[]={"M0 0Q5 10 10 0Z", "M0 0L10 0A10 10 0 0 1 0 10Z",
        "M16 12A20 10 36.86989764584402 1 1 -16 -12A20 10 36.86989764584402 1 1 16 12Z"};
    for(unsigned shape=0;shape<3;shape++) {
        svg_path_t *p=parse(c,curves[shape],SVG_OK);
        for(int y=-24;y<=24;y++)for(int x=-24;x<=24;x++) {
            bool expected;
            if(shape==0)expected=x>=0&&x<=10&&y>=0&&y<=2*x*(1-x/10.0);
            else if(shape==1)expected=x>=0&&y>=0&&x*x+y*y<=100;
            else {
                double u=.8*x+.6*y,v=-.6*x+.8*y,d=u*u/400+v*v/100;
                if(fabs(d-1)<1e-6)continue;
                expected=d<1;
            }
            bool inside=false;
            CHECK(svg_path_contains(c,p,(os64_geometry_point_t){x,y},false,&inside)==SVG_OK);
            CHECK(inside==expected);
        }
        CHECK(svg_path_release(c,p)==SVG_OK);
    }
    CHECK(svg_destroy(c)==SVG_OK && !l.live);
}
