/* THE LOOP'S EVERY-TURN CHECKS cost what they find, never what the page
 * holds: the image check walks the wrapped image elements alone, and none
 * while the provider's answers and the binding's own changes stand still
 * after a pass that found nothing; the canvas damage visit walks the
 * canvases that may have damage to tell alone (drawn on, or resized, since
 * a visit last found them with none); a listener removed under a dispatch is
 * swept from its own record. os64_dom_checks_visited counts what the two
 * checks visit. */
static uint64_t turn_answers = 1;
static unsigned turn_acquires;
static os64_dom_image_status_t turn_image(void *opaque, const os64_html_node_t *node, bool pixels,
                                          os64_dom_image_view_t *out)
{
    (void)opaque; (void)pixels;
    turn_acquires++;
    const os64_html_attr_t *src = os64_html_attr(node, "src");
    *out = (os64_dom_image_view_t){.generation = 1};
    if (!src || !strcmp(src->value, "pending")) {
        out->pending = true;
        return OS64_DOM_IMAGE_PENDING;
    }
    if (!strcmp(src->value, "broken")) return OS64_DOM_IMAGE_BROKEN;
    out->surface = (draw2d_surface_t){NULL, 2, 2, 8, 16, DRAW2D_ARGB8_STRAIGHT};
    out->origin_clean = true;
    return OS64_DOM_IMAGE_READY;
}
static void turn_image_return(void *opaque, os64_dom_image_view_t *view) { (void)opaque; (void)view; }
static uint64_t turn_image_answers(void *opaque) { (void)opaque; return turn_answers; }

static unsigned turn_damaged;
static void turn_damage(void *opaque, const os64_html_node_t *node, uint32_t w, uint32_t h,
                        const draw2d_damage_t *damage)
{
    (void)opaque; (void)node; (void)w; (void)h; (void)damage;
    turn_damaged++;
}

/* One image check: whether it found an event, and the records it visited. */
static bool turn_poll(DomFixture *f, bool consume, const os64_html_node_t **node, uint64_t *visited)
{
    uint64_t before = os64_dom_checks_visited(f->binding);
    bool found = os64_dom_image_event(f->binding, node, NULL, consume);
    *visited = os64_dom_checks_visited(f->binding) - before;
    return found;
}

static const char *turn_id(const os64_html_node_t *node)
{
    const os64_html_attr_t *id = node != NULL ? os64_html_attr(node, "id") : NULL;
    return id != NULL ? id->value : "";
}

static void dom_turn_check_cases(void)
{
    DomFixture f = fixture_new(NULL);
    if (!fixture_ready(&f)) return;
    os64_dom_image_provider_t provider = {.acquire = turn_image, .release = turn_image_return,
                                          .answers = turn_image_answers};
    os64_dom_set_images(f.binding, &provider);
    turn_answers = 1;
    /* Thousands of held wrappers that are no image and no canvas, three
     * images waiting, and two canvases with a context in the tree. */
    dom_script(&f, STORAGE_ASSERT
        "var held=[];for(var i=0;i<4000;i++)held.push(document.createElement('div'));"
        "var a=new Image();a.id='a';a.src='pending';var b=new Image();b.id='b';b.src='pending';"
        "var c=document.createElement('img');c.id='c';c.src='pending';document.body.appendChild(c);"
        "var k1=document.createElement('canvas');k1.id='k1';document.body.appendChild(k1);var x1=k1.getContext('2d');"
        "var k2=document.createElement('canvas');k2.id='k2';document.body.appendChild(k2);var x2=k2.getContext('2d');"
        "var lone=document.createElement('canvas');lone.id='lone';",
        "turn checks: a page holding thousands of wrappers");
    uint64_t visited = 0;
    const os64_html_node_t *node = NULL;
    check(!turn_poll(&f, false, &node, &visited) && visited == 3,
          "turn checks: the image check walks the three images, not every wrapper");
    unsigned acquires = turn_acquires;
    for (int i = 0; i < 5; i++) {
        bool found = turn_poll(&f, false, &node, &visited);
        check(!found && visited == 0, "turn checks: a quiet turn's image check visits nothing");
    }
    check(turn_acquires == acquires, "turn checks: and asks the provider for no image");
    check(!turn_poll(&f, true, &node, &visited) && visited == 0,
          "turn checks: a quiet consume visits nothing either");

    /* The provider's answers move: one pass, which finds nothing new. */
    turn_answers++;
    check(!turn_poll(&f, false, &node, &visited) && visited == 3 && !turn_poll(&f, false, &node, &visited) &&
          visited == 0, "turn checks: moved answers make one pass, then quiet again");

    /* Two images complete together: the newest wrapper first, as `values`
     * held them, each its own event, then quiet. The script's source writes
     * are the binding's own change, which the provider's answers need not
     * show. */
    dom_script(&f, "a.src='clean';b.src='broken';", "turn checks: two images settle");
    check(turn_poll(&f, false, &node, &visited) && !strcmp(turn_id(node), "b"),
          "turn checks: a peek finds the newest image's event");
    check(turn_poll(&f, true, &node, &visited) && !strcmp(turn_id(node), "b"),
          "turn checks: and consumes it");
    check(turn_poll(&f, true, &node, &visited) && !strcmp(turn_id(node), "a"),
          "turn checks: then the older one's");
    check(!turn_poll(&f, true, &node, &visited) && visited == 3,
          "turn checks: then a pass that finds none");
    check(!turn_poll(&f, true, &node, &visited) && visited == 0,
          "turn checks: and quiet");

    /* A new wrapped image is the binding's own change. */
    dom_script(&f, "var d=new Image();d.id='d';d.src='clean';", "turn checks: a new image");
    check(turn_poll(&f, true, &node, &visited) && !strcmp(turn_id(node), "d") && visited == 1,
          "turn checks: a new image is found at once, first");
    check(!turn_poll(&f, true, &node, &visited) && !turn_poll(&f, true, &node, &visited) && visited == 0,
          "turn checks: and quiet after");

    /* An image adopted from another document now answers to this one. */
    dom_script(&f, "var other=document.implementation.createHTMLDocument('');"
        "var e=other.createElement('img');e.id='e';e.src='clean';"
        "var f2=other.createElement('img');f2.id='f2';f2.src='pending';",
        "turn checks: images in another document");
    (void)turn_poll(&f, true, &node, &visited);
    while (turn_poll(&f, true, &node, &visited)) {}
    check(!turn_poll(&f, true, &node, &visited) && visited == 0, "turn checks: quiet before an adoption");
    dom_script(&f, "document.adoptNode(f2);", "turn checks: an image adopted");
    check(!turn_poll(&f, true, &node, &visited) && visited > 0,
          "turn checks: an adoption makes the next check walk");

    /* The canvas damage visit walks the canvases that may have something to
     * tell: a new context's whole bitmap until it is painted, then a drawn
     * one until it is painted again. */
    uint64_t before = os64_dom_checks_visited(f.binding);
    turn_damaged = 0;
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(turn_damaged == 2 && os64_dom_checks_visited(f.binding) - before == 2,
          "turn checks: the canvas visit looks at the two new contexts, not every wrapper");
    os64_dom_canvas_painted(f.binding, native_id(f.document->document, "k1"));
    os64_dom_canvas_painted(f.binding, native_id(f.document->document, "k2"));
    before = os64_dom_checks_visited(f.binding);
    turn_damaged = 0;
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(turn_damaged == 0 && os64_dom_checks_visited(f.binding) - before == 2,
          "turn checks: painted, each is visited once more and leaves");
    before = os64_dom_checks_visited(f.binding);
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(os64_dom_checks_visited(f.binding) - before == 0,
          "turn checks: and then none is, while nothing draws");
    dom_script(&f, "x2.fillStyle='red';x2.fillRect(0,0,4,4);", "turn checks: one canvas drawn");
    before = os64_dom_checks_visited(f.binding);
    turn_damaged = 0;
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(turn_damaged == 1 && os64_dom_checks_visited(f.binding) - before == 1,
          "turn checks: a drawn canvas is visited, alone");
    before = os64_dom_checks_visited(f.binding);
    turn_damaged = 0;
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(turn_damaged == 1 && os64_dom_checks_visited(f.binding) - before == 1,
          "turn checks: and again while its damage is not painted");
    os64_dom_canvas_painted(f.binding, native_id(f.document->document, "k2"));
    before = os64_dom_checks_visited(f.binding);
    turn_damaged = 0;
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(turn_damaged == 0 && os64_dom_checks_visited(f.binding) - before == 1,
          "turn checks: painted, the drawn one is visited once more and leaves");
    /* Every context method puts its canvas back on the list, whichever file
     * serves it: a path's, text's, an image's. */
    static const char *const draws[] = {"x2.strokeRect(0,0,3,3);", "x2.fillText('a',1,8);", "x2.drawImage(k1,0,0);"};
    for (size_t i = 0; i < sizeof(draws) / sizeof(draws[0]); i++) {
        dom_script(&f, draws[i], "turn checks: a canvas drawn another way");
        before = os64_dom_checks_visited(f.binding);
        os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
        check(os64_dom_checks_visited(f.binding) - before == 1, "turn checks: each context method lists its canvas");
        os64_dom_canvas_painted(f.binding, native_id(f.document->document, "k2"));
        os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
        before = os64_dom_checks_visited(f.binding);
        os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
        check(os64_dom_checks_visited(f.binding) - before == 0, "turn checks: and painted, it leaves again");
    }
    dom_script(&f, "document.body.appendChild(lone);var x3=lone.getContext('2d');x3.fillRect(0,0,2,2);"
        "x1.fillRect(0,0,2,2);",
        "turn checks: a third canvas gains a context and two draw");
    turn_damaged = 0;
    before = os64_dom_checks_visited(f.binding);
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(turn_damaged == 2 && os64_dom_checks_visited(f.binding) - before == 2,
          "turn checks: a context made later joins the visit");
    dom_script(&f, "k2.width=40;", "turn checks: a canvas resized");
    turn_damaged = 0;
    before = os64_dom_checks_visited(f.binding);
    os64_dom_canvas_visit_damage(f.binding, turn_damage, NULL);
    check(turn_damaged == 3 && os64_dom_checks_visited(f.binding) - before == 3,
          "turn checks: a resized canvas tells its reset");

    /* A provider with no answers makes every pass, as before. */
    provider.answers = NULL;
    os64_dom_set_images(f.binding, &provider);
    check(!turn_poll(&f, false, &node, &visited) && visited == 6 &&
          !turn_poll(&f, false, &node, &visited) && visited == 6,
          "turn checks: with no answers every pass walks the images");

    /* A listener removed under a dispatch is swept from its record when the
     * dispatch ends: the listeners' memory comes back. */
    size_t listeners = os64_dom_memory(f.binding, OS64_DOM_MEM_LISTENER).bytes;
    dom_script(&f, STORAGE_ASSERT
        "var n=0,t=held[1234];t.addEventListener('x',function(){n++},{once:true});"
        "t.addEventListener('y',function g(){t.removeEventListener('y',g);n++});"
        "t.dispatchEvent(new Event('x'));t.dispatchEvent(new Event('x'));"
        "t.dispatchEvent(new Event('y'));t.dispatchEvent(new Event('y'));assert(n===2);",
        "turn checks: listeners removed under their dispatch");
    check(os64_dom_memory(f.binding, OS64_DOM_MEM_LISTENER).bytes == listeners,
          "turn checks: a listener removed under a dispatch is swept from its record");
    dom_script(&f, STORAGE_ASSERT
        "var m=0;held[7].addEventListener('z',function(){m++;held[7].dispatchEvent(new Event('w'))},{once:true});"
        "held[7].addEventListener('w',function(){m++},{once:true});"
        "held[7].dispatchEvent(new Event('z'));held[7].dispatchEvent(new Event('z'));assert(m===2);"
        "held[7].addEventListener('z',function(){m++});held[7].dispatchEvent(new Event('z'));assert(m===3);",
        "turn checks: removals under nested dispatches");
    fixture_free(&f);
}
