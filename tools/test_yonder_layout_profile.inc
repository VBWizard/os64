typedef struct { int64_t now, step; unsigned calls; } ProfileClock;
static int64_t profile_test_clock(void *opaque)
{
    ProfileClock *clock=opaque; ++clock->calls; clock->now+=clock->step; return clock->now;
}
static void layout_profile_cases(void)
{
    probe_page("<style>p{margin:0}</style><p id=p>first line</p><p>second line</p>",false);
    flow_env_t env=s_env;env.cascade=g.page.cascade;env.ctx=&g.page;
    flow_profile_t profile;
    ProfileClock clock={1000,13,0};
    flow_tree_t *tree=flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,&profile,profile_test_clock,&clock);
    check(tree && clock.calls==8 && profile.style_us==13 && profile.boxes_us==13 &&
          profile.layout_us==13 && profile.public_us==13 && !profile.clock_errors &&
          profile.elements>=2 && profile.boxes>=2 && profile.measures>=2 && !profile.reused,
          "layout profile: four independent phase clocks and actual work counts");
    char measured[16384],plain[16384];
    int64_t measured_len=flow_dump(tree,measured,sizeof(measured));flow_free(tree);
    clock.calls=0;
    tree=flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,NULL,profile_test_clock,&clock);
    int64_t plain_len=flow_dump(tree,plain,sizeof(plain));
    check(tree && clock.calls==0 && measured_len==plain_len && measured_len>0 &&
          measured_len<(int64_t)sizeof(measured) && !strcmp(measured,plain),
          "layout profile: disabled instrumentation reads no clock and preserves layout dumps");
    flow_free(tree);
    clock.step=-13;clock.calls=0;
    tree=flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,&profile,profile_test_clock,&clock);
    check(tree && profile.clock_errors==4 && !profile.style_us && !profile.boxes_us &&
          !profile.layout_us && !profile.public_us,"layout profile: backward clocks are explicit invalid samples");
    flow_free(tree);
    env.text=NULL;profile.style_us=999;
    check(!flow_layout_profiled(page_doc(&g.page),page_model(&g.page),800,&env,&profile,profile_test_clock,&clock) && !profile.style_us,
          "layout profile: refused input clears the output sink");

    g.page.diag=diag_new_claimed("https://fixture.test/profile",1);
    int64_t saved_step=clock_step;clock_step=17;s_layout_profile=true;
    g.page.sheets_changed=true;
    check(page_lay_out(&g.page,800,600,g.zoom,&g.page),"layout profile: instrumented window layout");
    const char *line=yonder_diag_fact_value(g.page.diag,"layout");
    check(line && strstr(line,"cascade 17 us") && strstr(line,"style 17 us") &&
          strstr(line,"public 17 us") && strstr(line,"1 layout attempts") && strstr(line,"0 clock errors"),
          "layout profile: phase evidence reaches the owning page record");
    check(!os64_html_set_attr(g.page.way.doc,probe_id("p"),"class","changed",7) && page_model_refresh(&g.page),
          "layout profile: model rebuild instrumented");
    line=yonder_diag_fact_value(g.page.diag,"layout");
    check(line && strstr(line,"model rebuild 17 us") && strstr(line,"1 model rebuilds"),
          "layout profile: model cost remains separate from layout");
    s_layout_profile=false;clock_step=saved_step;
    probe_drop();
}
