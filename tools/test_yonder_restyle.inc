static void restyle_browser_cases(void)
{
    probe_page("<!doctype html><style id=css>p{margin:0}#kept{color:red}"
               ".wide{width:150px}</style><section><p id=changed>text</p></section>"
               "<aside><p id=kept>kept</p><a href=/one>link</a></aside>", false);
    uint64_t generation=garb_cascade_generation(g.page.cascade);
    os64_html_document_t *doc=g.page.way.doc;
    check(!os64_html_set_attr(doc,probe_id("changed"),"class","wide",4) && script_rebuild(),
          "restyle: changed page rebuilds");
    check(garb_cascade_generation(g.page.cascade)==generation &&
          garb_cascade_updates(g.page.cascade)==1 && flow_styles_reused(g.page.tree)>=3,
          "restyle: shown page retains cascade and copies unaffected styles");
    check(!os64_html_set_text(doc,probe_id("changed")->first_child,"measured",8) &&
          measured_layout(&g.page,g.page.state,g.page.way.url,800,600),
          "restyle: geometry read measures changed text");
    check(garb_cascade_generation(s_measured.cascade)==generation &&
          garb_cascade_updates(s_measured.cascade)==2,
          "restyle: geometry read shares incremental cascade");
    check(!os64_html_set_text(doc,probe_id("changed")->first_child,"again",5) &&
          measured_layout(&g.page,g.page.state,g.page.way.url,800,600),
          "restyle: second geometry read consumes next mutation");
    check(flow_styles_reused(s_measured.tree)>=3 &&
          garb_cascade_generation(s_measured.cascade)==generation,
          "restyle: repeated geometry reads reuse measured styles");
    check(script_rebuild() && !s_measured.doc &&
          garb_cascade_generation(g.page.cascade)==generation,
          "restyle: publishing releases measured owner while retaining shared cascade");
    const char *css="p{margin:0}#kept{color:blue;width:123px}";
    check(!os64_html_set_text(doc,probe_id("css")->first_child,css,strlen(css)) &&
          measured_layout(&g.page,g.page.state,g.page.way.url,800,600),
          "restyle: geometry read observes edited style element before publishing");
    const flow_box_t *box=flow_box_for(s_measured.tree,probe_id("kept"));
    check(box && box->style->color==0x0000ff && box->rect.w==123 &&
          garb_cascade_generation(s_measured.cascade)!=generation,
          "restyle: changed sheet replaces cascade and changes measured width and color");
    check(script_rebuild(),"restyle: stylesheet edit publishes");
    box=flow_box_for(g.page.tree,probe_id("kept"));
    check(box && box->style->color==0x0000ff && box->rect.w==123,
          "restyle: new sheet remains alive after measured and old shown trees are freed");
    generation=garb_cascade_generation(g.page.cascade);
    check(!os64_html_set_attr(doc,probe_id("css"),"media","not all",7) && script_rebuild(),
          "restyle: media mutation publishes");
    box=flow_box_for(g.page.tree,probe_id("kept"));
    check(box && box->style->color!=0x0000ff && garb_cascade_generation(g.page.cascade)!=generation,
          "restyle: media change invalidates an otherwise identical sheet parse");
    probe_drop();
}
