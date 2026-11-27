/* THE FULLSCREEN API, an element moved into a frame's document while full
 * screen (libdom/fullscreen.c). A frame's document is another arena of the
 * page's lifetime group (the child-install fixture's child, published to
 * the parent as C), so libhtml takes the element's state only from the
 * frame's document: the move asks the host to leave, and the leaving clears
 * `:fullscreen` through the element's own document, not the binding's.
 * Run by --fullscreen, after test_dom_fullscreen.inc's cases. */

static void dom_fullscreen_frame_cases(void)
{
    ChildInstallFixture f;
    int policy = OS64_JS_TEARDOWN_FATAL;
    if (!child_install_open(&f, policy, true)) {
        child_install_close(&f, policy);
        return;
    }
    memset(&fs_host, 0, sizeof(fs_host));
    memset(&fs_view, 0, sizeof(fs_view));
    fs_view.view.width = 800;
    fs_view.view.height = 600;
    os64_dom_view_provider_t view = {fake_view_read, fake_view_scroll, &fs_view, NULL};
    os64_dom_set_view(f.binding, &view);
    os64_dom_set_fullscreen(f.binding, &fs_provider);
    os64_js_outcome_t out;
    f.pending = child_install_document();
    os64_dom_child_t child = child_install_attempt(&f, &out);
    check(child.state == OS64_DOM_CHILD_ACTIVE && child.consumed, "fullscreen frame: the frame's document installed");
    child_install_unconsumed(&f.pending, child);
    if (child.state == OS64_DOM_CHILD_ACTIVE) {
        child_install_publish(&f, child, "C");
        fs_host.activated = true;
        realm_script(f.parent, STORAGE_ASSERT "globalThis.v=document.createElement('div');document.body.appendChild(v);"
                     "globalThis.R=[];v.requestFullscreen().then(()=>R.push('ok'),e=>R.push(e.name));",
                     "fullscreen frame: v asks");
        check(fs_host.entered == 1, "fullscreen frame: the host is asked");
        os64_dom_fullscreen_settled(f.binding, true);
        for (int i = 0; i < 32 && os64_dom_view_step(f.binding, &out); i++)
            check(out.status == OS64_JS_OK, "fullscreen frame: in, stepped");
        realm_script(f.parent, STORAGE_ASSERT "assert(R.join()==='ok'&&document.fullscreenElement===v&&"
                     "v.matches(':fullscreen'),'in: '+R.join());"
                     "C.document.body.appendChild(v);assert(v.ownerDocument===C.document,'moved into the frame');",
                     "fullscreen frame: v moved into the frame's document");
        const os64_html_node_t *moved = fs_host.asked;
        check(moved != NULL, "fullscreen frame: the host was told the element");
        unsigned before = fs_host.left;
        for (int i = 0; i < 32 && os64_dom_view_step(f.binding, &out); i++)
            check(out.status == OS64_JS_OK, "fullscreen frame: the move, stepped");
        check(fs_host.left == before + 1, "fullscreen frame: the move asks the host to leave");
        os64_dom_fullscreen_exit(f.binding);
        for (int i = 0; i < 32 && os64_dom_view_step(f.binding, &out); i++)
            check(out.status == OS64_JS_OK, "fullscreen frame: out, stepped");
        check(moved != NULL && (moved->state & OS64_HTML_STATE_FULLSCREEN) == 0,
              "fullscreen frame: the state bit is cleared in the frame's document");
        realm_script(f.parent, STORAGE_ASSERT "assert(document.fullscreenElement===null,'none');"
                     "assert(!v.matches(':fullscreen'),'v no longer matches :fullscreen');",
                     "fullscreen frame: cleared");
    }
    child_install_close(&f, policy);
}
